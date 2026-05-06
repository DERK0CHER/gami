#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    double x, y;
} OrigamiPoint;

typedef struct {
    guint    id;        /* stable id, unique within the OrigamiPaper */
    GArray  *vertices;  /* OrigamiPoint, CCW polygon */
    gboolean flipped;   /* TRUE when the back side is facing up */
    /* Free translation/rotation applied on top of the geometry above.
     * Used by the "pull" tool to detach a flap from the rest of the
     * stack without changing its underlying outline. */
    double   tx, ty;
    double   rot;       /* radians, around the layer centroid */
} OrigamiLayer;

typedef struct {
    GPtrArray *layers;  /* OrigamiLayer*, drawn bottom -> top */
} OrigamiState;

typedef struct {
    OrigamiPoint a, b;     /* fold line (paper coords) */
    int          fold_sign;/* +1 / -1 */
    guint        target;   /* layer id, or 0 for "all layers" */
    char        *summary;  /* human-readable description for the guide */
} OrigamiFoldRecord;

typedef struct {
    OrigamiState *current;
    GQueue       *history;   /* OrigamiState*, oldest at head */
    GArray       *records;   /* OrigamiFoldRecord, parallel to history */
    double        width;
    double        height;
    guint         next_layer_id;
} OrigamiPaper;

OrigamiPaper *origami_paper_new      (double width, double height);
void          origami_paper_free     (OrigamiPaper *paper);

void          origami_paper_reset    (OrigamiPaper *paper);
gboolean      origami_paper_can_undo (OrigamiPaper *paper);
void          origami_paper_undo     (OrigamiPaper *paper);

/* Apply a fold along the infinite line through a and b.
 * fold_sign is +1 or -1: the side whose sign matches gets reflected onto
 * the other side.
 *
 * `target_ids` is a GHashTable used as a set of layer IDs (keys are
 * GUINT_TO_POINTER(id), values ignored).  NULL or empty == fold every
 * layer that crosses the line.  Otherwise, only layers whose id is in
 * the set are folded; other layers pass through unchanged.
 *
 * On the way out, IDs of newly-created folded surfaces are appended to
 * `out_new_ids` (a GArray of guint) if non-NULL — the caller uses this
 * to grow the user's selection so further folds can compose. */
void          origami_paper_fold     (OrigamiPaper *paper,
                                      OrigamiPoint  a,
                                      OrigamiPoint  b,
                                      int           fold_sign,
                                      GHashTable   *target_ids,
                                      GArray       *out_new_ids);

/* Topmost layer that contains point p (in paper coords), or NULL.
 * Considers each layer's tx/ty/rot transform. */
OrigamiLayer *origami_paper_layer_at (OrigamiPaper *paper, OrigamiPoint p);

/* Apply a free transform (translate/rotate) to a single layer. */
void          origami_paper_translate_layer (OrigamiPaper *paper,
                                             guint         id,
                                             double        dx,
                                             double        dy);
void          origami_paper_rotate_layer    (OrigamiPaper *paper,
                                             guint         id,
                                             double        d_rot);

/* Apply the layer's transform to its vertices and return a freshly
 * allocated array. Caller frees with g_array_free. */
GArray       *origami_layer_world_polygon (OrigamiLayer *layer);

/* Signed cross product. Positive / negative tells which side of a->b p is on. */
double        origami_side_of_line   (OrigamiPoint p,
                                      OrigamiPoint a,
                                      OrigamiPoint b);

/* Mirror p across the infinite line through a, b. */
OrigamiPoint  origami_reflect_across_line (OrigamiPoint p,
                                           OrigamiPoint a,
                                           OrigamiPoint b);

/* For animation: place a vertex along the swing of the fold from theta=0
 * (pre-fold position, in the half-plane to be folded) through theta=pi
 * (post-fold position, on the kept side as a mirror).  v_post is the
 * vertex's final post-fold position. */
OrigamiPoint  origami_animate_vertex (OrigamiPoint v_post,
                                      OrigamiPoint a,
                                      OrigamiPoint b,
                                      double       theta);

/* Clip a polygon to the half-plane where side_of_line has the given sign.
 * Returns a freshly allocated GArray of OrigamiPoint (may be empty). */
GArray       *origami_clip_half_plane(GArray       *vertices,
                                      OrigamiPoint  a,
                                      OrigamiPoint  b,
                                      int           keep_sign);

G_END_DECLS
