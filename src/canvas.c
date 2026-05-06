#include "canvas.h"

#include <math.h>

/* Virtual coordinate space the paper lives in.  The widget letter-boxes this
 * space to fit while preserving aspect ratio.
 *
 * 1 virtual unit == 1 mm of paper for ruler purposes (PAPER_W is treated as
 * 210 mm-wide A4-ish paper here -- the canvas just shows ratios). */
#define VIRTUAL_W   1000.0
#define VIRTUAL_H   1000.0
#define PAPER_W      560.0
#define PAPER_H      560.0
#define MIN_LINE_LEN 12.0
#define RULER_PX      28.0   /* widget-pixel space reserved for rulers */

#define FOLD_ANIM_DURATION_US 400000   /* 0.4 s */

typedef struct {
    GArray  *vertices;        /* OrigamiPoint, post-fold position */
    gboolean original_flipped;/* the source layer's flipped state */
} AnimPoly;

typedef struct {
    OrigamiPoint a, b;        /* fold line */
    GArray      *polys;       /* AnimPoly, never NULL during anim */
    GHashTable  *new_ids;     /* set of post-fold layer ids that didn't
                                 exist pre-fold; these are hidden during
                                 the animation and the rotating polys are
                                 rendered in their place. */
    gint64       start_us;
    gint64       duration_us;
    guint        tick_id;
    gboolean     finishing;   /* set TRUE on the final draw to stop the
                                 tick callback re-entering */
} FoldAnim;

struct _OrigamiCanvas {
    GtkDrawingArea parent_instance;

    OrigamiPaper      *paper;
    OrigamiCanvasState state;
    OrigamiTool        tool;
    gboolean           show_rulers;

    /* fold-tool state */
    OrigamiPoint  p1;
    OrigamiPoint  p2;
    OrigamiPoint  hover;
    gboolean      has_hover;

    /* live modifier flags */
    gboolean      shift_held;
    gboolean      ctrl_held;

    /* pull tool */
    guint         pull_layer_id;
    gboolean      pulling;
    OrigamiPoint  pull_anchor;
    gboolean      pull_rotate; /* shift-drag rotates instead of translates */

    /* selection: which surfaces the next fold acts on.  Empty = all. */
    GHashTable   *selected_ids;   /* set of GUINT_TO_POINTER(id) */

    /* zoom + pan around the paper.  zoom = 1.0 / pan = (0,0) is fit. */
    double        zoom;
    double        pan_x, pan_y;

    /* middle-button pan-drag */
    gboolean      panning;
    double        pan_anchor_x, pan_anchor_y;

    /* fold animation */
    gboolean      animate;
    FoldAnim     *anim;
};

G_DEFINE_FINAL_TYPE (OrigamiCanvas, origami_canvas, GTK_TYPE_DRAWING_AREA)

enum {
    SIGNAL_STATE_CHANGED,
    SIGNAL_HISTORY_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static void
emit_state_changed (OrigamiCanvas *self)
{
    g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
emit_history_changed (OrigamiCanvas *self)
{
    g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
}

/* ---------- animation ---------- */

static void
anim_poly_clear (AnimPoly *ap)
{
    if (!ap) return;
    if (ap->vertices) {
        g_array_free (ap->vertices, TRUE);
        ap->vertices = NULL;
    }
}

static void
fold_anim_free (FoldAnim *anim)
{
    if (!anim) return;
    if (anim->polys) g_array_free (anim->polys, TRUE);
    if (anim->new_ids) g_hash_table_destroy (anim->new_ids);
    g_free (anim);
}

static void
anim_stop (OrigamiCanvas *self)
{
    if (!self->anim) return;
    if (self->anim->tick_id != 0) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self),
                                         self->anim->tick_id);
        self->anim->tick_id = 0;
    }
    fold_anim_free (self->anim);
    self->anim = NULL;
}

static GHashTable *
ids_set_from_state (OrigamiState *s)
{
    GHashTable *t = g_hash_table_new (g_direct_hash, g_direct_equal);
    if (!s) return t;
    for (guint i = 0; i < s->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (s->layers, i);
        g_hash_table_add (t, GUINT_TO_POINTER (l->id));
    }
    return t;
}

static gboolean
on_anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    OrigamiCanvas *self = user_data;
    if (!self->anim) return G_SOURCE_REMOVE;
    gint64 now = g_get_monotonic_time ();
    if (now - self->anim->start_us >= self->anim->duration_us) {
        self->anim->finishing = TRUE;
        gtk_widget_queue_draw (widget);
        anim_stop (self);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw (widget);
    return G_SOURCE_CONTINUE;
}

static void
kickoff_fold_animation (OrigamiCanvas *self,
                        OrigamiPoint  a,
                        OrigamiPoint  b,
                        GHashTable   *pre_fold_ids)
{
    if (!self->animate) {
        g_hash_table_destroy (pre_fold_ids);
        return;
    }

    GHashTable *post_ids = ids_set_from_state (self->paper->current);
    GHashTable *new_ids  = g_hash_table_new (g_direct_hash, g_direct_equal);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, post_ids);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        if (!g_hash_table_contains (pre_fold_ids, k))
            g_hash_table_add (new_ids, k);
    }
    g_hash_table_destroy (post_ids);
    g_hash_table_destroy (pre_fold_ids);

    if (g_hash_table_size (new_ids) == 0) {
        g_hash_table_destroy (new_ids);
        return;
    }

    GArray *polys = g_array_new (FALSE, FALSE, sizeof (AnimPoly));
    g_array_set_clear_func (polys, (GDestroyNotify) anim_poly_clear);

    /* For each new layer, capture its post-fold polygon (= where the
     * animation lands at theta=pi). We also need to know "original
     * flipped" — that's the OPPOSITE of the new layer's flipped state,
     * because the fold inverted it. */
    for (guint i = 0; i < self->paper->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (self->paper->current->layers, i);
        if (!g_hash_table_contains (new_ids, GUINT_TO_POINTER (l->id)))
            continue;
        AnimPoly ap;
        ap.vertices = origami_layer_world_polygon (l);
        ap.original_flipped = !l->flipped;
        g_array_append_val (polys, ap);
    }

    if (polys->len == 0) {
        g_array_free (polys, TRUE);
        g_hash_table_destroy (new_ids);
        return;
    }

    anim_stop (self);
    FoldAnim *anim = g_new0 (FoldAnim, 1);
    anim->a = a;
    anim->b = b;
    anim->polys = polys;
    anim->new_ids = new_ids;
    anim->start_us = g_get_monotonic_time ();
    anim->duration_us = FOLD_ANIM_DURATION_US;
    anim->tick_id = gtk_widget_add_tick_callback (
        GTK_WIDGET (self), on_anim_tick, self, NULL);
    self->anim = anim;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static double
anim_theta (FoldAnim *anim)
{
    if (!anim) return G_PI; /* finished */
    if (anim->finishing) return G_PI;
    gint64 now = g_get_monotonic_time ();
    double t = (double) (now - anim->start_us) / (double) anim->duration_us;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    /* Ease-out: feels less mechanical than linear. */
    t = 1.0 - (1.0 - t) * (1.0 - t);
    return t * G_PI;
}

/* ---------- coordinate transform ---------- */

static void
fit_transform (OrigamiCanvas *self, int w, int h,
               double *scale, double *tx, double *ty)
{
    double rw = self->show_rulers ? RULER_PX : 0;
    double avail_w = MAX (1.0, (double) w - rw);
    double avail_h = MAX (1.0, (double) h - rw);
    double sx = avail_w / VIRTUAL_W;
    double sy = avail_h / VIRTUAL_H;
    double s_fit = MIN (sx, sy);
    double s = s_fit * self->zoom;
    *scale = s;
    /* Centre the fit-sized paper, then offset by user pan. */
    *tx = rw + (avail_w - VIRTUAL_W * s) / 2.0 + self->pan_x;
    *ty = rw + (avail_h - VIRTUAL_H * s) / 2.0 + self->pan_y;
}

static void
widget_to_paper (OrigamiCanvas *self, double wx, double wy,
                 double *px, double *py)
{
    int w = gtk_widget_get_width  (GTK_WIDGET (self));
    int h = gtk_widget_get_height (GTK_WIDGET (self));
    double s, tx, ty;
    fit_transform (self, w, h, &s, &tx, &ty);
    if (s <= 0) { *px = 0; *py = 0; return; }
    *px = (wx - tx) / s;
    *py = (wy - ty) / s;
}

/* ---------- snapping helpers ---------- */

static OrigamiPoint
snap_endpoint_to_vertex (OrigamiCanvas *self, OrigamiPoint p, double tol)
{
    double best = tol * tol;
    OrigamiPoint best_p = p;
    if (!self->paper || !self->paper->current) return p;
    for (guint i = 0; i < self->paper->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (self->paper->current->layers, i);
        GArray *poly = origami_layer_world_polygon (l);
        for (guint j = 0; j < poly->len; j++) {
            OrigamiPoint v = g_array_index (poly, OrigamiPoint, j);
            double dx = v.x - p.x, dy = v.y - p.y;
            double d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; best_p = v; }

            OrigamiPoint vn = g_array_index (poly, OrigamiPoint,
                                             (j + 1) % poly->len);
            OrigamiPoint mid = { (v.x + vn.x) / 2.0, (v.y + vn.y) / 2.0 };
            dx = mid.x - p.x; dy = mid.y - p.y;
            d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; best_p = mid; }
        }
        g_array_free (poly, TRUE);
    }
    return best_p;
}

static OrigamiPoint
snap_angle (OrigamiPoint anchor, OrigamiPoint cursor)
{
    double dx = cursor.x - anchor.x;
    double dy = cursor.y - anchor.y;
    double r  = hypot (dx, dy);
    if (r < 1e-6) return cursor;
    double a = atan2 (dy, dx);
    double step = G_PI / 12.0; /* 15 degrees */
    double snapped = round (a / step) * step;
    OrigamiPoint p = {
        anchor.x + r * cos (snapped),
        anchor.y + r * sin (snapped)
    };
    return p;
}

/* Apply Shift/Ctrl modifiers to derive the effective second point. */
static OrigamiPoint
effective_second (OrigamiCanvas *self, OrigamiPoint raw)
{
    OrigamiPoint p = raw;
    if (self->ctrl_held)  p = snap_endpoint_to_vertex (self, p, 18.0);
    if (self->shift_held) p = snap_angle (self->p1, p);
    return p;
}

static OrigamiPoint
effective_first (OrigamiCanvas *self, OrigamiPoint raw)
{
    if (self->ctrl_held) return snap_endpoint_to_vertex (self, raw, 18.0);
    return raw;
}

/* ---------- drawing helpers ---------- */

static void
trace_polygon (cairo_t *cr, GArray *vs)
{
    if (vs->len < 3) return;
    OrigamiPoint *p = (OrigamiPoint *) vs->data;
    cairo_move_to (cr, p[0].x, p[0].y);
    for (guint i = 1; i < vs->len; i++)
        cairo_line_to (cr, p[i].x, p[i].y);
    cairo_close_path (cr);
}

static void
draw_layer (cairo_t *cr, OrigamiLayer *layer, gboolean is_target,
            gboolean is_hover)
{
    GArray *poly = origami_layer_world_polygon (layer);
    if (poly->len < 3) { g_array_free (poly, TRUE); return; }

    trace_polygon (cr, poly);

    if (layer->flipped) {
        cairo_set_source_rgb (cr, 0.91, 0.78, 0.55);
    } else {
        cairo_set_source_rgb (cr, 0.992, 0.969, 0.929);
    }
    cairo_fill_preserve (cr);

    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.18);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    if (is_target) {
        trace_polygon (cr, poly);
        cairo_set_source_rgba (cr, 0.208, 0.518, 0.894, 0.85);
        cairo_set_line_width (cr, 2.5);
        cairo_stroke (cr);
    } else if (is_hover) {
        trace_polygon (cr, poly);
        cairo_set_source_rgba (cr, 0.208, 0.518, 0.894, 0.45);
        cairo_set_line_width (cr, 1.5);
        cairo_stroke (cr);
    }

    g_array_free (poly, TRUE);
}

static void
draw_paper_drop_shadow (cairo_t *cr, OrigamiPaper *paper)
{
    if (!paper || !paper->current) return;
    cairo_save (cr);
    cairo_translate (cr, 4, 6);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.10);
    for (guint i = 0; i < paper->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (paper->current->layers, i);
        GArray *poly = origami_layer_world_polygon (l);
        trace_polygon (cr, poly);
        cairo_fill (cr);
        g_array_free (poly, TRUE);
    }
    cairo_restore (cr);
}

static void
draw_fold_line (cairo_t *cr, OrigamiPoint a, OrigamiPoint b, gboolean dashed)
{
    cairo_save (cr);
    cairo_set_source_rgb (cr, 0.208, 0.518, 0.894);
    cairo_set_line_width (cr, 2.0);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    if (dashed) {
        double dashes[] = { 8.0, 6.0 };
        cairo_set_dash (cr, dashes, 2, 0);
    }
    cairo_move_to (cr, a.x, a.y);
    cairo_line_to (cr, b.x, b.y);
    cairo_stroke (cr);
    cairo_restore (cr);
}

static void
draw_point_marker (cairo_t *cr, OrigamiPoint p)
{
    cairo_save (cr);
    cairo_arc (cr, p.x, p.y, 6.0, 0, 2 * G_PI);
    cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
    cairo_fill_preserve (cr);
    cairo_set_source_rgb (cr, 0.208, 0.518, 0.894);
    cairo_set_line_width (cr, 2.0);
    cairo_stroke (cr);
    cairo_restore (cr);
}

static void
highlight_side (cairo_t *cr, OrigamiPaper *paper,
                OrigamiPoint a, OrigamiPoint b, int side,
                GHashTable *only_ids)
{
    if (!paper || !paper->current) return;
    gboolean restrict_to = (only_ids != NULL
                            && g_hash_table_size (only_ids) > 0);

    cairo_save (cr);
    cairo_set_source_rgba (cr, 0.208, 0.518, 0.894, 0.28);

    for (guint i = 0; i < paper->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (paper->current->layers, i);
        if (restrict_to
            && !g_hash_table_contains (only_ids, GUINT_TO_POINTER (l->id)))
            continue;
        GArray *world = origami_layer_world_polygon (l);
        GArray *clipped = origami_clip_half_plane (world, a, b, side);
        if (clipped->len >= 3) {
            trace_polygon (cr, clipped);
            cairo_fill (cr);
        }
        g_array_free (clipped, TRUE);
        g_array_free (world,   TRUE);
    }
    cairo_restore (cr);
}

/* ---------- rulers ---------- */

static void
draw_rulers (cairo_t *cr, OrigamiCanvas *self, int width, int height)
{
    if (!self->show_rulers) return;

    double s, tx, ty;
    fit_transform (self, width, height, &s, &tx, &ty);

    cairo_save (cr);

    /* Ruler background. */
    cairo_set_source_rgba (cr, 0, 0, 0, 0.04);
    cairo_rectangle (cr, 0, 0, width, RULER_PX);
    cairo_fill (cr);
    cairo_rectangle (cr, 0, 0, RULER_PX, height);
    cairo_fill (cr);

    /* Determine paper origin in widget space. The paper's mm origin sits at
     * (1000-PAPER_W)/2 in virtual coordinates. */
    double paper_x0_mm = (VIRTUAL_W - PAPER_W) / 2.0;
    double paper_y0_mm = (VIRTUAL_H - PAPER_H) / 2.0;

    cairo_set_source_rgba (cr, 0, 0, 0, 0.55);
    cairo_set_line_width (cr, 1.0);
    cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, 9.0);

    /* Horizontal ruler: tick every 20 mm (relative to paper origin), label
     * every 100 mm. */
    int tick_step = 20;
    int label_step = 100;
    /* Range over which to draw ticks: cover the whole virtual paper area. */
    double w_paper = PAPER_W;
    double h_paper = PAPER_H;

    for (int mm = 0; mm <= (int) w_paper; mm += tick_step) {
        double vx = paper_x0_mm + mm;
        double wx = tx + vx * s;
        double tlen = (mm % label_step == 0) ? 10.0 : 5.0;
        cairo_move_to (cr, wx, RULER_PX);
        cairo_line_to (cr, wx, RULER_PX - tlen);
        cairo_stroke (cr);
        if (mm % label_step == 0) {
            char buf[32];
            g_snprintf (buf, sizeof buf, "%d", mm);
            cairo_move_to (cr, wx + 2, RULER_PX - 12);
            cairo_show_text (cr, buf);
        }
    }
    for (int mm = 0; mm <= (int) h_paper; mm += tick_step) {
        double vy = paper_y0_mm + mm;
        double wy = ty + vy * s;
        double tlen = (mm % label_step == 0) ? 10.0 : 5.0;
        cairo_move_to (cr, RULER_PX,        wy);
        cairo_line_to (cr, RULER_PX - tlen, wy);
        cairo_stroke (cr);
        if (mm % label_step == 0) {
            char buf[32];
            g_snprintf (buf, sizeof buf, "%d", mm);
            cairo_move_to (cr, 2, wy - 2);
            cairo_show_text (cr, buf);
        }
    }
    cairo_restore (cr);
}

/* ---------- overlay readout ---------- */

static void
draw_readout (cairo_t *cr, OrigamiCanvas *self, int width)
{
    if (self->state == ORIGAMI_CANVAS_STATE_FIRST_POINT)
        return;

    OrigamiPoint p2_eff = (self->state == ORIGAMI_CANVAS_STATE_SECOND_POINT
                           ? (self->has_hover
                              ? effective_second (self, self->hover)
                              : self->p1)
                           : self->p2);

    double dx = p2_eff.x - self->p1.x;
    double dy = p2_eff.y - self->p1.y;
    double len_mm = hypot (dx, dy);
    double angle_deg = atan2 (dy, dx) * 180.0 / G_PI;
    if (angle_deg < 0) angle_deg += 360.0;

    /* Distance from each fold-line endpoint to the nearest paper edge.
     * The paper rectangle in virtual coords spans [ox, ox+PAPER_W]
     * x [oy, oy+PAPER_H]. */
    double ox = (VIRTUAL_W - PAPER_W) / 2.0;
    double oy = (VIRTUAL_H - PAPER_H) / 2.0;
    double d1 = MIN (MIN (self->p1.x - ox, ox + PAPER_W - self->p1.x),
                     MIN (self->p1.y - oy, oy + PAPER_H - self->p1.y));
    double d2 = MIN (MIN (p2_eff.x - ox, ox + PAPER_W - p2_eff.x),
                     MIN (p2_eff.y - oy, oy + PAPER_H - p2_eff.y));
    double edge = MIN (d1, d2);

    char buf[200];
    guint sel = g_hash_table_size (self->selected_ids);
    if (sel > 0)
        g_snprintf (buf, sizeof buf,
                    "len %.1f mm   angle %.1f°   edge %.1f mm   "
                    "%u surface%s selected",
                    len_mm, angle_deg, edge, sel, sel == 1 ? "" : "s");
    else
        g_snprintf (buf, sizeof buf,
                    "len %.1f mm   angle %.1f°   edge %.1f mm   "
                    "all surfaces",
                    len_mm, angle_deg, edge);

    cairo_save (cr);
    cairo_set_font_size (cr, 12.0);
    cairo_text_extents_t te;
    cairo_text_extents (cr, buf, &te);
    double pad = 6.0;
    double bw = te.width + 2 * pad;
    double bh = te.height + 2 * pad;
    double bx = (width - bw) / 2.0;
    double by = 8.0;
    cairo_set_source_rgba (cr, 0, 0, 0, 0.72);
    cairo_rectangle (cr, bx, by, bw, bh);
    cairo_fill (cr);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_move_to (cr, bx + pad - te.x_bearing, by + pad - te.y_bearing);
    cairo_show_text (cr, buf);
    cairo_restore (cr);
}

/* ---------- snapshot / draw_func ---------- */

static void
draw_func (GtkDrawingArea *area, cairo_t *cr,
           int width, int height, gpointer user_data)
{
    OrigamiCanvas *self = ORIGAMI_CANVAS (area);

    double s, tx, ty;
    fit_transform (self, width, height, &s, &tx, &ty);

    cairo_save (cr);
    cairo_translate (cr, tx, ty);
    cairo_scale (cr, s, s);

    /* Subtle workspace tint behind the paper. */
    cairo_rectangle (cr, 0, 0, VIRTUAL_W, VIRTUAL_H);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.018);
    cairo_fill (cr);

    /* Hover-detect the topmost layer under the cursor for hint
     * highlight in Select mode (and so the user knows what they'll
     * select). */
    OrigamiLayer *hover_layer = NULL;
    if (self->tool == ORIGAMI_TOOL_SELECT
        && self->has_hover && self->paper) {
        hover_layer = origami_paper_layer_at (self->paper, self->hover);
    }

    if (self->paper && self->paper->current) {
        draw_paper_drop_shadow (cr, self->paper);
        for (guint i = 0; i < self->paper->current->layers->len; i++) {
            OrigamiLayer *l = g_ptr_array_index (self->paper->current->layers, i);
            /* During animation, hide the post-fold layers that didn't
             * exist before the fold; we'll draw their rotating
             * counterparts after the fixed layers. */
            if (self->anim
                && g_hash_table_contains (self->anim->new_ids,
                                          GUINT_TO_POINTER (l->id)))
                continue;
            gboolean is_target = g_hash_table_contains (
                self->selected_ids, GUINT_TO_POINTER (l->id));
            gboolean is_hover  = (hover_layer && l == hover_layer);
            draw_layer (cr, l, is_target, is_hover);
        }
    }

    if (self->anim) {
        double theta = anim_theta (self->anim);
        gboolean past_half = theta > G_PI / 2.0;
        for (guint i = 0; i < self->anim->polys->len; i++) {
            AnimPoly *ap = &g_array_index (self->anim->polys, AnimPoly, i);
            GArray *frame = g_array_new (FALSE, FALSE, sizeof (OrigamiPoint));
            for (guint j = 0; j < ap->vertices->len; j++) {
                OrigamiPoint v_post = g_array_index (ap->vertices,
                                                     OrigamiPoint, j);
                OrigamiPoint v_now = origami_animate_vertex (
                    v_post, self->anim->a, self->anim->b, theta);
                g_array_append_val (frame, v_now);
            }
            cairo_save (cr);
            trace_polygon (cr, frame);
            gboolean show_back = past_half
                ? (!ap->original_flipped)
                : ap->original_flipped;
            if (show_back) cairo_set_source_rgb (cr, 0.91, 0.78, 0.55);
            else           cairo_set_source_rgb (cr, 0.992, 0.969, 0.929);
            cairo_fill_preserve (cr);
            cairo_set_source_rgba (cr, 0, 0, 0, 0.30);
            cairo_set_line_width (cr, 1.0);
            cairo_stroke (cr);
            /* A subtle highlight along the crease while it's near edge-on
             * sells the rotation. */
            if (theta > 0 && theta < G_PI) {
                double k = sin (theta);
                cairo_set_source_rgba (cr, 0, 0, 0, 0.10 * k);
                cairo_move_to (cr, self->anim->a.x, self->anim->a.y);
                cairo_line_to (cr, self->anim->b.x, self->anim->b.y);
                cairo_set_line_width (cr, 1.4);
                cairo_stroke (cr);
            }
            cairo_restore (cr);
            g_array_free (frame, TRUE);
        }
    }

    if (self->state == ORIGAMI_CANVAS_STATE_SECOND_POINT) {
        if (self->has_hover) {
            OrigamiPoint eff = effective_second (self, self->hover);
            draw_fold_line (cr, self->p1, eff, TRUE);
        }
        draw_point_marker (cr, self->p1);
    } else if (self->state == ORIGAMI_CANVAS_STATE_PICK_SIDE) {
        if (self->has_hover) {
            double s_hover = origami_side_of_line (self->hover,
                                                   self->p1, self->p2);
            int side = (s_hover >= 0) ? 1 : -1;
            highlight_side (cr, self->paper, self->p1, self->p2, side,
                            self->selected_ids);
        }
        draw_fold_line (cr, self->p1, self->p2, FALSE);
        draw_point_marker (cr, self->p1);
        draw_point_marker (cr, self->p2);
    }

    cairo_restore (cr);

    draw_rulers  (cr, self, width, height);
    draw_readout (cr, self, width);
}

/* ---------- input ---------- */

static void
update_modifier_flags (OrigamiCanvas *self, GdkModifierType state)
{
    self->shift_held = (state & GDK_SHIFT_MASK) != 0;
    self->ctrl_held  = (state & GDK_CONTROL_MASK) != 0;
}

static void
on_pressed (GtkGestureClick *gesture, int n_press,
            double x, double y, gpointer user_data)
{
    OrigamiCanvas *self = user_data;

    GdkEvent *ev = gtk_event_controller_get_current_event (
        GTK_EVENT_CONTROLLER (gesture));
    if (ev) update_modifier_flags (self, gdk_event_get_modifier_state (ev));

    guint button = gtk_gesture_single_get_current_button (
        GTK_GESTURE_SINGLE (gesture));

    double px, py;
    widget_to_paper (self, x, y, &px, &py);
    OrigamiPoint p = { px, py };

    if (button == GDK_BUTTON_SECONDARY) {
        origami_canvas_cancel (self);
        return;
    }

    /* Middle button pans regardless of the active tool. */
    if (button == GDK_BUTTON_MIDDLE) {
        self->panning = TRUE;
        self->pan_anchor_x = x;
        self->pan_anchor_y = y;
        return;
    }

    if (self->tool == ORIGAMI_TOOL_PULL) {
        OrigamiLayer *l = self->paper
            ? origami_paper_layer_at (self->paper, p) : NULL;
        if (l) {
            self->pull_layer_id = l->id;
            self->pulling = TRUE;
            self->pull_anchor = p;
            self->pull_rotate = self->shift_held;
        }
        return;
    }

    if (self->tool == ORIGAMI_TOOL_SELECT) {
        OrigamiLayer *l = self->paper
            ? origami_paper_layer_at (self->paper, p) : NULL;
        if (!l) {
            /* Click on empty space clears the selection (unless the user
             * is extending). */
            if (!self->shift_held && !self->ctrl_held)
                origami_canvas_clear_selection (self);
            gtk_widget_queue_draw (GTK_WIDGET (self));
            emit_state_changed (self);
            return;
        }
        if (self->shift_held || self->ctrl_held)
            origami_canvas_toggle_selection (self, l->id);
        else
            origami_canvas_select_only (self, l->id);
        return;
    }

    /* FOLD tool below. */
    switch (self->state) {
    case ORIGAMI_CANVAS_STATE_FIRST_POINT: {
        OrigamiPoint eff = effective_first (self, p);
        self->p1 = eff;
        self->state = ORIGAMI_CANVAS_STATE_SECOND_POINT;
        break;
    }

    case ORIGAMI_CANVAS_STATE_SECOND_POINT: {
        OrigamiPoint eff = effective_second (self, p);
        double dx = eff.x - self->p1.x;
        double dy = eff.y - self->p1.y;
        if (dx * dx + dy * dy < MIN_LINE_LEN * MIN_LINE_LEN)
            return;
        self->p2 = eff;
        self->state = ORIGAMI_CANVAS_STATE_PICK_SIDE;
        break;
    }

    case ORIGAMI_CANVAS_STATE_PICK_SIDE: {
        double s = origami_side_of_line (p, self->p1, self->p2);
        int sign = (s >= 0) ? 1 : -1;
        anim_stop (self);
        GHashTable *pre_ids = ids_set_from_state (self->paper->current);
        GArray *new_ids = g_array_new (FALSE, FALSE, sizeof (guint));
        GHashTable *targets =
            (g_hash_table_size (self->selected_ids) > 0)
            ? self->selected_ids : NULL;
        origami_paper_fold (self->paper, self->p1, self->p2, sign,
                            targets, new_ids);
        /* Migrate selection: every freshly-created folded surface is
         * added so the user can keep folding the surface they just
         * made.  Kept slices retain their original id, so prior
         * selection survives. */
        for (guint i = 0; i < new_ids->len; i++) {
            guint id = g_array_index (new_ids, guint, i);
            origami_canvas_add_to_selection (self, id);
        }
        g_array_free (new_ids, TRUE);
        kickoff_fold_animation (self, self->p1, self->p2, pre_ids);
        self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
        emit_history_changed (self);
        break;
    }
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

static void
on_released (GtkGestureClick *gesture, int n_press,
             double x, double y, gpointer user_data)
{
    OrigamiCanvas *self = user_data;
    if (self->pulling) {
        self->pulling = FALSE;
        emit_history_changed (self);
    }
    if (self->panning) {
        self->panning = FALSE;
    }
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl,
           double dx, double dy, gpointer user_data)
{
    OrigamiCanvas *self = user_data;

    /* Zoom anchored on the cursor: keep the paper-space point under the
     * cursor invariant across the scale change. */
    int w = gtk_widget_get_width  (GTK_WIDGET (self));
    int h = gtk_widget_get_height (GTK_WIDGET (self));
    double cx = self->has_hover ? self->hover.x : VIRTUAL_W / 2.0;
    double cy = self->has_hover ? self->hover.y : VIRTUAL_H / 2.0;

    double s_old, tx_old, ty_old;
    fit_transform (self, w, h, &s_old, &tx_old, &ty_old);
    double widget_x = tx_old + cx * s_old;
    double widget_y = ty_old + cy * s_old;

    double factor = (dy > 0) ? (1.0 / 1.15) : 1.15;
    self->zoom *= factor;
    if (self->zoom < 0.25) self->zoom = 0.25;
    if (self->zoom > 12.0) self->zoom = 12.0;

    /* Recompute and shift pan so the cursor's paper point stays put. */
    double s_new, tx_new, ty_new;
    fit_transform (self, w, h, &s_new, &tx_new, &ty_new);
    double widget_x_new = tx_new + cx * s_new;
    double widget_y_new = ty_new + cy * s_new;
    self->pan_x += widget_x - widget_x_new;
    self->pan_y += widget_y - widget_y_new;

    gtk_widget_queue_draw (GTK_WIDGET (self));
    return TRUE;
}

static void
on_motion (GtkEventControllerMotion *ctrl,
           double x, double y, gpointer user_data)
{
    OrigamiCanvas *self = user_data;

    GdkEvent *ev = gtk_event_controller_get_current_event (
        GTK_EVENT_CONTROLLER (ctrl));
    if (ev) update_modifier_flags (self, gdk_event_get_modifier_state (ev));

    /* Mid-button pan: translate by widget-pixel delta. */
    if (self->panning) {
        self->pan_x += (x - self->pan_anchor_x);
        self->pan_y += (y - self->pan_anchor_y);
        self->pan_anchor_x = x;
        self->pan_anchor_y = y;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
    }

    double px, py;
    widget_to_paper (self, x, y, &px, &py);
    OrigamiPoint last = self->hover;
    self->hover.x = px;
    self->hover.y = py;
    self->has_hover = TRUE;

    if (self->pulling && self->paper) {
        double dx = px - self->pull_anchor.x;
        double dy = py - self->pull_anchor.y;
        if (self->pull_rotate) {
            /* Rotate around layer centroid based on cursor angle delta. */
            OrigamiLayer *l = NULL;
            for (guint i = 0; i < self->paper->current->layers->len; i++) {
                OrigamiLayer *cand = g_ptr_array_index (
                    self->paper->current->layers, i);
                if (cand->id == self->pull_layer_id) { l = cand; break; }
            }
            if (l) {
                double cx = 0, cy = 0;
                for (guint i = 0; i < l->vertices->len; i++) {
                    OrigamiPoint v = g_array_index (l->vertices,
                                                    OrigamiPoint, i);
                    cx += v.x; cy += v.y;
                }
                cx /= l->vertices->len; cy /= l->vertices->len;
                double a0 = atan2 (last.y - cy, last.x - cx);
                double a1 = atan2 (py    - cy, px    - cx);
                l->rot += (a1 - a0);
            }
        } else {
            for (guint i = 0; i < self->paper->current->layers->len; i++) {
                OrigamiLayer *l = g_ptr_array_index (
                    self->paper->current->layers, i);
                if (l->id == self->pull_layer_id) {
                    l->tx += dx;
                    l->ty += dy;
                    break;
                }
            }
        }
        self->pull_anchor.x = px;
        self->pull_anchor.y = py;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *ctrl, gpointer user_data)
{
    OrigamiCanvas *self = user_data;
    if (!self->has_hover) return;
    self->has_hover = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_key_pressed (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer user_data)
{
    OrigamiCanvas *self = user_data;
    update_modifier_flags (self, state);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return FALSE;
}

static gboolean
on_key_released (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                 GdkModifierType state, gpointer user_data)
{
    OrigamiCanvas *self = user_data;
    update_modifier_flags (self, state);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return FALSE;
}

/* ---------- public API ---------- */

OrigamiCanvasState
origami_canvas_get_state (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self),
                          ORIGAMI_CANVAS_STATE_FIRST_POINT);
    return self->state;
}

void
origami_canvas_reset (OrigamiCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    anim_stop (self);
    origami_paper_reset (self->paper);
    self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    g_hash_table_remove_all (self->selected_ids);
    origami_canvas_zoom_reset (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
    emit_history_changed (self);
}

void
origami_canvas_undo (OrigamiCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    if (self->state != ORIGAMI_CANVAS_STATE_FIRST_POINT) {
        self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        emit_state_changed (self);
        return;
    }
    if (origami_paper_can_undo (self->paper)) {
        anim_stop (self);
        origami_paper_undo (self->paper);
        gtk_widget_queue_draw (GTK_WIDGET (self));
        emit_state_changed (self);
        emit_history_changed (self);
    }
}

gboolean
origami_canvas_can_undo (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), FALSE);
    return self->state != ORIGAMI_CANVAS_STATE_FIRST_POINT
        || origami_paper_can_undo (self->paper);
}

void
origami_canvas_cancel (OrigamiCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    if (self->state == ORIGAMI_CANVAS_STATE_FIRST_POINT
        && !self->pulling) return;
    self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    self->pulling = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

OrigamiTool
origami_canvas_get_tool (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), ORIGAMI_TOOL_FOLD);
    return self->tool;
}

void
origami_canvas_set_tool (OrigamiCanvas *self, OrigamiTool tool)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    self->tool = tool;
    self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    self->pulling = FALSE;
    switch (tool) {
    case ORIGAMI_TOOL_SELECT:
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "default");
        break;
    case ORIGAMI_TOOL_PULL:
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "grab");
        break;
    case ORIGAMI_TOOL_FOLD:
    default:
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "crosshair");
        break;
    }
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

void
origami_canvas_set_show_rulers (OrigamiCanvas *self, gboolean enabled)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    self->show_rulers = enabled;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
origami_canvas_get_show_rulers (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), FALSE);
    return self->show_rulers;
}

OrigamiPaper *
origami_canvas_get_paper (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), NULL);
    return self->paper;
}

void
origami_canvas_redraw (OrigamiCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
origami_canvas_replay_to (OrigamiCanvas *self, guint n)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    OrigamiPaper *p = self->paper;
    if (!p) return;
    anim_stop (self);
    while (p->records->len > n && origami_paper_can_undo (p))
        origami_paper_undo (p);
    self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
    emit_history_changed (self);
}

/* ---------- selection ---------- */

gboolean
origami_canvas_is_selected (OrigamiCanvas *self, guint id)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), FALSE);
    return g_hash_table_contains (self->selected_ids, GUINT_TO_POINTER (id));
}

gboolean
origami_canvas_has_selection (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), FALSE);
    return g_hash_table_size (self->selected_ids) > 0;
}

guint
origami_canvas_selection_size (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), 0);
    return g_hash_table_size (self->selected_ids);
}

GArray *
origami_canvas_get_selection (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), NULL);
    GArray *out = g_array_new (FALSE, FALSE, sizeof (guint));
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, self->selected_ids);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        guint id = GPOINTER_TO_UINT (k);
        g_array_append_val (out, id);
    }
    return out;
}

void
origami_canvas_clear_selection (OrigamiCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    if (g_hash_table_size (self->selected_ids) == 0) return;
    g_hash_table_remove_all (self->selected_ids);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

void
origami_canvas_select_only (OrigamiCanvas *self, guint id)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    g_hash_table_remove_all (self->selected_ids);
    if (id != 0)
        g_hash_table_add (self->selected_ids, GUINT_TO_POINTER (id));
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

void
origami_canvas_toggle_selection (OrigamiCanvas *self, guint id)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    if (id == 0) return;
    gpointer key = GUINT_TO_POINTER (id);
    if (g_hash_table_contains (self->selected_ids, key))
        g_hash_table_remove (self->selected_ids, key);
    else
        g_hash_table_add (self->selected_ids, key);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

void
origami_canvas_add_to_selection (OrigamiCanvas *self, guint id)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    if (id == 0) return;
    g_hash_table_add (self->selected_ids, GUINT_TO_POINTER (id));
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

/* ---------- zoom ---------- */

void
origami_canvas_zoom_by (OrigamiCanvas *self, double factor)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    if (factor <= 0) {
        origami_canvas_zoom_reset (self);
        return;
    }
    self->zoom *= factor;
    if (self->zoom < 0.25) self->zoom = 0.25;
    if (self->zoom > 12.0) self->zoom = 12.0;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
origami_canvas_zoom_reset (OrigamiCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    self->zoom  = 1.0;
    self->pan_x = 0.0;
    self->pan_y = 0.0;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
origami_canvas_mm_to_virtual (OrigamiCanvas *self,
                              double mm_x, double mm_y,
                              double *vx, double *vy)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    double ox = (VIRTUAL_W - PAPER_W) / 2.0;
    double oy = (VIRTUAL_H - PAPER_H) / 2.0;
    if (vx) *vx = ox + mm_x;
    if (vy) *vy = oy + mm_y;
}

void
origami_canvas_fold_mm (OrigamiCanvas *self,
                        double x1_mm, double y1_mm,
                        double x2_mm, double y2_mm,
                        int sign)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    if (!self->paper) return;
    OrigamiPoint a, b;
    origami_canvas_mm_to_virtual (self, x1_mm, y1_mm, &a.x, &a.y);
    origami_canvas_mm_to_virtual (self, x2_mm, y2_mm, &b.x, &b.y);
    double dx = b.x - a.x, dy = b.y - a.y;
    if (dx * dx + dy * dy < MIN_LINE_LEN * MIN_LINE_LEN)
        return;

    anim_stop (self);
    GHashTable *pre_ids = ids_set_from_state (self->paper->current);
    GArray *new_ids = g_array_new (FALSE, FALSE, sizeof (guint));
    GHashTable *targets = (g_hash_table_size (self->selected_ids) > 0)
                          ? self->selected_ids : NULL;
    origami_paper_fold (self->paper, a, b, sign, targets, new_ids);
    for (guint i = 0; i < new_ids->len; i++) {
        guint id = g_array_index (new_ids, guint, i);
        g_hash_table_add (self->selected_ids, GUINT_TO_POINTER (id));
    }
    g_array_free (new_ids, TRUE);
    kickoff_fold_animation (self, a, b, pre_ids);

    self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
    emit_history_changed (self);
}

void
origami_canvas_set_animate (OrigamiCanvas *self, gboolean enabled)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS (self));
    self->animate = enabled;
    if (!enabled) anim_stop (self);
}

gboolean
origami_canvas_get_animate (OrigamiCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CANVAS (self), FALSE);
    return self->animate;
}

GtkWidget *
origami_canvas_new (void)
{
    return g_object_new (ORIGAMI_TYPE_CANVAS, NULL);
}

/* ---------- GObject lifecycle ---------- */

static void
origami_canvas_dispose (GObject *object)
{
    OrigamiCanvas *self = ORIGAMI_CANVAS (object);
    anim_stop (self);
    g_clear_pointer (&self->selected_ids, g_hash_table_destroy);
    g_clear_pointer (&self->paper, origami_paper_free);
    G_OBJECT_CLASS (origami_canvas_parent_class)->dispose (object);
}

static void
origami_canvas_class_init (OrigamiCanvasClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = origami_canvas_dispose;

    signals[SIGNAL_STATE_CHANGED] = g_signal_new (
        "state-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_HISTORY_CHANGED] = g_signal_new (
        "history-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void
origami_canvas_init (OrigamiCanvas *self)
{
    self->paper        = origami_paper_new (PAPER_W, PAPER_H);
    self->state        = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    self->tool         = ORIGAMI_TOOL_SELECT;
    self->show_rulers  = TRUE;
    self->has_hover    = FALSE;
    self->animate      = TRUE;
    self->zoom         = 1.0;
    self->pan_x        = 0.0;
    self->pan_y        = 0.0;
    self->selected_ids = g_hash_table_new (g_direct_hash, g_direct_equal);

    gtk_drawing_area_set_content_width  (GTK_DRAWING_AREA (self), 600);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (self), 600);
    gtk_drawing_area_set_draw_func (
        GTK_DRAWING_AREA (self), draw_func, self, NULL);

    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    g_signal_connect (click, "pressed",  G_CALLBACK (on_pressed),  self);
    g_signal_connect (click, "released", G_CALLBACK (on_released), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
    g_signal_connect (motion, "leave",  G_CALLBACK (on_leave),  self);
    gtk_widget_add_controller (GTK_WIDGET (self), motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new (
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    gtk_widget_add_controller (GTK_WIDGET (self), scroll);

    GtkEventController *keys = gtk_event_controller_key_new ();
    g_signal_connect (keys, "key-pressed",  G_CALLBACK (on_key_pressed),  self);
    g_signal_connect (keys, "key-released", G_CALLBACK (on_key_released), self);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);
    gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);

    gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "default");
}
