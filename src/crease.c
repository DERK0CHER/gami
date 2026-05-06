#include "crease.h"

#include <math.h>
#include <string.h>

/* Tolerance for "this point lies exactly on this line / equals this
 * other vertex". The flat paper lives in mm-scale coordinates, so
 * 1e-6 mm is well below any feature we care about. */
#define CR_EPS 1e-6

/* ---------- low-level helpers ---------- */

static double
side_of_line (double px, double py,
              double ax, double ay,
              double bx, double by)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static guint
cp_add_vertex (CreasePattern *cp, double x, double y)
{
    for (guint i = 0; i < cp->vertices->len; i++) {
        CreaseVertex *v = &g_array_index (cp->vertices, CreaseVertex, i);
        if (fabs (v->fx - x) < CR_EPS && fabs (v->fy - y) < CR_EPS)
            return i;
    }
    CreaseVertex v = { .fx = x, .fy = y, .X = x, .Y = y, .Z = 0 };
    g_array_append_val (cp->vertices, v);
    return cp->vertices->len - 1;
}

static double
default_angle_for (CreaseAssignment a)
{
    switch (a) {
    case CR_MOUNTAIN: return -G_PI;
    case CR_VALLEY:   return  G_PI;
    default:          return  0.0;
    }
}

static guint
cp_add_edge (CreasePattern *cp, guint v0, guint v1, CreaseAssignment a)
{
    CreaseEdge e = {
        .v0 = v0,
        .v1 = v1,
        .assignment = a,
        .fold_angle = default_angle_for (a),
        .left  = G_MAXUINT,
        .right = G_MAXUINT,
    };
    g_array_append_val (cp->edges, e);
    return cp->edges->len - 1;
}

static CreaseFace *
face_new (void)
{
    CreaseFace *f = g_new0 (CreaseFace, 1);
    f->vertices = g_array_new (FALSE, FALSE, sizeof (guint));
    f->edges    = g_array_new (FALSE, FALSE, sizeof (guint));
    return f;
}

static void
face_free (CreaseFace *f)
{
    if (!f) return;
    g_array_free (f->vertices, TRUE);
    g_array_free (f->edges,    TRUE);
    g_free (f);
}

static gboolean
edge_is_alive (CreaseEdge *e)
{
    return e->assignment != CR_DELETED;
}

/* ---------- lifecycle ---------- */

CreasePattern *
crease_pattern_new_rectangle (double w, double h)
{
    CreasePattern *cp = g_new0 (CreasePattern, 1);
    cp->width  = w;
    cp->height = h;
    cp->vertices = g_array_new (FALSE, FALSE, sizeof (CreaseVertex));
    cp->edges    = g_array_new (FALSE, FALSE, sizeof (CreaseEdge));
    cp->faces    = g_ptr_array_new ();

    guint v0 = cp_add_vertex (cp, 0, 0);
    guint v1 = cp_add_vertex (cp, w, 0);
    guint v2 = cp_add_vertex (cp, w, h);
    guint v3 = cp_add_vertex (cp, 0, h);

    guint e0 = cp_add_edge (cp, v0, v1, CR_BOUNDARY);
    guint e1 = cp_add_edge (cp, v1, v2, CR_BOUNDARY);
    guint e2 = cp_add_edge (cp, v2, v3, CR_BOUNDARY);
    guint e3 = cp_add_edge (cp, v3, v0, CR_BOUNDARY);

    CreaseFace *f = face_new ();
    g_array_append_val (f->vertices, v0);
    g_array_append_val (f->vertices, v1);
    g_array_append_val (f->vertices, v2);
    g_array_append_val (f->vertices, v3);
    g_array_append_val (f->edges, e0);
    g_array_append_val (f->edges, e1);
    g_array_append_val (f->edges, e2);
    g_array_append_val (f->edges, e3);
    g_ptr_array_add (cp->faces, f);

    g_array_index (cp->edges, CreaseEdge, e0).left = 0;
    g_array_index (cp->edges, CreaseEdge, e1).left = 0;
    g_array_index (cp->edges, CreaseEdge, e2).left = 0;
    g_array_index (cp->edges, CreaseEdge, e3).left = 0;

    return cp;
}

CreasePattern *
crease_pattern_new_crane (double w, double h)
{
    CreasePattern *cp = crease_pattern_new_rectangle (w, h);

    /* Bird-base scaffolding.  The two diagonals are valleys that bring
     * opposite corners together; the two perpendicular bisectors are
     * mountains so the paper pre-creases for the corner collapse.
     * Together they're the core of every crane fold. */
    crease_pattern_add_line (cp, 0, 0,    w, h, CR_VALLEY);
    crease_pattern_add_line (cp, w, 0,    0, h, CR_VALLEY);
    crease_pattern_add_line (cp, w/2, 0,  w/2, h, CR_MOUNTAIN);
    crease_pattern_add_line (cp, 0, h/2,  w,   h/2, CR_MOUNTAIN);

    /* Petal-fold creases: a diamond connecting the four edge midpoints.
     * These are the bird-base creases that lift the front flap into a
     * kite shape during the corner collapse — without them the
     * preliminary base just stays a small square. */
    crease_pattern_add_line (cp, w/2, 0,    w,   h/2, CR_VALLEY);
    crease_pattern_add_line (cp, w,   h/2,  w/2, h,   CR_VALLEY);
    crease_pattern_add_line (cp, w/2, h,    0,   h/2, CR_VALLEY);
    crease_pattern_add_line (cp, 0,   h/2,  w/2, 0,   CR_VALLEY);

    return cp;
}

CreasePattern *
crease_pattern_copy (const CreasePattern *src)
{
    if (!src) return NULL;
    CreasePattern *dst = g_new0 (CreasePattern, 1);
    dst->width  = src->width;
    dst->height = src->height;
    dst->vertices = g_array_sized_new (FALSE, FALSE,
                                       sizeof (CreaseVertex),
                                       src->vertices->len);
    g_array_append_vals (dst->vertices, src->vertices->data,
                         src->vertices->len);
    dst->edges = g_array_sized_new (FALSE, FALSE,
                                    sizeof (CreaseEdge),
                                    src->edges->len);
    g_array_append_vals (dst->edges, src->edges->data, src->edges->len);
    dst->faces = g_ptr_array_new ();
    for (guint i = 0; i < src->faces->len; i++) {
        CreaseFace *sf = src->faces->pdata[i];
        if (!sf) {
            g_ptr_array_add (dst->faces, NULL);
            continue;
        }
        CreaseFace *df = face_new ();
        df->layer = sf->layer;
        g_array_append_vals (df->vertices, sf->vertices->data,
                             sf->vertices->len);
        g_array_append_vals (df->edges, sf->edges->data, sf->edges->len);
        g_ptr_array_add (dst->faces, df);
    }
    return dst;
}

void
crease_pattern_free (CreasePattern *cp)
{
    if (!cp) return;
    g_array_free (cp->vertices, TRUE);
    g_array_free (cp->edges, TRUE);
    for (guint i = 0; i < cp->faces->len; i++)
        face_free (cp->faces->pdata[i]);
    g_ptr_array_free (cp->faces, TRUE);
    g_free (cp);
}

/* ---------- queries ---------- */

guint
crease_pattern_alive_face_count (CreasePattern *cp)
{
    guint n = 0;
    for (guint i = 0; i < cp->faces->len; i++)
        if (cp->faces->pdata[i] != NULL) n++;
    return n;
}

guint
crease_pattern_alive_edge_count (CreasePattern *cp)
{
    guint n = 0;
    for (guint i = 0; i < cp->edges->len; i++) {
        CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, i);
        if (edge_is_alive (e)) n++;
    }
    return n;
}

static gboolean
face_contains (CreasePattern *cp, CreaseFace *f, double x, double y)
{
    /* Standard ray-cast point-in-polygon, in flat-paper coords. */
    gboolean inside = FALSE;
    guint n = f->vertices->len;
    if (n < 3) return FALSE;
    for (guint i = 0, j = n - 1; i < n; j = i++) {
        guint vi = g_array_index (f->vertices, guint, i);
        guint vj = g_array_index (f->vertices, guint, j);
        CreaseVertex *Vi = &g_array_index (cp->vertices, CreaseVertex, vi);
        CreaseVertex *Vj = &g_array_index (cp->vertices, CreaseVertex, vj);
        if (((Vi->fy > y) != (Vj->fy > y)) &&
            (x < (Vj->fx - Vi->fx) * (y - Vi->fy)
                  / (Vj->fy - Vi->fy + 1e-30) + Vi->fx))
            inside = !inside;
    }
    return inside;
}

guint
crease_pattern_face_at (CreasePattern *cp, double fx, double fy)
{
    for (guint i = 0; i < cp->faces->len; i++) {
        CreaseFace *f = cp->faces->pdata[i];
        if (!f) continue;
        if (face_contains (cp, f, fx, fy)) return i;
    }
    return G_MAXUINT;
}

/* ---------- assignment editing ---------- */

void
crease_pattern_flip_assignment (CreasePattern *cp, guint edge)
{
    if (edge >= cp->edges->len) return;
    CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, edge);
    if (e->assignment == CR_MOUNTAIN) {
        e->assignment = CR_VALLEY;
        e->fold_angle = +G_PI;
    } else if (e->assignment == CR_VALLEY) {
        e->assignment = CR_MOUNTAIN;
        e->fold_angle = -G_PI;
    }
}

void
crease_pattern_set_fold_angle (CreasePattern *cp, guint edge, double radians)
{
    if (edge >= cp->edges->len) return;
    CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, edge);
    e->fold_angle = radians;
}

/* ---------- splitting an existing edge at a new vertex ----------
 *
 * `orig_e` is the edge to split. `new_v` is a vertex that lies on it
 * (a fresh interior point we just added). We mark `orig_e` deleted,
 * append two sub-edges with the same assignment, and update the up-to-
 * two faces that referenced the original to splice in the sub-edges
 * and the new vertex.
 *
 * This function is only called from add_line, and the caller is
 * responsible for ALSO updating its own face's vertex/edge lists. We
 * touch only the *other* face here. */
static void
split_edge_in_other_face (CreasePattern *cp,
                          guint  orig_e,
                          guint  vi, guint vj,
                          guint  new_v,
                          guint  sub1, guint sub2,
                          guint  this_face)
{
    CreaseEdge *orig = &g_array_index (cp->edges, CreaseEdge, orig_e);
    guint other = (orig->left == this_face) ? orig->right : orig->left;
    if (other == G_MAXUINT || other >= cp->faces->len) return;
    CreaseFace *F = cp->faces->pdata[other];
    if (!F) return;

    for (guint k = 0; k < F->edges->len; k++) {
        if (g_array_index (F->edges, guint, k) != orig_e) continue;

        guint vk = g_array_index (F->vertices, guint, k);
        guint k1 = (k + 1) % F->vertices->len;
        guint vk1 = g_array_index (F->vertices, guint, k1);
        (void) vk1;

        if (vk == vi) {
            /* `other` traverses vi -> vj (same direction as we did). */
            g_array_index (F->edges, guint, k) = sub1;
            g_array_insert_val (F->edges, k + 1, sub2);
            g_array_insert_val (F->vertices, k + 1, new_v);
        } else {
            /* `other` traverses vj -> vi (opposite direction). */
            g_array_index (F->edges, guint, k) = sub2;
            g_array_insert_val (F->edges, k + 1, sub1);
            g_array_insert_val (F->vertices, k + 1, new_v);
        }
        return;
    }
}

/* ---------- the workhorse: add_line ---------- */

typedef struct {
    /* Index in face->vertices of the hit vertex.  After splitting an
     * edge mid-segment we insert the new vertex, so this index is the
     * post-insertion position. */
    guint vertex_idx;
    gboolean was_existing;  /* TRUE if we hit an existing vertex */
} Hit;

static gboolean
hit_already_recorded (Hit *hits, guint count, guint v)
{
    for (guint i = 0; i < count; i++)
        if (hits[i].vertex_idx == v) return TRUE;
    return FALSE;
}

static gboolean
process_face (CreasePattern *cp,
              guint           fi,
              double ax, double ay,
              double bx, double by,
              CreaseAssignment assign)
{
    CreaseFace *F = cp->faces->pdata[fi];
    if (!F) return FALSE;

    Hit hits[8];
    guint hit_count = 0;

    /* Walk edges of F.  We may insert into F->vertices/edges mid-loop
     * (via interior crossings); each insert grows n by 1 and we keep
     * iterating with the post-insertion indices. */
    for (guint i = 0; i < F->vertices->len && hit_count < G_N_ELEMENTS (hits); i++) {
        guint n = F->vertices->len;
        guint j = (i + 1) % n;
        guint vi_id = g_array_index (F->vertices, guint, i);
        guint vj_id = g_array_index (F->vertices, guint, j);
        CreaseVertex *V0 = &g_array_index (cp->vertices, CreaseVertex, vi_id);
        CreaseVertex *V1 = &g_array_index (cp->vertices, CreaseVertex, vj_id);

        double s0 = side_of_line (V0->fx, V0->fy, ax, ay, bx, by);
        double s1 = side_of_line (V1->fx, V1->fy, ax, ay, bx, by);
        gboolean v0_on = fabs (s0) < CR_EPS;

        if (v0_on) {
            if (!hit_already_recorded (hits, hit_count, vi_id)) {
                hits[hit_count].vertex_idx   = vi_id;
                hits[hit_count].was_existing = TRUE;
                hit_count++;
            }
            continue;
        }

        if (fabs (s1) < CR_EPS) {
            /* The endpoint hit will be picked up on the next iteration
             * when v0_on fires for vj_id.  Don't double-record here. */
            continue;
        }

        if ((s0 > 0) == (s1 > 0)) continue;  /* same side, no crossing */

        /* Genuine interior crossing on edge (i). */
        double t = s0 / (s0 - s1);
        double px = V0->fx + t * (V1->fx - V0->fx);
        double py = V0->fy + t * (V1->fy - V0->fy);
        guint  new_v = cp_add_vertex (cp, px, py);
        /* (cp_add_vertex may have invalidated V0/V1 — rebind below if
         * we use them again.  We don't.) */

        guint orig_e = g_array_index (F->edges, guint, i);
        CreaseEdge *orig = &g_array_index (cp->edges, CreaseEdge, orig_e);
        CreaseAssignment orig_assign = orig->assignment;
        guint orig_left  = orig->left;
        guint orig_right = orig->right;
        orig->assignment = CR_DELETED;
        orig = NULL;

        guint sub1 = cp_add_edge (cp, vi_id, new_v, orig_assign);
        guint sub2 = cp_add_edge (cp, new_v, vj_id, orig_assign);
        g_array_index (cp->edges, CreaseEdge, sub1).left  = orig_left;
        g_array_index (cp->edges, CreaseEdge, sub1).right = orig_right;
        g_array_index (cp->edges, CreaseEdge, sub2).left  = orig_left;
        g_array_index (cp->edges, CreaseEdge, sub2).right = orig_right;

        /* Splice into F itself. */
        g_array_index (F->edges, guint, i) = sub1;
        guint pos = i + 1;
        g_array_insert_val (F->edges,    pos, sub2);
        g_array_insert_val (F->vertices, pos, new_v);

        /* And mirror the splice into the face on the other side, if any. */
        split_edge_in_other_face (cp, orig_e, vi_id, vj_id,
                                  new_v, sub1, sub2, fi);

        /* Record the new hit. */
        if (!hit_already_recorded (hits, hit_count, new_v)) {
            hits[hit_count].vertex_idx   = new_v;
            hits[hit_count].was_existing = FALSE;
            hit_count++;
        }
        /* Skip past the freshly-inserted vertex: it sits at i+1, and
         * we'd otherwise revisit it on the next iteration as v0_on. */
        i++;
    }

    if (hit_count != 2) return FALSE;

    guint vA = hits[0].vertex_idx;
    guint vB = hits[1].vertex_idx;
    if (vA == vB) return FALSE;

    /* Find both vertices in F's vertex list. */
    gint iA = -1, iB = -1;
    for (guint k = 0; k < F->vertices->len; k++) {
        guint v = g_array_index (F->vertices, guint, k);
        if (v == vA && iA < 0) { iA = (gint) k; }
        else if (v == vB && iB < 0) { iB = (gint) k; }
    }
    if (iA < 0 || iB < 0) return FALSE;
    if (iA > iB) {
        gint tmp = iA; iA = iB; iB = tmp;
        guint tv = vA; vA = vB; vB = tv;
    }

    /* The new crease, going vA -> vB. */
    guint new_edge = cp_add_edge (cp, vA, vB, assign);

    CreaseFace *A = face_new ();
    A->layer = F->layer;
    for (gint k = iA; k <= iB; k++)
        g_array_append_val (A->vertices,
                            g_array_index (F->vertices, guint, k));
    for (gint k = iA; k < iB; k++)
        g_array_append_val (A->edges,
                            g_array_index (F->edges, guint, k));
    g_array_append_val (A->edges, new_edge);

    CreaseFace *B = face_new ();
    B->layer = F->layer;
    for (guint k = (guint) iB; k < F->vertices->len; k++)
        g_array_append_val (B->vertices,
                            g_array_index (F->vertices, guint, k));
    for (gint k = 0; k <= iA; k++)
        g_array_append_val (B->vertices,
                            g_array_index (F->vertices, guint, k));
    for (guint k = (guint) iB; k < F->edges->len; k++)
        g_array_append_val (B->edges,
                            g_array_index (F->edges, guint, k));
    for (gint k = 0; k < iA; k++)
        g_array_append_val (B->edges,
                            g_array_index (F->edges, guint, k));
    g_array_append_val (B->edges, new_edge);

    face_free (F);
    cp->faces->pdata[fi] = NULL;
    guint A_idx = cp->faces->len; g_ptr_array_add (cp->faces, A);
    guint B_idx = cp->faces->len; g_ptr_array_add (cp->faces, B);

    /* Fix every adjacent edge's left/right reference. */
    for (guint k = 0; k < A->edges->len; k++) {
        guint ei = g_array_index (A->edges, guint, k);
        CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, ei);
        if (ei == new_edge) {
            if (e->left == G_MAXUINT) e->left = A_idx;
            else if (e->right == G_MAXUINT) e->right = A_idx;
        } else {
            if (e->left  == fi) e->left  = A_idx;
            if (e->right == fi) e->right = A_idx;
        }
    }
    for (guint k = 0; k < B->edges->len; k++) {
        guint ei = g_array_index (B->edges, guint, k);
        CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, ei);
        if (ei == new_edge) {
            if (e->left == G_MAXUINT) e->left = B_idx;
            else if (e->right == G_MAXUINT) e->right = B_idx;
        } else {
            if (e->left  == fi) e->left  = B_idx;
            if (e->right == fi) e->right = B_idx;
        }
    }

    return TRUE;
}

gboolean
crease_pattern_add_line (CreasePattern *cp,
                         double ax, double ay,
                         double bx, double by,
                         CreaseAssignment a)
{
    if (!cp) return FALSE;
    if (a == CR_DELETED) return FALSE;
    /* Snapshot face count: face splits append new faces, and we don't
     * want to re-split the freshly-added child faces with the same
     * line. */
    guint orig = cp->faces->len;
    gboolean changed = FALSE;
    for (guint fi = 0; fi < orig; fi++)
        if (cp->faces->pdata[fi] != NULL)
            changed |= process_face (cp, fi, ax, ay, bx, by, a);
    return changed;
}

/* ---------- compute_folded ----------
 *
 * Walk the dual graph of faces from `root_face`, accumulating per-face
 * affine transforms.  Each transform takes a (fx, fy, 0) flat-paper
 * point to its 3D position in the folded sheet.
 *
 * Crossing a crease edge applies a rotation around the edge's 3D axis
 * by the edge's fold_angle.  Boundary edges and deleted edges aren't
 * traversable.
 */

typedef struct {
    double r[9];   /* row-major 3x3 */
    double t[3];
} Tform;

static void
tform_identity (Tform *T)
{
    memset (T->r, 0, sizeof T->r);
    T->r[0] = T->r[4] = T->r[8] = 1.0;
    T->t[0] = T->t[1] = T->t[2] = 0.0;
}

static void
tform_apply (const Tform *T, const double in[3], double out[3])
{
    out[0] = T->r[0]*in[0] + T->r[1]*in[1] + T->r[2]*in[2] + T->t[0];
    out[1] = T->r[3]*in[0] + T->r[4]*in[1] + T->r[5]*in[2] + T->t[1];
    out[2] = T->r[6]*in[0] + T->r[7]*in[1] + T->r[8]*in[2] + T->t[2];
}

static void
mat3_mul (const double A[9], const double B[9], double out[9])
{
    double tmp[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++)
                s += A[i*3 + k] * B[k*3 + j];
            tmp[i*3 + j] = s;
        }
    memcpy (out, tmp, sizeof tmp);
}

static void
mat3_apply (const double R[9], const double v[3], double out[3])
{
    double tmp[3] = {
        R[0]*v[0] + R[1]*v[1] + R[2]*v[2],
        R[3]*v[0] + R[4]*v[1] + R[5]*v[2],
        R[6]*v[0] + R[7]*v[1] + R[8]*v[2],
    };
    memcpy (out, tmp, sizeof tmp);
}

/* Rodrigues rotation around unit axis n by angle theta. */
static void
rotation_axis_angle (double nx, double ny, double nz, double theta,
                     double R[9])
{
    double c = cos (theta), s = sin (theta), C = 1 - c;
    R[0] = c + nx*nx*C;     R[1] = nx*ny*C - nz*s;  R[2] = nx*nz*C + ny*s;
    R[3] = ny*nx*C + nz*s;  R[4] = c + ny*ny*C;     R[5] = ny*nz*C - nx*s;
    R[6] = nz*nx*C - ny*s;  R[7] = nz*ny*C + nx*s;  R[8] = c + nz*nz*C;
}

void
crease_pattern_compute_folded (CreasePattern *cp, guint root_face)
{
    if (!cp || root_face >= cp->faces->len || !cp->faces->pdata[root_face])
        return;

    guint nfaces = cp->faces->len;
    Tform *T = g_new (Tform, nfaces);
    gboolean *visited = g_new0 (gboolean, nfaces);

    tform_identity (&T[root_face]);
    visited[root_face] = TRUE;

    GQueue *q = g_queue_new ();
    g_queue_push_tail (q, GUINT_TO_POINTER (root_face));

    while (!g_queue_is_empty (q)) {
        guint fi = GPOINTER_TO_UINT (g_queue_pop_head (q));
        CreaseFace *F = cp->faces->pdata[fi];
        if (!F) continue;

        for (guint k = 0; k < F->edges->len; k++) {
            guint ei = g_array_index (F->edges, guint, k);
            CreaseEdge *e = &g_array_index (cp->edges, CreaseEdge, ei);
            if (!edge_is_alive (e)) continue;
            if (e->assignment == CR_BOUNDARY) continue;

            guint other = (e->left == fi) ? e->right : e->left;
            if (other == G_MAXUINT || other >= nfaces) continue;
            if (visited[other]) continue;

            /* Find the edge's two endpoints in 3D under T_fi. */
            CreaseVertex *V0 = &g_array_index (cp->vertices, CreaseVertex, e->v0);
            CreaseVertex *V1 = &g_array_index (cp->vertices, CreaseVertex, e->v1);
            double p0_flat[3] = { V0->fx, V0->fy, 0 };
            double p1_flat[3] = { V1->fx, V1->fy, 0 };
            double p0[3], p1[3];
            tform_apply (&T[fi], p0_flat, p0);
            tform_apply (&T[fi], p1_flat, p1);

            double ax = p1[0] - p0[0];
            double ay = p1[1] - p0[1];
            double az = p1[2] - p0[2];
            double len = sqrt (ax*ax + ay*ay + az*az);
            if (len < 1e-12) {
                /* Degenerate edge — copy the parent transform. */
                T[other] = T[fi];
                visited[other] = TRUE;
                g_queue_push_tail (q, GUINT_TO_POINTER (other));
                continue;
            }
            ax /= len; ay /= len; az /= len;

            /* Sign convention: positive `fold_angle` means a valley
             * fold, so the OTHER face should rotate up (+Z) out of the
             * sheet.  Right-hand rule around the axis v0->v1 only puts
             * OTHER up when OTHER's flat centroid sits on the LEFT of
             * the axis direction (positive 2D cross product); on the
             * RIGHT side we negate to keep the convention consistent. */
            CreaseFace *F_other = cp->faces->pdata[other];
            double cx = 0, cy = 0;
            for (guint kk = 0; kk < F_other->vertices->len; kk++) {
                guint vi = g_array_index (F_other->vertices, guint, kk);
                CreaseVertex *vv = &g_array_index (cp->vertices,
                                                   CreaseVertex, vi);
                cx += vv->fx; cy += vv->fy;
            }
            cx /= F_other->vertices->len;
            cy /= F_other->vertices->len;
            double e_dx = V1->fx - V0->fx;
            double e_dy = V1->fy - V0->fy;
            double cross = e_dx * (cy - V0->fy) - e_dy * (cx - V0->fx);
            double angle_sign = (cross >= 0) ? 1.0 : -1.0;

            double R[9];
            rotation_axis_angle (ax, ay, az,
                                 angle_sign * e->fold_angle, R);

            /* T_other = R_axis * T_fi, but the rotation is around p0.
             * Compose: new_R = R * old_R; new_t = R*(old_t - p0) + p0. */
            double new_R[9];
            mat3_mul (R, T[fi].r, new_R);

            double shifted[3] = {
                T[fi].t[0] - p0[0],
                T[fi].t[1] - p0[1],
                T[fi].t[2] - p0[2],
            };
            double rotated_t[3];
            mat3_apply (R, shifted, rotated_t);

            memcpy (T[other].r, new_R, sizeof new_R);
            T[other].t[0] = rotated_t[0] + p0[0];
            T[other].t[1] = rotated_t[1] + p0[1];
            T[other].t[2] = rotated_t[2] + p0[2];

            visited[other] = TRUE;
            g_queue_push_tail (q, GUINT_TO_POINTER (other));
        }
    }

    g_queue_free (q);

    /* Apply each face's transform to its vertices.  A vertex may be
     * shared by multiple faces; the BFS ordering means transforms are
     * consistent up to the fold angles, so any face's transform gives
     * the same answer for shared vertices.  Walk faces in BFS order
     * and take the first transform that covers each vertex. */
    gboolean *vert_done = g_new0 (gboolean, cp->vertices->len);
    for (guint fi = 0; fi < nfaces; fi++) {
        if (!cp->faces->pdata[fi] || !visited[fi]) continue;
        CreaseFace *F = cp->faces->pdata[fi];
        for (guint k = 0; k < F->vertices->len; k++) {
            guint vi = g_array_index (F->vertices, guint, k);
            if (vert_done[vi]) continue;
            CreaseVertex *V = &g_array_index (cp->vertices, CreaseVertex, vi);
            double in[3]  = { V->fx, V->fy, 0 };
            double out[3];
            tform_apply (&T[fi], in, out);
            V->X = out[0]; V->Y = out[1]; V->Z = out[2];
            vert_done[vi] = TRUE;
        }
    }

    g_free (vert_done);
    g_free (visited);
    g_free (T);
}
