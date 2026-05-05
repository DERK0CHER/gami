#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    double x, y;
} OrigamiPoint;

typedef struct {
    GArray *vertices;   /* OrigamiPoint, CCW polygon */
    gboolean flipped;   /* TRUE when the back side is facing up */
} OrigamiLayer;

typedef struct {
    GPtrArray *layers;  /* OrigamiLayer*, drawn bottom -> top */
} OrigamiState;

typedef struct {
    OrigamiState *current;
    GQueue       *history;   /* OrigamiState*, oldest at head */
    double        width;
    double        height;
} OrigamiPaper;

OrigamiPaper *origami_paper_new      (double width, double height);
void          origami_paper_free     (OrigamiPaper *paper);

void          origami_paper_reset    (OrigamiPaper *paper);
gboolean      origami_paper_can_undo (OrigamiPaper *paper);
void          origami_paper_undo     (OrigamiPaper *paper);

/* Apply a fold along the infinite line through a and b.
 * fold_sign is +1 or -1: the side whose sign matches gets reflected onto the
 * other side. */
void          origami_paper_fold     (OrigamiPaper *paper,
                                      OrigamiPoint  a,
                                      OrigamiPoint  b,
                                      int           fold_sign);

/* Signed cross product. Positive / negative tells which side of a->b p is on. */
double        origami_side_of_line   (OrigamiPoint p,
                                      OrigamiPoint a,
                                      OrigamiPoint b);

/* Clip a polygon to the half-plane where side_of_line has the given sign.
 * Returns a freshly allocated GArray of OrigamiPoint (may be empty). */
GArray       *origami_clip_half_plane(GArray       *vertices,
                                      OrigamiPoint  a,
                                      OrigamiPoint  b,
                                      int           keep_sign);

G_END_DECLS
