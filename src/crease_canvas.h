#pragma once

#include <gtk/gtk.h>

#include "crease.h"

G_BEGIN_DECLS

#define ORIGAMI_TYPE_CREASE_CANVAS (origami_crease_canvas_get_type ())
G_DECLARE_FINAL_TYPE (OrigamiCreaseCanvas, origami_crease_canvas,
                      ORIGAMI, CREASE_CANVAS, GtkDrawingArea)

GtkWidget     *origami_crease_canvas_new           (void);

CreasePattern *origami_crease_canvas_get_pattern   (OrigamiCreaseCanvas *self);

/* Reset the pattern to a single-face square. */
void           origami_crease_canvas_reset         (OrigamiCreaseCanvas *self);

/* Replace the current pattern with the crane / bird-base preset. */
void           origami_crease_canvas_load_crane    (OrigamiCreaseCanvas *self);

/* Undo the last fold. */
gboolean       origami_crease_canvas_can_undo      (OrigamiCreaseCanvas *self);
void           origami_crease_canvas_undo          (OrigamiCreaseCanvas *self);

/* Mountain or valley for the next fold the user places. */
void           origami_crease_canvas_set_assignment (OrigamiCreaseCanvas *self,
                                                     CreaseAssignment a);
CreaseAssignment origami_crease_canvas_get_assignment (OrigamiCreaseCanvas *self);

void           origami_crease_canvas_cancel        (OrigamiCreaseCanvas *self);
void           origami_crease_canvas_redraw        (OrigamiCreaseCanvas *self);

G_END_DECLS
