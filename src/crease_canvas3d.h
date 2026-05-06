#pragma once

#include <gtk/gtk.h>

#include "crease.h"

G_BEGIN_DECLS

#define ORIGAMI_TYPE_CREASE_CANVAS3D (origami_crease_canvas3d_get_type ())
G_DECLARE_FINAL_TYPE (OrigamiCreaseCanvas3D, origami_crease_canvas3d,
                      ORIGAMI, CREASE_CANVAS3D, GtkDrawingArea)

GtkWidget *origami_crease_canvas3d_new       (void);
void       origami_crease_canvas3d_set_pattern (OrigamiCreaseCanvas3D *self,
                                                CreasePattern *cp);
/* Multiplier applied to every crease's fold_angle when rendering. 0 =
 * unfolded sheet, 1 = full fold. Use this for "completion" sliders. */
void       origami_crease_canvas3d_set_completion (OrigamiCreaseCanvas3D *self,
                                                   double t);
void       origami_crease_canvas3d_redraw    (OrigamiCreaseCanvas3D *self);

G_END_DECLS
