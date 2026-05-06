#include "crease_canvas.h"

#include <math.h>

/* The crease pattern lives on a 100x100 flat-paper square. We display
 * it inside a 1000x1000 virtual workspace centered horizontally and
 * vertically. */
#define PAPER_W   100.0
#define PAPER_H   100.0
#define VIRTUAL_W 140.0
#define VIRTUAL_H 140.0

#define MIN_LINE  3.0  /* paper-mm; below this the click is ignored. */

typedef enum {
    STATE_FIRST_POINT  = 0,
    STATE_SECOND_POINT = 1,
} CanvasState;

struct _OrigamiCreaseCanvas {
    GtkDrawingArea parent_instance;

    CreasePattern   *cp;
    GQueue          *history;       /* CreasePattern* (deep copies, oldest at head) */

    CanvasState      state;
    CreaseAssignment next_assign;   /* M / V for the next placed crease */

    double  p1x, p1y;
    double  hover_x, hover_y;
    gboolean has_hover;
};

G_DEFINE_FINAL_TYPE (OrigamiCreaseCanvas, origami_crease_canvas,
                     GTK_TYPE_DRAWING_AREA)

enum { SIG_CHANGED, N_SIGS };
static guint signals[N_SIGS];

static void
emit_changed (OrigamiCreaseCanvas *self)
{
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

/* ---------- transforms ---------- */

static void
fit_transform (int w, int h, double *scale, double *tx, double *ty)
{
    double sx = (double) w / VIRTUAL_W;
    double sy = (double) h / VIRTUAL_H;
    double s = MIN (sx, sy);
    *scale = s;
    *tx = (w - VIRTUAL_W * s) / 2.0;
    *ty = (h - VIRTUAL_H * s) / 2.0;
}

static void
widget_to_paper (OrigamiCreaseCanvas *self, double wx, double wy,
                 double *px, double *py)
{
    int w = gtk_widget_get_width  (GTK_WIDGET (self));
    int h = gtk_widget_get_height (GTK_WIDGET (self));
    double s, tx, ty;
    fit_transform (w, h, &s, &tx, &ty);
    if (s <= 0) { *px = 0; *py = 0; return; }
    /* The paper sits at virtual ((VIRTUAL-PAPER)/2, ...). Paper-mm is
     * relative to the paper top-left. */
    double paper_x_off = (VIRTUAL_W - PAPER_W) / 2.0;
    double paper_y_off = (VIRTUAL_H - PAPER_H) / 2.0;
    *px = (wx - tx) / s - paper_x_off;
    *py = (wy - ty) / s - paper_y_off;
}

/* ---------- drawing helpers ---------- */

static void
trace_face (cairo_t *cr, CreasePattern *cp, CreaseFace *f, double ox, double oy)
{
    if (!f || f->vertices->len < 3) return;
    for (guint i = 0; i < f->vertices->len; i++) {
        guint vi = g_array_index (f->vertices, guint, i);
        CreaseVertex *v = &g_array_index (cp->vertices, CreaseVertex, vi);
        if (i == 0) cairo_move_to (cr, ox + v->fx, oy + v->fy);
        else        cairo_line_to (cr, ox + v->fx, oy + v->fy);
    }
    cairo_close_path (cr);
}

static void
set_assignment_stroke (cairo_t *cr, CreaseAssignment a)
{
    switch (a) {
    case CR_BOUNDARY: {
        cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
        cairo_set_line_width (cr, 1.4);
        cairo_set_dash (cr, NULL, 0, 0);
        break;
    }
    case CR_MOUNTAIN: {
        cairo_set_source_rgb (cr, 0.85, 0.20, 0.25);
        cairo_set_line_width (cr, 1.2);
        double dashes[] = { 5, 2, 1, 2 };  /* dash-dot */
        cairo_set_dash (cr, dashes, 4, 0);
        break;
    }
    case CR_VALLEY: {
        cairo_set_source_rgb (cr, 0.18, 0.45, 0.86);
        cairo_set_line_width (cr, 1.2);
        double dashes[] = { 4, 3 };
        cairo_set_dash (cr, dashes, 2, 0);
        break;
    }
    default: {
        cairo_set_source_rgba (cr, 0.4, 0.4, 0.4, 0.6);
        cairo_set_line_width (cr, 0.8);
        cairo_set_dash (cr, NULL, 0, 0);
        break;
    }
    }
}

static void
draw_func (GtkDrawingArea *area, cairo_t *cr,
           int width, int height, gpointer user_data)
{
    OrigamiCreaseCanvas *self = ORIGAMI_CREASE_CANVAS (area);

    double s, tx, ty;
    fit_transform (width, height, &s, &tx, &ty);

    cairo_save (cr);
    cairo_translate (cr, tx, ty);
    cairo_scale (cr, s, s);

    /* Workspace tint. */
    cairo_rectangle (cr, 0, 0, VIRTUAL_W, VIRTUAL_H);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.018);
    cairo_fill (cr);

    if (!self->cp) { cairo_restore (cr); return; }

    double ox = (VIRTUAL_W - PAPER_W) / 2.0;
    double oy = (VIRTUAL_H - PAPER_H) / 2.0;

    /* Faces: light fill (cream) so we can see them. Hover face slightly
     * brighter. */
    guint hover_face = G_MAXUINT;
    if (self->has_hover)
        hover_face = crease_pattern_face_at (self->cp,
                                             self->hover_x, self->hover_y);
    for (guint i = 0; i < self->cp->faces->len; i++) {
        CreaseFace *f = self->cp->faces->pdata[i];
        if (!f) continue;
        trace_face (cr, self->cp, f, ox, oy);
        if (i == hover_face)
            cairo_set_source_rgba (cr, 0.992, 0.969, 0.929, 1.0);
        else
            cairo_set_source_rgba (cr, 0.992, 0.969, 0.929, 0.85);
        cairo_fill (cr);
    }

    /* Edges, drawn boundary first so creases sit on top. */
    static const CreaseAssignment order[] = {
        CR_BOUNDARY, CR_FLAT, CR_UNASSIGNED, CR_MOUNTAIN, CR_VALLEY,
    };
    for (guint pass = 0; pass < G_N_ELEMENTS (order); pass++) {
        for (guint i = 0; i < self->cp->edges->len; i++) {
            CreaseEdge *e = &g_array_index (self->cp->edges, CreaseEdge, i);
            if (e->assignment == CR_DELETED) continue;
            if (e->assignment != order[pass]) continue;
            CreaseVertex *V0 = &g_array_index (self->cp->vertices,
                                               CreaseVertex, e->v0);
            CreaseVertex *V1 = &g_array_index (self->cp->vertices,
                                               CreaseVertex, e->v1);
            cairo_save (cr);
            set_assignment_stroke (cr, e->assignment);
            cairo_move_to (cr, ox + V0->fx, oy + V0->fy);
            cairo_line_to (cr, ox + V1->fx, oy + V1->fy);
            cairo_stroke (cr);
            cairo_restore (cr);
        }
    }

    /* In-progress fold preview. */
    if (self->state == STATE_SECOND_POINT && self->has_hover) {
        cairo_save (cr);
        set_assignment_stroke (cr, self->next_assign);
        cairo_set_line_width (cr, 1.6);
        cairo_move_to (cr, ox + self->p1x, oy + self->p1y);
        cairo_line_to (cr, ox + self->hover_x, oy + self->hover_y);
        cairo_stroke (cr);
        cairo_restore (cr);

        cairo_save (cr);
        cairo_arc (cr, ox + self->p1x, oy + self->p1y, 1.5, 0, 2 * G_PI);
        cairo_set_source_rgb (cr, 0.18, 0.45, 0.86);
        cairo_fill (cr);
        cairo_restore (cr);
    }

    cairo_restore (cr);

    /* HUD: which assignment will the next click create. */
    cairo_save (cr);
    const char *label;
    cairo_set_font_size (cr, 12);
    if (self->next_assign == CR_MOUNTAIN) {
        label = "Next: mountain (M)";
        cairo_set_source_rgb (cr, 0.85, 0.20, 0.25);
    } else if (self->next_assign == CR_VALLEY) {
        label = "Next: valley (V)";
        cairo_set_source_rgb (cr, 0.18, 0.45, 0.86);
    } else {
        label = "Next: unassigned";
        cairo_set_source_rgba (cr, 0, 0, 0, 0.6);
    }
    cairo_text_extents_t te;
    cairo_text_extents (cr, label, &te);
    cairo_move_to (cr, 12, 12 - te.y_bearing);
    cairo_show_text (cr, label);
    cairo_restore (cr);
}

/* ---------- input ---------- */

static void
push_history (OrigamiCreaseCanvas *self)
{
    g_queue_push_tail (self->history, crease_pattern_copy (self->cp));
    while (g_queue_get_length (self->history) > 64)
        crease_pattern_free (g_queue_pop_head (self->history));
}

static void
on_pressed (GtkGestureClick *gesture, int n_press,
            double x, double y, gpointer user_data)
{
    OrigamiCreaseCanvas *self = user_data;
    guint button = gtk_gesture_single_get_current_button (
        GTK_GESTURE_SINGLE (gesture));

    if (button == GDK_BUTTON_SECONDARY) {
        origami_crease_canvas_cancel (self);
        return;
    }

    double px, py;
    widget_to_paper (self, x, y, &px, &py);

    if (self->state == STATE_FIRST_POINT) {
        /* Snap to paper bounds so users can place clean book folds. */
        if (px < 0)        px = 0;
        if (px > PAPER_W)  px = PAPER_W;
        if (py < 0)        py = 0;
        if (py > PAPER_H)  py = PAPER_H;
        self->p1x = px;
        self->p1y = py;
        self->state = STATE_SECOND_POINT;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
    }

    if (px < 0)        px = 0;
    if (px > PAPER_W)  px = PAPER_W;
    if (py < 0)        py = 0;
    if (py > PAPER_H)  py = PAPER_H;
    double dx = px - self->p1x;
    double dy = py - self->p1y;
    if (dx * dx + dy * dy < MIN_LINE * MIN_LINE) {
        /* Too short — treat as a cancel. */
        self->state = STATE_FIRST_POINT;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
    }

    push_history (self);
    crease_pattern_add_line (self->cp,
                             self->p1x, self->p1y, px, py,
                             self->next_assign);
    self->state = STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_changed (self);
}

static void
on_motion (GtkEventControllerMotion *ctrl,
           double x, double y, gpointer user_data)
{
    OrigamiCreaseCanvas *self = user_data;
    double px, py;
    widget_to_paper (self, x, y, &px, &py);
    self->hover_x = px;
    self->hover_y = py;
    self->has_hover = TRUE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *ctrl, gpointer user_data)
{
    OrigamiCreaseCanvas *self = user_data;
    if (!self->has_hover) return;
    self->has_hover = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_key (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
        GdkModifierType state, gpointer user_data)
{
    OrigamiCreaseCanvas *self = user_data;
    if (keyval == GDK_KEY_m || keyval == GDK_KEY_M) {
        origami_crease_canvas_set_assignment (self, CR_MOUNTAIN);
        return TRUE;
    }
    if (keyval == GDK_KEY_v || keyval == GDK_KEY_V) {
        origami_crease_canvas_set_assignment (self, CR_VALLEY);
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
        origami_crease_canvas_cancel (self);
        return TRUE;
    }
    return FALSE;
}

/* ---------- public API ---------- */

GtkWidget *
origami_crease_canvas_new (void)
{
    return g_object_new (ORIGAMI_TYPE_CREASE_CANVAS, NULL);
}

CreasePattern *
origami_crease_canvas_get_pattern (OrigamiCreaseCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CREASE_CANVAS (self), NULL);
    return self->cp;
}

void
origami_crease_canvas_reset (OrigamiCreaseCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS (self));
    while (!g_queue_is_empty (self->history))
        crease_pattern_free (g_queue_pop_head (self->history));
    crease_pattern_free (self->cp);
    self->cp = crease_pattern_new_rectangle (PAPER_W, PAPER_H);
    self->state = STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_changed (self);
}

void
origami_crease_canvas_load_crane (OrigamiCreaseCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS (self));
    while (!g_queue_is_empty (self->history))
        crease_pattern_free (g_queue_pop_head (self->history));
    crease_pattern_free (self->cp);
    self->cp = crease_pattern_new_crane (PAPER_W, PAPER_H);
    self->state = STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_changed (self);
}

gboolean
origami_crease_canvas_can_undo (OrigamiCreaseCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CREASE_CANVAS (self), FALSE);
    return self->state != STATE_FIRST_POINT
        || !g_queue_is_empty (self->history);
}

void
origami_crease_canvas_undo (OrigamiCreaseCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS (self));
    if (self->state != STATE_FIRST_POINT) {
        self->state = STATE_FIRST_POINT;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
    }
    if (g_queue_is_empty (self->history)) return;
    crease_pattern_free (self->cp);
    self->cp = g_queue_pop_tail (self->history);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    emit_changed (self);
}

void
origami_crease_canvas_set_assignment (OrigamiCreaseCanvas *self,
                                      CreaseAssignment a)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS (self));
    self->next_assign = a;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

CreaseAssignment
origami_crease_canvas_get_assignment (OrigamiCreaseCanvas *self)
{
    g_return_val_if_fail (ORIGAMI_IS_CREASE_CANVAS (self),
                          CR_VALLEY);
    return self->next_assign;
}

void
origami_crease_canvas_cancel (OrigamiCreaseCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS (self));
    if (self->state == STATE_FIRST_POINT) return;
    self->state = STATE_FIRST_POINT;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
origami_crease_canvas_redraw (OrigamiCreaseCanvas *self)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS (self));
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---------- GObject ---------- */

static void
origami_crease_canvas_dispose (GObject *object)
{
    OrigamiCreaseCanvas *self = ORIGAMI_CREASE_CANVAS (object);
    if (self->history) {
        while (!g_queue_is_empty (self->history))
            crease_pattern_free (g_queue_pop_head (self->history));
        g_clear_pointer (&self->history, g_queue_free);
    }
    g_clear_pointer (&self->cp, crease_pattern_free);
    G_OBJECT_CLASS (origami_crease_canvas_parent_class)->dispose (object);
}

static void
origami_crease_canvas_class_init (OrigamiCreaseCanvasClass *klass)
{
    GObjectClass *gobj = G_OBJECT_CLASS (klass);
    gobj->dispose = origami_crease_canvas_dispose;

    signals[SIG_CHANGED] = g_signal_new (
        "changed", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void
origami_crease_canvas_init (OrigamiCreaseCanvas *self)
{
    self->cp          = crease_pattern_new_rectangle (PAPER_W, PAPER_H);
    self->history     = g_queue_new ();
    self->state       = STATE_FIRST_POINT;
    self->next_assign = CR_VALLEY;
    self->has_hover   = FALSE;

    gtk_drawing_area_set_content_width  (GTK_DRAWING_AREA (self), 500);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (self), 500);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self),
                                    draw_func, self, NULL);

    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
    g_signal_connect (motion, "leave",  G_CALLBACK (on_leave),  self);
    gtk_widget_add_controller (GTK_WIDGET (self), motion);

    GtkEventController *keys = gtk_event_controller_key_new ();
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);
    gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);

    gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "crosshair");
}
