#pragma once

/* Crease-pattern model for the origami-fold rewrite.
 *
 * The existing `paper.c` represents folded paper as a stack of opaque
 * 2D polygons.  That model is fine for "flatten one half over the
 * other" but can't represent the connectivity needed for reverse
 * folds, petal folds, the preliminary base, or any 3D collapse — none
 * of those are simple half-plane reflections.
 *
 * This module introduces a real crease graph:
 *
 *   - vertices live in flat-paper coordinates (the unfolded square)
 *     plus a computed (X, Y, Z) in folded space;
 *   - edges connect two vertices and carry an assignment (boundary,
 *     mountain, valley, flat-guide, unassigned) and a fold angle;
 *   - faces are CCW vertex/edge cycles, with a layer index used when
 *     several faces overlap in the flat fold.
 *
 * The data model is intentionally close to the FOLD format, so we can
 * import/export and so anyone familiar with it can read this code.
 *
 * No deletion: vertices and edges only ever grow; when we split an
 * edge we mark the old one as deleted (assignment = CR_DELETED) and
 * append two new edges.  Faces use a GPtrArray with NULL slots to keep
 * indices stable (the edges' left/right face indices are durable).
 */

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    CR_BOUNDARY   = 0,
    CR_MOUNTAIN   = 1,
    CR_VALLEY     = 2,
    CR_FLAT       = 3,
    CR_UNASSIGNED = 4,
    CR_DELETED    = 5,   /* edge was split; ignore in iteration */
} CreaseAssignment;

typedef struct {
    /* Position in the flat (unfolded) paper. */
    double fx, fy;
    /* Position in folded 3D space.  Re-populated by
     * `crease_pattern_compute_folded`. */
    double X, Y, Z;
} CreaseVertex;

typedef struct {
    guint            v0, v1;        /* vertex indices */
    CreaseAssignment assignment;
    double           fold_angle;    /* radians; 0 == flat-fold guide */
    /* Adjacent face indices.  G_MAXUINT means "no face here yet" /
     * boundary on that side. */
    guint            left;
    guint            right;
} CreaseEdge;

typedef struct {
    GArray *vertices;   /* guint, CCW */
    GArray *edges;      /* guint, parallel to vertices: edges[i] connects
                         * vertices[i] -> vertices[(i+1) % n] */
    int     layer;      /* stack order in the folded state */
} CreaseFace;

typedef struct {
    GArray    *vertices;   /* CreaseVertex */
    GArray    *edges;      /* CreaseEdge */
    GPtrArray *faces;      /* CreaseFace*, NULL slots = retired */
    double     width, height;
} CreasePattern;

/* ---------- lifecycle ---------- */

CreasePattern *crease_pattern_new_rectangle (double w, double h);
CreasePattern *crease_pattern_copy          (const CreasePattern *src);
void           crease_pattern_free          (CreasePattern *cp);

/* Preset: a "crane base" crease pattern on a w x h rectangle.
 *
 * Loads the bird-base creases used as the first stage of folding a paper
 * crane: both diagonals as valleys, both edge bisectors as mountains, and
 * four petal-fold creases that bisect each diagonal/bisector pair. The
 * caller can then drive `crease_pattern_compute_folded` (via the 3D
 * viewer's completion slider) to watch the sheet collapse. */
CreasePattern *crease_pattern_new_crane     (double w, double h);

/* ---------- queries ---------- */

guint    crease_pattern_alive_face_count  (CreasePattern *cp);
guint    crease_pattern_alive_edge_count  (CreasePattern *cp);

/* Index of the face that contains the flat-paper point (fx, fy), or
 * G_MAXUINT if the point lies outside every face. */
guint    crease_pattern_face_at           (CreasePattern *cp,
                                           double fx, double fy);

/* ---------- editing ---------- */

/* Run a fold line through every face it crosses; split the affected
 * faces and add new crease edges with the given assignment.
 *
 * Returns TRUE iff at least one face was split.  No-op (returns FALSE)
 * for lines that miss every face or are collinear with existing edges.
 *
 * a, b are in flat-paper coordinates.  The new crease has a fold_angle
 * derived from the assignment: M -> -π, V -> +π, FLAT -> 0,
 * UNASSIGNED -> 0.  Use `crease_pattern_set_fold_angle` afterwards to
 * tweak it (partial folds for animation, etc.). */
gboolean crease_pattern_add_line          (CreasePattern   *cp,
                                           double ax, double ay,
                                           double bx, double by,
                                           CreaseAssignment a);

/* Flip mountain<->valley for an existing edge, including its
 * `fold_angle` sign.  Boundary / flat / unassigned edges are left
 * alone. */
void     crease_pattern_flip_assignment   (CreasePattern *cp, guint edge);

/* Set fold_angle directly (radians, signed).  Does not touch
 * assignment. */
void     crease_pattern_set_fold_angle    (CreasePattern *cp,
                                           guint edge, double radians);

/* ---------- folding ---------- */

/* Recompute every vertex's (X, Y, Z) by walking the dual graph from
 * the given root face (which stays in the z = 0 plane).  Honors each
 * edge's `fold_angle`. */
void     crease_pattern_compute_folded    (CreasePattern *cp,
                                           guint root_face);

G_END_DECLS
