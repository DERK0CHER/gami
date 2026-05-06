#include "canvas3d.h"

#include <math.h>

/* Cairo-based fake-3D viewer. We don't need real OpenGL: the paper is a
 * stack of flat polygons, so projecting them with a simple rotation matrix
 * and drawing them painter-order (back -> front) is plenty. */

#define VIRTUAL_W 1000.0
#define VIRTUAL_H 1000.0
#define LAYER_DZ    1.6  /* virtual-mm of vertical separation per layer */

struct _OrigamiCanvas3D {
    GtkDrawingArea parent_instance;
    OrigamiPaper *paper;     /* not owned */

    double yaw;              /* radians */
    double pitch;
    double zoom;             /* 1.0 = fit */

    gboolean dragging;
    double   drag_x, drag_y;     /* drag start (widget coords) */
    double   drag_last_dx;
    double   drag_last_dy;
};

G_DEFINE_FINAL_TYPE (OrigamiCanvas3D, origami_canvas3d, GTK_TYPE_DRAWING_AREA)

static void
project (OrigamiCanvas3D *self,
         double x, double y, double z,
         double *sx, double *sy)
{
    /* Center the paper on the origin. */
    double cx = VIRTUAL_W * 0.5;
    double cy = VIRTUAL_H * 0.5;
    double X = x - cx;
    double Y = y - cy;
    double Z = z;

    /* Yaw around Y axis. */
    double cy_ = cos (self->yaw);
    double sy_ = sin (self->yaw);
    double X1 =  cy_ * X + sy_ * Z;
    double Z1 = -sy_ * X + cy_ * Z;

    /* Pitch around X axis. */
    double cp = cos (self->pitch);
    double sp = sin (self->pitch);
    double Y1 =  cp * Y - sp * Z1;
    /* Z2 = sp * Y + cp * Z1; -- not used after 2D projection */

    *sx = X1;
    *sy = Y1;
}

static void
draw_func (GtkDrawingArea *area, cairo_t *cr,
           int width, int height, gpointer user_data)
{
    OrigamiCanvas3D *self = ORIGAMI_CANVAS3D (area);

    /* Background gradient. */
    cairo_pattern_t *bg = cairo_pattern_create_linear (0, 0, 0, height);
    cairo_pattern_add_color_stop_rgb (bg, 0, 0.96, 0.96, 0.97);
    cairo_pattern_add_color_stop_rgb (bg, 1, 0.85, 0.85, 0.88);
    cairo_set_source (cr, bg);
    cairo_paint (cr);
    cairo_pattern_destroy (bg);

    if (!self->paper || !self->paper->current) return;

    double s = MIN (width, height) / VIRTUAL_W * 0.9 * self->zoom;
    double tx = width / 2.0;
    double ty = height / 2.0;

    /* Floor grid for a sense of depth. */
    cairo_save (cr);
    cairo_set_line_width (cr, 0.6);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.10);
    int floor_z = 60;
    int half = 600;
    for (int g = -half; g <= half; g += 60) {
        double sx0, sy0, sx1, sy1;
        project (self, VIRTUAL_W/2 + g, VIRTUAL_H/2 - half, floor_z, &sx0, &sy0);
        project (self, VIRTUAL_W/2 + g, VIRTUAL_H/2 + half, floor_z, &sx1, &sy1);
        cairo_move_to (cr, tx + sx0 * s, ty + sy0 * s);
        cairo_line_to (cr, tx + sx1 * s, ty + sy1 * s);
        cairo_stroke (cr);

        project (self, VIRTUAL_W/2 - half, VIRTUAL_H/2 + g, floor_z, &sx0, &sy0);
        project (self, VIRTUAL_W/2 + half, VIRTUAL_H/2 + g, floor_z, &sx1, &sy1);
        cairo_move_to (cr, tx + sx0 * s, ty + sy0 * s);
        cairo_line_to (cr, tx + sx1 * s, ty + sy1 * s);
        cairo_stroke (cr);
    }
    cairo_restore (cr);

    guint n = self->paper->current->layers->len;
    for (guint i = 0; i < n; i++) {
        OrigamiLayer *l = g_ptr_array_index (self->paper->current->layers, i);
        GArray *poly = origami_layer_world_polygon (l);
        if (poly->len < 3) { g_array_free (poly, TRUE); continue; }

        /* z grows downward from the top of the stack so highest stack
         * index sits highest in the air. Add an extra lift if the layer
         * has been pulled (translation present). */
        double lift = (l->tx != 0 || l->ty != 0 || l->rot != 0) ? 18.0 : 0.0;
        double z = - (double) i * LAYER_DZ - lift;

        cairo_save (cr);
        cairo_new_path (cr);
        for (guint j = 0; j < poly->len; j++) {
            OrigamiPoint p = g_array_index (poly, OrigamiPoint, j);
            double sx, sy;
            project (self, p.x, p.y, z, &sx, &sy);
            if (j == 0) cairo_move_to (cr, tx + sx * s, ty + sy * s);
            else        cairo_line_to (cr, tx + sx * s, ty + sy * s);
        }
        cairo_close_path (cr);

        if (l->flipped) cairo_set_source_rgb (cr, 0.91, 0.78, 0.55);
        else            cairo_set_source_rgb (cr, 0.992, 0.969, 0.929);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, 0, 0, 0, 0.30);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);

        /* Side stripe to suggest paper thickness. */
        cairo_set_source_rgba (cr, 0, 0, 0, 0.12);
        for (guint j = 0; j < poly->len; j++) {
            OrigamiPoint a = g_array_index (poly, OrigamiPoint, j);
            OrigamiPoint b = g_array_index (poly, OrigamiPoint,
                                            (j + 1) % poly->len);
            double sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
            project (self, a.x, a.y, z,            &sx0, &sy0);
            project (self, b.x, b.y, z,            &sx1, &sy1);
            project (self, b.x, b.y, z + LAYER_DZ, &sx2, &sy2);
            project (self, a.x, a.y, z + LAYER_DZ, &sx3, &sy3);
            cairo_move_to (cr, tx + sx0 * s, ty + sy0 * s);
            cairo_line_to (cr, tx + sx1 * s, ty + sy1 * s);
            cairo_line_to (cr, tx + sx2 * s, ty + sy2 * s);
            cairo_line_to (cr, tx + sx3 * s, ty + sy3 * s);
            cairo_close_path (cr);
            cairo_fill (cr);
        }

        cairo_restore (cr);
        g_array_free (poly, TRUE);
    }
}

static void
on_drag_begin (GtkGestureDrag *g, double x, double y, gpointer ud)
{
    OrigamiCanvas3D *self = ud;
    self->dragging = TRUE;
    self->drag_x = x;
    self->drag_y = y;
    self->drag_last_dx = 0;
    self->drag_last_dy = 0;
}

static void
on_drag_update (GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    OrigamiCanvas3D *self = ud;
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

static void
on_drag_end (GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    OrigamiCanvas3D *self = ud;
    self->dragging = FALSE;
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl,
           double dx, double dy, gpointer ud)
{
    OrigamiCanvas3D *self = ud;
    self->zoom *= (dy > 0) ? 0.9 : 1.1;
    if (self->zoom < 0.2) self->zoom = 0.2;
    if (self->zoom > 5.0) self->zoom = 5.0;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return TRUE;
}

GtkWidget *
origami_canvas3d_new (void)
{
    return g_object_new (ORIGAMI_TYPE_CANVAS3D, NULL);
}

void
origami_canvas3d_set_paper (OrigamiCanvas3D *self, OrigamiPaper *paper)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS3D (self));
    self->paper = paper;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
origami_canvas3d_redraw (OrigamiCanvas3D *self)
{
    g_return_if_fail (ORIGAMI_IS_CANVAS3D (self));
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
origami_canvas3d_class_init (OrigamiCanvas3DClass *klass) {}

static void
origami_canvas3d_init (OrigamiCanvas3D *self)
{
    self->yaw   = -0.6;
    self->pitch =  0.5;
    self->zoom  =  1.0;

    gtk_drawing_area_set_content_width  (GTK_DRAWING_AREA (self), 600);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (self), 600);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self),
                                    draw_func, self, NULL);

    GtkGesture *drag = gtk_gesture_drag_new ();
    g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
    g_signal_connect (drag, "drag-end",    G_CALLBACK (on_drag_end),    self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

    GtkEventController *scroll = gtk_event_controller_scroll_new (
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    gtk_widget_add_controller (GTK_WIDGET (self), scroll);
}
