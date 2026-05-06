#include "crease_canvas3d.h"

#include <math.h>

#define PAPER_W 100.0
#define PAPER_H 100.0

struct _OrigamiCreaseCanvas3D {
    GtkDrawingArea parent_instance;
    CreasePattern *cp;          /* not owned */
    double completion;          /* 0..1 multiplier on every fold_angle */
    double yaw;
    double pitch;
    double zoom;
    double drag_last_dx;
    double drag_last_dy;

    /* Cached folded snapshot. Rebuilt only when the source pattern or the
     * completion factor changes; orbit/zoom drags reuse it. Without this
     * cache draw_func deep-copies the whole pattern and runs the BFS on
     * every frame, which during a fast drag pegs the allocator and can
     * push the system into swap. */
    CreasePattern *snap;
    double         snap_completion;
    gboolean       snap_valid;
};

G_DEFINE_FINAL_TYPE (OrigamiCreaseCanvas3D, origami_crease_canvas3d,
                     GTK_TYPE_DRAWING_AREA)

/* ---------- view transform ---------- */

static void
project (OrigamiCreaseCanvas3D *self,
         double X, double Y, double Z, double *sx, double *sy)
{
    /* Centre on the paper midpoint. */
    double x = X - PAPER_W * 0.5;
    double y = Y - PAPER_H * 0.5;
    double z = Z;

    double cyaw = cos (self->yaw),  syaw = sin (self->yaw);
    double cpit = cos (self->pitch), spit = sin (self->pitch);

    double X1 =  cyaw * x + syaw * z;
    double Z1 = -syaw * x + cyaw * z;
    double Y1 =  cpit * y - spit * Z1;
    /* (Z2 unused; we drop it for the 2D projection.) */

    *sx = X1;
    *sy = Y1;
}

/* ---------- folded-pattern build ---------- */

static CreasePattern *
build_folded_snapshot (CreasePattern *src, double completion)
{
    if (!src) return NULL;
    CreasePattern *cp = crease_pattern_copy (src);
    /* Scale every crease's fold angle by `completion`. */
    for (guint i = 0; i < cp->edges->len; i++) {
        CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, i);
        if (e->assignment == CR_DELETED) continue;
        e->fold_angle *= completion;
    }
    /* Choose the lowest-index alive face as root. Stable across runs. */
    guint root = G_MAXUINT;
    for (guint i = 0; i < cp->faces->len; i++) {
        if (cp->faces->pdata[i] != NULL) { root = i; break; }
    }
    if (root != G_MAXUINT)
        crease_pattern_compute_folded (cp, root);
    return cp;
}

/* ---------- painter-order draw ---------- */

typedef struct {
    guint face_idx;
    double avg_z;
} FaceDepth;

static int
fd_cmp (const void *a, const void *b)
{
    const FaceDepth *fa = a;
    const FaceDepth *fb = b;
    if (fa->avg_z < fb->avg_z) return -1;
    if (fa->avg_z > fb->avg_z) return  1;
    return 0;
}

static void
draw_func (GtkDrawingArea *area, cairo_t *cr,
           int width, int height, gpointer user_data)
{
    OrigamiCreaseCanvas3D *self = ORIGAMI_CREASE_CANVAS3D (area);

    cairo_pattern_t *bg = cairo_pattern_create_linear (0, 0, 0, height);
    cairo_pattern_add_color_stop_rgb (bg, 0, 0.96, 0.96, 0.97);
    cairo_pattern_add_color_stop_rgb (bg, 1, 0.85, 0.85, 0.88);
    cairo_set_source (cr, bg);
    cairo_paint (cr);
    cairo_pattern_destroy (bg);

    if (!self->cp) return;

    if (!self->snap_valid || self->snap == NULL
        || self->snap_completion != self->completion) {
        g_clear_pointer (&self->snap, crease_pattern_free);
        self->snap = build_folded_snapshot (self->cp, self->completion);
        self->snap_completion = self->completion;
        self->snap_valid = (self->snap != NULL);
    }
    CreasePattern *snap = self->snap;
    if (!snap) return;

    double s  = MIN (width, height) / 200.0 * self->zoom;
    double tx = width / 2.0;
    double ty = height / 2.0;

    /* For each alive face, compute its average view-Z so we can paint
     * back-to-front. Yaw/pitch put the user roughly in front, so larger
     * view-Z = closer to camera = drawn last. */
    guint nfaces = snap->faces->len;
    FaceDepth *fd = g_new (FaceDepth, nfaces);
    guint nd = 0;

    /* The view "depth" axis after our 2D projection drop is Z2, which
     * we re-derive here. Keep it consistent with project(). */
    double cyaw = cos (self->yaw),  syaw = sin (self->yaw);
    double cpit = cos (self->pitch), spit = sin (self->pitch);

    for (guint i = 0; i < nfaces; i++) {
        CreaseFace *f = snap->faces->pdata[i];
        if (!f || f->vertices->len < 3) continue;
        double sum = 0;
        for (guint k = 0; k < f->vertices->len; k++) {
            guint vi = g_array_index (f->vertices, guint, k);
            CreaseVertex *v = &g_array_index (snap->vertices,
                                              CreaseVertex, vi);
            double x = v->X - PAPER_W * 0.5;
            double y = v->Y - PAPER_H * 0.5;
            double z = v->Z;
            double Z1 = -syaw * x + cyaw * z;
            double Z2 = spit * y + cpit * Z1;
            sum += Z2;
        }
        fd[nd].face_idx = i;
        fd[nd].avg_z = sum / f->vertices->len;
        nd++;
    }
    qsort (fd, nd, sizeof (FaceDepth), fd_cmp);

    for (guint k = 0; k < nd; k++) {
        guint i = fd[k].face_idx;
        CreaseFace *f = snap->faces->pdata[i];
        if (!f) continue;

        cairo_new_path (cr);
        for (guint j = 0; j < f->vertices->len; j++) {
            guint vi = g_array_index (f->vertices, guint, j);
            CreaseVertex *v = &g_array_index (snap->vertices,
                                              CreaseVertex, vi);
            double sx, sy;
            project (self, v->X, v->Y, v->Z, &sx, &sy);
            if (j == 0) cairo_move_to (cr, tx + sx * s, ty + sy * s);
            else        cairo_line_to (cr, tx + sx * s, ty + sy * s);
        }
        cairo_close_path (cr);
        /* Faces on the back side (looking flipped to viewer) get the
         * tan back color. Detect by signed area in screen space.
         * Negative = clockwise = back. */
        double area = 0;
        for (guint j = 0; j < f->vertices->len; j++) {
            guint vi = g_array_index (f->vertices, guint, j);
            guint vj = g_array_index (f->vertices, guint,
                                       (j + 1) % f->vertices->len);
            CreaseVertex *V0 = &g_array_index (snap->vertices,
                                               CreaseVertex, vi);
            CreaseVertex *V1 = &g_array_index (snap->vertices,
                                               CreaseVertex, vj);
            double sx0, sy0, sx1, sy1;
            project (self, V0->X, V0->Y, V0->Z, &sx0, &sy0);
            project (self, V1->X, V1->Y, V1->Z, &sx1, &sy1);
            area += (sx1 - sx0) * (sy1 + sy0);
        }
        if (area < 0) cairo_set_source_rgb (cr, 0.91, 0.78, 0.55);
        else          cairo_set_source_rgb (cr, 0.992, 0.969, 0.929);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, 0, 0, 0, 0.30);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);
    }

    g_free (fd);
    /* `snap` is owned by self->snap and reused across redraws. */
}

/* ---------- input ---------- */

static void
on_drag_begin (GtkGestureDrag *g, double x, double y, gpointer ud)
{
    OrigamiCreaseCanvas3D *self = ud;
    self->drag_last_dx = 0;
    self->drag_last_dy = 0;
}

static void
on_drag_update (GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    OrigamiCreaseCanvas3D *self = ud;
    double ddx = dx - self->drag_last_dx;
    double ddy = dy - self->drag_last_dy;
    self->drag_last_dx = dx;
    self->drag_last_dy = dy;
    self->yaw   += ddx * 0.005;
    self->pitch += ddy * 0.005;
    if (self->pitch >  G_PI/2 - 0.05) self->pitch =  G_PI/2 - 0.05;
    if (self->pitch < -G_PI/2 + 0.05) self->pitch = -G_PI/2 + 0.05;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl,
           double dx, double dy, gpointer ud)
{
    OrigamiCreaseCanvas3D *self = ud;
    self->zoom *= (dy > 0) ? 0.9 : 1.1;
    if (self->zoom < 0.2) self->zoom = 0.2;
    if (self->zoom > 6.0) self->zoom = 6.0;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return TRUE;
}

/* ---------- public API ---------- */

GtkWidget *
origami_crease_canvas3d_new (void)
{
    return g_object_new (ORIGAMI_TYPE_CREASE_CANVAS3D, NULL);
}

void
origami_crease_canvas3d_set_pattern (OrigamiCreaseCanvas3D *self,
                                     CreasePattern *cp)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS3D (self));
    self->cp = cp;
    self->snap_valid = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
origami_crease_canvas3d_set_completion (OrigamiCreaseCanvas3D *self,
                                        double t)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS3D (self));
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (self->completion == t) return;
    self->completion = t;
    self->snap_valid = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
origami_crease_canvas3d_redraw (OrigamiCreaseCanvas3D *self)
{
    g_return_if_fail (ORIGAMI_IS_CREASE_CANVAS3D (self));
    self->snap_valid = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---------- GObject ---------- */

static void
origami_crease_canvas3d_dispose (GObject *object)
{
    OrigamiCreaseCanvas3D *self = ORIGAMI_CREASE_CANVAS3D (object);
    g_clear_pointer (&self->snap, crease_pattern_free);
    G_OBJECT_CLASS (origami_crease_canvas3d_parent_class)->dispose (object);
}

static void
origami_crease_canvas3d_class_init (OrigamiCreaseCanvas3DClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = origami_crease_canvas3d_dispose;
}

static void
origami_crease_canvas3d_init (OrigamiCreaseCanvas3D *self)
{
    self->yaw        = -0.6;
    self->pitch      =  0.5;
    self->zoom       =  1.0;
    self->completion =  1.0;

    gtk_drawing_area_set_content_width  (GTK_DRAWING_AREA (self), 500);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (self), 500);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self),
                                    draw_func, self, NULL);

    GtkGesture *drag = gtk_gesture_drag_new ();
    g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

    GtkEventController *scroll = gtk_event_controller_scroll_new (
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    gtk_widget_add_controller (GTK_WIDGET (self), scroll);
}
