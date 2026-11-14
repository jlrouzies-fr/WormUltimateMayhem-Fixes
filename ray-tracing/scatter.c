/* ---- scattered stones and tufts on the terrain (included by rt_core.c) ---------------
   Small objects made from the terrain's own triangles, by what each terrain texture is
   (wumrt_surfaces.ini):
     rock   pebbles on its flat parts, and rubble along the foot of its cliffs (the bottom edge
            of a steep triangle that a flat triangle shares);
     sand   a few pebbles;
     grass, moss: chunky tufts of blades along the border of the grass (edges that no other
            grass triangle shares: where it meets rock, sand or a cliff's lip).
   The objects are made once per terrain draw (and again when its vertex data changes) and
   drawn right after it with the game's own terrain programs, so they get its lighting, its
   texture (coordinates continue the ground's) and the traced light. They are also handed to
   the tracer as cached static casters, so they cast and receive traced shadows and the
   primary rays see them.

   An edge map over all terrain draws (world space, shared corners matched exactly after
   rounding to 1/16 unit) tells which edges are shared with flat or grass triangles of other
   draws; a draw is only filled once the map has held still for a few frames. */

#define SC_SETS 2048
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#define SC_EDGE_BITS 20
#define SC_EDGES (1u << SC_EDGE_BITS)

enum { SK_NONE = 0, SK_GRASS = 1, SK_MOSS = 2, SK_ROCK = 3, SK_SAND = 4 };

/* One terrain draw. */
struct sc_set {
    u64 id;                     /* the draw's mesh identity and texture; 0 = free */
    u64 content;                /* versions of the buffers it was made from */
    u32 tex, kind, state;       /* state 1: edges registered, 2: objects made */
    u32 seen, first_seen, gen;
    float model[16];
    float xf[16]; int has_xf;
    float lo[3], hi[3];         /* world box */
    u64 *edges; u8 *eflags; u32 nedges;     /* what it put in the edge map */
    u8 *verts; u32 nverts;      /* 36 bytes each: position, normal, uv, colour */
    u32 *idx; u32 nidx;
    GLuint vbo, ibo; int uploaded;
    u32 nstones, nrubble, ntufts;
};

static struct {
    struct sc_set sets[SC_SETS];
    u64 *ekey; u32 *ecnt; u32 eused;    /* edge map: counts all | grass << 8 | flat << 16 */
    u32 echanged;                       /* frame of the last change to the map */
    int on; float density, size; u32 gen;
    u32 builds_logged, state_logged;
    u32 frame_builds, frame_built_at;
    double build_us; u32 nbuilds;
    void (APIENTRY *ClientActiveTexture)(GLenum);
    void (APIENTRY *DeleteBuffers)(GLsizei, const GLuint *);
    int injecting; u32 inject_frame;    /* inject_frame: R.frame + 1 of the last injection */
} SC;

typedef struct { float p[3], n[3], t[2]; u8 c[4]; } SVert;     /* 36 bytes */

void rt_set_scatter(int on, float density, float size)
{
    if (density <= 0) density = 1;
    if (size <= 0) size = 1;
    if (density != SC.density || size != SC.size) {
        SC.density = density; SC.size = size;
        SC.gen++;                       /* every set is made again */
    }
    SC.on = on;
}

static u32 sc_rand(u32 *s)
{
    u32 x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x ? x : 0x9E3779B9u;
}
static float sc_rf(u32 *s) { return (float)(sc_rand(s) >> 8) * (1.0f / 16777216.0f); }

static u64 sc_point_key(const float *w)
{
    u64 h = 0x6A09E667F3BCC909ull;
    h = mix(h, (u32)(i32)floorf(w[0] * 16.0f + 0.5f));
    h = mix(h, (u32)(i32)floorf(w[1] * 16.0f + 0.5f));
    h = mix(h, (u32)(i32)floorf(w[2] * 16.0f + 0.5f));
    return h;
}

static u64 sc_edge_key(const float *a, const float *b)
{
    u64 ka = sc_point_key(a), kb = sc_point_key(b), k;
    k = ka < kb ? mix(ka, (u32)kb ^ (u32)(kb >> 32)) : mix(kb, (u32)ka ^ (u32)(ka >> 32));
    k = mix(k, ka < kb ? kb : ka);
    return k ? k : 1;
}

/* Flat ground is also noted as points (its corners and samples about every 3 units) in
   cells of 3 units, in the same map: a cliff's foot is found by flat ground next to it even
   where the two are separate meshes that share no edge. */
#define SC_CELL 3.0f
static u64 sc_cell_key(i32 x, i32 y, i32 z)
{
    u64 k = mix(mix(mix(0xC3A5C85C97CB3127ull, (u32)x), (u32)y), (u32)z);
    return k ? k : 2;
}
static u64 sc_point_cell(const float *w)
{
    return sc_cell_key((i32)floorf(w[0] / SC_CELL), (i32)floorf(w[1] / SC_CELL), (i32)floorf(w[2] / SC_CELL));
}

static u32 *sc_edge_slot(u64 key, int insert)
{
    u32 i = (u32)(key >> (64 - SC_EDGE_BITS)), n;
    if (!SC.ekey) {
        if (!insert) return NULL;
        SC.ekey = calloc(SC_EDGES, sizeof(u64));
        SC.ecnt = calloc(SC_EDGES, sizeof(u32));
        if (!SC.ekey || !SC.ecnt) { free(SC.ekey); free(SC.ecnt); SC.ekey = NULL; SC.ecnt = NULL; return NULL; }
    }
    for (n = 0; n < 64; n++, i = (i + 1) & (SC_EDGES - 1)) {
        if (SC.ekey[i] == key) return &SC.ecnt[i];
        if (!SC.ekey[i]) {
            if (!insert || SC.eused > SC_EDGES / 4 * 3) return NULL;
            SC.ekey[i] = key; SC.ecnt[i] = 0; SC.eused++;
            return &SC.ecnt[i];
        }
    }
    return NULL;
}

static void sc_edges_apply(struct sc_set *s, int sign)
{
    u32 i;
    for (i = 0; i < s->nedges; i++) {
        u32 *c = sc_edge_slot(s->edges[i], sign > 0), f = s->eflags[i], add;
        if (!c) continue;
        add = 1u | (f & 1 ? 1u << 8 : 0) | (f & 2 ? 1u << 16 : 0);
        if (sign > 0) *c += add;
        else if ((*c & 0xff) && ((*c >> 8) & 0xff) >= ((add >> 8) & 1) && ((*c >> 16) & 0xff) >= ((add >> 16) & 1)) *c -= add;
    }
    SC.echanged = R.frame;
}

static void sc_free_geometry(struct sc_set *s)
{
    free(s->verts); free(s->idx);
    s->verts = NULL; s->idx = NULL; s->nverts = s->nidx = 0;
    s->uploaded = 0;
}

static void sc_release(struct sc_set *s)
{
    if (s->state) sc_edges_apply(s, -1);
    free(s->edges); free(s->eflags);
    sc_free_geometry(s);
    if ((s->vbo || s->ibo) && SC.DeleteBuffers && wglGetCurrentContext()) {
        GLuint b[2]; b[0] = s->vbo; b[1] = s->ibo;
        SC.DeleteBuffers(2, b);
    }
    memset(s, 0, sizeof *s);
}

/* The vertex data of a terrain draw, as the CPU mirror holds it. */
struct sc_src {
    const u8 *v, *n, *t, *c, *idx;
    u32 vs, ns, ts, cs, isz, maxi;
    GLenum ctype; int csize;
    u32 *tri; u32 ntri;         /* corner indices, 3 per triangle */
};

static const u8 *sc_buffer_data(u32 buf, const void *ptr, u32 need)
{
    struct shadow_buf *b;
    if (!buf) return ptr && !IsBadReadPtr(ptr, need) ? ptr : NULL;
    b = buf < MAX_BUF ? bufs[buf] : NULL;
    if (!b || !b->data || (u32)ptr > b->size || need > b->size - (u32)ptr) return NULL;
    return b->data + (u32)ptr;
}

static u64 sc_content(const struct rt_draw *d)
{
    u64 h = 0x3C6EF372FE94F82Bull;
    u32 list[4], i;
    list[0] = d->v_buf; list[1] = d->elem_buf; list[2] = d->n_on ? d->n_buf : 0; list[3] = d->tc_on ? d->tc_buf : 0;
    for (i = 0; i < 4; i++) {
        struct shadow_buf *b = list[i] && list[i] < MAX_BUF ? bufs[list[i]] : NULL;
        h = mix(mix(h, list[i]), b ? b->version : 0);
    }
    return h;
}

static int sc_source(const struct rt_draw *d, struct sc_src *s)
{
    static u32 *tri; static u32 tri_cap;
    u32 count = d->count, i, n = 0, maxtri;
    const u8 *idata = NULL;
    memset(s, 0, sizeof *s);
    if (!d->v_on || d->v_type != GL_FLOAT || d->v_size < 3 || count < 3) return 0;
    if (d->mode != GL_TRIANGLES && d->mode != GL_TRIANGLE_STRIP && d->mode != GL_TRIANGLE_FAN) return 0;
    if (!d->arrays) {
        s->isz = d->itype == GL_UNSIGNED_SHORT ? 2 : d->itype == GL_UNSIGNED_INT ? 4 : d->itype == GL_UNSIGNED_BYTE ? 1 : 0;
        if (!s->isz) return 0;
        idata = sc_buffer_data(d->elem_buf, d->indices, count * s->isz);
        if (!idata) return 0;
        for (i = 0; i < count; i++) { u32 x = index_at(idata, s->isz, i); if (x > s->maxi) s->maxi = x; }
    } else s->maxi = d->first + count - 1;
    if (s->maxi > 2000000) return 0;
    s->vs = d->v_stride ? d->v_stride : d->v_size * 4;
    s->v = sc_buffer_data(d->v_buf, d->v_ptr, s->maxi * s->vs + 12);
    if (!s->v) return 0;
    if (d->n_on && d->n_type == GL_FLOAT) {
        s->ns = d->n_stride ? d->n_stride : 12;
        s->n = sc_buffer_data(d->n_buf, d->n_ptr, s->maxi * s->ns + 12);
    }
    if (d->tc_on && d->tc_type == GL_FLOAT && d->tc_size >= 2) {
        s->ts = d->tc_stride ? d->tc_stride : d->tc_size * 4;
        s->t = sc_buffer_data(d->tc_buf, d->tc_ptr, s->maxi * s->ts + 8);
    }
    /* The colour array is not tracked by the hooks: ask GL (this runs at the draw, rarely). */
    if (glIsEnabled(GL_COLOR_ARRAY)) {
        GLint v = 0; void *p = NULL; u32 buf, need;
        glGetIntegerv(GL_COLOR_ARRAY_SIZE, &v); s->csize = v;
        glGetIntegerv(GL_COLOR_ARRAY_TYPE, &v); s->ctype = (GLenum)v;
        glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &v); s->cs = (u32)v;
        glGetPointerv(GL_COLOR_ARRAY_POINTER, &p);
        glGetIntegerv(0x8898, &v); buf = (u32)v;        /* GL_COLOR_ARRAY_BUFFER_BINDING */
        if ((s->ctype == GL_UNSIGNED_BYTE || s->ctype == GL_FLOAT) && s->csize >= 3) {
            u32 esz = s->ctype == GL_FLOAT ? 4 : 1;
            if (!s->cs) s->cs = esz * (u32)s->csize;
            need = s->maxi * s->cs + esz * (u32)s->csize;
            s->c = sc_buffer_data(buf, p, need);
        }
    }
    maxtri = d->mode == GL_TRIANGLES ? count / 3 : count - 2;
    if (!grow_pool((void **)&tri, &tri_cap, maxtri * 3, sizeof(u32))) return 0;
#define SIDX(k) (d->arrays ? d->first + (k) : index_at(idata, s->isz, (k)))
#define SPUT(a, b, c) do { u32 ia = (a), ib = (b), ic = (c); \
        if (ia != ib && ib != ic && ia != ic) { tri[n * 3] = ia; tri[n * 3 + 1] = ib; tri[n * 3 + 2] = ic; n++; } } while (0)
    if (d->mode == GL_TRIANGLES) for (i = 0; i + 2 < count; i += 3) SPUT(SIDX(i), SIDX(i + 1), SIDX(i + 2));
    else if (d->mode == GL_TRIANGLE_STRIP) for (i = 0; i + 2 < count; i++) SPUT(SIDX(i), SIDX(i + 1), SIDX(i + 2));
    else for (i = 1; i + 1 < count; i++) SPUT(SIDX(0), SIDX(i), SIDX(i + 1));
#undef SIDX
#undef SPUT
    s->tri = tri; s->ntri = n;
    return n > 0;
}

static void sc_pos(const struct sc_src *s, u32 i, float *o) { memcpy(o, s->v + (size_t)i * s->vs, 12); }
static void sc_world(const float *m, const float *p, float *w)
{
    int j;
    for (j = 0; j < 3; j++) w[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
}
static float sc_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sc_cross(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static float sc_norm(float *a)
{
    float l = sqrtf(sc_dot(a, a));
    if (l > 1e-20f) { a[0] /= l; a[1] /= l; a[2] /= l; }
    return l;
}

/* A triangle in world space with what is needed to place things on it. */
struct sc_tri {
    float w[3][3];      /* world corners */
    float o[3][3];      /* object corners */
    float t[3][2];      /* texture coordinates */
    float c[3][4];      /* vertex colours */
    float f[3];         /* unit face normal (world), on the side the vertex normals give */
    float fo[3];        /* the same in object space */
    float area, slope;  /* slope: f . up */
    float gu[3], gv[3]; /* texture coordinate gradients (object space, in the triangle's plane) */
};

static void sc_get_tri(const struct sc_src *s, const float *m, const u32 *c3, struct sc_tri *T)
{
    float e1[3], e2[3], ns[3] = { 0, 0, 0 }, eo1[3], eo2[3], no[3];
    int k, j;
    for (k = 0; k < 3; k++) {
        sc_pos(s, c3[k], T->o[k]);
        sc_world(m, T->o[k], T->w[k]);
        if (s->t) memcpy(T->t[k], s->t + (size_t)c3[k] * s->ts, 8); else T->t[k][0] = T->t[k][1] = 0;
        T->c[k][0] = T->c[k][1] = T->c[k][2] = T->c[k][3] = 1;
        if (s->c) {
            const u8 *p = s->c + (size_t)c3[k] * s->cs;
            for (j = 0; j < s->csize && j < 4; j++)
                T->c[k][j] = s->ctype == GL_FLOAT ? ((const float *)p)[j] : p[j] * (1.0f / 255.0f);
        }
        if (s->n) { float nn[3]; memcpy(nn, s->n + (size_t)c3[k] * s->ns, 12); ns[0] += nn[0]; ns[1] += nn[1]; ns[2] += nn[2]; }
    }
    for (j = 0; j < 3; j++) { e1[j] = T->w[1][j] - T->w[0][j]; e2[j] = T->w[2][j] - T->w[0][j]; }
    sc_cross(e1, e2, T->f);
    T->area = 0.5f * sc_norm(T->f);
    for (j = 0; j < 3; j++) { eo1[j] = T->o[1][j] - T->o[0][j]; eo2[j] = T->o[2][j] - T->o[0][j]; }
    sc_cross(eo1, eo2, no);
    sc_norm(no);
    /* Vertex normals (object space) say which side is up; without them, world up does. */
    if (s->n ? sc_dot(no, ns) < 0 : T->f[1] < 0) {
        for (j = 0; j < 3; j++) { T->f[j] = -T->f[j]; no[j] = -no[j]; }
    }
    memcpy(T->fo, no, 12);
    T->slope = T->f[1];
    /* uv(p) = uv0 + gu . (p - o0), in the plane (object space) */
    {
        float d00 = sc_dot(eo1, eo1), d01 = sc_dot(eo1, eo2), d11 = sc_dot(eo2, eo2), det = d00 * d11 - d01 * d01;
        float du1 = T->t[1][0] - T->t[0][0], du2 = T->t[2][0] - T->t[0][0];
        float dv1 = T->t[1][1] - T->t[0][1], dv2 = T->t[2][1] - T->t[0][1];
        float a, b;
        if (fabsf(det) < 1e-20f) det = 1e-20f;
        a = (du1 * d11 - du2 * d01) / det; b = (du2 * d00 - du1 * d01) / det;
        for (j = 0; j < 3; j++) T->gu[j] = a * eo1[j] + b * eo2[j];
        a = (dv1 * d11 - dv2 * d01) / det; b = (dv2 * d00 - dv1 * d01) / det;
        for (j = 0; j < 3; j++) T->gv[j] = a * eo1[j] + b * eo2[j];
    }
}

/* ---- the objects ---------------------------------------------------------------- */
static SVert *sc_v; static u32 sc_nv, sc_vcap;
static u32 *sc_i; static u32 sc_ni, sc_icap;

static u32 sc_add_vert(void)
{
    if (sc_nv >= sc_vcap && !grow_pool((void **)&sc_v, &sc_vcap, sc_nv + 1, sizeof(SVert))) return 0xffffffffu;
    return sc_nv++;
}
static void sc_add_tri(u32 a, u32 b, u32 c)
{
    if (a == 0xffffffffu || b == 0xffffffffu || c == 0xffffffffu) return;
    if (sc_ni + 3 > sc_icap && !grow_pool((void **)&sc_i, &sc_icap, sc_ni + 3, sizeof(u32))) return;
    sc_i[sc_ni++] = a; sc_i[sc_ni++] = b; sc_i[sc_ni++] = c;
}
static void sc_colour(u8 *out, const float *rgb, float a)
{
    int j;
    for (j = 0; j < 3; j++) { float x = rgb[j] * 255.0f; out[j] = (u8)(x < 0 ? 0 : x > 255 ? 255 : x + 0.5f); }
    out[3] = (u8)(a < 0 ? 0 : a > 1 ? 255 : a * 255.0f + 0.5f);
}

/* Object-space up: world up through the inverse of the model's rotation (rows 0-2, column 1). */
static void sc_up_object(const float *m, float *up)
{
    up[0] = m[1]; up[1] = m[5]; up[2] = m[9];
    if (sc_norm(up) < 1e-12f) { up[0] = 0; up[1] = 1; up[2] = 0; }
}

static void sc_basis(const float *n, float *t1, float *t2)
{
    float a[3] = { 1, 0, 0 };
    if (fabsf(n[0]) > 0.9f) { a[0] = 0; a[1] = 0; a[2] = 1; }
    sc_cross(n, a, t1); sc_norm(t1);
    sc_cross(n, t1, t2); sc_norm(t2);
}

static const float ico_v[12][3] = {
    { -0.5257f, 0.8507f, 0 }, { 0.5257f, 0.8507f, 0 }, { -0.5257f, -0.8507f, 0 }, { 0.5257f, -0.8507f, 0 },
    { 0, -0.5257f, 0.8507f }, { 0, 0.5257f, 0.8507f }, { 0, -0.5257f, -0.8507f }, { 0, 0.5257f, -0.8507f },
    { 0.8507f, 0, -0.5257f }, { 0.8507f, 0, 0.5257f }, { -0.8507f, 0, -0.5257f }, { -0.8507f, 0, 0.5257f } };
static const unsigned char ico_f[20][3] = {
    { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 },
    { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 },
    { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };

/* A stone: a squashed, jittered icosahedron (chunky, low-poly like the game's own props,
   with round normals). foot: object-space point on the ground; n: object-space up for it;
   r: radius. Texture coordinates continue the ground's from the foot, shifted per stone. */
static void sc_stone(const struct sc_tri *T, const float *foot, const float *uvfoot, const float *colour,
                     const float *n, float r, float rise, float dark, u32 *rs)
{
    float t1[3], t2[3], sx = 1.0f + 0.35f * sc_rf(rs), sy = 0.75f + 0.35f * sc_rf(rs), sz = 0.55f + 0.35f * sc_rf(rs);
    float ang = 6.2831853f * sc_rf(rs), ca = cosf(ang), sa = sinf(ang), centre[3], tint[3], shade;
    float du = sc_rf(rs), dv = sc_rf(rs);
    u32 base = sc_nv, k, j;
    sc_basis(n, t1, t2);
    for (j = 0; j < 3; j++) centre[j] = foot[j] + n[j] * (r * sz * rise);
    shade = (0.62f + 0.33f * sc_rf(rs)) * dark;
    for (j = 0; j < 3; j++) tint[j] = colour[j] * shade * (0.95f + 0.1f * sc_rf(rs));
    for (k = 0; k < 12; k++) {
        u32 vi = sc_add_vert();
        float jit = 0.85f + 0.3f * sc_rf(rs);
        float lx = ico_v[k][0] * sx * jit, ly = ico_v[k][2] * sy * jit, lz = ico_v[k][1] * sz * jit;
        float nx = ico_v[k][0] / sx, ny = ico_v[k][2] / sy, nz = ico_v[k][1] / sz;
        float rx = lx * ca - ly * sa, ry = lx * sa + ly * ca, mx = nx * ca - ny * sa, my = nx * sa + ny * ca;
        SVert *v;
        float d[3];
        if (vi == 0xffffffffu) return;
        v = &sc_v[vi];
        for (j = 0; j < 3; j++) {
            v->p[j] = centre[j] + r * (t1[j] * rx + t2[j] * ry + n[j] * lz);
            v->n[j] = t1[j] * mx + t2[j] * my + n[j] * nz;
            d[j] = v->p[j] - foot[j];
        }
        sc_norm(v->n);
        v->t[0] = uvfoot[0] + sc_dot(T->gu, d) + du;
        v->t[1] = uvfoot[1] + sc_dot(T->gv, d) + dv;
        sc_colour(v->c, tint, 1.0f);
    }
    /* (y of the table is up here: that swap of two axes mirrors it, so the winding is flipped back) */
    for (k = 0; k < 20; k++) sc_add_tri(base + ico_f[k][0], base + ico_f[k][2], base + ico_f[k][1]);
}

/* A tuft: a few chunky blades (three-sided spikes) leaning outwards over the border. All of
   a tuft takes the texture's colour at its foot, dark at the root and full at the tip, and
   the ground's normal, so it is lit like the grass it grows from. */
static void sc_tuft(const struct sc_tri *T, const float *foot, const float *uvfoot, const float *colour,
                    const float *n, const float *out, const float *along, u32 *rs)
{
    int blades = 5 + (int)(sc_rand(rs) % 4), b, k, j;
    float sz = SC.size;
    for (b = 0; b < blades; b++) {
        float base[3], tip[3], lean[3], t1[3], t2[3], h = sz * (3.5f + 2.5f * sc_rf(rs)), w = sz * (0.45f + 0.25f * sc_rf(rs));
        float a0 = 6.2831853f * sc_rf(rs), sa = sc_rf(rs) - 0.5f, so = sc_rf(rs) * 0.9f - 0.3f;
        float root[3], top[3], uvs[2];
        u32 vi[4];
        for (j = 0; j < 3; j++) {
            base[j] = foot[j] + along[j] * sa * 1.2f * sz + out[j] * so * sz - n[j] * 0.15f * sz;
            lean[j] = out[j] * (0.6f + 0.5f * sc_rf(rs)) + along[j] * (sc_rf(rs) - 0.5f) * 0.6f;
            tip[j] = base[j] + n[j] * h + lean[j] * h * 0.6f;
        }
        sc_basis(n, t1, t2);
        uvs[0] = uvfoot[0] + 0.02f * (sc_rf(rs) - 0.5f); uvs[1] = uvfoot[1] + 0.02f * (sc_rf(rs) - 0.5f);
        for (j = 0; j < 3; j++) { root[j] = colour[j] * 0.55f; top[j] = colour[j] * (0.95f + 0.1f * sc_rf(rs)); }
        for (k = 0; k < 4; k++) {
            SVert *v;
            vi[k] = sc_add_vert();
            if (vi[k] == 0xffffffffu) return;
            v = &sc_v[vi[k]];
            if (k < 3) {
                float a = a0 + 2.0943951f * (float)k, ca = cosf(a), sn = sinf(a);
                for (j = 0; j < 3; j++) {
                    v->p[j] = base[j] + w * (t1[j] * ca + t2[j] * sn);
                    v->n[j] = n[j] + 0.35f * (t1[j] * ca + t2[j] * sn);
                }
                sc_colour(v->c, root, 1.0f);
            } else {
                for (j = 0; j < 3; j++) { v->p[j] = tip[j]; v->n[j] = n[j] + 0.3f * lean[j]; }
                sc_colour(v->c, top, 0.0f);      /* alpha 0 = tip: the vertex program sways it */
            }
            sc_norm(v->n);
            v->t[0] = uvs[0]; v->t[1] = uvs[1];
        }
        sc_add_tri(vi[0], vi[1], vi[3]); sc_add_tri(vi[1], vi[2], vi[3]); sc_add_tri(vi[2], vi[0], vi[3]);
    }
    (void)T;
}

static void sc_lerp_corner(const struct sc_tri *T, float b0, float b1, float b2, float *o, float *uv, float *col)
{
    int j;
    for (j = 0; j < 3; j++) o[j] = T->o[0][j] * b0 + T->o[1][j] * b1 + T->o[2][j] * b2;
    for (j = 0; j < 2; j++) uv[j] = T->t[0][j] * b0 + T->t[1][j] * b1 + T->t[2][j] * b2;
    for (j = 0; j < 4; j++) col[j] = T->c[0][j] * b0 + T->c[1][j] * b1 + T->c[2][j] * b2;
}

/* Registers a draw's edges (first sight, or new vertex data). */
static void sc_register(struct sc_set *s, const struct sc_src *src)
{
    u32 i, k;
    int grass = s->kind == SK_GRASS || s->kind == SK_MOSS;
    if (s->state) sc_edges_apply(s, -1);
    free(s->edges); free(s->eflags);
    {
        /* room for three edges per triangle and the points of the flat ones */
        size_t room = (size_t)src->ntri * 3, i2;
        for (i2 = 0; i2 < src->ntri; i2++) {
            struct sc_tri T;
            sc_get_tri(src, s->model, src->tri + i2 * 3, &T);
            if (T.slope > 0.6f && s->kind != SK_ROCK) room += 3 + (size_t)(T.area / 2.25f > 256 ? 256 : T.area / 2.25f);
        }
        s->edges = malloc(room * sizeof(u64));
        s->eflags = malloc(room);
    }
    s->nedges = 0;
    s->lo[0] = s->lo[1] = s->lo[2] = 1e30f; s->hi[0] = s->hi[1] = s->hi[2] = -1e30f;
    if (!s->edges || !s->eflags) { free(s->edges); free(s->eflags); s->edges = NULL; s->eflags = NULL; s->state = 1; return; }
    for (i = 0; i < src->ntri; i++) {
        struct sc_tri T;
        u8 f;
        sc_get_tri(src, s->model, src->tri + i * 3, &T);
        if (T.area < 1e-8f) continue;
        f = (u8)((grass ? 1 : 0) | (T.slope > 0.6f ? 2 : 0));
        for (k = 0; k < 3; k++) {
            int j;
            s->edges[s->nedges] = sc_edge_key(T.w[k], T.w[(k + 1) % 3]);
            s->eflags[s->nedges++] = f;
            for (j = 0; j < 3; j++) {
                if (T.w[k][j] < s->lo[j]) s->lo[j] = T.w[k][j];
                if (T.w[k][j] > s->hi[j]) s->hi[j] = T.w[k][j];
            }
        }
        if (T.slope > 0.6f && s->kind != SK_ROCK) {     /* the ground a cliff stands on, not shelves of the cliff itself */
            float per = T.area / 2.25f;
            u32 n = (u32)(per > 256 ? 256 : per), q;
            u32 rs = (u32)sc_point_key(T.w[0]) | 1;
            for (q = 0; q < n; q++) {
                float b1, b2, w[3];
                int j;
                b1 = sc_rf(&rs); b2 = sc_rf(&rs);
                if (b1 + b2 > 1) { b1 = 1 - b1; b2 = 1 - b2; }
                for (j = 0; j < 3; j++) w[j] = T.w[0][j] * (1 - b1 - b2) + T.w[1][j] * b1 + T.w[2][j] * b2;
                s->edges[s->nedges] = sc_point_cell(w);
                s->eflags[s->nedges++] = 2;
            }
        }
    }
    s->state = 1;
    sc_edges_apply(s, 1);
    /* Built neighbours may have taken a shared edge for a border: make them again. */
    for (i = 0; i < SC_SETS; i++) {
        struct sc_set *o = &SC.sets[i];
        if (o == s || o->state != 2) continue;
        if (o->lo[0] > s->hi[0] + 1 || o->hi[0] < s->lo[0] - 1 || o->lo[1] > s->hi[1] + 1 || o->hi[1] < s->lo[1] - 1 ||
            o->lo[2] > s->hi[2] + 1 || o->hi[2] < s->lo[2] - 1) continue;
        o->state = 1;
    }
}

/* Is there flat ground in the cell that holds p (two samples or more: not just a small facet)? */
static int sc_flat_at(const float *p)
{
    u32 *c = sc_edge_slot(sc_point_cell(p), 0);
    return c && ((*c >> 16) & 0xff) >= 2;
}

static u32 sc_edge_count(const float *a, const float *b)
{
    u32 *c = sc_edge_slot(sc_edge_key(a, b), 0);
    return c ? *c : 0;
}

/* Makes the objects of one draw. */
static void sc_build(struct sc_set *s, const struct sc_src *src)
{
    float up[3], dens = SC.density, sz = SC.size, sd = 1.0f;
    u32 i, k, layer = s->tex < 65536 ? layer_of_tex[s->tex] : 0;
    double t0 = qpc_us();
    int grass = s->kind == SK_GRASS || s->kind == SK_MOSS, fringe = 0;
    sc_nv = sc_ni = 0;
    s->nstones = s->nrubble = s->ntufts = 0;
    if (layer && mats[layer].scat_d > 0) sd = mats[layer].scat_d;
    sc_up_object(s->model, up);
    if (grass && s->nedges) {
        /* A grass draw that is mostly border is a strip of the game's own (painted blades
           hanging over a cliff's lip): it needs no tufts. */
        u32 border = 0;
        for (i = 0; i < s->nedges; i++) {
            u32 *c = sc_edge_slot(s->edges[i], 0);
            if (c && ((*c >> 8) & 0xff) == 1) border++;
        }
        if (border * 3 > s->nedges) fringe = 1;
    }
    for (i = 0; i < src->ntri && sc_nv < 200000; i++) {
        struct sc_tri T;
        u32 rs;
        sc_get_tri(src, s->model, src->tri + i * 3, &T);
        if (T.area < 1e-6f) continue;
        rs = (u32)sc_point_key(T.w[0]) ^ (u32)(sc_point_key(T.w[1]) >> 7) ^ (u32)(sc_point_key(T.w[2]) >> 13) ^ 0xA5A5A5A5u;
        sc_rand(&rs);
        if (s->kind == SK_ROCK || s->kind == SK_SAND) {
            /* pebbles on flat ground */
            if (T.slope > 0.55f) {
                float per = (s->kind == SK_ROCK ? 0.006f : 0.002f) * dens * sd / (sz * sz);
                float want = T.area * per;
                u32 n = (u32)want + (sc_rf(&rs) < want - floorf(want) ? 1 : 0);
                for (k = 0; k < n && k < 64; k++) {
                    float b1 = sc_rf(&rs), b2 = sc_rf(&rs), foot[3], uv[2], col[4], r;
                    if (b1 + b2 > 1) { b1 = 1 - b1; b2 = 1 - b2; }
                    sc_lerp_corner(&T, 1 - b1 - b2, b1, b2, foot, uv, col);
                    /* (the vertex colour's alpha is not a material weight: terrain draws do not blend) */
                    r = sz * (1.2f + 3.3f * sc_rf(&rs) * sc_rf(&rs));
                    if (s->kind == SK_SAND) { float k = 0.66f + 0.2f * sc_rf(&rs); col[0] *= k; col[1] *= k * 0.95f; col[2] *= k * 0.9f; }
                    sc_stone(&T, foot, uv, col, T.fo, r, 0.35f, 0.85f, &rs);
                    s->nstones++;
                }
            }
            /* rubble along the foot of a cliff: the bottom edge of a steep triangle that a
               flat triangle shares */
            if (s->kind == SK_ROCK && T.slope < 0.45f && T.slope > -0.15f) {
                for (k = 0; k < 3; k++) {
                    const float *a = T.w[k], *b = T.w[(k + 1) % 3], *c = T.w[(k + 2) % 3];
                    float mid[3], dc[3], len, e[3], hor[3], hl, lift = 0;
                    u32 n, q;
                    int j;
                    for (j = 0; j < 3; j++) { mid[j] = 0.5f * (a[j] + b[j]); dc[j] = c[j] - mid[j]; e[j] = b[j] - a[j]; }
                    len = sqrtf(sc_dot(e, e));
                    if (len < 1.5f * sz || fabsf(e[1]) > 0.35f * len) continue;               /* not a level edge */
                    if (c[1] < a[1] + 0.3f * len || c[1] < b[1] + 0.3f * len) continue;      /* not the bottom edge */
                    hor[0] = T.f[0]; hor[1] = 0; hor[2] = T.f[2];
                    hl = sc_norm(hor);
                    if (hl < 0.2f) continue;
                    {
                        /* flat ground just in front of the wall, a little below the edge (or a
                           little above it, where the wall goes on under the sand) */
                        static const float offs[2] = { -0.75f, 0.75f };
                        float lowy = a[1] < b[1] ? a[1] : b[1], q1[3];
                        int o, found = 0;
                        for (o = 0; o < 2 && !found; o++) {
                            q1[0] = mid[0] + hor[0] * 1.5f; q1[1] = lowy + offs[o]; q1[2] = mid[2] + hor[2] * 1.5f;
                            if (sc_flat_at(q1)) { found = 1; lift = offs[o] > 0 ? offs[o] : 0; }
                        }
                        if (!found) continue;
                    }
                    n = (u32)(len / (7.0f * sz) * dens * sd + sc_rf(&rs));
                    for (q = 0; q < n && q < 32; q++) {
                        float tt = sc_rf(&rs), foot[3], uv[2], col[4], r, wfoot[3], b0, b1, b2, ho[3], m4[3];
                        if (sc_rf(&rs) > 0.75f) continue;
                        r = sz * (3.5f + 5.0f * sc_rf(&rs) * sc_rf(&rs));
                        /* along the edge in barycentric terms */
                        b0 = b1 = b2 = 0;
                        if (k == 0) { b0 = 1 - tt; b1 = tt; } else if (k == 1) { b1 = 1 - tt; b2 = tt; } else { b2 = 1 - tt; b0 = tt; }
                        sc_lerp_corner(&T, b0, b1, b2, foot, uv, col);
                        /* step out from the wall by part of the radius (object space) */
                        for (j = 0; j < 3; j++) wfoot[j] = hor[j];
                        ho[0] = wfoot[0] * s->model[0] + wfoot[1] * s->model[1] + wfoot[2] * s->model[2];
                        ho[1] = wfoot[0] * s->model[4] + wfoot[1] * s->model[5] + wfoot[2] * s->model[6];
                        ho[2] = wfoot[0] * s->model[8] + wfoot[1] * s->model[9] + wfoot[2] * s->model[10];
                        sc_norm(ho);
                        for (j = 0; j < 3; j++) m4[j] = foot[j] + ho[j] * (0.45f * r) + up[j] * (lift - 0.1f * r);
                        sc_stone(&T, m4, uv, col, up, r, 0.5f, 0.7f, &rs);
                        s->nrubble++;
                    }
                }
            }
        } else if (grass && !fringe && T.slope > 0.2f) {
            /* tufts along the border of the grass */
            for (k = 0; k < 3; k++) {
                const float *a = T.w[k], *b = T.w[(k + 1) % 3], *c = T.w[(k + 2) % 3];
                const float *oa = T.o[k], *ob = T.o[(k + 1) % 3], *oc = T.o[(k + 2) % 3];
                float e[3], len, out[3], along[3], dd;
                u32 cnt = sc_edge_count(a, b), n, q;
                int j;
                if (((cnt >> 8) & 0xff) != 1) continue;     /* shared with other grass */
                for (j = 0; j < 3; j++) { e[j] = ob[j] - oa[j]; along[j] = e[j]; }
                len = sc_norm(along);
                if (len < 0.3f * sz) continue;
                for (j = 0; j < 3; j++) out[j] = 0.5f * (oa[j] + ob[j]) - oc[j];
                dd = sc_dot(out, along);
                for (j = 0; j < 3; j++) out[j] -= along[j] * dd;
                if (sc_norm(out) < 1e-6f) continue;
                n = (u32)(len / (1.2f * sz) * dens * sd + sc_rf(&rs));
                for (q = 0; q < n && q < 48; q++) {
                    float tt = sc_rf(&rs), foot[3], uv[2], col[4], b0 = 0, b1 = 0, b2 = 0;
                    if (sc_rf(&rs) > 0.8f) continue;
                    if (k == 0) { b0 = 1 - tt; b1 = tt; } else if (k == 1) { b1 = 1 - tt; b2 = tt; } else { b2 = 1 - tt; b0 = tt; }
                    sc_lerp_corner(&T, b0, b1, b2, foot, uv, col);
                    sc_tuft(&T, foot, uv, col, T.fo, out, along, &rs);
                    s->ntufts++;
                }
                (void)c;
            }
        }
    }
    sc_free_geometry(s);
    if (sc_ni) {
        s->verts = malloc((size_t)sc_nv * sizeof(SVert));
        s->idx = malloc((size_t)sc_ni * sizeof(u32));
        if (s->verts && s->idx) {
            memcpy(s->verts, sc_v, (size_t)sc_nv * sizeof(SVert));
            memcpy(s->idx, sc_i, (size_t)sc_ni * sizeof(u32));
            s->nverts = sc_nv; s->nidx = sc_ni;
        } else sc_free_geometry(s);
    }
    s->state = 2;
    s->gen = SC.gen;
    SC.build_us += qpc_us() - t0; SC.nbuilds++;
    if (s->nidx && SC.builds_logged < 12) {
        rt_log("rt: scatter: terrain draw (texture %08x, kind %u, %u triangles) -> %u stones, %u rubble, %u tufts, "
               "%u vertices, %.2f ms", layer ? mats[layer].hash : 0, s->kind, src->ntri, s->nstones, s->nrubble, s->ntufts,
               s->nverts, (qpc_us() - t0) / 1000.0);
    }
}

static struct sc_set *sc_find(u64 id, int insert)
{
    u32 i = (u32)(id >> 21) & (SC_SETS - 1), n, oldest = i;
    for (n = 0; n < 32; n++) {
        struct sc_set *s = &SC.sets[(i + n) & (SC_SETS - 1)];
        if (s->id == id) return s;
        if (!s->id) { if (!insert) return NULL; s->id = id; return s; }
        if (s->seen < SC.sets[oldest].seen) oldest = (i + n) & (SC_SETS - 1);
    }
    if (!insert) return NULL;
    sc_release(&SC.sets[oldest]);
    SC.sets[oldest].id = id;
    return &SC.sets[oldest];
}

/* After a sun-lit terrain draw: draw its stones and tufts. */
int rt_scatter_draw(const struct rt_draw *d, const float *model)
{
    struct sc_set *s;
    struct sc_src src;
    u64 id, content;
    u32 layer;
    if (!SC.on || !d->texture || d->texture >= 65536) return 0;
    if (!gl_init()) return 0;
    if (!SC.ClientActiveTexture) {
        SC.ClientActiveTexture = (void *)rt_real_proc("glClientActiveTexture");
        SC.DeleteBuffers = (void *)rt_real_proc("glDeleteBuffers");
        if (!SC.ClientActiveTexture || !SC.DeleteBuffers) return 0;
    }
    if (SC.frame_built_at != R.frame) { SC.frame_built_at = R.frame; SC.frame_builds = 0; }
    id = mix(mesh_identity(d), d->texture);
    content = sc_content(d);
    s = sc_find(id, 1);
    layer = layer_of_tex[d->texture];
    if (!s->state || s->content != content || s->tex != d->texture) {
        if (SC.frame_builds >= 24) return 0;     /* spread first sights over frames */
        SC.frame_builds++;
        s->tex = d->texture;
        s->kind = layer ? mats[layer].kind : 0;
        s->content = content;
        memcpy(s->model, model, sizeof s->model);
        s->has_xf = d->tex_xf != NULL;
        if (d->tex_xf) memcpy(s->xf, d->tex_xf, sizeof s->xf);
        if (!s->first_seen) s->first_seen = R.frame;
        if (!sc_source(d, &src)) { s->state = 2; sc_free_geometry(s); s->seen = R.frame; return 0; }
        if (SC.state_logged < 6) {
            SC.state_logged++;
            rt_log("rt: scatter: terrain draw state: kind %u, %u triangles, blend %d, normals %d, uv %d, colours %d (size %d, type %#x)",
                   s->kind, src.ntri, d->blend, src.n != NULL, src.t != NULL, src.c != NULL, src.csize, src.ctype);
        }
        sc_register(s, &src);
    }
    s->seen = R.frame;
    if (s->kind != SK_NONE && (s->state == 1 || s->gen != SC.gen) && R.frame - SC.echanged >= 3 &&
        SC.frame_builds < 24) {
        SC.frame_builds++;
        if (sc_source(d, &src)) {
            memcpy(s->model, model, sizeof s->model);
            sc_build(s, &src);
        } else s->state = 2;
    }
    if (s->state != 2 || !s->nidx) return 0;
    if (!s->vbo) { GLuint b[2] = { 0, 0 }; gl.GenBuffers(2, b); s->vbo = b[0]; s->ibo = b[1]; s->uploaded = 0; }
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    gl.BindBuffer(GL_ARRAY_BUFFER, s->vbo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibo);
    if (!s->uploaded) {
        gl.BufferData(GL_ARRAY_BUFFER, (ptrdiff_t)((size_t)s->nverts * sizeof(SVert)), s->verts, GL_STATIC_DRAW);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (ptrdiff_t)((size_t)s->nidx * sizeof(u32)), s->idx, GL_STATIC_DRAW);
        s->uploaded = 1;
    }
    {
        int u;
        for (u = 1; u < 4; u++) { SC.ClientActiveTexture(GL_TEXTURE0 + u); glDisableClientState(GL_TEXTURE_COORD_ARRAY); }
        SC.ClientActiveTexture(GL_TEXTURE0);
    }
    glDisableClientState(0x845E);       /* GL_SECONDARY_COLOR_ARRAY */
    glDisableClientState(0x8457);       /* GL_FOG_COORD_ARRAY */
    glDisableClientState(GL_INDEX_ARRAY);
    glDisableClientState(GL_EDGE_FLAG_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(3, GL_FLOAT, sizeof(SVert), (void *)0);
    glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, sizeof(SVert), (void *)12);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, sizeof(SVert), (void *)24);
    glEnableClientState(GL_COLOR_ARRAY); glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SVert), (void *)32);
    glDrawElements(GL_TRIANGLES, (GLsizei)s->nidx, GL_UNSIGNED_INT, (void *)0);
    glPopClientAttrib();
    return 1;
}

/* At the start of a shadow pass: the objects of every terrain draw seen lately go into the
   traced scene as cached casters (their vertex data never changes once made). */
static void sc_inject(const float *light_view)
{
    u32 i;
    if (!SC.on || SC.injecting || SC.inject_frame == R.frame + 1) return;
    SC.inject_frame = R.frame + 1;
    SC.injecting = 1;
    for (i = 0; i < SC_SETS; i++) {
        struct sc_set *s = &SC.sets[i];
        struct rt_draw d;
        /* A minute after its draw was last seen: out of view, it still casts shadows; and a set that
           went in and out of the merged BVH would be kept out of it for a while. */
        if (s->state != 2 || !s->nidx || R.frame - s->seen > 1800) continue;
        /* Tufts stay out: thin, many triangles, and the memory of this 32-bit game is short. */
        if (s->kind == SK_GRASS || s->kind == SK_MOSS) continue;
        memset(&d, 0, sizeof d);
        d.mode = GL_TRIANGLES; d.count = s->nidx; d.itype = GL_UNSIGNED_INT; d.indices = s->idx;
        d.v_on = 1; d.v_size = 3; d.v_stride = sizeof(SVert); d.v_type = GL_FLOAT; d.v_ptr = s->verts;
        d.tc_on = 1; d.tc_size = 2; d.tc_stride = sizeof(SVert); d.tc_type = GL_FLOAT; d.tc_ptr = s->verts + 24;
        d.texture = s->tex;
        d.tex_xf = s->has_xf ? s->xf : NULL;
        d.static_key = mix(mix(s->id, s->content), (u64)s->gen << 32 | s->nidx);
        rt_on_caster(&d, s->model, light_view);
    }
    SC.injecting = 0;
}

static void sc_forget(void)
{
    u32 i;
    for (i = 0; i < SC_SETS; i++) { SC.sets[i].vbo = SC.sets[i].ibo = 0; SC.sets[i].uploaded = 0; }
}

static void sc_shutdown(void)
{
    u32 i;
    for (i = 0; i < SC_SETS; i++) {
        struct sc_set *s = &SC.sets[i];
        if ((s->vbo || s->ibo) && SC.DeleteBuffers) { GLuint b[2]; b[0] = s->vbo; b[1] = s->ibo; SC.DeleteBuffers(2, b); }
        s->vbo = s->ibo = 0;
    }
}
