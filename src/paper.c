#include "paper.h"

#include <math.h>
#include <string.h>

#define MAX_HISTORY 64

/* ---------- helpers ---------- */

double
origami_side_of_line (OrigamiPoint p, OrigamiPoint a, OrigamiPoint b)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

static OrigamiPoint
line_line_intersect (OrigamiPoint p1, OrigamiPoint p2,
                     OrigamiPoint a,  OrigamiPoint b)
{
    double s1 = origami_side_of_line (p1, a, b);
    double s2 = origami_side_of_line (p2, a, b);
    double denom = s1 - s2;
    /* When both endpoints are on the line the segment is degenerate; just
     * return the midpoint - the caller's logic will discard it via the
     * "in/out" classification. */
    double t = (fabs (denom) < 1e-12) ? 0.5 : (s1 / denom);
    OrigamiPoint r = {
        p1.x + t * (p2.x - p1.x),
        p1.y + t * (p2.y - p1.y)
    };
    return r;
}

static OrigamiPoint
reflect_across_line (OrigamiPoint p, OrigamiPoint a, OrigamiPoint b)
{
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double len2 = dx * dx + dy * dy;
    if (len2 < 1e-12)
        return p;
    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    double fx = a.x + t * dx;
    double fy = a.y + t * dy;
    OrigamiPoint r = { 2.0 * fx - p.x, 2.0 * fy - p.y };
    return r;
}

GArray *
origami_clip_half_plane (GArray *vertices,
                         OrigamiPoint a, OrigamiPoint b,
                         int keep_sign)
{
    GArray *out = g_array_new (FALSE, FALSE, sizeof (OrigamiPoint));
    if (vertices->len == 0)
        return out;

    for (guint i = 0; i < vertices->len; i++) {
        OrigamiPoint cur  = g_array_index (vertices, OrigamiPoint, i);
        OrigamiPoint next = g_array_index (vertices, OrigamiPoint,
                                           (i + 1) % vertices->len);

        double s_cur  = origami_side_of_line (cur,  a, b);
        double s_next = origami_side_of_line (next, a, b);

        gboolean cur_in  = (keep_sign > 0) ? (s_cur  >= 0) : (s_cur  <= 0);
        gboolean next_in = (keep_sign > 0) ? (s_next >= 0) : (s_next <= 0);

        if (cur_in) {
            g_array_append_val (out, cur);
            if (!next_in) {
                OrigamiPoint x = line_line_intersect (cur, next, a, b);
                g_array_append_val (out, x);
            }
        } else if (next_in) {
            OrigamiPoint x = line_line_intersect (cur, next, a, b);
            g_array_append_val (out, x);
        }
    }
    return out;
}

/* ---------- layers / state ---------- */

static OrigamiLayer *
layer_new (void)
{
    OrigamiLayer *l = g_new0 (OrigamiLayer, 1);
    l->vertices = g_array_new (FALSE, FALSE, sizeof (OrigamiPoint));
    return l;
}

static void
layer_free (OrigamiLayer *l)
{
    if (!l) return;
    if (l->vertices) g_array_free (l->vertices, TRUE);
    g_free (l);
}

static OrigamiLayer *
layer_copy (OrigamiLayer *src)
{
    OrigamiLayer *dst = layer_new ();
    g_array_append_vals (dst->vertices, src->vertices->data, src->vertices->len);
    dst->flipped = src->flipped;
    return dst;
}

static OrigamiState *
state_new (void)
{
    OrigamiState *s = g_new0 (OrigamiState, 1);
    s->layers = g_ptr_array_new_with_free_func ((GDestroyNotify) layer_free);
    return s;
}

static void
state_free (OrigamiState *s)
{
    if (!s) return;
    g_ptr_array_free (s->layers, TRUE);
    g_free (s);
}

static OrigamiState *
state_copy (OrigamiState *src)
{
    OrigamiState *dst = state_new ();
    for (guint i = 0; i < src->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (src->layers, i);
        g_ptr_array_add (dst->layers, layer_copy (l));
    }
    return dst;
}

static void
state_reset_to_sheet (OrigamiState *s, double w, double h)
{
    g_ptr_array_set_size (s->layers, 0);

    OrigamiLayer *l = layer_new ();
    OrigamiPoint cx = { (1000.0 - w) / 2.0, (1000.0 - h) / 2.0 };
    OrigamiPoint p;
    p.x = cx.x;         p.y = cx.y;         g_array_append_val (l->vertices, p);
    p.x = cx.x + w;     p.y = cx.y;         g_array_append_val (l->vertices, p);
    p.x = cx.x + w;     p.y = cx.y + h;     g_array_append_val (l->vertices, p);
    p.x = cx.x;         p.y = cx.y + h;     g_array_append_val (l->vertices, p);
    l->flipped = FALSE;
    g_ptr_array_add (s->layers, l);
}

/* ---------- public API ---------- */

OrigamiPaper *
origami_paper_new (double width, double height)
{
    OrigamiPaper *p = g_new0 (OrigamiPaper, 1);
    p->width   = width;
    p->height  = height;
    p->current = state_new ();
    p->history = g_queue_new ();
    state_reset_to_sheet (p->current, width, height);
    return p;
}

void
origami_paper_free (OrigamiPaper *p)
{
    if (!p) return;
    state_free (p->current);
    while (!g_queue_is_empty (p->history))
        state_free (g_queue_pop_head (p->history));
    g_queue_free (p->history);
    g_free (p);
}

void
origami_paper_reset (OrigamiPaper *p)
{
    while (!g_queue_is_empty (p->history))
        state_free (g_queue_pop_head (p->history));
    state_free (p->current);
    p->current = state_new ();
    state_reset_to_sheet (p->current, p->width, p->height);
}

gboolean
origami_paper_can_undo (OrigamiPaper *p)
{
    return !g_queue_is_empty (p->history);
}

void
origami_paper_undo (OrigamiPaper *p)
{
    if (g_queue_is_empty (p->history))
        return;
    OrigamiState *prev = g_queue_pop_tail (p->history);
    state_free (p->current);
    p->current = prev;
}

static void
push_history (OrigamiPaper *p)
{
    g_queue_push_tail (p->history, state_copy (p->current));
    while (g_queue_get_length (p->history) > MAX_HISTORY)
        state_free (g_queue_pop_head (p->history));
}

void
origami_paper_fold (OrigamiPaper *p,
                    OrigamiPoint  a,
                    OrigamiPoint  b,
                    int           fold_sign)
{
    /* Degenerate line - ignore. */
    double dx = b.x - a.x, dy = b.y - a.y;
    if (dx * dx + dy * dy < 1e-9)
        return;

    push_history (p);

    GPtrArray *kept   = g_ptr_array_new_with_free_func ((GDestroyNotify) layer_free);
    GPtrArray *folded = g_ptr_array_new_with_free_func ((GDestroyNotify) layer_free);

    int keep_sign = -fold_sign;

    for (guint i = 0; i < p->current->layers->len; i++) {
        OrigamiLayer *layer = g_ptr_array_index (p->current->layers, i);

        GArray *keep_v = origami_clip_half_plane (layer->vertices, a, b, keep_sign);
        if (keep_v->len >= 3) {
            OrigamiLayer *k = layer_new ();
            g_array_free (k->vertices, TRUE);
            k->vertices = keep_v;
            k->flipped  = layer->flipped;
            g_ptr_array_add (kept, k);
        } else {
            g_array_free (keep_v, TRUE);
        }

        GArray *fold_v = origami_clip_half_plane (layer->vertices, a, b, fold_sign);
        if (fold_v->len >= 3) {
            for (guint j = 0; j < fold_v->len; j++) {
                OrigamiPoint *pp = &g_array_index (fold_v, OrigamiPoint, j);
                *pp = reflect_across_line (*pp, a, b);
            }
            /* reflection reverses winding; restore by reversing the array */
            for (guint j = 0, k = fold_v->len - 1; j < k; j++, k--) {
                OrigamiPoint tmp = g_array_index (fold_v, OrigamiPoint, j);
                g_array_index (fold_v, OrigamiPoint, j) =
                    g_array_index (fold_v, OrigamiPoint, k);
                g_array_index (fold_v, OrigamiPoint, k) = tmp;
            }
            OrigamiLayer *f = layer_new ();
            g_array_free (f->vertices, TRUE);
            f->vertices = fold_v;
            f->flipped  = !layer->flipped;
            g_ptr_array_add (folded, f);
        } else {
            g_array_free (fold_v, TRUE);
        }
    }

    /* New stack: kept layers in original order, then the folded layers on
     * top in reverse order (the flip reverses which side faces up, so the
     * topmost old layer ends up at the bottom of the new stack of folded
     * layers). */
    GPtrArray *new_layers = g_ptr_array_new_with_free_func ((GDestroyNotify) layer_free);
    g_ptr_array_set_free_func (kept,   NULL);
    g_ptr_array_set_free_func (folded, NULL);
    for (guint i = 0; i < kept->len; i++)
        g_ptr_array_add (new_layers, g_ptr_array_index (kept, i));
    for (guint i = folded->len; i > 0; i--)
        g_ptr_array_add (new_layers, g_ptr_array_index (folded, i - 1));
    g_ptr_array_free (kept,   TRUE);
    g_ptr_array_free (folded, TRUE);

    g_ptr_array_free (p->current->layers, TRUE);
    p->current->layers = new_layers;
}
