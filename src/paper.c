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

OrigamiPoint
origami_reflect_across_line (OrigamiPoint p, OrigamiPoint a, OrigamiPoint b)
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

OrigamiPoint
origami_animate_vertex (OrigamiPoint v_post,
                        OrigamiPoint a, OrigamiPoint b,
                        double theta)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) return v_post;
    double t = ((v_post.x - a.x) * dx + (v_post.y - a.y) * dy) / len2;
    double fx = a.x + t * dx;
    double fy = a.y + t * dy;
    /* Perpendicular vector from foot to v_post (= the "post" side). */
    double px = v_post.x - fx;
    double py = v_post.y - fy;
    /* -cos(theta) goes -1 -> 0 -> +1 across [0, pi/2, pi].
     * The 2D projection of the rotating point follows that scaling of
     * the perpendicular offset. */
    double k = -cos (theta);
    OrigamiPoint r = { fx + k * px, fy + k * py };
    return r;
}

/* Backwards-compatible internal alias. */
static inline OrigamiPoint
reflect_across_line (OrigamiPoint p, OrigamiPoint a, OrigamiPoint b)
{
    return origami_reflect_across_line (p, a, b);
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
layer_new (guint id)
{
    OrigamiLayer *l = g_new0 (OrigamiLayer, 1);
    l->id       = id;
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
    OrigamiLayer *dst = layer_new (src->id);
    g_array_append_vals (dst->vertices, src->vertices->data, src->vertices->len);
    dst->flipped = src->flipped;
    dst->tx      = src->tx;
    dst->ty      = src->ty;
    dst->rot     = src->rot;
    return dst;
}

static void
layer_centroid (OrigamiLayer *l, double *cx, double *cy)
{
    double sx = 0, sy = 0;
    if (l->vertices->len == 0) { *cx = 0; *cy = 0; return; }
    for (guint i = 0; i < l->vertices->len; i++) {
        OrigamiPoint p = g_array_index (l->vertices, OrigamiPoint, i);
        sx += p.x;
        sy += p.y;
    }
    *cx = sx / l->vertices->len;
    *cy = sy / l->vertices->len;
}

GArray *
origami_layer_world_polygon (OrigamiLayer *l)
{
    GArray *out = g_array_new (FALSE, FALSE, sizeof (OrigamiPoint));
    if (!l || l->vertices->len == 0) return out;

    double cx, cy;
    layer_centroid (l, &cx, &cy);
    double c = cos (l->rot), s = sin (l->rot);

    for (guint i = 0; i < l->vertices->len; i++) {
        OrigamiPoint p = g_array_index (l->vertices, OrigamiPoint, i);
        double x = p.x - cx;
        double y = p.y - cy;
        OrigamiPoint q = {
            cx + c * x - s * y + l->tx,
            cy + s * x + c * y + l->ty
        };
        g_array_append_val (out, q);
    }
    return out;
}

static gboolean
point_in_polygon (GArray *poly, OrigamiPoint p)
{
    gboolean inside = FALSE;
    guint n = poly->len;
    if (n < 3) return FALSE;
    for (guint i = 0, j = n - 1; i < n; j = i++) {
        OrigamiPoint pi = g_array_index (poly, OrigamiPoint, i);
        OrigamiPoint pj = g_array_index (poly, OrigamiPoint, j);
        if (((pi.y > p.y) != (pj.y > p.y)) &&
            (p.x < (pj.x - pi.x) * (p.y - pi.y) / (pj.y - pi.y + 1e-30) + pi.x))
            inside = !inside;
    }
    return inside;
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
state_reset_to_sheet (OrigamiState *s, double w, double h, guint id)
{
    g_ptr_array_set_size (s->layers, 0);

    OrigamiLayer *l = layer_new (id);
    OrigamiPoint cx = { (1000.0 - w) / 2.0, (1000.0 - h) / 2.0 };
    OrigamiPoint p;
    p.x = cx.x;         p.y = cx.y;         g_array_append_val (l->vertices, p);
    p.x = cx.x + w;     p.y = cx.y;         g_array_append_val (l->vertices, p);
    p.x = cx.x + w;     p.y = cx.y + h;     g_array_append_val (l->vertices, p);
    p.x = cx.x;         p.y = cx.y + h;     g_array_append_val (l->vertices, p);
    l->flipped = FALSE;
    g_ptr_array_add (s->layers, l);
}

/* ---------- fold records ---------- */

static void
records_clear (GArray *records)
{
    for (guint i = 0; i < records->len; i++) {
        OrigamiFoldRecord *r = &g_array_index (records, OrigamiFoldRecord, i);
        g_clear_pointer (&r->summary, g_free);
    }
    g_array_set_size (records, 0);
}

static void
record_clear_func (OrigamiFoldRecord *r)
{
    g_clear_pointer (&r->summary, g_free);
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
    p->records = g_array_new (FALSE, FALSE, sizeof (OrigamiFoldRecord));
    g_array_set_clear_func (p->records,
                            (GDestroyNotify) record_clear_func);
    p->next_layer_id = 1;
    state_reset_to_sheet (p->current, width, height, p->next_layer_id++);
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
    records_clear (p->records);
    g_array_free (p->records, TRUE);
    g_free (p);
}

void
origami_paper_reset (OrigamiPaper *p)
{
    while (!g_queue_is_empty (p->history))
        state_free (g_queue_pop_head (p->history));
    state_free (p->current);
    p->current = state_new ();
    records_clear (p->records);
    p->next_layer_id = 1;
    state_reset_to_sheet (p->current, p->width, p->height, p->next_layer_id++);
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
    if (p->records->len > 0) {
        OrigamiFoldRecord *r = &g_array_index (p->records,
                                               OrigamiFoldRecord,
                                               p->records->len - 1);
        record_clear_func (r);
        g_array_set_size (p->records, p->records->len - 1);
    }
}

static void
push_history (OrigamiPaper *p)
{
    g_queue_push_tail (p->history, state_copy (p->current));
    while (g_queue_get_length (p->history) > MAX_HISTORY) {
        state_free (g_queue_pop_head (p->history));
        if (p->records->len > 0) {
            record_clear_func (&g_array_index (p->records,
                                               OrigamiFoldRecord, 0));
            g_array_remove_index (p->records, 0);
        }
    }
}

OrigamiLayer *
origami_paper_layer_at (OrigamiPaper *paper, OrigamiPoint p)
{
    if (!paper || !paper->current) return NULL;
    for (guint i = paper->current->layers->len; i > 0; i--) {
        OrigamiLayer *l = g_ptr_array_index (paper->current->layers, i - 1);
        GArray *world = origami_layer_world_polygon (l);
        gboolean hit = point_in_polygon (world, p);
        g_array_free (world, TRUE);
        if (hit) return l;
    }
    return NULL;
}

void
origami_paper_translate_layer (OrigamiPaper *p, guint id, double dx, double dy)
{
    if (!p) return;
    for (guint i = 0; i < p->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (p->current->layers, i);
        if (l->id == id) {
            push_history (p);
            l->tx += dx;
            l->ty += dy;
            return;
        }
    }
}

void
origami_paper_rotate_layer (OrigamiPaper *p, guint id, double d_rot)
{
    if (!p) return;
    for (guint i = 0; i < p->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (p->current->layers, i);
        if (l->id == id) {
            push_history (p);
            l->rot += d_rot;
            return;
        }
    }
}

static char *
build_summary (OrigamiPoint a, OrigamiPoint b, int fold_sign,
               guint target, double paper_w, double paper_h)
{
    /* Convert virtual coords to paper-local mm: paper is centered in a
     * 1000x1000 virtual field, so the offset per axis is (1000-w)/2. */
    double ox = (1000.0 - paper_w) / 2.0;
    double oy = (1000.0 - paper_h) / 2.0;
    double ax = a.x - ox, ay = a.y - oy;
    double bx = b.x - ox, by = b.y - oy;
    double angle_deg = atan2 (b.y - a.y, b.x - a.x) * 180.0 / G_PI;
    if (angle_deg < 0) angle_deg += 180.0;
    if (angle_deg >= 180.0) angle_deg -= 180.0;
    const char *who = (target == 0) ? "all layers" : "selected layer";
    const char *side = (fold_sign > 0) ? "right/below" : "left/above";
    return g_strdup_printf (
        "Fold %s along (%.0f,%.0f)→(%.0f,%.0f) mm "
        "at %.1f°; bring the %s side over.",
        who, ax, ay, bx, by, angle_deg, side);
}

void
origami_paper_fold (OrigamiPaper *p,
                    OrigamiPoint  a,
                    OrigamiPoint  b,
                    int           fold_sign,
                    GHashTable   *target_ids,
                    GArray       *out_new_ids)
{
    /* Degenerate line - ignore. */
    double dx = b.x - a.x, dy = b.y - a.y;
    if (dx * dx + dy * dy < 1e-9)
        return;

    gboolean target_all = (target_ids == NULL
                           || g_hash_table_size (target_ids) == 0);

    push_history (p);

    int keep_sign = -fold_sign;

    /* Walk the current stack and produce a new stack in place.  For
     * each targeted layer that the line crosses we emit a kept slice
     * (keeping the original id) and a folded slice (with a fresh id);
     * non-targeted layers pass through unchanged.  Stack order is
     * preserved so visual layering doesn't jump around. */
    GPtrArray *new_layers = g_ptr_array_new_with_free_func (
        (GDestroyNotify) layer_free);

    for (guint i = 0; i < p->current->layers->len; i++) {
        OrigamiLayer *layer = g_ptr_array_index (p->current->layers, i);

        gboolean is_target = target_all
            || g_hash_table_contains (target_ids,
                                      GUINT_TO_POINTER (layer->id));
        if (!is_target) {
            g_ptr_array_add (new_layers, layer_copy (layer));
            continue;
        }

        /* Bake any free transform into world-space geometry before
         * clipping; the resulting layer no longer carries a transform. */
        GArray *src = (layer->tx != 0 || layer->ty != 0 || layer->rot != 0)
            ? origami_layer_world_polygon (layer)
            : layer->vertices;

        GArray *keep_v = origami_clip_half_plane (src, a, b, keep_sign);
        GArray *fold_v = origami_clip_half_plane (src, a, b, fold_sign);

        gboolean produced_anything = FALSE;

        if (keep_v->len >= 3) {
            OrigamiLayer *k = layer_new (layer->id);
            g_array_free (k->vertices, TRUE);
            k->vertices = keep_v;
            k->flipped  = layer->flipped;
            g_ptr_array_add (new_layers, k);
            produced_anything = TRUE;
        } else {
            g_array_free (keep_v, TRUE);
        }

        if (fold_v->len >= 3) {
            for (guint j = 0; j < fold_v->len; j++) {
                OrigamiPoint *pp = &g_array_index (fold_v, OrigamiPoint, j);
                *pp = reflect_across_line (*pp, a, b);
            }
            for (guint j = 0, k = fold_v->len - 1; j < k; j++, k--) {
                OrigamiPoint tmp = g_array_index (fold_v, OrigamiPoint, j);
                g_array_index (fold_v, OrigamiPoint, j) =
                    g_array_index (fold_v, OrigamiPoint, k);
                g_array_index (fold_v, OrigamiPoint, k) = tmp;
            }
            guint new_id = p->next_layer_id++;
            OrigamiLayer *f = layer_new (new_id);
            g_array_free (f->vertices, TRUE);
            f->vertices = fold_v;
            f->flipped  = !layer->flipped;
            /* Insert the folded slice immediately after the kept slice
             * so multi-fold ordering matches the visual: the new
             * surface stacks on top of its source. */
            g_ptr_array_add (new_layers, f);
            if (out_new_ids)
                g_array_append_val (out_new_ids, new_id);
            produced_anything = TRUE;
        } else {
            g_array_free (fold_v, TRUE);
        }

        if (!produced_anything) {
            /* Fold line missed this layer entirely — keep it as-is so
             * the user doesn't lose a surface to a misclick. */
            g_ptr_array_add (new_layers, layer_copy (layer));
        }

        if (src != layer->vertices) g_array_free (src, TRUE);
    }

    g_ptr_array_free (p->current->layers, TRUE);
    p->current->layers = new_layers;

    OrigamiFoldRecord rec = {
        .a = a, .b = b,
        .fold_sign = fold_sign,
        .target = target_all ? 0 : 1,   /* legacy: 0 = all, 1 = subset */
        .summary = build_summary (a, b, fold_sign,
                                  target_all ? 0 : 1,
                                  p->width, p->height),
    };
    g_array_append_val (p->records, rec);
}
