/* Smoke test for the crease-graph rewrite. Links only against glib. */

#include "crease.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT(cond, ...) do {                                          \
    if (!(cond)) {                                                      \
        fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
        fprintf (stderr, __VA_ARGS__);                                  \
        fprintf (stderr, "\n");                                         \
        failures++;                                                     \
    }                                                                   \
} while (0)

static void
test_rectangle (void)
{
    CreasePattern *cp = crease_pattern_new_rectangle (100, 100);
    EXPECT (cp != NULL, "alloc returned NULL");
    EXPECT (cp->vertices->len == 4, "want 4 vertices, got %u",
            cp->vertices->len);
    EXPECT (crease_pattern_alive_edge_count (cp) == 4,
            "want 4 alive edges, got %u",
            crease_pattern_alive_edge_count (cp));
    EXPECT (crease_pattern_alive_face_count (cp) == 1,
            "want 1 face, got %u",
            crease_pattern_alive_face_count (cp));
    EXPECT (crease_pattern_face_at (cp, 50, 50) == 0,
            "midpoint should be in face 0");
    EXPECT (crease_pattern_face_at (cp, 200, 200) == G_MAXUINT,
            "outside should not be in any face");
    crease_pattern_free (cp);
}

static void
test_book_fold (void)
{
    /* One vertical valley fold across the middle. */
    CreasePattern *cp = crease_pattern_new_rectangle (100, 100);
    gboolean changed = crease_pattern_add_line (cp, 50, 0, 50, 100, CR_VALLEY);
    EXPECT (changed, "add_line should have split the face");
    EXPECT (crease_pattern_alive_face_count (cp) == 2,
            "want 2 alive faces after one fold, got %u",
            crease_pattern_alive_face_count (cp));
    /* 4 boundary edges originally: top and bottom got split (+2 new
     * each, original deleted), left and right untouched, plus 1 new
     * crease.  Alive = 2 (left/right untouched) + 4 (sub-edges) + 1
     * (crease) = 7. */
    EXPECT (crease_pattern_alive_edge_count (cp) == 7,
            "want 7 alive edges after book fold, got %u",
            crease_pattern_alive_edge_count (cp));
    EXPECT (cp->vertices->len == 6, "want 6 vertices, got %u",
            cp->vertices->len);
    crease_pattern_free (cp);
}

static void
test_preliminary_creases (void)
{
    /* Vertical + horizontal — the two book folds that start a
     * preliminary base. */
    CreasePattern *cp = crease_pattern_new_rectangle (100, 100);
    gboolean a = crease_pattern_add_line (cp, 50, 0, 50, 100, CR_VALLEY);
    gboolean b = crease_pattern_add_line (cp, 0, 50, 100, 50, CR_VALLEY);
    EXPECT (a && b, "both folds should split");
    EXPECT (crease_pattern_alive_face_count (cp) == 4,
            "want 4 alive faces, got %u",
            crease_pattern_alive_face_count (cp));
    /* Vertices: 4 corners + 4 mid-edge points + 1 center = 9. */
    EXPECT (cp->vertices->len == 9,
            "want 9 vertices, got %u", cp->vertices->len);
    crease_pattern_free (cp);
}

static void
test_fold_3d_flat (void)
{
    /* One book fold at +pi (full valley) -> the right half should land
     * on top of the left half (mirrored), with z = 0. */
    CreasePattern *cp = crease_pattern_new_rectangle (100, 100);
    crease_pattern_add_line (cp, 50, 0, 50, 100, CR_VALLEY);
    /* Root in the left half so the right half folds onto it. */
    guint root = crease_pattern_face_at (cp, 25, 50);
    EXPECT (root != G_MAXUINT, "left face missing");
    crease_pattern_compute_folded (cp, root);

    /* Check: after folding, the vertex at flat (100, 0) should have
     * landed at (0, 0, 0) — full mirror across x = 50. */
    gboolean found = FALSE;
    for (guint i = 0; i < cp->vertices->len; i++) {
        CreaseVertex *v = &g_array_index (cp->vertices, CreaseVertex, i);
        if (fabs (v->fx - 100) < 1e-3 && fabs (v->fy - 0) < 1e-3) {
            found = TRUE;
            EXPECT (fabs (v->X - 0) < 1e-3 && fabs (v->Y - 0) < 1e-3
                    && fabs (v->Z - 0) < 1e-3,
                    "(100,0) should fold to (0,0,0); got (%.3f, %.3f, %.3f)",
                    v->X, v->Y, v->Z);
            break;
        }
    }
    EXPECT (found, "no vertex at (100, 0)");
    crease_pattern_free (cp);
}

static void
test_fold_3d_quarter (void)
{
    /* Half-fold: one valley at +pi/2.  The right half should be in
     * the YZ plane, perpendicular to the unfolded sheet. */
    CreasePattern *cp = crease_pattern_new_rectangle (100, 100);
    crease_pattern_add_line (cp, 50, 0, 50, 100, CR_VALLEY);
    /* Find the new crease (between (50,0) and (50,100)) and set angle. */
    for (guint i = 0; i < cp->edges->len; i++) {
        CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, i);
        if (e->assignment == CR_VALLEY) {
            e->fold_angle = G_PI / 2.0;
        }
    }
    /* Root: face that contains (25, 50). */
    guint root = crease_pattern_face_at (cp, 25, 50);
    EXPECT (root != G_MAXUINT, "left face missing");
    crease_pattern_compute_folded (cp, root);

    /* Vertex (100, 0) should now be at (50, 0, 50). */
    for (guint i = 0; i < cp->vertices->len; i++) {
        CreaseVertex *v = &g_array_index (cp->vertices, CreaseVertex, i);
        if (fabs (v->fx - 100) < 1e-3 && fabs (v->fy - 0) < 1e-3) {
            EXPECT (fabs (v->X - 50) < 1e-3
                    && fabs (v->Y - 0)  < 1e-3
                    && fabs (v->Z - 50) < 1e-3,
                    "(100,0) at +pi/2 should be (50,0,50); got (%.3f, %.3f, %.3f)",
                    v->X, v->Y, v->Z);
            break;
        }
    }
    crease_pattern_free (cp);
}

int
main (void)
{
    test_rectangle ();
    test_book_fold ();
    test_preliminary_creases ();
    test_fold_3d_flat ();
    test_fold_3d_quarter ();
    if (failures == 0)
        fprintf (stdout, "all crease tests passed\n");
    else
        fprintf (stdout, "%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
