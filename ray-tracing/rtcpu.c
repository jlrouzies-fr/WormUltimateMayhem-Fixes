/*
 * rtcpu - small CPU reference ray tracer for captured frames.
 *
 *   rtcpu scene.bin out.bin [shadow_samples] [sun_radius_degrees]
 *
 * scene.bin (written by trace_capture.py):
 *   u32 magic 'RTSC', ntri, width, height
 *   float eye[3], base[3], dx[3], dy[3]   ray through pixel (x, y) = base + x*dx + y*dy
 *   float light[3]                         unit vector towards the sun
 *   ntri * { float v[9]; u32 flags; }      flags: bit 0 visible to the camera, bit 1 casts shadows,
 *                                          bits 8..15 class
 * out.bin: width*height * { float t; float ndotl; float visibility; u32 tri; }  (t < 0: no hit)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int u32;
typedef struct { float x, y, z; } V;
typedef struct { V a, e1, e2; u32 flags; } Tri;
typedef struct { V lo, hi; u32 left, count; u32 mask; } Node;   /* count > 0: leaf, left = first tri */

static Tri *tris;
static u32 *order;
static V *cent;
static Node *nodes;
static u32 nnodes;

static V sub(V a, V b) { V r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static V add(V a, V b) { V r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static V mul(V a, float s) { V r = { a.x * s, a.y * s, a.z * s }; return r; }
static V cross(V a, V b) { V r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }
static float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V norm(V a) { float l = sqrtf(dot(a, a)); return l > 0 ? mul(a, 1 / l) : a; }
static float fmin2(float a, float b) { return a < b ? a : b; }
static float fmax2(float a, float b) { return a > b ? a : b; }
static V vmin(V a, V b) { V r = { fmin2(a.x, b.x), fmin2(a.y, b.y), fmin2(a.z, b.z) }; return r; }
static V vmax(V a, V b) { V r = { fmax2(a.x, b.x), fmax2(a.y, b.y), fmax2(a.z, b.z) }; return r; }
static float axis(V a, int k) { return k == 0 ? a.x : k == 1 ? a.y : a.z; }

static u32 build(u32 first, u32 count)
{
    u32 me = nnodes++, i, mid;
    Node *n = &nodes[me];
    V lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f }, clo = lo, chi = hi, ext;
    u32 mask = 0;
    int k;
    float split;

    for (i = first; i < first + count; i++) {
        Tri *t = &tris[order[i]];
        V b = add(t->a, t->e1), c = add(t->a, t->e2);
        lo = vmin(lo, vmin(t->a, vmin(b, c)));
        hi = vmax(hi, vmax(t->a, vmax(b, c)));
        clo = vmin(clo, cent[order[i]]);
        chi = vmax(chi, cent[order[i]]);
        mask |= t->flags & 3;
    }
    n->lo = lo; n->hi = hi; n->mask = mask;
    if (count <= 4) { n->left = first; n->count = count; return me; }
    ext = sub(chi, clo);
    k = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    split = 0.5f * (axis(clo, k) + axis(chi, k));
    mid = first;
    for (i = first; i < first + count; i++)
        if (axis(cent[order[i]], k) < split) {
            u32 tmp = order[i]; order[i] = order[mid]; order[mid] = tmp;
            mid++;
        }
    if (mid == first || mid == first + count) mid = first + count / 2;
    n->count = 0;
    build(first, mid - first);                      /* left child is me + 1 */
    nodes[me].left = build(mid, first + count - mid);   /* right child index */
    return me;
}

static int hit_box(const Node *n, V o, V inv, float tmax)
{
    float t0 = 0, t1 = tmax, a, b;
    a = (n->lo.x - o.x) * inv.x; b = (n->hi.x - o.x) * inv.x;
    t0 = fmax2(t0, fmin2(a, b)); t1 = fmin2(t1, fmax2(a, b));
    a = (n->lo.y - o.y) * inv.y; b = (n->hi.y - o.y) * inv.y;
    t0 = fmax2(t0, fmin2(a, b)); t1 = fmin2(t1, fmax2(a, b));
    a = (n->lo.z - o.z) * inv.z; b = (n->hi.z - o.z) * inv.z;
    t0 = fmax2(t0, fmin2(a, b)); t1 = fmin2(t1, fmax2(a, b));
    return t0 <= t1;
}

/* Closest hit (any = 0) or any hit (any = 1) among triangles with a flag in `mask`. */
static float trace(V o, V d, float tmax, u32 mask, int any, u32 *tri_out)
{
    u32 stack[64], sp = 0, i;
    V inv = { 1 / d.x, 1 / d.y, 1 / d.z };
    float best = tmax;
    int found = 0;

    stack[sp++] = 0;
    while (sp) {
        const Node *n = &nodes[stack[--sp]];
        if (!(n->mask & mask) || !hit_box(n, o, inv, best)) continue;
        if (n->count) {
            for (i = n->left; i < n->left + n->count; i++) {
                const Tri *t = &tris[order[i]];
                V p, s, q;
                float det, u, v, tt;
                if (!(t->flags & mask)) continue;
                p = cross(d, t->e2);
                det = dot(t->e1, p);
                if (fabsf(det) < 1e-12f) continue;
                s = sub(o, t->a);
                u = dot(s, p) / det;
                if (u < 0 || u > 1) continue;
                q = cross(s, t->e1);
                v = dot(d, q) / det;
                if (v < 0 || u + v > 1) continue;
                tt = dot(t->e2, q) / det;
                if (tt > 1e-4f && tt < best) {
                    best = tt;
                    found = 1;
                    if (tri_out) *tri_out = order[i];
                    if (any) return best;
                }
            }
        } else {
            stack[sp++] = n->left;
            stack[sp++] = (u32)(n - nodes) + 1;
        }
    }
    return found ? best : -1;
}

static u32 rng_state = 12345;
static float rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (rng_state >> 8) * (1.0f / 16777216.0f);
}

int main(int argc, char **argv)
{
    FILE *f;
    u32 hdr[4], ntri, w, h, i, x, y;
    float cam[12], lightv[3];
    int samples = argc > 3 ? atoi(argv[3]) : 16;
    float radius = (argc > 4 ? (float)atof(argv[4]) : 0.5f) * 3.14159265f / 180.0f;
    V eye, base, dx, dy, L, lu, lv;
    struct { float v[9]; u32 flags; } rec;
    struct { float t, ndotl, vis; u32 tri; } out;

    if (argc < 3 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: rtcpu scene.bin out.bin [samples] [radius]\n"); return 1; }
    fread(hdr, 4, 4, f);
    if (hdr[0] != 0x43535452) { fprintf(stderr, "bad scene file\n"); return 1; }
    ntri = hdr[1]; w = hdr[2]; h = hdr[3];
    fread(cam, 4, 12, f);
    fread(lightv, 4, 3, f);
    eye = (V){ cam[0], cam[1], cam[2] }; base = (V){ cam[3], cam[4], cam[5] };
    dx = (V){ cam[6], cam[7], cam[8] }; dy = (V){ cam[9], cam[10], cam[11] };
    L = norm((V){ lightv[0], lightv[1], lightv[2] });
    tris = malloc(sizeof(Tri) * ntri); order = malloc(4 * ntri); cent = malloc(sizeof(V) * ntri);
    nodes = malloc(sizeof(Node) * (2 * ntri + 1));
    for (i = 0; i < ntri; i++) {
        V a, b, c;
        fread(&rec, sizeof rec, 1, f);
        a = (V){ rec.v[0], rec.v[1], rec.v[2] }; b = (V){ rec.v[3], rec.v[4], rec.v[5] }; c = (V){ rec.v[6], rec.v[7], rec.v[8] };
        tris[i].a = a; tris[i].e1 = sub(b, a); tris[i].e2 = sub(c, a); tris[i].flags = rec.flags;
        cent[i] = mul(add(a, add(b, c)), 1.0f / 3);
        order[i] = i;
    }
    fclose(f);
    build(0, ntri);
    fprintf(stderr, "%u triangles, %u BVH nodes, %ux%u, %d shadow samples\n", ntri, nnodes, w, h, samples);

    lu = norm(cross(L, fabsf(L.y) < 0.9f ? (V){ 0, 1, 0 } : (V){ 1, 0, 0 }));
    lv = cross(L, lu);
    f = fopen(argv[2], "wb");
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            V d = norm(sub(add(base, add(mul(dx, (float)x), mul(dy, (float)y))), eye));
            u32 ti = 0;
            out.t = trace(eye, d, 1e30f, 1, 0, &ti);
            out.ndotl = 0; out.vis = 1; out.tri = ti;
            if (out.t > 0) {
                V n = norm(cross(tris[ti].e1, tris[ti].e2)), p = add(eye, mul(d, out.t));
                int s, lit = 0;
                if (dot(n, d) > 0) n = mul(n, -1);          /* face the camera */
                out.ndotl = dot(n, L);
                p = add(p, mul(n, 0.02f));
                for (s = 0; s < samples; s++) {
                    float r = radius * sqrtf(rnd()), a = 6.2831853f * rnd();
                    V sd = samples == 1 ? L : norm(add(L, add(mul(lu, r * cosf(a)), mul(lv, r * sinf(a)))));
                    if (trace(p, sd, 1e30f, 2, 1, NULL) < 0) lit++;
                }
                out.vis = (float)lit / samples;
            }
            fwrite(&out, sizeof out, 1, f);
        }
    fclose(f);
    return 0;
}
