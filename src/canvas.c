#include "canvas.h"

#include <math.h>

/* Virtual coordinate space the paper lives in.  The widget letter-boxes this
 * space to fit while preserving aspect ratio. */
#define VIRTUAL_W   1000.0
#define VIRTUAL_H   1000.0
#define PAPER_W      560.0
#define PAPER_H      560.0
#define MIN_LINE_LEN 12.0   /* require some movement before committing line */

struct _OrigamiCanvas {
    GtkDrawingArea parent_instance;

    OrigamiPaper      *paper;
    OrigamiCanvasState state;

    OrigamiPoint  p1;
    OrigamiPoint  p2;
    OrigamiPoint  hover;
    gboolean      has_hover;
};

G_DEFINE_FINAL_TYPE (OrigamiCanvas, origami_canvas, GTK_TYPE_DRAWING_AREA)

enum {
    SIGNAL_STATE_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static void
emit_state_changed (OrigamiCanvas *self)
{
    g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

/* ---------- coordinate transform ---------- */

static void
fit_transform (int w, int h, double *scale, double *tx, double *ty)
{
    double sx = (double) w / VIRTUAL_W;
    double sy = (double) h / VIRTUAL_H;
    double s  = MIN (sx, sy);
    *scale = s;
    *tx = (w - VIRTUAL_W * s) / 2.0;
    *ty = (h - VIRTUAL_H * s) / 2.0;
}

static void
widget_to_paper (OrigamiCanvas *self, double wx, double wy,
                 double *px, double *py)
{
    int w = gtk_widget_get_width  (GTK_WIDGET (self));
    int h = gtk_widget_get_height (GTK_WIDGET (self));
    double s, tx, ty;
    fit_transform (w, h, &s, &tx, &ty);
    if (s <= 0) { *px = 0; *py = 0; return; }
    *px = (wx - tx) / s;
    *py = (wy - ty) / s;
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
draw_layer (cairo_t *cr, OrigamiLayer *layer)
{
    if (layer->vertices->len < 3) return;

    trace_polygon (cr, layer->vertices);

    if (layer->flipped) {
        /* warm tan – the back of the sheet */
        cairo_set_source_rgb (cr, 0.91, 0.78, 0.55);
    } else {
        /* warm cream – the front */
        cairo_set_source_rgb (cr, 0.992, 0.969, 0.929);
    }
    cairo_fill_preserve (cr);

    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.18);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
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
        trace_polygon (cr, l->vertices);
        cairo_fill (cr);
    }
    cairo_restore (cr);
}

static void
draw_fold_line (cairo_t *cr, OrigamiPoint a, OrigamiPoint b, gboolean dashed)
{
    cairo_save (cr);
    /* GNOME accent blue */
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
                OrigamiPoint a, OrigamiPoint b, int side)
{
    if (!paper || !paper->current) return;

    cairo_save (cr);
    cairo_set_source_rgba (cr, 0.208, 0.518, 0.894, 0.28);

    for (guint i = 0; i < paper->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (paper->current->layers, i);
        GArray *clipped = origami_clip_half_plane (l->vertices, a, b, side);
        if (clipped->len >= 3) {
            trace_polygon (cr, clipped);
            cairo_fill (cr);
        }
        g_array_free (clipped, TRUE);
    }
    cairo_restore (cr);
}

/* ---------- snapshot / draw_func ---------- */

static void
draw_func (GtkDrawingArea *area, cairo_t *cr,
           int width, int height, gpointer user_data)
{
    OrigamiCanvas *self = ORIGAMI_CANVAS (area);

    double s, tx, ty;
    fit_transform (width, height, &s, &tx, &ty);

    cairo_save (cr);
    cairo_translate (cr, tx, ty);
    cairo_scale (cr, s, s);

    /* Subtle workspace tint behind the paper. */
    cairo_rectangle (cr, 0, 0, VIRTUAL_W, VIRTUAL_H);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.018);
    cairo_fill (cr);

    if (self->paper && self->paper->current) {
        draw_paper_drop_shadow (cr, self->paper);
        for (guint i = 0; i < self->paper->current->layers->len; i++) {
            OrigamiLayer *l = g_ptr_array_index (self->paper->current->layers, i);
            draw_layer (cr, l);
        }
    }

    if (self->state == ORIGAMI_CANVAS_STATE_SECOND_POINT) {
        if (self->has_hover) {
            draw_fold_line (cr, self->p1, self->hover, TRUE);
        }
        draw_point_marker (cr, self->p1);
    } else if (self->state == ORIGAMI_CANVAS_STATE_PICK_SIDE) {
        if (self->has_hover) {
            double s_hover = origami_side_of_line (self->hover,
                                                   self->p1, self->p2);
            int side = (s_hover >= 0) ? 1 : -1;
            highlight_side (cr, self->paper, self->p1, self->p2, side);
        }
        draw_fold_line (cr, self->p1, self->p2, FALSE);
        draw_point_marker (cr, self->p1);
        draw_point_marker (cr, self->p2);
    }

    cairo_restore (cr);
}

/* ---------- input ---------- */

static void
on_pressed (GtkGestureClick *gesture, int n_press,
            double x, double y, gpointer user_data)
{
    OrigamiCanvas *self = user_data;

    guint button = gtk_gesture_single_get_current_button (
        GTK_GESTURE_SINGLE (gesture));

    double px, py;
    widget_to_paper (self, x, y, &px, &py);
    OrigamiPoint p = { px, py };

    if (button == GDK_BUTTON_SECONDARY) {
        origami_canvas_cancel (self);
        return;
    }

    switch (self->state) {
    case ORIGAMI_CANVAS_STATE_FIRST_POINT:
        self->p1 = p;
        self->state = ORIGAMI_CANVAS_STATE_SECOND_POINT;
        break;

    case ORIGAMI_CANVAS_STATE_SECOND_POINT: {
        double dx = p.x - self->p1.x;
        double dy = p.y - self->p1.y;
        if (dx * dx + dy * dy < MIN_LINE_LEN * MIN_LINE_LEN)
            return;
        self->p2 = p;
        self->state = ORIGAMI_CANVAS_STATE_PICK_SIDE;
        break;
    }

    case ORIGAMI_CANVAS_STATE_PICK_SIDE: {
        double s = origami_side_of_line (p, self->p1, self->p2);
        int sign = (s >= 0) ? 1 : -1;
        origami_paper_fold (self->paper, self->p1, self->p2, sign);
        self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
        break;
    }
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
}

static void
on_motion (GtkEventControllerMotion *ctrl,
           double x, double y, gpointer user_data)
{
    OrigamiCanvas *self = user_data;
    double px, py;
    widget_to_paper (self, x, y, &px, &py);
    self->hover.x = px;
    self->hover.y = py;
    self->has_hover = TRUE;

    /* Only redraw if the hover state actually influences what is shown. */
    if (self->state == ORIGAMI_CANVAS_STATE_SECOND_POINT ||
        self->state == ORIGAMI_CANVAS_STATE_PICK_SIDE)
        gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *ctrl, gpointer user_data)
{
    OrigamiCanvas *self = user_data;
    if (!self->has_hover) return;
    self->has_hover = FALSE;
    if (self->state != ORIGAMI_CANVAS_STATE_FIRST_POINT)
        gtk_widget_queue_draw (GTK_WIDGET (self));
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
    origami_paper_reset (self->paper);
    self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
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
        origami_paper_undo (self->paper);
        gtk_widget_queue_draw (GTK_WIDGET (self));
        emit_state_changed (self);
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
    if (self->state == ORIGAMI_CANVAS_STATE_FIRST_POINT) return;
    self->state = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_state_changed (self);
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
}

static void
origami_canvas_init (OrigamiCanvas *self)
{
    self->paper     = origami_paper_new (PAPER_W, PAPER_H);
    self->state     = ORIGAMI_CANVAS_STATE_FIRST_POINT;
    self->has_hover = FALSE;

    gtk_drawing_area_set_content_width  (GTK_DRAWING_AREA (self), 600);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (self), 600);
    gtk_drawing_area_set_draw_func (
        GTK_DRAWING_AREA (self), draw_func, self, NULL);

    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
    g_signal_connect (motion, "leave",  G_CALLBACK (on_leave),  self);
    gtk_widget_add_controller (GTK_WIDGET (self), motion);

    gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "crosshair");
}
