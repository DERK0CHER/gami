#pragma once

#include <gtk/gtk.h>

#include "paper.h"

G_BEGIN_DECLS

#define ORIGAMI_TYPE_CANVAS (origami_canvas_get_type ())
G_DECLARE_FINAL_TYPE (OrigamiCanvas, origami_canvas, ORIGAMI, CANVAS, GtkDrawingArea)

typedef enum {
    ORIGAMI_CANVAS_STATE_FIRST_POINT  = 0,
    ORIGAMI_CANVAS_STATE_SECOND_POINT = 1,
    ORIGAMI_CANVAS_STATE_PICK_SIDE    = 2,
} OrigamiCanvasState;

GtkWidget          *origami_canvas_new       (void);

OrigamiCanvasState  origami_canvas_get_state (OrigamiCanvas *self);

void                origami_canvas_reset     (OrigamiCanvas *self);
void                origami_canvas_undo      (OrigamiCanvas *self);
gboolean            origami_canvas_can_undo  (OrigamiCanvas *self);
void                origami_canvas_cancel    (OrigamiCanvas *self);

G_END_DECLS
