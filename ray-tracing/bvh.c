/* BVH construction for the sun shadow tracer. See rt.h for the node layout. */
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define LEAF_MAX 4
#define BINS 12
#define MAX_DEPTH 56

/* While building: depth-first nodes. Inner: left child is the next node, `a` the right
   child, b == 0. Leaf: `a` the first primitive, b the count. */
struct bnode { float lo[3]; u32 a; float hi[3]; u32 b; };

/* Scratch memory and settings of one builder. Each thread that builds uses its own. */
struct bvh_ctx {
    float *bb;          /* per primitive: lo xyz, hi xyz */
    float *cent;        /* per primitive: centroid xyz */
    u32 *idx;
    u32 cap;
    struct bnode *nodes;
    u32 nn;
    BvhNode *out;
    u32 nout;
    int quality, leaf_max;
    int boxes;          /* building over boxes: every leaf holds exactly one */
};

static struct bvh_ctx default_ctx;

struct bvh_ctx *bvh_ctx_new(void)
{
    return calloc(1, sizeof(struct bvh_ctx));
}

static int reserve(struct bvh_ctx *c, u32 n)
{
    if (n <= c->cap) return 1;
    n += n / 2 + 256;
    free(c->bb); free(c->cent); free(c->idx); free(c->nodes);
    c->bb = malloc((size_t)n * 6 * sizeof(float));
    c->cent = malloc((size_t)n * 3 * sizeof(float));
    c->idx = malloc((size_t)n * sizeof(u32));
    c->nodes = malloc((size_t)n * 2 * sizeof(struct bnode));
    c->cap = (c->bb && c->cent && c->idx && c->nodes) ? n : 0;
    return c->cap != 0;
}

static float half_area(const float *lo, const float *hi)
{
    float x = hi[0] - lo[0], y = hi[1] - lo[1], z = hi[2] - lo[2];
    return x * y + y * z + z * x;
}

static void grow(float *lo, float *hi, const float *blo, const float *bhi)
{
    int k;
    for (k = 0; k < 3; k++) {
        if (blo[k] < lo[k]) lo[k] = blo[k];
        if (bhi[k] > hi[k]) hi[k] = bhi[k];
    }
}

static u32 partition(struct bvh_ctx *c, u32 first, u32 count, int axis, float pos)
{
    u32 i, mid = first;
    for (i = first; i < first + count; i++)
        if (c->cent[c->idx[i] * 3 + axis] < pos) {
            u32 t = c->idx[i]; c->idx[i] = c->idx[mid]; c->idx[mid] = t;
            mid++;
        }
    return mid;
}

static u32 build(struct bvh_ctx *c, u32 first, u32 count, int depth)
{
    u32 me = c->nn++, i, mid;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
    int axis = 0, k;
    float pos = 0;

    for (i = first; i < first + count; i++) {
        const float *b = &c->bb[c->idx[i] * 6], *ce = &c->cent[c->idx[i] * 3];
        grow(lo, hi, b, b + 3);
        grow(clo, chi, ce, ce);
    }
    memcpy(c->nodes[me].lo, lo, sizeof lo);
    memcpy(c->nodes[me].hi, hi, sizeof hi);
    if (count <= (u32)c->leaf_max || depth >= MAX_DEPTH) {
        c->nodes[me].a = first; c->nodes[me].b = count;
        return me;
    }

    mid = first;
    if (c->boxes && depth > 24) {
        mid = first + count / 2;        /* keep the tree shallow whatever the layout */
    } else if (c->quality) {
        float best = 1e30f;
        int best_axis = -1, best_plane = 0;
        for (k = 0; k < 3; k++) {
            float ext = chi[k] - clo[k], scale;
            u32 cnt[BINS];
            float blo[BINS][3], bhi[BINS][3], rlo[3], rhi[3], larea[BINS];
            u32 lcnt[BINS], n;
            int b;
            if (ext <= 1e-12f) continue;
            scale = BINS / ext;
            for (b = 0; b < BINS; b++) {
                cnt[b] = 0;
                blo[b][0] = blo[b][1] = blo[b][2] = 1e30f;
                bhi[b][0] = bhi[b][1] = bhi[b][2] = -1e30f;
            }
            for (i = first; i < first + count; i++) {
                u32 p = c->idx[i];
                b = (int)((c->cent[p * 3 + k] - clo[k]) * scale);
                if (b >= BINS) b = BINS - 1;
                if (b < 0) b = 0;
                cnt[b]++;
                grow(blo[b], bhi[b], &c->bb[p * 6], &c->bb[p * 6 + 3]);
            }
            rlo[0] = rlo[1] = rlo[2] = 1e30f; rhi[0] = rhi[1] = rhi[2] = -1e30f;
            for (b = 0, n = 0; b < BINS - 1; b++) {
                n += cnt[b];
                if (cnt[b]) grow(rlo, rhi, blo[b], bhi[b]);
                lcnt[b] = n;
                larea[b] = n ? half_area(rlo, rhi) : 0;
            }
            rlo[0] = rlo[1] = rlo[2] = 1e30f; rhi[0] = rhi[1] = rhi[2] = -1e30f;
            for (b = BINS - 1, n = 0; b >= 1; b--) {
                float cost;
                n += cnt[b];
                if (cnt[b]) grow(rlo, rhi, blo[b], bhi[b]);
                if (!n || !lcnt[b - 1]) continue;
                cost = larea[b - 1] * lcnt[b - 1] + half_area(rlo, rhi) * n;
                if (cost < best) { best = cost; best_axis = k; best_plane = b; }
            }
        }
        if (best_axis >= 0) {
            float ext = chi[best_axis] - clo[best_axis];
            if (!c->boxes && count <= 8 && best >= half_area(lo, hi) * count) {
                c->nodes[me].a = first; c->nodes[me].b = count;
                return me;
            }
            axis = best_axis;
            pos = clo[axis] + ext * best_plane / BINS;
            mid = partition(c, first, count, axis, pos);
        }
    } else {
        float ex = chi[0] - clo[0], ey = chi[1] - clo[1], ez = chi[2] - clo[2];
        axis = ex > ey ? (ex > ez ? 0 : 2) : (ey > ez ? 1 : 2);
        pos = 0.5f * (clo[axis] + chi[axis]);
        mid = partition(c, first, count, axis, pos);
    }
    if (mid == first || mid == first + count) {
        if (count <= 16 && !c->boxes) {     /* coincident centroids: a leaf is as good as it gets */
            c->nodes[me].a = first; c->nodes[me].b = count;
            return me;
        }
        mid = first + count / 2;
    }
    build(c, first, mid - first, depth + 1);
    i = build(c, mid, first + count - mid, depth + 1);
    c->nodes[me].a = i;
    c->nodes[me].b = 0;
    return me;
}

/* Turn the depth-first nodes into the output layout. Returns the output index of node i
   (which must be an inner node). */
static u32 emit(struct bvh_ctx *c, u32 i)
{
    u32 me = c->nout++, child[2], k;
    child[0] = i + 1; child[1] = c->nodes[i].a;
    for (k = 0; k < 2; k++) {
        const struct bnode *n = &c->nodes[child[k]];
        float *lo = k ? c->out[me].lo1 : c->out[me].lo0, *hi = k ? c->out[me].hi1 : c->out[me].hi0;
        u32 ref, cnt;
        memcpy(lo, n->lo, 12); memcpy(hi, n->hi, 12);
        if (n->b) { ref = n->a; cnt = n->b; }
        else { ref = emit(c, child[k]); cnt = 0; }
        if (k) { c->out[me].c1 = ref; c->out[me].n1 = cnt; } else { c->out[me].c0 = ref; c->out[me].n0 = cnt; }
    }
    return me;
}

static u32 finish(struct bvh_ctx *c, BvhNode *out)
{
    c->out = out; c->nout = 0;
    if (c->nodes[0].b) {            /* everything in one leaf */
        memcpy(out[0].lo0, c->nodes[0].lo, 12); memcpy(out[0].hi0, c->nodes[0].hi, 12);
        out[0].c0 = c->nodes[0].a; out[0].n0 = c->nodes[0].b;
        /* The second child is the same leaf again. It must never be left empty: an empty
           slot reads as "inner node 0", the root itself, and a ray would circle for ever. */
        memcpy(out[0].lo1, c->nodes[0].lo, 12); memcpy(out[0].hi1, c->nodes[0].hi, 12);
        out[0].c1 = out[0].c0; out[0].n1 = out[0].n0;
        return 1;
    }
    emit(c, 0);
    return c->nout;
}

void bvh_bounds(const BvhNode *root, float *lo, float *hi)
{
    int k;
    for (k = 0; k < 3; k++) {
        lo[k] = root->lo0[k] < root->lo1[k] ? root->lo0[k] : root->lo1[k];
        hi[k] = root->hi0[k] > root->hi1[k] ? root->hi0[k] : root->hi1[k];
    }
}

u32 bvh_build(struct bvh_ctx *c, const float *tri9, const u32 *uv3, u32 n, int quality, BvhNode *nodes, BvhTri *tris)
{
    u32 i;
    int k;
    if (!c) c = &default_ctx;
    if (!n || !reserve(c, n)) return 0;
    for (i = 0; i < n; i++) {
        const float *t = tri9 + (size_t)i * 9;
        float *b = &c->bb[i * 6];
        for (k = 0; k < 3; k++) {
            float a = t[k], e = t[3 + k], d = t[6 + k];
            float mn = a < e ? a : e, mx = a > e ? a : e;
            b[k] = mn < d ? mn : d;
            b[3 + k] = mx > d ? mx : d;
            c->cent[i * 3 + k] = (a + e + d) * (1.0f / 3);
        }
        c->idx[i] = i;
    }
    c->nn = 0; c->quality = quality; c->leaf_max = LEAF_MAX; c->boxes = 0;
    build(c, 0, n, 0);
    for (i = 0; i < n; i++) {
        const float *t = tri9 + (size_t)c->idx[i] * 9;
        const u32 *uv = uv3 ? uv3 + (size_t)c->idx[i] * 3 : NULL;
        BvhTri *o = &tris[i];
        for (k = 0; k < 3; k++) {
            o->v0[k] = t[k];
            o->e1[k] = t[3 + k] - t[k];
            o->e2[k] = t[6 + k] - t[k];
        }
        o->uv0 = uv ? uv[0] : 0; o->uv1 = uv ? uv[1] : 0; o->uv2 = uv ? uv[2] : 0;
    }
    return finish(c, nodes);
}

u32 bvh_build_boxes(const float *box6, u32 n, BvhNode *nodes)
{
    struct bvh_ctx *c = &default_ctx;
    u32 i;
    int k;
    if (!n || !reserve(c, n)) return 0;
    for (i = 0; i < n; i++) {
        memcpy(&c->bb[i * 6], box6 + (size_t)i * 6, 6 * sizeof(float));
        for (k = 0; k < 3; k++)
            c->cent[i * 3 + k] = 0.5f * (box6[(size_t)i * 6 + k] + box6[(size_t)i * 6 + 3 + k]);
        c->idx[i] = i;
    }
    c->nn = 0; c->quality = 1; c->leaf_max = 1; c->boxes = 1;
    build(c, 0, n, 0);
    for (i = 0; i < c->nn; i++)
        if (c->nodes[i].b) c->nodes[i].a = c->idx[c->nodes[i].a];    /* leaf: the box index itself */
    return finish(c, nodes);
}

const u32 *bvh_order(struct bvh_ctx *c)
{
    return (c ? c : &default_ctx)->idx;
}

void bvh_refit(BvhNode *nodes, u32 nnodes, const BvhTri *tris)
{
    u32 i, j;
    int k, a;
    for (i = nnodes; i-- > 0;)          /* children come after their parent: go backwards */
        for (k = 0; k < 2; k++) {
            u32 c = k ? nodes[i].c1 : nodes[i].c0, n = k ? nodes[i].n1 : nodes[i].n0;
            float *lo = k ? nodes[i].lo1 : nodes[i].lo0, *hi = k ? nodes[i].hi1 : nodes[i].hi0;
            if (n) {
                lo[0] = lo[1] = lo[2] = 1e30f; hi[0] = hi[1] = hi[2] = -1e30f;
                for (j = c; j < c + n; j++)
                    for (a = 0; a < 3; a++) {
                        float p0 = tris[j].v0[a], p1 = p0 + tris[j].e1[a], p2 = p0 + tris[j].e2[a];
                        float mn = p0 < p1 ? p0 : p1, mx = p0 > p1 ? p0 : p1;
                        if (p2 < mn) mn = p2;
                        if (p2 > mx) mx = p2;
                        if (mn < lo[a]) lo[a] = mn;
                        if (mx > hi[a]) hi[a] = mx;
                    }
            } else if (c) {
                bvh_bounds(&nodes[c], lo, hi);
            }
        }
}
