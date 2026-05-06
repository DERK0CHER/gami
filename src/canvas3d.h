#pragma once

#include <gtk/gtk.h>

#include "paper.h"

G_BEGIN_DECLS

#define ORIGAMI_TYPE_CANVAS3D (origami_canvas3d_get_type ())
G_DECLARE_FINAL_TYPE (OrigamiCanvas3D, origami_canvas3d,
                      ORIGAMI, CANVAS3D, GtkDrawingArea)

GtkWidget *origami_canvas3d_new       (void);
void       origami_canvas3d_set_paper (OrigamiCanvas3D *self,
                                       OrigamiPaper    *paper);
void       origami_canvas3d_redraw    (OrigamiCanvas3D *self);

G_END_DECLS
