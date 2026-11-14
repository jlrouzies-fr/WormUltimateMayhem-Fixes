/* round.c - rounded shapes (README "Rounded shapes"). Included by wumrt.c, whose GL state
   tracking (gs) and real_* entry points it uses.

   The game's worms and props are low-polygon meshes shaded with smooth vertex normals, so
   they look round inside but have polygonal outlines. With `round = 1` every sun-lit mesh
   draw that is curved enough is replaced by a finer one: each triangle becomes a cubic
   Bezier patch through its three corners that follows their normals (PN triangles, Vlachos
   et al. 2001), cut into level x level small triangles. Edges are treated so that the
   surface has no cracks:
   - an edge whose two triangles share the corner normals (a smooth edge) bends along them;
   - an edge where the normals differ (a crease: the rim of a barrel, the edge of a crate)
     follows the crease line, the direction both tangent planes share (cross product of the
     two normals), so a round rim stays round and a straight crate edge stays straight;
   - an edge with one triangle (a boundary) or more than two stays straight.
   Each edge's control points depend only on that edge's two corners and the normals on both
   sides, computed the same way from either triangle, so neighbours agree bit for bit.
   Meshes whose control points all lie on their flat triangles (crates, walls) are drawn as
   they are. Normals of the new vertices are interpolated from the corners (kept as the
   game made them, creases included); texture coordinates and colours too.

   The tracer is handed the same rounded mesh for the mesh's shadow-pass draw (round_casters),
   so shadows, sky light and the mask's eye-distance test agree with what is drawn. The
   shadow pass binds no normals, so a mesh is rounded there only once its main-pass draw has
   been seen (the normals, texture coordinates and texture are remembered per mesh).

   Vertex data in the engine's buffers is read from the CPU mirror (rt_buffer_data); data in
   plain memory (the worms, skinned on the CPU every frame) is hashed to notice changes. */

static int round_on;                    /* round: 0 = off (the default) */
static int round_level = 3;             /* round_level: small triangles along each edge (2..6) */
static int round_casters = 1;           /* round_casters: the tracer traces the rounded mesh too: 0 never, 1 static meshes
                                           (props), 2 also animated ones (worms: rounded again in the shadow
                                           pass when they moved, and rebuilt by the tracer every frame) */
static float round_strength = 1.0f;     /* round_strength: 1 = full PN bulge, 0 = flat */
static u32 round_max_tris = 12000;      /* round_max_tris: bigger meshes are left alone */
static float round_min_bend = 0.004f;
static float round_dense = 0.06f;       /* round_dense: meshes bending less than this share of an edge get 2 steps */
static int rnd_logged;
static float round_min_pixels = 10.0f;  /* round_min_pixels: meshes smaller than this radius on screen are drawn as they are */
static u32 rnd_small;                   /* draws left alone this frame because they were small */
static float round_full_pixels = 40.0f; /* round_full_pixels: animated meshes smaller than this radius on screen get 2 steps */
static float round_caster_pixels = 40.0f; /* round_caster_pixels: the shadow pass rounds a moving worm part only from this
                                           radius on screen (below it the fringe it removes is under a pixel) */
static u32 round_caster_steps = 2;     /* round_caster_steps: steps per edge of the shadow pass's own rounding (2 = 4x the
                                           triangles; the main pass's 3 only moves the outline by about 1/7 more of the bulge) */
static u32 rnd_cap_n = 6;               /* most steps for the draw being made (main pass) */   /* a mesh whose control points stray less than this share of an edge's length is flat */

#define RND_MESHES 256
#define RND_NONE 0xffffffffu

struct rnd_src { int on; u32 buf; GLint size; GLenum type; u32 stride; const void *ptr; };

struct rnd_mesh {
    u64 id;                     /* where its vertices and indices come from (as mesh_identity in rt_core) */
    u32 used_frame;
    /* topology, rebuilt when the indices change */
    u64 topo_key;
    u32 ntri, maxv;
    u32 *corner;                /* 3 per triangle: source vertex indices (triangle list, game's winding) */
    u32 *pid;                   /* 3 per triangle: welded position id of each corner */
    u32 *across;                /* 6 per triangle: for edge k (corner k -> k+1) the neighbour's vertex at our
                                   corner k and at corner k+1, RND_NONE if the edge has no single neighbour */
    /* what the main pass draws it with */
    int has_main;
    struct rnd_src n, tc, col;
    u32 tex;
    float texxf[16]; int has_xf;
    /* the rounded mesh */
    u64 data_key; int level, flat, ok, dynamic;
    int has_tc, has_col;            /* the rounded mesh has texture coordinates / colours */
    u32 nv, ni, cap_v, cap_n, cap_t, cap_c, idx_level, idx_ntri;
    float *pos, *nrm, *tco;     /* nv x 3, nv x 3, nv x 2 */
    u8 *colo;                   /* nv x 4 */
    u32 *idx;                   /* = idxs[level] */
    u32 *idxs[7], nis[7];
    /* static meshes: the rounded mesh in our own GL buffers (pos, nrm, tc, col one after the other) */
    GLuint vbo, ibo; u64 vbo_key; HGLRC vbo_ctx; u32 off_n, off_t, off_c;
    int borrowed;
    u32 auto_n;                 /* most steps per edge this mesh needs (0 = not decided yet) */               /* pos/nrm/tco/colo belong to a slot of rnd_dyn (animated meshes) */
};
static struct rnd_mesh rnd_meshes[RND_MESHES];

/* Animated meshes: the game skins every worm part on the CPU into one shared scratch array
   and draws it, so one mesh (same pointers, same indices) comes by once per worm with other
   contents, in the shadow pass and again in the main pass. Rounded results are kept per
   content (positions hash) here; the main pass makes them, the shadow pass (whose normals in
   that array are zero) only uses one the main pass made from the same positions. */
#define RND_DYN 160
struct rnd_dyn { u64 key, owner; u32 frame, nv, cap_v, cap_n, cap_t, cap_c; float *pos, *nrm, *tco; u8 *colo;
                 int ok, has_tc, has_col, level, flat; };
static struct rnd_dyn rnd_dyn[RND_DYN];

static void rnd_borrow(struct rnd_mesh *m, const struct rnd_dyn *s)
{
    if (!m->borrowed) { free(m->pos); free(m->nrm); free(m->tco); free(m->colo); }
    m->pos = s->pos; m->nrm = s->nrm; m->tco = s->tco; m->colo = s->colo;
    m->cap_v = s->cap_v; m->cap_n = s->cap_n; m->cap_t = s->cap_t; m->cap_c = s->cap_c;
    m->ok = s->ok; m->has_tc = s->has_tc; m->has_col = s->has_col; m->level = s->level; m->flat = s->flat;
    m->borrowed = 1;
}

static void rnd_unborrow(struct rnd_mesh *m)
{
    if (m->borrowed) { m->pos = m->nrm = m->tco = NULL; m->colo = NULL; m->cap_v = m->cap_n = m->cap_t = m->cap_c = 0; m->borrowed = 0; }
}
static void (APIENTRY *rnd_GenBuffers)(GLsizei, GLuint *);
static void (APIENTRY *rnd_DeleteBuffers)(GLsizei, const GLuint *);
static void (APIENTRY *rnd_BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
static void (APIENTRY *rnd_BufferSubData)(GLenum, ptrdiff_t, ptrdiff_t, const void *);

static void rnd_drop_buffers(struct rnd_mesh *m)
{
    if ((m->vbo || m->ibo) && rnd_DeleteBuffers && m->vbo_ctx == wglGetCurrentContext()) {
        GLuint b[2] = { m->vbo, m->ibo };
        rnd_DeleteBuffers(2, b);
    }
    m->vbo = m->ibo = 0; m->vbo_key = 0;
}
static u32 rnd_frame;
static double rnd_cpu_us;       /* time spent rounding this frame */
static u32 rnd_draws, rnd_tris_in, rnd_tris_out, rnd_made, rnd_casters, rnd_uploads;
static u32 rnd_last_log;
static float *rnd_ctrl;         /* scratch: per triangle 7 control points (6 edge + centre) x 3 */
static u32 rnd_ctrl_cap;
static u32 *rnd_hash_tab; static u32 rnd_hash_cap;
static float *rnd_nn; static u32 rnd_nn_cap;          /* the source normals, normalised, 3 per vertex */
static float *rnd_pidn; static u32 rnd_pidn_cap;      /* per position: count, then up to 4 different normals */
static float *rnd_gn; static u32 rnd_gn_cap;          /* normals worked out from the positions (shadow pass of worms) */
static float rnd_cam[3], rnd_cam_k; static u32 rnd_cam_frame = 0xffffffffu;   /* the main pass's eye and pixel scale */
static u32 rnd_caster_cap;              /* shadow pass of an animated mesh: most steps, 0 = leave it */
static u32 rnd_casters_own;             /* animated casters rounded in the shadow pass itself this frame */
static int rnd_geo_normals(struct rnd_mesh *m, const u8 *v, u32 vs);

static u64 rnd_mix(u64 h, u64 v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 33);
}

static u64 rnd_hash(u64 h, const void *data, u32 n)
{
    const u8 *p = data;
    u32 i;
    for (i = 0; i + 8 <= n; i += 8) {
        u64 v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x9FB21C651E98DF25ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return rnd_mix(h, n);
}

static u64 rnd_identity(u32 vbuf, const void *vptr, u32 ebuf, const void *indices, u32 count, u32 first, GLenum mode)
{
    u64 h = 0x6A09E667F3BCC908ull;
    h = rnd_mix(rnd_mix(rnd_mix(h, vbuf), (u32)vptr), ebuf);
    h = rnd_mix(rnd_mix(rnd_mix(rnd_mix(h, (u32)indices), count), first), mode);
    return h ? h : 1;
}

static struct rnd_mesh *rnd_find(u64 id, int insert)
{
    u32 i = (u32)(id >> 24) & (RND_MESHES - 1), n, victim = i;
    for (n = 0; n < 8; n++) {
        struct rnd_mesh *m = &rnd_meshes[(i + n) & (RND_MESHES - 1)];
        if (m->id == id) return m;
        if (!m->id || m->used_frame < rnd_meshes[victim].used_frame) victim = (i + n) & (RND_MESHES - 1);
    }
    if (!insert) return NULL;
    {
        struct rnd_mesh *m = &rnd_meshes[victim];
        rnd_drop_buffers(m);
        rnd_unborrow(m);
        free(m->corner); free(m->pid); free(m->across);
        free(m->pos); free(m->nrm); free(m->tco); free(m->colo);
        { int q; for (q = 0; q < 7; q++) free(m->idxs[q]); }
        memset(m, 0, sizeof *m);
        m->id = id;
        return m;
    }
}

static void rnd_free_all(void)
{
    u32 i;
    for (i = 0; i < RND_MESHES; i++) {
        struct rnd_mesh *m = &rnd_meshes[i];
        rnd_drop_buffers(m);
        rnd_unborrow(m);
        free(m->corner); free(m->pid); free(m->across);
        free(m->pos); free(m->nrm); free(m->tco); free(m->colo);
        { int q; for (q = 0; q < 7; q++) free(m->idxs[q]); }
        memset(m, 0, sizeof *m);
    }
    for (i = 0; i < RND_DYN; i++) {
        struct rnd_dyn *s = &rnd_dyn[i];
        free(s->pos); free(s->nrm); free(s->tco); free(s->colo);
        memset(s, 0, sizeof *s);
    }
    free(rnd_ctrl); rnd_ctrl = NULL; rnd_ctrl_cap = 0;
    free(rnd_hash_tab); rnd_hash_tab = NULL; rnd_hash_cap = 0;
    free(rnd_nn); rnd_nn = NULL; rnd_nn_cap = 0;
    free(rnd_pidn); rnd_pidn = NULL; rnd_pidn_cap = 0;
    free(rnd_gn); rnd_gn = NULL; rnd_gn_cap = 0;
}

/* Where a source array's bytes are: the engine's buffer (CPU mirror) or plain memory.
   avail: bytes readable from the returned pointer. */
static const u8 *rnd_data(u32 buf, const void *ptr, u32 *avail, u32 *version)
{
    if (buf) {
        u32 size = 0, ver = 0;
        const u8 *b = rt_buffer_data(buf, &size, &ver);
        if (!b || (u32)ptr >= size) return NULL;
        *avail = size - (u32)ptr;
        if (version) *version = ver;
        return b + (u32)ptr;
    }
    *avail = 0xffffffffu;
    if (version) *version = 0;
    return ptr;
}

static u32 rnd_index(const u8 *idata, u32 isz, u32 i)
{
    return isz == 2 ? ((const unsigned short *)idata)[i] : isz == 4 ? ((const u32 *)idata)[i] : idata[i];
}

/* ---- topology: triangle list, welded corners, neighbour across each edge ---------------- */
static float rnd_weld_inv;      /* grid cells per world unit for welding */
static void rnd_cell(const float *p, int *g)
{
    int k;
    for (k = 0; k < 3; k++) g[k] = (int)floorf(p[k] * rnd_weld_inv + 0.5f);
}
static int rnd_topology(struct rnd_mesh *m, GLenum mode, u32 first, u32 count, const u8 *idata, u32 isz,
                        const u8 *vdata, u32 vstride)
{
    u32 maxtri = mode == GL_TRIANGLES ? count / 3 : mode == GL_QUADS ? count / 2 : count - 2;
    u32 i, t = 0, maxv = 0, cap, mask, nv;
    u32 *c, *link;
    free(m->corner); free(m->pid); free(m->across);
    m->corner = malloc((size_t)maxtri * 3 * sizeof(u32));
    m->pid = malloc((size_t)maxtri * 3 * sizeof(u32));
    m->across = malloc((size_t)maxtri * 6 * sizeof(u32));
    m->ntri = 0;
    if (!m->corner || !m->pid || !m->across) return 0;
    c = m->corner;
#define RIDX(k) (idata ? rnd_index(idata, isz, (k)) : first + (k))
#define RTRI(a, b, cc) do { u32 ia = (a), ib = (b), ic = (cc); \
        if (ia != ib && ib != ic && ia != ic) { c[0] = ia; c[1] = ib; c[2] = ic; c += 3; t++; \
            if (ia > maxv) maxv = ia; if (ib > maxv) maxv = ib; if (ic > maxv) maxv = ic; } } while (0)
    if (mode == GL_TRIANGLES) for (i = 0; i + 2 < count; i += 3) RTRI(RIDX(i), RIDX(i + 1), RIDX(i + 2));
    else if (mode == GL_TRIANGLE_STRIP)
        for (i = 0; i + 2 < count; i++) { if (i & 1) RTRI(RIDX(i + 1), RIDX(i), RIDX(i + 2)); else RTRI(RIDX(i), RIDX(i + 1), RIDX(i + 2)); }
    else if (mode == GL_TRIANGLE_FAN) for (i = 1; i + 1 < count; i++) RTRI(RIDX(0), RIDX(i), RIDX(i + 1));
    else for (i = 0; i + 3 < count; i += 4) { RTRI(RIDX(i), RIDX(i + 1), RIDX(i + 2)); RTRI(RIDX(i), RIDX(i + 2), RIDX(i + 3)); }
#undef RIDX
#undef RTRI
    m->ntri = t;
    m->maxv = maxv;
    if (!t) return 0;

    /* Weld corners by position, to a grid of 1/1000 of the mesh's size: split vertices (creases,
       texture seams) share a pid, and so do the corners of faces that were modelled apart. */
    nv = maxv + 1;
    {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, ext = 0;
        int k;
        for (i = 0; i < t * 3; i++) {
            const float *p = (const float *)(vdata + (size_t)m->corner[i] * vstride);
            for (k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
        }
        for (k = 0; k < 3; k++) if (hi[k] - lo[k] > ext) ext = hi[k] - lo[k];
        rnd_weld_inv = ext > 0 ? 1000.0f / ext : 1.0f;
    }
    for (cap = 64; cap < nv * 2; cap <<= 1) ;
    for (; cap < t * 6 * 2; cap <<= 1) ;          /* also big enough for the edge table below */
    if (cap > rnd_hash_cap) {
        free(rnd_hash_tab);
        rnd_hash_tab = malloc((size_t)cap * 2 * sizeof(u32));
        rnd_hash_cap = rnd_hash_tab ? cap : 0;
        if (!rnd_hash_tab) return 0;
    }
    mask = cap - 1;
    {
        u32 *vpid = malloc((size_t)nv * sizeof(u32));
        if (!vpid) return 0;
        for (i = 0; i < nv; i++) vpid[i] = RND_NONE;
        memset(rnd_hash_tab, 0xff, (size_t)cap * 2 * sizeof(u32));
        for (i = 0; i < t * 3; i++) {
            u32 v = m->corner[i], h, k;
            if (vpid[v] == RND_NONE) {
                int g[3], go[3];
                rnd_cell((const float *)(vdata + (size_t)v * vstride), g);
                h = (u32)rnd_hash(0x1234, g, 12);
                for (k = h & mask;; k = (k + 1) & mask) {
                    u32 o = rnd_hash_tab[k * 2];
                    if (o == RND_NONE) { rnd_hash_tab[k * 2] = v; vpid[v] = v; break; }
                    rnd_cell((const float *)(vdata + (size_t)o * vstride), go);
                    if (g[0] == go[0] && g[1] == go[1] && g[2] == go[2]) { vpid[v] = vpid[o]; break; }
                }
            }
            m->pid[i] = vpid[v];
        }
        free(vpid);
    }

    /* Edges: (pid lo, pid hi) -> first triangle edge; a second one links both; a third marks
       the edge as shared by too many (left straight). */
    link = malloc((size_t)t * 3 * sizeof(u32));
    if (!link) return 0;
    for (i = 0; i < t * 3; i++) link[i] = RND_NONE;
    memset(rnd_hash_tab, 0xff, (size_t)cap * 2 * sizeof(u32));
    for (i = 0; i < t * 3; i++) {
        u32 tri = i / 3, k = i % 3, a = m->pid[i], b = m->pid[tri * 3 + (k + 1) % 3];
        u32 lo = a < b ? a : b, hi = a < b ? b : a, h = (u32)rnd_mix(lo, hi), s;
        if (a == b) continue;
        for (s = h & mask;; s = (s + 1) & mask) {
            u32 o = rnd_hash_tab[s * 2];
            if (o == RND_NONE) { rnd_hash_tab[s * 2] = i; rnd_hash_tab[s * 2 + 1] = RND_NONE; break; }
            {
                u32 ot = o / 3, ok = o % 3, oa = m->pid[o], ob = m->pid[ot * 3 + (ok + 1) % 3];
                u32 olo = oa < ob ? oa : ob, ohi = oa < ob ? ob : oa;
                if (olo != lo || ohi != hi) continue;
                if (rnd_hash_tab[s * 2 + 1] == RND_NONE && link[o] == RND_NONE) {
                    link[o] = i; link[i] = o; rnd_hash_tab[s * 2 + 1] = i;
                } else {
                    if (link[o] != RND_NONE && link[o] != 0xfffffffeu) link[link[o]] = 0xfffffffeu;
                    link[o] = 0xfffffffeu; link[i] = 0xfffffffeu;
                }
                break;
            }
        }
    }
    for (i = 0; i < t * 3; i++) {
        u32 tri = i / 3, k = i % 3, o = link[i];
        u32 *ac = m->across + (size_t)tri * 6 + k * 2;
        if (o >= 0xfffffffeu) { ac[0] = ac[1] = RND_NONE; continue; }
        {
            u32 ot = o / 3, ok = o % 3, j;
            u32 pa = m->pid[i], pb = m->pid[tri * 3 + (k + 1) % 3];
            ac[0] = ac[1] = RND_NONE;
            for (j = 0; j < 3; j++) {
                (void)ok;
                if (m->pid[ot * 3 + j] == pa) ac[0] = m->corner[ot * 3 + j];
                if (m->pid[ot * 3 + j] == pb) ac[1] = m->corner[ot * 3 + j];
            }
            if (ac[0] == RND_NONE || ac[1] == RND_NONE) ac[0] = ac[1] = RND_NONE;
        }
    }
    free(link);
    return 1;
}

/* ---- the patch --------------------------------------------------------------------- */
static void rnd_norm(float *v)
{
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 1e-20f) { l = 1.0f / sqrtf(l); v[0] *= l; v[1] *= l; v[2] *= l; }
}

/* Control point of edge a -> b next to a. na: the normal at a on this side, nn: on the other
   side (NULL: no single neighbour). Symmetric in na/nn, so both triangles get the same point. */
static void rnd_ctrl_point(const float *a, const float *na, const float *nn, const float *b, float *out)
{
    float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, q[3];
    float s = round_strength;
    int k;
    for (k = 0; k < 3; k++) q[k] = e[k] / 3.0f;          /* straight */
    if (nn && s > 0) {
        float c = na[0] * nn[0] + na[1] * nn[1] + na[2] * nn[2];
        if (c > 0.985f) {
            /* smooth edge: project onto the tangent plane of the (shared) normal */
            float n[3] = { na[0] + nn[0], na[1] + nn[1], na[2] + nn[2] }, d;
            if (na[0] == nn[0] && na[1] == nn[1] && na[2] == nn[2]) memcpy(n, na, 12);     /* same normal: no work */
            else rnd_norm(n);
            d = e[0] * n[0] + e[1] * n[1] + e[2] * n[2];
            for (k = 0; k < 3; k++) q[k] = (e[k] - d * n[k] * s) / 3.0f;
        } else {
            /* crease: along the line both tangent planes share */
            float t[3] = { na[1] * nn[2] - na[2] * nn[1], na[2] * nn[0] - na[0] * nn[2], na[0] * nn[1] - na[1] * nn[0] };
            float l = t[0] * t[0] + t[1] * t[1] + t[2] * t[2], d, el = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            if (l > 1e-12f) {
                l = 1.0f / sqrtf(l); t[0] *= l; t[1] *= l; t[2] *= l;
                d = e[0] * t[0] + e[1] * t[1] + e[2] * t[2];
                if (d * d > 0.5f * el)      /* the edge runs along the crease (not across it) */
                    for (k = 0; k < 3; k++) q[k] = (e[k] * (1 - s) + t[k] * d * s) / 3.0f;
            }
        }
    }
    for (k = 0; k < 3; k++) out[k] = a[k] + q[k];
}

/* Point m/N along the cubic p0 c0 c1 p1. Called with the edge in a fixed orientation (lower
   pid first), so both triangles of an edge compute the same bits. */
static void rnd_bez(const float *p0, const float *c0, const float *c1, const float *p1, int m, int N, int warp, float *out)
{
    float t = (float)m / (float)N, s = (float)(N - m) / (float)N, w0, w1, w2, w3;
    int k;
    if (warp) { float s2 = s * s, t2 = t * t, q = s2 + t2; s = s2 / q; t = t2 / q; }    /* rows pulled to the edges */
    w0 = s * s * s; w1 = 3 * s * s * t; w2 = 3 * s * t * t; w3 = t * t * t;
    for (k = 0; k < 3; k++) out[k] = p0[k] * w0 + c0[k] * w1 + c1[k] * w2 + p1[k] * w3;
}

static int rnd_grow(void **p, u32 *cap, u32 need, size_t elem)
{
    void *q;
    if (need <= *cap && *p) return 1;
    q = realloc(*p, (size_t)need * elem);
    if (!q) return 0;
    *p = q;
    *cap = need;
    return 1;
}

struct rnd_in {
    const u8 *v; u32 vs;                        /* positions (float x 3) */
    const u8 *n; u32 ns;                        /* normals (float x 3) */
    const u8 *tc; u32 tcs;                      /* texture coordinates (float x 2), or NULL */
    const u8 *col; u32 cols; GLenum col_type; int col_size;   /* colours (ubyte x 4 or float x 3/4), or NULL */
    int still;                                  /* made once and kept (not an animated mesh): bevels allowed */
    u32 max_n;                                  /* most steps per edge for this one */
};

/* Bevels: with round_bevel on, a kept mesh with hard edges (crates, barrel rims, posts) gets a
   narrow band of rounded normals along them, the way a bevelled edge catches the light in
   CG. It is cut 5 x 5 with the rows pulled towards the edges (at 6%, 31%, 69%, 94%); the
   vertices on a hard edge get the mean of the normals on both sides of it, and corners where
   hard edges meet the mean of all the different normals there. Only normals change: the
   outline stays as it was. */
static int round_bevel;                 /* round_bevel: 0 = off */
static float round_bevel_max = 40.0f;   /* round_bevel_max: meshes bigger than this radius (world units) keep sharp edges */

/* Build the rounded mesh into m->pos/nrm/tco/colo/idx. Returns 0 if the mesh is flat (or
   something failed): draw it as it is. */
static int rnd_make(struct rnd_mesh *m, const struct rnd_in *in)
{
    u32 t, nt = m->ntri, N, V, i, j;
    float maxbend = 0, sumbend = 0, lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    u32 nbend = 0, sharp = 0, nbound = 0;
    int bevel, warp;
    float *C;
    double t0 = 0;
    LARGE_INTEGER q0, q1, fq;
    QueryPerformanceCounter(&q0);

    if (!rnd_grow((void **)&rnd_ctrl, &rnd_ctrl_cap, nt * 21, sizeof(float))) return 0;
    C = rnd_ctrl;
    if (!rnd_grow((void **)&rnd_nn, &rnd_nn_cap, (m->maxv + 1) * 3, sizeof(float))) return 0;
    for (i = 0; i <= m->maxv; i++) {
        float *q = rnd_nn + (size_t)i * 3;
        memcpy(q, in->n + (size_t)i * in->ns, 12);
        rnd_norm(q);
    }
#define RNN(v) (rnd_nn + (size_t)(v) * 3)
    /* Control points: per triangle, edge k gives C[k*2] (next to corner k) and C[k*2+1]
       (next to corner k+1); C[6] is the centre. */
    for (t = 0; t < nt; t++) {
        const u32 *cv = m->corner + (size_t)t * 3, *ac = m->across + (size_t)t * 6;
        float p[3][3], n[3][3], E[3] = { 0, 0, 0 }, Vc[3];
        float *ct = C + (size_t)t * 21;
        int k;
        for (k = 0; k < 3; k++) {
            memcpy(p[k], in->v + (size_t)cv[k] * in->vs, 12);
            memcpy(n[k], RNN(cv[k]), 12);
        }
        for (k = 0; k < 3; k++) {
            int b = (k + 1) % 3;
            float nna[3], nnb[3];
            const float *pa = NULL, *pb = NULL;
            float len2, d0, d1, dx;
            int a2;
            if (ac[k * 2] != RND_NONE) {
                memcpy(nna, RNN(ac[k * 2]), 12); pa = nna;
                memcpy(nnb, RNN(ac[k * 2 + 1]), 12); pb = nnb;
                if (n[k][0] * nna[0] + n[k][1] * nna[1] + n[k][2] * nna[2] < 0.9f) sharp++;
            } else nbound++;
            for (a2 = 0; a2 < 3; a2++) { if (p[k][a2] < lo[a2]) lo[a2] = p[k][a2]; if (p[k][a2] > hi[a2]) hi[a2] = p[k][a2]; }
            rnd_ctrl_point(p[k], n[k], pa, p[b], ct + k * 6);
            rnd_ctrl_point(p[b], n[b], pb, p[k], ct + k * 6 + 3);
            /* how far the control points leave the straight edge, relative to its length */
            len2 = (p[b][0] - p[k][0]) * (p[b][0] - p[k][0]) + (p[b][1] - p[k][1]) * (p[b][1] - p[k][1]) +
                   (p[b][2] - p[k][2]) * (p[b][2] - p[k][2]);
            if (len2 > 0) {
                float s0[3], s1[3];
                int a;
                for (a = 0; a < 3; a++) { s0[a] = (2 * p[k][a] + p[b][a]) / 3; s1[a] = (p[k][a] + 2 * p[b][a]) / 3; }
                d0 = (ct[k * 6] - s0[0]) * (ct[k * 6] - s0[0]) + (ct[k * 6 + 1] - s0[1]) * (ct[k * 6 + 1] - s0[1]) +
                     (ct[k * 6 + 2] - s0[2]) * (ct[k * 6 + 2] - s0[2]);
                d1 = (ct[k * 6 + 3] - s1[0]) * (ct[k * 6 + 3] - s1[0]) + (ct[k * 6 + 4] - s1[1]) * (ct[k * 6 + 4] - s1[1]) +
                     (ct[k * 6 + 5] - s1[2]) * (ct[k * 6 + 5] - s1[2]);
                dx = (d0 > d1 ? d0 : d1) / len2;
                if (dx > maxbend) maxbend = dx;
                if (!m->auto_n) { sumbend += sqrtf(dx); nbend++; }
            }
        }
        for (k = 0; k < 18; k++) E[k % 3] += ct[k];
        for (k = 0; k < 3; k++) {
            E[k] /= 6.0f;
            Vc[k] = (p[0][k] + p[1][k] + p[2][k]) / 3.0f;
            ct[18 + k] = E[k] + (E[k] - Vc[k]) * 0.5f;
        }
    }
    m->flat = maxbend < round_min_bend * round_min_bend;
    bevel = round_bevel && in->still && sharp &&
            (hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]) <
            4 * round_bevel_max * round_bevel_max;
    if (round_bevel && in->still && rnd_logged < 300) {
        rnd_logged++;
        logmsg("round: bevel check %08x, %u triangles, %u hard edges, %u boundary edge ends, size %.1f, flat %d -> %s", (u32)m->id, nt, sharp, nbound,
               sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])), m->flat, bevel ? "bevel" : "no");
    }
    if (m->flat && !bevel) return 0;
    warp = bevel;
    if (bevel) {
        /* the different normals at every position */
        u32 np = m->maxv + 1, c;
        if (!rnd_grow((void **)&rnd_pidn, &rnd_pidn_cap, np * 13, sizeof(float))) return 0;
        for (c = 0; c < np; c++) rnd_pidn[c * 13] = 0;
        for (c = 0; c < nt * 3; c++) {
            float nn[3], *e = rnd_pidn + (size_t)m->pid[c] * 13;
            int q, cnt = (int)e[0], dup = 0;
            memcpy(nn, RNN(m->corner[c]), 12);
            for (q = 0; q < cnt; q++) if (nn[0] * e[1 + q * 3] + nn[1] * e[2 + q * 3] + nn[2] * e[3 + q * 3] > 0.985f) dup = 1;
            if (!dup && cnt < 4) { memcpy(e + 1 + cnt * 3, nn, 12); e[0] = (float)(cnt + 1); }
        }
    }

    N = (u32)round_level;
    if (N < 2) N = 2;
    if (N > 6) N = 6;
    /* Meshes that bend little per triangle (already dense) get two steps per edge; decided once per
       mesh so animated ones do not switch back and forth. */
    if (bevel) m->auto_n = 5;
    if (!m->auto_n) {
        float b = nbend ? sumbend / (float)nbend : 0;     /* mean over the edges */
        m->auto_n = b < round_dense ? 2 : 6;
        if (rnd_logged < 300) { rnd_logged++; logmsg("round: mesh %08x, %u triangles, bend %.3f -> %s", (u32)m->id, nt, b, b < round_dense ? "2 steps" : "full level"); }
    }
    if (N > m->auto_n || bevel) N = m->auto_n;
    if (!bevel && in->max_n >= 2 && N > in->max_n) N = in->max_n;
    while (N > 2 && nt * N * N > 150000) N--;
    V = (N + 1) * (N + 2) / 2;
    /* grown, never shrunk: the tracer recognises a mesh by where its arrays are */
    if (!rnd_grow((void **)&m->pos, &m->cap_v, nt * V * 3, sizeof(float))) return 0;
    if (!rnd_grow((void **)&m->nrm, &m->cap_n, nt * V * 3, sizeof(float))) return 0;
    if (in->tc && !rnd_grow((void **)&m->tco, &m->cap_t, nt * V * 2, sizeof(float))) return 0;
    if (in->col && !rnd_grow((void **)&m->colo, &m->cap_c, nt * V * 4, 1)) return 0;
    m->nv = nt * V;

    for (t = 0; t < nt; t++) {
        const u32 *cv = m->corner + (size_t)t * 3, *cp = m->pid + (size_t)t * 3, *ac = m->across + (size_t)t * 6;
        const float *ct = C + (size_t)t * 21;
        float p[3][3], n[3][3], uv[3][2], cl[3][4];
        float *po = m->pos + (size_t)t * V * 3, *no = m->nrm + (size_t)t * V * 3;
        float *to = in->tc ? m->tco + (size_t)t * V * 2 : NULL;
        u8 *co = in->col ? m->colo + (size_t)t * V * 4 : NULL;
        int k;
        for (k = 0; k < 3; k++) {
            memcpy(p[k], in->v + (size_t)cv[k] * in->vs, 12);
            memcpy(n[k], RNN(cv[k]), 12);
            if (in->tc) memcpy(uv[k], in->tc + (size_t)cv[k] * in->tcs, 8);
            if (in->col) {
                const u8 *s = in->col + (size_t)cv[k] * in->cols;
                if (in->col_type == GL_UNSIGNED_BYTE) { int a; for (a = 0; a < 4; a++) cl[k][a] = s[a]; }
                else { float f[4] = { 1, 1, 1, 1 }; int a; memcpy(f, s, (size_t)in->col_size * 4);
                       for (a = 0; a < 4; a++) cl[k][a] = f[a] * 255.0f; }
            }
        }
        for (j = 0; j <= N; j++) {
            for (i = 0; i + j <= N; i++) {
                /* barycentric weights of corners 0, 1, 2 */
                float w1 = (float)i / (float)N, w2 = (float)j / (float)N, w0 = (float)(N - i - j) / (float)N;
                float *P = po, *Nn = no;
                int bev = 0;            /* 1: this vertex got a bevel normal */
                if (warp) { float a0 = w0 * w0, a1 = w1 * w1, a2 = w2 * w2, q = a0 + a1 + a2; w0 = a0 / q; w1 = a1 / q; w2 = a2 / q; }
                if (i == 0 && j == 0) memcpy(P, p[0], 12);
                else if (i == N) memcpy(P, p[1], 12);
                else if (j == N) memcpy(P, p[2], 12);
                else if (j == 0 || i + j == N || i == 0) {
                    /* on an edge: evaluate it in its fixed orientation */
                    int e = j == 0 ? 0 : i + j == N ? 1 : 2;
                    int a = e, b = (e + 1) % 3;
                    int mstep = e == 0 ? (int)i : e == 1 ? (int)j : (int)(N - j);    /* steps from corner a */
                    const float *ca = ct + e * 6, *cb = ct + e * 6 + 3;
                    if (cp[a] < cp[b] || (cp[a] == cp[b] && cv[a] < cv[b])) rnd_bez(p[a], ca, cb, p[b], mstep, (int)N, warp, P);
                    else rnd_bez(p[b], cb, ca, p[a], (int)N - mstep, (int)N, warp, P);
                    if (bevel && ac[e * 2] != RND_NONE) {
                        /* on a hard edge: the mean of the normals on both sides */
                        float oa[3], ob[3], ws = e == 0 ? w0 : e == 1 ? w1 : w2, wt = e == 0 ? w1 : e == 1 ? w2 : w0;
                        int q;
                        memcpy(oa, RNN(ac[e * 2]), 12);
                        memcpy(ob, RNN(ac[e * 2 + 1]), 12);
                        if (n[a][0] * oa[0] + n[a][1] * oa[1] + n[a][2] * oa[2] < 0.985f ||
                            n[b][0] * ob[0] + n[b][1] * ob[1] + n[b][2] * ob[2] < 0.985f) {
                            for (q = 0; q < 3; q++) Nn[q] = (n[a][q] + oa[q]) * ws + (n[b][q] + ob[q]) * wt;
                            rnd_norm(Nn);
                            bev = 1;
                        }
                    }
                } else {
                    float u = w1, v = w2, w = w0;
                    float b300 = w * w * w, b030 = u * u * u, b003 = v * v * v;
                    float b210 = 3 * w * w * u, b120 = 3 * w * u * u, b021 = 3 * u * u * v;
                    float b012 = 3 * u * v * v, b102 = 3 * w * v * v, b201 = 3 * w * w * v, b111 = 6 * u * v * w;
                    int a;
                    for (a = 0; a < 3; a++)
                        P[a] = p[0][a] * b300 + p[1][a] * b030 + p[2][a] * b003 +
                               ct[0 + a] * b210 + ct[3 + a] * b120 +            /* edge 0: corner 0 -> 1 */
                               ct[6 + a] * b021 + ct[9 + a] * b012 +            /* edge 1: corner 1 -> 2 */
                               ct[12 + a] * b102 + ct[15 + a] * b201 +          /* edge 2: corner 2 -> 0 */
                               ct[18 + a] * b111;
                }
                if (bevel && ((i == 0 && j == 0) || i == N || j == N)) {
                    /* a corner where hard edges meet: the mean of the different normals there */
                    const float *e = rnd_pidn + (size_t)cp[i == N ? 1 : j == N ? 2 : 0] * 13;
                    if (e[0] >= 2) {
                        int q, a;
                        Nn[0] = Nn[1] = Nn[2] = 0;
                        for (q = 0; q < (int)e[0]; q++) for (a = 0; a < 3; a++) Nn[a] += e[1 + q * 3 + a];
                        rnd_norm(Nn);
                        bev = 1;
                    }
                }
                if (!bev) {
                    int a;
                    for (a = 0; a < 3; a++) Nn[a] = n[0][a] * w0 + n[1][a] * w1 + n[2][a] * w2;
                    rnd_norm(Nn);
                }
                if (to) { to[0] = uv[0][0] * w0 + uv[1][0] * w1 + uv[2][0] * w2; to[1] = uv[0][1] * w0 + uv[1][1] * w1 + uv[2][1] * w2; to += 2; }
                if (co) { int a; for (a = 0; a < 4; a++) co[a] = (u8)(cl[0][a] * w0 + cl[1][a] * w1 + cl[2][a] * w2 + 0.5f); co += 4; }
                po += 3; no += 3;
            }
        }
    }

    /* Indices: the same pattern in every triangle; kept per level (worms near and far use
       different levels), made again only when the triangle count changes. */
    if (m->idx_ntri != nt) {
        u32 q;
        for (q = 0; q < 7; q++) { free(m->idxs[q]); m->idxs[q] = NULL; }
        m->idx_ntri = nt;
    }
    if (!m->idxs[N]) {
        u32 per = N * N * 3, *o;
        m->idxs[N] = malloc((size_t)nt * per * sizeof(u32));
        if (!m->idxs[N]) { m->idx = NULL; return 0; }
        o = m->idxs[N];
        for (t = 0; t < nt; t++) {
            u32 base = t * V;
#define RV(ii, jj) (base + (jj) * (N + 1) - (jj) * ((jj) - 1) / 2 + (ii))
            for (j = 0; j < N; j++)
                for (i = 0; i + j < N; i++) {
                    o[0] = RV(i, j); o[1] = RV(i + 1, j); o[2] = RV(i, j + 1); o += 3;
                    if (i + j + 1 < N) { o[0] = RV(i + 1, j); o[1] = RV(i + 1, j + 1); o[2] = RV(i, j + 1); o += 3; }
                }
#undef RV
        }
        m->nis[N] = (u32)(o - m->idxs[N]);
    }
    m->idx = m->idxs[N]; m->ni = m->nis[N]; m->idx_level = N;
#undef RNN
    m->level = (int)N;
    rnd_made++;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&fq);
    t0 = (double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)fq.QuadPart;
    rnd_cpu_us += t0;
    return 1;
}

/* Bring m's rounded mesh up to date with the data a draw points at. pos: the draw's
   positions. Returns 1 if there is a rounded mesh to use. */
static int rnd_update(struct rnd_mesh *m, const struct rt_draw *d, int caster_pass)
{
    u32 isz = 0, iavail = 0, vavail = 0, navail = 0, tavail = 0, cavail = 0, vver = 0, nver = 0, tver = 0, cver = 0, iver = 0;
    const u8 *idata = NULL, *vdata, *ndata, *tdata = NULL, *cdata = NULL;
    u32 vs = d->v_stride ? d->v_stride : d->v_size * 4;
    u32 ns = m->n.stride ? m->n.stride : 12;
    u32 ts = m->tc.stride ? m->tc.stride : (u32)m->tc.size * 4;
    u32 cs = m->col.stride ? m->col.stride : (u32)m->col.size * (m->col.type == GL_UNSIGNED_BYTE ? 1 : 4);
    u64 tkey, dkey;
    int dynamic = 0;
    struct rnd_in in;
    LARGE_INTEGER q0, q1, fq;

    if (!m->has_main) return 0;
    QueryPerformanceCounter(&q0);
    vdata = rnd_data(d->v_buf, d->v_ptr, &vavail, &vver);
    ndata = rnd_data(m->n.buf, m->n.ptr, &navail, &nver);
    if (!vdata || !ndata) return 0;
    if (m->tc.on) tdata = rnd_data(m->tc.buf, m->tc.ptr, &tavail, &tver);
    if (m->col.on) cdata = rnd_data(m->col.buf, m->col.ptr, &cavail, &cver);
    if (!d->arrays) {
        isz = d->itype == GL_UNSIGNED_SHORT ? 2 : d->itype == GL_UNSIGNED_INT ? 4 : d->itype == GL_UNSIGNED_BYTE ? 1 : 0;
        if (!isz) return 0;
        idata = rnd_data(d->elem_buf, d->indices, &iavail, &iver);
        if (!idata || (u64)d->count * isz > iavail) return 0;
        if (!d->elem_buf && IsBadReadPtr(idata, d->count * isz)) return 0;
    }
    /* topology key */
    tkey = rnd_mix(rnd_mix(rnd_mix(0x7154, d->mode), d->count), d->first);
    if (!d->arrays) {
        if (d->elem_buf) tkey = rnd_mix(rnd_mix(rnd_mix(tkey, d->elem_buf), iver), (u32)d->indices);
        else tkey = rnd_hash(tkey, idata, d->count * isz);
    }
    if (tkey != m->topo_key || !m->corner) {
        u32 i, maxv = 0;
        if (d->arrays) maxv = d->first + d->count - 1;
        else for (i = 0; i < d->count; i++) { u32 x = rnd_index(idata, isz, i); if (x > maxv) maxv = x; }
        if ((u64)maxv * vs + 12 > vavail || (!d->v_buf && IsBadReadPtr(vdata, maxv * vs + 12))) return 0;
        m->topo_key = 0;
        m->data_key = 0;
        m->auto_n = 0;
        if (!rnd_topology(m, d->mode, d->first, d->count, idata, isz, vdata, vs)) return 0;
        m->topo_key = tkey;
    }
    /* are all sources readable up to the highest vertex? */
    if ((u64)m->maxv * vs + 12 > vavail || (u64)m->maxv * ns + 12 > navail) return 0;
    if (tdata && (u64)m->maxv * ts + 8 > tavail) tdata = NULL;
    if (cdata && (u64)m->maxv * cs + 4 > cavail) cdata = NULL;
    if (!d->v_buf && IsBadReadPtr(vdata, m->maxv * vs + 12)) return 0;
    if (!m->n.buf && IsBadReadPtr(ndata, m->maxv * ns + 12)) return 0;
    if (tdata && !m->tc.buf && IsBadReadPtr(tdata, m->maxv * ts + 8)) tdata = NULL;
    if (cdata && !m->col.buf && IsBadReadPtr(cdata, m->maxv * cs + 4)) cdata = NULL;

    /* data key: buffer versions, or the bytes of what lives in plain memory */
    dkey = rnd_mix(rnd_mix(tkey, (u64)round_level << 8 | (tdata != NULL) << 1 | (cdata != NULL)), *(u32 *)&round_strength);
    dkey = rnd_mix(rnd_mix(dkey, *(u32 *)&round_min_bend), *(u32 *)&round_dense);
    dkey = rnd_mix(rnd_mix(dkey, (u32)round_bevel), *(u32 *)&round_bevel_max);
    if (d->v_buf) dkey = rnd_mix(rnd_mix(rnd_mix(dkey, d->v_buf), vver), (u32)d->v_ptr);
    else { dkey = rnd_hash(dkey, vdata, m->maxv * vs + 12); dynamic = 1; }
    /* normals in plain memory follow the positions (and are zero in the shadow pass): not hashed */
    if (m->n.buf) dkey = rnd_mix(rnd_mix(rnd_mix(dkey, m->n.buf), nver), (u32)m->n.ptr);
    else dynamic = 1;
    if (tdata) {
        if (m->tc.buf) dkey = rnd_mix(rnd_mix(rnd_mix(dkey, m->tc.buf), tver), (u32)m->tc.ptr);
        else if (tdata < vdata || tdata >= vdata + vs) { dkey = rnd_hash(dkey, tdata, m->maxv * ts + 8); dynamic = 1; }
    }
    if (cdata) {
        if (m->col.buf) dkey = rnd_mix(rnd_mix(rnd_mix(dkey, m->col.buf), cver), (u32)m->col.ptr);
        else if (cdata < vdata || cdata >= vdata + vs) { dkey = rnd_hash(dkey, cdata, m->maxv * cs + 4); dynamic = 1; }
    }
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&fq);
    rnd_cpu_us += (double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)fq.QuadPart;
    if (dkey == m->data_key) return m->ok;
    in.v = vdata; in.vs = vs; in.n = ndata; in.ns = ns;
    in.tc = tdata; in.tcs = ts;
    in.col = cdata; in.cols = cs; in.col_type = m->col.type; in.col_size = m->col.size;
    in.still = !dynamic;
    in.max_n = dynamic && !caster_pass ? rnd_cap_n : 6;
    if (dynamic) {
        u32 i, oldest = 0;
        int own = 0;
        struct rnd_dyn *s = NULL;
        dkey = rnd_mix(dkey, m->id);
        if (!caster_pass) dkey = rnd_mix(dkey, rnd_cap_n);
        else {
            /* the shadow pass takes whichever level the main pass made from these positions */
            u64 k0 = dkey;
            for (i = 0; i < RND_DYN; i++)
                if (rnd_dyn[i].owner == m->id && (rnd_dyn[i].key == rnd_mix(k0, 2) || rnd_dyn[i].key == rnd_mix(k0, 6))) {
                    dkey = rnd_dyn[i].key; break;
                }
            /* none (the worm moved since: the shadow pass comes first): round_casters 2 makes
               its own from normals worked out from the positions */
            if (dkey == k0) {
                if (round_casters < 2 || !rnd_caster_cap) return 0;
                dkey = rnd_mix(k0, 0x47450000u | rnd_caster_cap);
                own = 1;
            }
        }
        for (i = 0; i < RND_DYN; i++) {
            if (rnd_dyn[i].key == dkey && rnd_dyn[i].owner == m->id) { s = &rnd_dyn[i]; break; }
            if (rnd_dyn[i].frame < rnd_dyn[oldest].frame || !rnd_dyn[i].key) oldest = i;
        }
        if (!s) {
            if (caster_pass && !own) return 0;  /* zero normals here: wait for the main pass */
            if (own) {
                if (!rnd_geo_normals(m, vdata, vs)) return 0;
                in.n = (const u8 *)rnd_gn; in.ns = 12;
                in.max_n = rnd_caster_cap;
                rnd_casters_own++;
            }
            s = &rnd_dyn[oldest];
            if (s->owner) {                     /* whoever borrowed this slot's arrays lets go */
                u32 k;
                for (k = 0; k < RND_MESHES; k++)
                    if (rnd_meshes[k].borrowed && rnd_meshes[k].pos == s->pos) { rnd_unborrow(&rnd_meshes[k]); rnd_meshes[k].data_key = 0; }
            }
            s->key = 0; s->owner = m->id;
            rnd_borrow(m, s);
            m->ok = rnd_make(m, &in);
            m->has_tc = tdata != NULL;
            m->has_col = cdata != NULL;
            s->pos = m->pos; s->nrm = m->nrm; s->tco = m->tco; s->colo = m->colo;
            s->cap_v = m->cap_v; s->cap_n = m->cap_n; s->cap_t = m->cap_t; s->cap_c = m->cap_c;
            s->ok = m->ok; s->has_tc = m->has_tc; s->has_col = m->has_col; s->level = m->level; s->flat = m->flat;
            s->nv = m->nv;
            s->key = dkey;
        } else {
            rnd_borrow(m, s);
            m->nv = s->nv;
        }
        s->frame = rnd_frame;
        m->data_key = dkey;
        m->dynamic = 1;
        /* the index pattern must match the level this result was made with */
        if (m->ok && m->level >= 2 && m->level <= 6 && m->idxs[m->level]) { m->idx = m->idxs[m->level]; m->ni = m->nis[m->level]; m->idx_level = (u32)m->level; }
        return m->ok && m->idx && m->idx_level == (u32)m->level && m->nv == s->nv;
    }
    if (m->borrowed) rnd_unborrow(m);
    m->data_key = dkey;
    m->dynamic = 0;
    m->ok = rnd_make(m, &in);
    m->has_tc = tdata != NULL;
    m->has_col = cdata != NULL;
    return m->ok;
}

/* A static rounded mesh goes into buffers of our own once (and again when its data changes). */
static int rnd_upload(struct rnd_mesh *m)
{
    u32 bytes;
    if (m->vbo_ctx != wglGetCurrentContext()) { m->vbo = m->ibo = 0; m->vbo_key = 0; }   /* new context: old ids are gone */
    if (m->vbo && m->vbo_key == m->data_key) return 1;
    if (!rnd_GenBuffers) {
        rnd_GenBuffers = (void *)real_wglGetProcAddress("glGenBuffers");
        rnd_DeleteBuffers = (void *)real_wglGetProcAddress("glDeleteBuffers");
        rnd_BufferData = (void *)real_wglGetProcAddress("glBufferData");
        rnd_BufferSubData = (void *)real_wglGetProcAddress("glBufferSubData");
    }
    if (!rnd_GenBuffers || !rnd_DeleteBuffers || !rnd_BufferData || !rnd_BufferSubData) return 0;
    if (!m->vbo) { GLuint b[2] = { 0, 0 }; rnd_GenBuffers(2, b); m->vbo = b[0]; m->ibo = b[1]; m->vbo_ctx = wglGetCurrentContext(); }
    if (!m->vbo || !m->ibo) return 0;
    m->off_n = m->nv * 12;
    m->off_t = m->off_n + m->nv * 12;
    m->off_c = m->off_t + (m->has_tc ? m->nv * 8 : 0);
    bytes = m->off_c + (m->has_col ? m->nv * 4 : 0);
    p_glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    rnd_BufferData(GL_ARRAY_BUFFER, (ptrdiff_t)bytes, NULL, 0x88E4 /* GL_STATIC_DRAW */);
    rnd_BufferSubData(GL_ARRAY_BUFFER, 0, (ptrdiff_t)m->nv * 12, m->pos);
    rnd_BufferSubData(GL_ARRAY_BUFFER, (ptrdiff_t)m->off_n, (ptrdiff_t)m->nv * 12, m->nrm);
    if (m->has_tc) rnd_BufferSubData(GL_ARRAY_BUFFER, (ptrdiff_t)m->off_t, (ptrdiff_t)m->nv * 8, m->tco);
    if (m->has_col) rnd_BufferSubData(GL_ARRAY_BUFFER, (ptrdiff_t)m->off_c, (ptrdiff_t)m->nv * 4, m->colo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    rnd_BufferData(GL_ELEMENT_ARRAY_BUFFER, (ptrdiff_t)m->ni * 4, m->idx, 0x88E4);
    m->vbo_key = m->data_key;
    rnd_uploads++;
    return 1;
}

/* Distance LOD: is the mesh at least round_min_pixels in radius on screen? Its bounding box
   is taken from the vertices it uses (the worms' scratch array holds another worm at every
   draw, so this is done per draw; about a microsecond per hundred vertices). */
static float rnd_pixels(struct rnd_mesh *m, const struct rt_draw *d, const struct prog *vp)
{
    u32 avail = 0, vs = d->v_stride ? d->v_stride : d->v_size * 4, i;
    const u8 *v = rnd_data(d->v_buf, d->v_ptr, &avail, NULL);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, c[3], r, w[3], e[3], dist, sc;
    static GLint vpt[4]; static u32 vpt_frame = 0xffffffffu;
    int k;
    if (!v || (u64)m->maxv * vs + 12 > avail || (!d->v_buf && IsBadReadPtr(v, m->maxv * vs + 12))) return 1e9f;
    for (i = 0; i <= m->maxv; i++) {
        const float *p = (const float *)(v + (size_t)i * vs);
        for (k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
    }
    for (k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) * 0.5f;
    r = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    /* row vectors: world = p @ model, eye = world @ view */
    for (k = 0; k < 3; k++) w[k] = c[0] * vp->model[k] + c[1] * vp->model[4 + k] + c[2] * vp->model[8 + k] + vp->model[12 + k];
    for (k = 0; k < 3; k++) e[k] = w[0] * vp->view[k] + w[1] * vp->view[4 + k] + w[2] * vp->view[8 + k] + vp->view[12 + k];
    sc = sqrtf(vp->model[0] * vp->model[0] + vp->model[1] * vp->model[1] + vp->model[2] * vp->model[2]);
    dist = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (vpt_frame != rnd_frame) { glGetIntegerv(GL_VIEWPORT, vpt); vpt_frame = rnd_frame; }
    /* keep the eye (eye = world @ view, so world = -t @ R^T) for the next shadow pass */
    for (k = 0; k < 3; k++) rnd_cam[k] = -(vp->view[12] * vp->view[k * 4] + vp->view[13] * vp->view[k * 4 + 1] + vp->view[14] * vp->view[k * 4 + 2]);
    rnd_cam_k = fabsf(vp->proj[5]) * 0.5f * (float)vpt[3];
    rnd_cam_frame = rnd_frame;
    if (dist <= r * sc) return 1e9f;                        /* the camera is inside it */
    return r * sc * fabsf(vp->proj[5]) * 0.5f * (float)vpt[3] / dist;
}

/* The same size test in the shadow pass, whose view is not the camera's: measured from the
   eye the main pass had last (one frame old). */
static float rnd_pixels_cam(struct rnd_mesh *m, const struct rt_draw *d, const float *model)
{
    u32 avail = 0, vs = d->v_stride ? d->v_stride : d->v_size * 4, i;
    const u8 *v = rnd_data(d->v_buf, d->v_ptr, &avail, NULL);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, c[3], r, w[3], dist, sc;
    int k;
    if (rnd_frame - rnd_cam_frame > 2) return 0;            /* no recent main pass: leave it */
    if (!v || (u64)m->maxv * vs + 12 > avail || (!d->v_buf && IsBadReadPtr(v, m->maxv * vs + 12))) return 0;
    for (i = 0; i <= m->maxv; i++) {
        const float *p = (const float *)(v + (size_t)i * vs);
        for (k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
    }
    for (k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) * 0.5f;
    r = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    for (k = 0; k < 3; k++) w[k] = c[0] * model[k] + c[1] * model[4 + k] + c[2] * model[8 + k] + model[12 + k] - rnd_cam[k];
    sc = sqrtf(model[0] * model[0] + model[1] * model[1] + model[2] * model[2]);
    dist = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (dist <= r * sc) return 1e9f;
    return r * sc * rnd_cam_k / dist;
}

/* Smooth normals from the positions alone (area-weighted face normals summed per welded
   position), turned to point away from the mesh's middle. For the shadow pass of animated
   meshes, whose own normals are zero there. Hard edges get rounded over (no normals to tell
   them), which moves the outline a little against the main pass's; well inside the distance
   tolerance the lit shaders use to match a pixel with the tracer. */
static int rnd_geo_normals(struct rnd_mesh *m, const u8 *v, u32 vs)
{
    u32 t, i, k, nv = m->maxv + 1;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, mid[3];
    double out = 0;
    if (!rnd_grow((void **)&rnd_gn, &rnd_gn_cap, nv * 6, sizeof(float))) return 0;
    memset(rnd_gn, 0, (size_t)nv * 6 * sizeof(float));
    for (i = 0; i < m->ntri * 3; i++) {
        const float *p = (const float *)(v + (size_t)m->corner[i] * vs);
        for (k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
    }
    for (k = 0; k < 3; k++) mid[k] = (lo[k] + hi[k]) * 0.5f;
    /* sums per position id live in the second half of rnd_gn */
    for (t = 0; t < m->ntri; t++) {
        const float *a = (const float *)(v + (size_t)m->corner[t * 3] * vs);
        const float *b = (const float *)(v + (size_t)m->corner[t * 3 + 1] * vs);
        const float *c = (const float *)(v + (size_t)m->corner[t * 3 + 2] * vs);
        float e1[3], e2[3], f[3];
        for (k = 0; k < 3; k++) { e1[k] = b[k] - a[k]; e2[k] = c[k] - a[k]; }
        f[0] = e1[1] * e2[2] - e1[2] * e2[1];
        f[1] = e1[2] * e2[0] - e1[0] * e2[2];
        f[2] = e1[0] * e2[1] - e1[1] * e2[0];
        for (k = 0; k < 3; k++) out += (double)f[k] * ((a[k] + b[k] + c[k]) / 3.0f - mid[k]);
        for (i = 0; i < 3; i++) {
            float *s = rnd_gn + (size_t)(nv + m->pid[t * 3 + i]) * 3;
            s[0] += f[0]; s[1] += f[1]; s[2] += f[2];
        }
    }
    for (i = 0; i < nv; i++) rnd_gn[i * 3 + 2] = 1.0f;     /* vertices no triangle uses */
    for (i = 0; i < m->ntri * 3; i++) {
        const float *s = rnd_gn + (size_t)(nv + m->pid[i]) * 3;
        float *q = rnd_gn + (size_t)m->corner[i] * 3;
        float sg = out < 0 ? -1.0f : 1.0f;
        q[0] = s[0] * sg; q[1] = s[1] * sg; q[2] = s[2] * sg;
        if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] < 1e-30f) { q[0] = 0; q[1] = 0; q[2] = 1; }
        else rnd_norm(q);
    }
    return 1;
}

/* ---- main pass: draw the rounded mesh in place of the game's ------------------------- */
#define GL_COLOR_ARRAY_BUFFER_BINDING_ 0x8898

static int round_main(u32 kind, GLenum mode, u32 first, u32 count, GLenum itype, const void *indices)
{
    struct prog *vp = (cur_vp >= 0 && vp_on) ? &progs[cur_vp] : NULL;
    struct prog *fp = (cur_fp >= 0 && fp_on) ? &progs[cur_fp] : NULL;
    struct rnd_mesh *m;
    struct rt_draw d;
    int col_on;
    GLint csize = 0, ctype = 0, cstride = 0, cbuf = 0;
    void *cptr = NULL;

    if (!round_on || kind > 2 || !vp || !fp || !vp->sun_lit || vp->landscape || strstr(vp->entry, "Skinned")) return 0;
    if (mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP && mode != GL_TRIANGLE_FAN && mode != GL_QUADS) return 0;
    if (count < 24 || gs.alpha_test || !gs.vtx_on || !gs.nrm_on) return 0;
    if (gs.vtx.type != GL_FLOAT || gs.vtx.size < 3 || gs.nrm.type != GL_FLOAT) return 0;
    if (gs.tc0_on && (gs.tc0.type != GL_FLOAT || gs.tc0.size < 2)) return 0;
    if (!p_glBindBuffer || (gs.client_unit != 0 && !real_glClientActiveTexture)) return 0;
    col_on = glIsEnabled(GL_COLOR_ARRAY);
    if (col_on) {
        glGetIntegerv(GL_COLOR_ARRAY_SIZE, &csize);
        glGetIntegerv(GL_COLOR_ARRAY_TYPE, &ctype);
        glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &cstride);
        glGetPointerv(GL_COLOR_ARRAY_POINTER, &cptr);
        glGetIntegerv(GL_COLOR_ARRAY_BUFFER_BINDING_, &cbuf);
        if (!((ctype == GL_UNSIGNED_BYTE && csize == 4) || (ctype == GL_FLOAT && csize >= 3))) return 0;
    }

    memset(&d, 0, sizeof d);
    d.mode = mode; d.first = first; d.count = count; d.itype = itype; d.indices = indices; d.arrays = kind == 1;
    d.v_on = 1; d.v_size = gs.vtx.size; d.v_stride = gs.vtx.stride; d.v_buf = gs.vtx.buf; d.v_type = gs.vtx.type; d.v_ptr = gs.vtx.ptr;
    d.elem_buf = kind == 1 ? 0 : gs.elem_buf;
    m = rnd_find(rnd_identity(d.v_buf, d.v_ptr, d.elem_buf, kind == 1 ? NULL : indices, count, first, mode), 1);
    if (!m) return 0;
    m->used_frame = rnd_frame;
    if (m->topo_key && m->ntri > round_max_tris) return 0;
    if (count / 3 > round_max_tris * 2) return 0;
    /* remember the main pass's arrays (the shadow pass has none of them) */
    {
        struct rnd_src n = { 1, gs.nrm.buf, 3, gs.nrm.type, gs.nrm.stride, gs.nrm.ptr };
        struct rnd_src tc = { gs.tc0_on, gs.tc0.buf, (GLint)gs.tc0.size, gs.tc0.type, gs.tc0.stride, gs.tc0.ptr };
        struct rnd_src cl = { col_on, (u32)cbuf, csize, (GLenum)ctype, (u32)cstride, cptr };
        if (!gs.tc0_on) memset(&tc, 0, sizeof tc);
        if (!col_on) memset(&cl, 0, sizeof cl);
        if (memcmp(&n, &m->n, sizeof n) || memcmp(&tc, &m->tc, sizeof tc) || memcmp(&cl, &m->col, sizeof cl)) m->data_key = 0;
        m->n = n; m->tc = tc; m->col = cl;
    }
    m->tex = fp->tex0;
    m->has_xf = (vp->have & 64) != 0;
    if (m->has_xf) memcpy(m->texxf, vp->texxf, sizeof m->texxf);
    m->has_main = 1;
    rnd_cap_n = 6;
    if (m->corner && (vp->have & 7) == 7) {
        float px = rnd_pixels(m, &d, vp);
        if (px < round_min_pixels) { rnd_small++; return 0; }
        if (px < round_full_pixels) rnd_cap_n = 2;          /* animated meshes only (see rnd_make) */
    }
    if (!rnd_update(m, &d, 0)) return 0;
    if (m->ntri > round_max_tris || (gs.tc0_on && !m->has_tc) || (col_on && !m->has_col)) return 0;

    /* draw it from our buffers (static meshes) or plain memory (animated ones), then put every
       array back as the game left it */
    if (!m->dynamic && rnd_upload(m)) {
        p_glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
        real_glVertexPointer(3, GL_FLOAT, 12, (const void *)0);
        real_glNormalPointer(GL_FLOAT, 12, (const void *)m->off_n);
        if (gs.tc0_on) {
            if (gs.client_unit != 0) real_glClientActiveTexture(GL_TEXTURE0);
            real_glTexCoordPointer(2, GL_FLOAT, 8, (const void *)m->off_t);
        }
        if (col_on) glColorPointer(4, GL_UNSIGNED_BYTE, 4, (const void *)m->off_c);
        real_glDrawElements(GL_TRIANGLES, (GLsizei)m->ni, GL_UNSIGNED_INT, (const void *)0);
    } else {
        p_glBindBuffer(GL_ARRAY_BUFFER, 0);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        real_glVertexPointer(3, GL_FLOAT, 12, m->pos);
        real_glNormalPointer(GL_FLOAT, 12, m->nrm);
        if (gs.tc0_on) {
            if (gs.client_unit != 0) real_glClientActiveTexture(GL_TEXTURE0);
            real_glTexCoordPointer(2, GL_FLOAT, 8, m->tco);
        }
        if (col_on) glColorPointer(4, GL_UNSIGNED_BYTE, 4, m->colo);
        real_glDrawElements(GL_TRIANGLES, (GLsizei)m->ni, GL_UNSIGNED_INT, m->idx);
    }
    rnd_draws++; rnd_tris_in += m->ntri; rnd_tris_out += m->ni / 3;

    p_glBindBuffer(GL_ARRAY_BUFFER, gs.vtx.buf);
    real_glVertexPointer((GLint)gs.vtx.size, gs.vtx.type, (GLsizei)gs.vtx.stride, gs.vtx.ptr);
    p_glBindBuffer(GL_ARRAY_BUFFER, gs.nrm.buf);
    real_glNormalPointer(gs.nrm.type, (GLsizei)gs.nrm.stride, gs.nrm.ptr);
    if (gs.tc0_on) {
        p_glBindBuffer(GL_ARRAY_BUFFER, gs.tc0.buf);
        real_glTexCoordPointer((GLint)gs.tc0.size, gs.tc0.type, (GLsizei)gs.tc0.stride, gs.tc0.ptr);
        if (gs.client_unit != 0) real_glClientActiveTexture(GL_TEXTURE0 + gs.client_unit);
    }
    if (col_on) {
        p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)cbuf);
        glColorPointer(csize, (GLenum)ctype, cstride, cptr);
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, gs.array_buf);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gs.elem_buf);
    return 1;
}

/* ---- shadow pass: hand the tracer the rounded mesh ----------------------------------- */
static int round_caster(const struct rt_draw *d, struct rt_draw *out)
{
    struct rnd_mesh *m;
    if (!round_on || !round_casters || d->alpha_test || !d->v_on || d->v_type != GL_FLOAT || d->v_size < 3) return 0;
    m = rnd_find(rnd_identity(d->v_buf, d->v_ptr, d->arrays ? 0 : d->elem_buf, d->arrays ? NULL : d->indices,
                              d->count, d->first, d->mode), 0);
    if (!m || !m->has_main || rnd_frame - m->used_frame > 120) return 0;
    if (m->topo_key && m->ntri > round_max_tris) return 0;
    rnd_caster_cap = 0;
    if (round_casters >= 2 && m->corner && cur_vp >= 0 && vp_on && (progs[cur_vp].have & 1)) {
        /* animated meshes the shadow pass may have to round itself: only big ones on screen
           (round_caster_pixels), and with round_caster_steps steps per edge */
        float px = rnd_pixels_cam(m, d, progs[cur_vp].model);
        rnd_caster_cap = (px < round_caster_pixels || px < round_min_pixels) ? 0 : round_caster_steps < 2 ? 2 : round_caster_steps;
    }
    if (!rnd_update(m, d, 1) || m->ntri > round_max_tris) return 0;
    if (m->dynamic && round_casters < 2) return 0;     /* animated: the tracer would rebuild 9x the triangles every frame */
    memset(out, 0, sizeof *out);
    out->mode = GL_TRIANGLES; out->count = m->ni; out->itype = GL_UNSIGNED_INT; out->indices = m->idx;
    out->v_on = 1; out->v_size = 3; out->v_stride = 12; out->v_type = GL_FLOAT; out->v_ptr = m->pos;
    if (m->has_tc) { out->tc_on = 1; out->tc_size = 2; out->tc_stride = 8; out->tc_type = GL_FLOAT; out->tc_ptr = m->tco; }
    out->n_on = 1; out->n_stride = 12; out->n_type = GL_FLOAT; out->n_ptr = m->nrm;
    out->texture = m->tex;
    out->tex_xf = m->has_xf ? m->texxf : NULL;
    out->own = 1;
    /* everything from the engine's buffers: never changes under this key */
    out->static_key = m->dynamic ? 0 : rnd_mix(m->data_key, 0x524E44);
    rnd_casters++;
    return 1;
}

static void round_frame_end(void)
{
    if (round_on && (rnd_frame - rnd_last_log >= 300) && (rnd_draws || rnd_made)) {
        logmsg("round: %u draws rounded (%u -> %u triangles), %u too small, %u meshes remade, %u uploaded, %u casters (%u made in the shadow pass), %.2f ms CPU this frame",
               rnd_draws, rnd_tris_in, rnd_tris_out, rnd_small, rnd_made, rnd_uploads, rnd_casters, rnd_casters_own, rnd_cpu_us / 1000.0);
        rnd_last_log = rnd_frame;
    }
    rnd_frame++;
    rnd_cpu_us = 0;
    rnd_draws = rnd_tris_in = rnd_tris_out = rnd_made = rnd_casters = rnd_uploads = rnd_small = rnd_casters_own = 0;
}

static void round_settings_begin(void)       /* defaults for what wumrt.ini leaves out */
{
    round_on = 0; round_level = 3; round_casters = 1; round_strength = 1.0f; round_max_tris = 12000;
    round_min_bend = 0.004f; round_dense = 0.06f; round_min_pixels = 10.0f; round_full_pixels = 40.0f; round_caster_pixels = 40.0f; round_caster_steps = 2; round_bevel = 0; round_bevel_max = 40.0f;
}

static void round_read_settings(const char *name, float v)
{
    if (!strcmp(name, "round")) round_on = v != 0;
    else if (!strcmp(name, "round_level")) round_level = (int)v;
    else if (!strcmp(name, "round_casters")) round_casters = (int)v;
    else if (!strcmp(name, "round_strength")) round_strength = v;
    else if (!strcmp(name, "round_max_tris")) round_max_tris = v > 0 ? (u32)v : 0;
    else if (!strcmp(name, "round_min_bend")) round_min_bend = v;
    else if (!strcmp(name, "round_dense")) round_dense = v;
    else if (!strcmp(name, "round_min_pixels")) round_min_pixels = v;
    else if (!strcmp(name, "round_full_pixels")) round_full_pixels = v;
    else if (!strcmp(name, "round_caster_pixels")) round_caster_pixels = v;
    else if (!strcmp(name, "round_caster_steps")) round_caster_steps = v < 2 ? 2 : v > 6 ? 6 : (u32)v;
    else if (!strcmp(name, "round_bevel")) round_bevel = v != 0;
    else if (!strcmp(name, "round_bevel_max")) round_bevel_max = v;
    else return;
    {
        u32 i;
        for (i = 0; i < RND_MESHES; i++) { rnd_meshes[i].data_key = 0; rnd_meshes[i].auto_n = 0; }     /* remake with the new settings */
    }
}
