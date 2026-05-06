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

typedef enum {
    ORIGAMI_FOLD_MODE_ALL  = 0,
    ORIGAMI_FOLD_MODE_PAGE = 1,
} OrigamiFoldMode;

typedef enum {
    ORIGAMI_TOOL_FOLD = 0,
    ORIGAMI_TOOL_PULL = 1,
} OrigamiTool;

GtkWidget          *origami_canvas_new          (void);

OrigamiCanvasState  origami_canvas_get_state    (OrigamiCanvas *self);

void                origami_canvas_reset        (OrigamiCanvas *self);
void                origami_canvas_undo         (OrigamiCanvas *self);
gboolean            origami_canvas_can_undo     (OrigamiCanvas *self);
void                origami_canvas_cancel       (OrigamiCanvas *self);

OrigamiFoldMode     origami_canvas_get_fold_mode(OrigamiCanvas *self);
void                origami_canvas_set_fold_mode(OrigamiCanvas *self,
                                                 OrigamiFoldMode mode);

OrigamiTool         origami_canvas_get_tool     (OrigamiCanvas *self);
void                origami_canvas_set_tool     (OrigamiCanvas *self,
                                                 OrigamiTool tool);

void                origami_canvas_set_show_rulers (OrigamiCanvas *self,
                                                    gboolean enabled);
gboolean            origami_canvas_get_show_rulers (OrigamiCanvas *self);

OrigamiPaper       *origami_canvas_get_paper    (OrigamiCanvas *self);
void                origami_canvas_redraw       (OrigamiCanvas *self);

/* Restore the paper to the state right after step `n` (n folds applied).
 * n == 0 -> the unfolded sheet. */
void                origami_canvas_replay_to    (OrigamiCanvas *self, guint n);

/* "Sticky" target layer: when non-zero, every fold (mouse or by-coord)
 * uses this layer as the target instead of picking from the click. */
void                origami_canvas_set_target_layer_id (OrigamiCanvas *self,
                                                        guint id);
guint               origami_canvas_get_target_layer_id (OrigamiCanvas *self);

/* Apply a fold given paper-mm coordinates (origin = top-left of paper).
 * sign is +1 / -1 for which side of the line is folded over. */
void                origami_canvas_fold_mm     (OrigamiCanvas *self,
                                                double x1_mm, double y1_mm,
                                                double x2_mm, double y2_mm,
                                                int sign);

/* Convert paper-local mm to the canvas' virtual coordinate space. */
void                origami_canvas_mm_to_virtual (OrigamiCanvas *self,
                                                  double mm_x, double mm_y,
                                                  double *vx, double *vy);

/* Toggle whether folds animate (default TRUE). */
void                origami_canvas_set_animate (OrigamiCanvas *self,
                                                gboolean enabled);
gboolean            origami_canvas_get_animate (OrigamiCanvas *self);

G_END_DECLS
