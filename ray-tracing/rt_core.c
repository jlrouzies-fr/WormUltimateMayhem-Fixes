/*
 * Real-time sun shadow tracer, running inside the game's OpenGL context.
 *
 * Each frame the engine draws its shadow map first. Those draws are the complete list
 * of shadow casters, so we collect them (rt_on_caster) as instances of small BVHs:
 * meshes whose data repeats from frame to frame are built once and cached, the rest
 * are rebuilt for the frame. When the first sun-lit draw of the main pass arrives
 * (rt_prepare_main) the instances get a top-level BVH, everything is uploaded, and a
 * compute shader traces one camera ray and its sun ray(s) per pixel into a mask
 * texture (sun visibility, distance from the eye) that the game's shaders then read.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "rt.h"
#include "fx.h"

struct rt_stats rt_stats;

/* ---- GL beyond 1.1 ------------------------------------------------------------- */
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_VERTEX_ARRAY_BUFFER_BINDING 0x8896
#define GL_READ_ONLY 0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_COMPUTE_SHADER 0x91B9
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_RG 0x8227
#define GL_RG16F 0x822F
#define GL_RGBA16F 0x881A
#define GL_R16F 0x822D
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x8
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_BINDING_2D_ARRAY 0x8C1D
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x20

static struct {
    GLuint (APIENTRY *CreateShader)(GLenum);
    void (APIENTRY *ShaderSource)(GLuint, GLsizei, const char **, const GLint *);
    void (APIENTRY *CompileShader)(GLuint);
    void (APIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
    void (APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    void (APIENTRY *DeleteShader)(GLuint);
    GLuint (APIENTRY *CreateProgram)(void);
    void (APIENTRY *AttachShader)(GLuint, GLuint);
    void (APIENTRY *LinkProgram)(GLuint);
    void (APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
    void (APIENTRY *GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    void (APIENTRY *DeleteProgram)(GLuint);
    void (APIENTRY *UseProgram)(GLuint);
    GLint (APIENTRY *GetUniformLocation)(GLuint, const char *);
    void (APIENTRY *Uniform1i)(GLint, GLint);
    void (APIENTRY *Uniform3f)(GLint, float, float, float);
    void (APIENTRY *Uniform4f)(GLint, float, float, float, float);
    void (APIENTRY *GenBuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindBuffer)(GLenum, GLuint);
    void (APIENTRY *BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
    void (APIENTRY *BufferSubData)(GLenum, ptrdiff_t, ptrdiff_t, const void *);
    void (APIENTRY *BindBufferBase)(GLenum, GLuint, GLuint);
    void (APIENTRY *DispatchCompute)(GLuint, GLuint, GLuint);
    void (APIENTRY *MemoryBarrier)(GLbitfield);
    void (APIENTRY *BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    void (APIENTRY *TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (APIENTRY *GenQueries)(GLsizei, GLuint *);
    void (APIENTRY *BeginQuery)(GLenum, GLuint);
    void (APIENTRY *EndQuery)(GLenum);
    void (APIENTRY *GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
    void (APIENTRY *GetBufferSubData)(GLenum, ptrdiff_t, ptrdiff_t, void *);
    void (APIENTRY *UniformMatrix4fv)(GLint, GLsizei, GLboolean, const float *);
    void (APIENTRY *ActiveTexture)(GLenum);
    void (APIENTRY *TexStorage3D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
    void (APIENTRY *TexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (APIENTRY *GenFramebuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindFramebuffer)(GLenum, GLuint);
    void (APIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (APIENTRY *GenerateMipmap)(GLenum);
    void (APIENTRY *CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
} gl;
static int gl_state;    /* 0 not tried, 1 ready, -1 unavailable */

static int gl_init(void)
{
    static const char *names[] = {
        "glCreateShader", "glShaderSource", "glCompileShader", "glGetShaderiv", "glGetShaderInfoLog",
        "glDeleteShader", "glCreateProgram", "glAttachShader", "glLinkProgram", "glGetProgramiv",
        "glGetProgramInfoLog", "glDeleteProgram", "glUseProgram", "glGetUniformLocation", "glUniform1i",
        "glUniform3f", "glUniform4f", "glGenBuffers", "glBindBuffer", "glBufferData", "glBufferSubData",
        "glBindBufferBase", "glDispatchCompute", "glMemoryBarrier", "glBindImageTexture", "glTexStorage2D",
        "glGenQueries", "glBeginQuery", "glEndQuery", "glGetQueryObjectuiv", "glGetBufferSubData", "glUniformMatrix4fv",
        "glActiveTexture", "glTexStorage3D", "glTexSubImage3D", "glGenFramebuffers", "glBindFramebuffer",
        "glFramebufferTexture2D", "glGenerateMipmap", "glCompressedTexImage2D" };
    PROC *slot = (PROC *)&gl;
    int i;
    if (gl_state) return gl_state > 0;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
        slot[i] = rt_real_proc(names[i]);
        if (!slot[i]) {
            rt_log("rt: %s is not available; real-time tracing disabled", names[i]);
            gl_state = -1;
            rt_stats.error = 1;
            return 0;
        }
    }
    gl_state = 1;
    return 1;
}

/* ---- timing -------------------------------------------------------------------- */
static double qpc_us(void)
{
    static double scale;
    LARGE_INTEGER t;
    if (scale == 0) { QueryPerformanceFrequency(&t); scale = 1e6 / (double)t.QuadPart; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * scale;
}

/* ---- CPU mirror of the engine's vertex and index buffers ----------------------- */
#define MAX_BUF 65536
struct shadow_buf { u32 size, version; u8 *data; void *map_ptr; GLenum map_access; };
static struct shadow_buf *bufs[MAX_BUF];
static u32 shadow_bytes;

static void (APIENTRY *real_BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
static void *(APIENTRY *real_MapBuffer)(GLenum, GLenum);
static GLboolean (APIENTRY *real_UnmapBuffer)(GLenum);
static void (APIENTRY *real_DeleteBuffers)(GLsizei, const GLuint *);

static struct shadow_buf *bound_buf(GLenum target, int create)
{
    GLint id = 0;
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) return NULL;
    glGetIntegerv(target == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : GL_ELEMENT_ARRAY_BUFFER_BINDING, &id);
    if (id <= 0 || id >= MAX_BUF) return NULL;
    if (!bufs[id] && create) bufs[id] = calloc(1, sizeof(struct shadow_buf));
    return bufs[id];
}

static void APIENTRY my_BufferData(GLenum target, ptrdiff_t size, const void *data, GLenum usage)
{
    struct shadow_buf *b = bound_buf(target, 1);
    if (b && size >= 0) {
        if ((u32)size != b->size) {
            shadow_bytes -= b->size;
            free(b->data);
            b->data = size ? malloc((size_t)size) : NULL;
            b->size = b->data ? (u32)size : 0;
            shadow_bytes += b->size;
        }
        if (b->data) {
            if (data) memcpy(b->data, data, b->size);
            else memset(b->data, 0, b->size);
        }
        b->version++;
    }
    real_BufferData(target, size, data, usage);
}

static void *APIENTRY my_MapBuffer(GLenum target, GLenum access)
{
    void *p = real_MapBuffer(target, access);
    struct shadow_buf *b = bound_buf(target, 0);
    if (b) { b->map_ptr = p; b->map_access = access; }
    return p;
}

static GLboolean APIENTRY my_UnmapBuffer(GLenum target)
{
    struct shadow_buf *b = bound_buf(target, 0);
    if (b && b->map_ptr && b->data && b->map_access != GL_READ_ONLY) {
        memcpy(b->data, b->map_ptr, b->size);       /* still mapped: take what the engine wrote */
        b->version++;
    }
    if (b) b->map_ptr = NULL;
    return real_UnmapBuffer(target);
}

static void APIENTRY my_DeleteBuffers(GLsizei n, const GLuint *ids)
{
    GLsizei i;
    for (i = 0; ids && i < n; i++)
    {
        rt_buffer_deleted(ids[i]);
        if (ids[i] < MAX_BUF && bufs[ids[i]]) {
            shadow_bytes -= bufs[ids[i]]->size;
            free(bufs[ids[i]]->data);
            free(bufs[ids[i]]);
            bufs[ids[i]] = NULL;
        }
    }
    real_DeleteBuffers(n, ids);
}

const u8 *rt_buffer_data(u32 id, u32 *size, u32 *version)
{
    struct shadow_buf *b = id && id < MAX_BUF ? bufs[id] : NULL;
    if (!b || !b->data || b->map_ptr) return NULL;      /* mapped: the mirror is updated at unmap */
    if (size) *size = b->size;
    if (version) *version = b->version;
    return b->data;
}

PROC rt_wrap_proc(const char *name, PROC real)
{
    if (!name || !real) return real;
    if (!strcmp(name, "glBufferData") || !strcmp(name, "glBufferDataARB")) {
        real_BufferData = (void *)real; return (PROC)my_BufferData;
    }
    if (!strcmp(name, "glMapBuffer") || !strcmp(name, "glMapBufferARB")) {
        real_MapBuffer = (void *)real; return (PROC)my_MapBuffer;
    }
    if (!strcmp(name, "glUnmapBuffer") || !strcmp(name, "glUnmapBufferARB")) {
        real_UnmapBuffer = (void *)real; return (PROC)my_UnmapBuffer;
    }
    if (!strcmp(name, "glDeleteBuffers") || !strcmp(name, "glDeleteBuffersARB")) {
        real_DeleteBuffers = (void *)real; return (PROC)my_DeleteBuffers;
    }
    return real;
}

/* ---- scene state --------------------------------------------------------------- */
struct entry {              /* one mesh, identified by a hash of where its data comes from */
    u64 key;
    u32 state;              /* 0 empty, 1 seen (built for one frame), 2 cached */
    u32 last_frame, streak;     /* streak: consecutive frames it was drawn unchanged */
    u32 restless_until;         /* frame until which its instances stay out of the merged BVH */
    u32 node_off, nnodes, tri_off, ntris;   /* cached: in the static pool; seen: in the frame pool */
    float lo[3], hi[3];
};
#define TABLE_SIZE 16384    /* power of two */
static struct entry *table;
static u32 table_used;

#define MAX_CAMS 4
struct inst { float model[16]; u32 node_off, tri_off, ntris; int in_frame_pool; float lo[3], hi[3];
              u32 alpha_off; float alpha_ref;
              u64 iid, key; int mergeable, covered; u32 color, layer; };     /* iid: mesh + placement, for cached opaque meshes */

static struct {
    int enabled, want_reload;
    u32 frame, collect_frame;
    int finalized;
    float light_view[16];

    BvhNode *sn; BvhTri *st; u32 sn_count, st_count, sn_cap, st_cap;    /* static pool */
    /* Smooth normals, one NrmTri per triangle, in the same order as the triangle pools
       (sm: static pool, fm: frame pool); n4: the mesh being expanded, input order. */
    NrmTri *sm, *fm, *n4; u32 sm_cap, fm_cap, n4_cap, sm_uploaded, gpu_nrm_cap;
    u32 *cidx, cidx_cap;                            /* corner vertex indices of the mesh being expanded */
    GLuint buf_nrm;
    u32 f_smooth_inst;                              /* instances traced with smooth normals, last frame */
    u32 sn_uploaded, st_uploaded, dead_tris;
    BvhNode *fn; BvhTri *ft; u32 fn_count, ft_count, fn_cap, ft_cap;    /* frame pool */
    struct inst *inst; u32 ninst, inst_cap;
    float *tri9; u32 tri9_cap;
    u32 *uv3; u32 uv3_cap;
    BvhNode *tlas; float *boxes; float *inst_gpu; u32 tlas_cap, tlas_count;

    GLuint prog, fprog, tprog, aprog, buf_nodes, buf_tris, buf_tlas, buf_inst, buf_alpha, buf_stat, query;
    int big_stacks;         /* a ray overflowed the small traversal stacks once: use large ones */
    GLint f_eye, f_base, f_dx, f_dy, f_params, u_sky;
    GLint t_eye, t_base, t_dx, t_dy, t_hist, t_prev_vp, t_prev_eye;    /* the temporal pass */
    GLint a_eye, a_base, a_dx, a_dy, a_step;                           /* the indirect light's filter passes */
    GLint u_reflect;
    GLuint buf_mat;
    GLint u_caps, u_ncaps;
    GLint u_leaf; int leaf_on; float leaf_depth;   /* foliage (rt_set_foliage) */
    GLint u_char; int char_on; float char_depth, char_contact, char_reach;   /* characters (rt_set_characters) */
    int smooth;
    int specular; float specular_strength;          /* trace what glossy surfaces mirror */
    float clearcoat;                                /* strength of the clear coat highlight (0 = off) */
    int reflect; float reflect_strength;            /* trace what the sea mirrors */
    int denoise;                                    /* filter passes for the indirect light */
    u32 last_cam, last_cam_frame;                   /* the scene camera whose draws are going on */
    int filter_radius;
    int sky_rays; float sky_dist, sky_strength;     /* sky visibility (ambient occlusion) */
    float bounce;                                   /* strength of bounced sunlight, 0 = off */
    float accum;                                    /* weight of last frame's sky/bounce result, 0 = off */
    int gi; float gi_sky, gi_bounce, gi_tint;       /* traced sky light instead of constant ambient */
    float sun_col[3], amb_col[3]; u32 light_frame;  /* the game's light colours, and when they were last given */
    float water_y; u32 water_frame;                 /* the sea plane, and when it was last drawn */
    float water_sun[3];                             /* towards the sun, for the sea's glints (water.cg) */
    GLint u_sun_col, u_sky_gain, u_fallback, u_gi, u_prev_vp, u_prev_eye, u_albedo, u_sky_atlas, u_sky_tint;
    GLint u_gi_rate;
    GLint u_cloud; float cloud[3];                 /* moss: cloud shadows: strength, size, speed (rt_set_clouds) */
    int gi_rate; float gi_far;                      /* cheaper indirect light: see rt_set_gi_rate */
    u32 gpu_node_cap, gpu_tri_cap;
    GLint u_eye, u_base, u_dx, u_dy, u_sun, u_params, u_counts;
    int query_pending;

    /* One mask per scene camera per frame (main view, picture-in-picture). */
    /* raw: what the tracer writes (visibility, distance, normal). mask: the filtered result. */
    struct cam {
        float view[16], proj[16];
        GLuint raw, sky, mask[2], bounce[2], out, out_bounce;   /* mask/bounce: this frame's and last frame's */
        GLuint acc[2], mom[2];      /* accumulated over time, unfiltered: sky/indirect light; its moments, sun, frame count */
        GLuint tmp[2], refl;        /* filter passes' scratch; what the sea mirrors */
        int has_refl, tex_bound;
        u32 w, h, vw, vh;
        int ok, has_sky, cur, gi;
        GLuint tex_restore[3];
        u32 hist_frame; float prev_vp[16], prev_eye[3];         /* when and from where mask[cur] was made */
    } cams[MAX_CAMS];
    float scale;            /* mask size relative to the viewport */
    float max_pixels;       /* and its upper limit */
    u32 cam_frame, ncams, time_cam;
    float scene_proj[8][16]; u32 nscene_proj;      /* projections seen drawing the landscape */
    int samples; float radius;

    u32 f_built_static, f_built_frame, f_skipped, f_alpha_inst, f_alpha_skipped;
    u32 skip_logged;
    double p_hash, p_expand, p_build, p_total; u32 p_frames, p_built, p_tris, p_refit;   /* profile of the collect step */
    double p_normals;                               /* of which making smooth normals */
    double f_collect_us, f_finalize_us, f_trace_us;
} R = { 0 };

/* ---- hashing and the mesh table -------------------------------------------------- */
static u64 mix(u64 h, u64 v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 33);
}

static u64 hash_bytes(u64 h, const void *data, u32 n)
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
    return mix(h, n);
}

static struct entry *table_find(u64 key, int insert)
{
    u32 i, n;
    if (!table) table = calloc(TABLE_SIZE, sizeof(struct entry));
    if (!table) return NULL;
    if (!key) key = 1;
    for (i = (u32)(key >> 20) & (TABLE_SIZE - 1), n = 0; n < TABLE_SIZE; n++, i = (i + 1) & (TABLE_SIZE - 1)) {
        if (table[i].state && table[i].key == key) return &table[i];
        if (!table[i].state) {
            if (!insert || table_used > TABLE_SIZE * 3 / 4) return NULL;
            table[i].key = key;
            table_used++;
            return &table[i];
        }
    }
    return NULL;
}

/* Drop meshes not drawn for a while; compact the static pool when much of it is dead. */
static void table_sweep(void)
{
    struct entry *old = table, *e;
    u32 i, live_tris = 0;
    BvhNode *nn; BvhTri *nt; NrmTri *nm;
    u32 nn_count = 0, nt_count = 0;
    int compact;

    if (!old) return;
    for (i = 0; i < TABLE_SIZE; i++)
        if (old[i].state == 2 && R.frame - old[i].last_frame < 900) live_tris += old[i].ntris;
    compact = R.st_count > 4096 && live_tris < R.st_count - R.st_count / 4;
    table = calloc(TABLE_SIZE, sizeof(struct entry));
    if (!table) { table = old; return; }
    table_used = 0;
    nn = compact ? malloc((size_t)R.sn_cap * sizeof(BvhNode)) : NULL;
    nt = compact ? malloc((size_t)R.st_cap * sizeof(BvhTri)) : NULL;
    nm = compact ? malloc((size_t)(R.sm_cap > R.st_cap ? R.sm_cap : R.st_cap) * sizeof(NrmTri)) : NULL;
    if (compact && (!nn || !nt || !nm)) { free(nn); free(nt); free(nm); nn = NULL; nt = NULL; nm = NULL; compact = 0; }
    for (i = 0; i < TABLE_SIZE; i++) {
        u32 age = R.frame - old[i].last_frame;
        if (!old[i].state) continue;
        if (old[i].state == 1 && age > 4) continue;
        if (old[i].state == 2 && age >= 900 && compact) continue;
        e = table_find(old[i].key, 1);
        if (!e) continue;
        *e = old[i];
        if (compact && e->state == 2) {
            memcpy(nn + nn_count, R.sn + e->node_off, (size_t)e->nnodes * sizeof(BvhNode));
            memcpy(nt + nt_count, R.st + e->tri_off, (size_t)e->ntris * sizeof(BvhTri));
            memcpy(nm + nt_count, R.sm + e->tri_off, (size_t)e->ntris * sizeof(NrmTri));
            e->node_off = nn_count; e->tri_off = nt_count;
            nn_count += e->nnodes; nt_count += e->ntris;
        }
    }
    free(old);
    if (compact) {
        rt_log("rt: static pool compacted, %u -> %u triangles", R.st_count, nt_count);
        free(R.sn); free(R.st); free(R.sm);
        R.sn = nn; R.st = nt; R.sn_count = nn_count; R.st_count = nt_count;
        R.sm = nm; R.sm_cap = R.sm_cap > R.st_cap ? R.sm_cap : R.st_cap;
        R.sn_uploaded = R.st_uploaded = R.sm_uploaded = 0;
        R.collect_frame = 0xffffffff;       /* instances refer to old offsets: wait for a new shadow pass */
        R.ninst = 0;
        R.finalized = 0;
    }
}

/* ---- pools --------------------------------------------------------------------- */
static int grow_pool(void **p, u32 *cap, u32 need, size_t elem)
{
    void *q;
    u32 n;
    if (need <= *cap) return 1;
    n = need + need / 2 + 1024;
    q = realloc(*p, (size_t)n * elem);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

/* ---- collecting shadow casters --------------------------------------------------- */
/* Trees of meshes that change every frame (animated characters). The triangles move but
   the mesh stays the same, so its tree is kept and only the boxes are recomputed; a fresh
   tree is built now and then. Keyed by where the indices of the mesh come from. */
struct slot { u64 id; u32 ntris, nnodes, used, built; BvhNode *nodes; u32 *order; };
#define SLOTS 512
#define REBUILD_EVERY 300
static struct slot slots[SLOTS];

static struct slot *slot_find(u64 id, int insert)
{
    u32 i = (u32)(id >> 20) & (SLOTS - 1), n, victim = i;
    for (n = 0; n < 8; n++) {
        struct slot *sl = &slots[(i + n) & (SLOTS - 1)];
        if (sl->id == id) return sl;
        if (sl->used < slots[victim].used) victim = (i + n) & (SLOTS - 1);
    }
    if (!insert) return NULL;
    free(slots[victim].nodes); free(slots[victim].order);
    memset(&slots[victim], 0, sizeof slots[victim]);
    slots[victim].id = id;
    return &slots[victim];
}

static u32 index_at(const u8 *idata, u32 isz, u32 i)
{
    return isz == 2 ? ((const unsigned short *)idata)[i] : isz == 4 ? ((const u32 *)idata)[i] : idata[i];
}

/* Alpha-tested meshes. The shadow pass binds neither their texture nor their texture
   coordinates, so both are noted when the mesh is drawn in the main pass. */
struct alpha_mesh { u64 id; u32 tex, tc_buf, tc_stride; const void *tc_ptr; float ref; u32 frame; int has_tc, alpha, has_xf;
                    float xf[6];
                    int has_n; u32 n_buf, n_stride; const void *n_ptr; };   /* vertex normals (floats) */     /* xf: u' = u * xf[0] + v * xf[1] + xf[2], v' likewise with xf[3..5] */
#define ALPHA_MESHES 4096
static struct alpha_mesh alpha_meshes[ALPHA_MESHES];

static u64 mesh_identity(const struct rt_draw *d)
{
    u64 h = 0x13198A2E03707344ull;
    h = mix(mix(mix(h, d->v_buf), (u32)d->v_ptr), d->elem_buf);
    h = mix(mix(mix(mix(h, (u32)d->indices), d->count), d->first), d->mode);
    return h ? h : 1;
}

/* scatter.c (included at the end) */
static void sc_inject(const float *light_view);
static void sc_forget(void);
static void sc_shutdown(void);

static struct alpha_mesh *alpha_mesh_slot(u64 id, int insert)
{
    u32 i = (u32)(id >> 24) & (ALPHA_MESHES - 1), n, oldest = i;
    for (n = 0; n < 8; n++) {
        struct alpha_mesh *m = &alpha_meshes[(i + n) & (ALPHA_MESHES - 1)];
        if (m->id == id) return m;
        if (m->frame < alpha_meshes[oldest].frame || !m->id) oldest = (i + n) & (ALPHA_MESHES - 1);
    }
    if (!insert) return NULL;
    alpha_meshes[oldest].id = id;
    return &alpha_meshes[oldest];
}

static unsigned short layer_of_tex[65536];     /* texture id -> layer of the colour array (0 = none yet) */
static void layer_material_from_game(u32 layer, const float *mm);

void rt_on_main_alpha(const struct rt_draw *d)
{
    struct alpha_mesh *m;
    if (!R.enabled || !d->texture) return;
    m = alpha_mesh_slot(mesh_identity(d), 1);
    m->tex = d->texture;
    m->frame = R.frame;
    if (d->material && d->texture < 65536 && layer_of_tex[d->texture])
        layer_material_from_game(layer_of_tex[d->texture], d->material);
    m->has_tc = d->tc_on && d->tc_buf && d->tc_type == GL_FLOAT && d->tc_size >= 2;
    m->alpha = d->alpha_test;
    m->has_xf = 0;
    if (m->has_tc && d->tex_xf) {
        const float *x = d->tex_xf;
        m->xf[0] = x[0]; m->xf[1] = x[4]; m->xf[2] = x[12];
        m->xf[3] = x[1]; m->xf[4] = x[5]; m->xf[5] = x[13];
        m->has_xf = x[0] != 1.0f || x[4] != 0.0f || x[12] != 0.0f || x[1] != 0.0f || x[5] != 1.0f || x[13] != 0.0f;
    }
    m->has_n = d->n_on && d->n_type == GL_FLOAT;
    if (m->has_n) { m->n_buf = d->n_buf; m->n_ptr = d->n_ptr; m->n_stride = d->n_stride ? d->n_stride : 12; }
    else if (d->n_on) {
        static int logged;
        if (logged++ < 4) rt_log("rt: a lit draw has normals of type %#x (only floats are used)", d->n_type);
    }
    if (m->has_tc) {
        m->ref = d->alpha_ref;
        m->tc_buf = d->tc_buf; m->tc_ptr = d->tc_ptr;
        m->tc_stride = d->tc_stride ? d->tc_stride : d->tc_size * 4;
    }
}

/* Average colour of a material texture, as r | g << 8 | b << 16 | 1 << 24 (never 0). Used as
   the surface colour for bounced light. */
static u8 tex_known[65536];         /* something is cached for this texture id (see rt_texture_gone) */
static struct { u32 tex, rgb; } tex_colors[512];
static u32 n_tex_colors;

static u32 texture_color(u32 tex)
{
    GLint prev = 0, w = 0, h = 0, level = 0;
    u32 i, rgb = 0x01808080u;
    u8 *buf;
    for (i = 0; i < n_tex_colors; i++)
        if (tex_colors[i].tex == tex) return tex_colors[i].rgb;
    if (n_tex_colors == 512) return rgb;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* The smallest mip level that still has a few pixels is plenty for an average. */
    for (level = 0; level < 14; level++) {
        GLint nw = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level + 1, GL_TEXTURE_WIDTH, &nw);
        if (nw < 4) break;
    }
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
    buf = (w > 0 && h > 0 && w <= 4096 && h <= 4096) ? malloc((size_t)w * h * 3) : NULL;
    if (buf) {
        u64 sum[3] = { 0, 0, 0 };
        u32 n = (u32)w * (u32)h;
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glGetTexImage(GL_TEXTURE_2D, level, GL_RGB, GL_UNSIGNED_BYTE, buf);
        glPopClientAttrib();
        for (i = 0; i < n; i++) { sum[0] += buf[i * 3]; sum[1] += buf[i * 3 + 1]; sum[2] += buf[i * 3 + 2]; }
        rgb = (u32)(sum[0] / n) | (u32)(sum[1] / n) << 8 | (u32)(sum[2] / n) << 16 | 0x01000000u;
        free(buf);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    tex_colors[n_tex_colors].tex = tex;
    tex_colors[n_tex_colors++].rgb = rgb;
    if (tex < 65536) tex_known[tex] = 1;
    return rgb;
}

/* Colours of the material textures, for shading what a ray hits: every texture gets a
   128 x 128 layer (with mip levels, the smallest being its average colour) in one array
   texture. Layer 0 is mid grey (texture unknown), layer 1 white. */
#define LAYER_SIZE 128
#define LAYER_LEVELS 8
#define MAX_LAYERS 512
static struct { GLuint array; u32 used, nfree; unsigned short free_list[MAX_LAYERS]; u32 full_logged; } LY;
/* Material of each layer. It starts matte; the game's own material data fills it in when a
   mesh using the texture is drawn (specular power -> roughness, specular colour ->
   reflectivity, emissive colour), and wumrt_materials.ini overrides that per texture, found
   by a hash of the layer's pixels (texture ids and names are not stable or not known). */
static struct layer_mat {
    float rough, refl, metal, emis[3]; u32 hash; int from_file, logged;
    float coat;                     /* clear coat (wumrt_materials.ini 6th column, 0..1): toy plastic, car paint */
    float shell_h, shell_d;         /* grass or moss: relative height and blade density (0 = no shells) */
    int terrain_logged;
    u32 kind;                       /* what the terrain is made of (scatter.c: SK_*), 0 = not listed */
    float scat_d;                   /* rock or sand: how many stones, relative to the setting */
    u32 chr;                        /* on the characters (wumrt_characters.ini): 1 skin, 2 skin with the eyes, 3 eye */
} mats[MAX_LAYERS];
static int mats_dirty = 1;
static struct { u32 hash; float rough, refl, metal, emis, coat; } overrides[2048];
static u32 n_overrides;
static int overrides_loaded;
/* wumrt_surfaces.ini: what a terrain texture is made of, one per line:
       HASH KIND [HEIGHT [DENSITY]]
   KIND grass or moss gives it shell layers; HEIGHT and DENSITY scale shell_height and
   shell_density for this texture (grass 1 1, moss 0.45 1.6 unless given). KIND rock or sand
   (HASH rock [DENSITY]) gets scattered stones (scatter.c); grass and moss get tufts along
   their border. */
static struct { u32 hash, kind; float h, d; } surfaces[1024];
static u32 n_surfaces;

static void rt_reload_textures(void);

/* wumrt_characters.ini: the worms' textures, one per line: HASH skin|face|eye. skin gets the
   soft-skin lighting (light wrap, light through thin parts, rim light); face is a skin texture
   that also holds the eyes (catchlights on its near-white texels); eye is an eye of its own. */
static struct { u32 hash, kind; } char_tex[256];
static u32 n_chars;

static void load_characters(void)
{
    FILE *f = fopen("wumrt_characters.ini", "r");
    char line[256], kind[16];
    n_chars = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && n_chars < 256) {
        u32 h;
        if (sscanf(line, " %8x %15[a-z]", &h, kind) != 2) continue;
        char_tex[n_chars].hash = h;
        char_tex[n_chars].kind = !strcmp(kind, "skin") ? 1 : !strcmp(kind, "face") ? 2 : !strcmp(kind, "eye") ? 3 : !strcmp(kind, "foliage") ? 4 : !strcmp(kind, "accessory") ? 5 : 0;
        if (char_tex[n_chars].kind) n_chars++;
    }
    fclose(f);
    rt_log("rt: %u character textures read from wumrt_characters.ini", n_chars);
}

static void load_overrides(void)
{
    FILE *f = fopen("wumrt_materials.ini", "r");
    char line[256];
    overrides_loaded = 1;
    n_overrides = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && n_overrides < 2048) {
        u32 h; float a, b, c, e = 0, k = 0;
        if (sscanf(line, " %8x %f %f %f %f %f", &h, &a, &b, &c, &e, &k) >= 4) {
            overrides[n_overrides].hash = h; overrides[n_overrides].rough = a; overrides[n_overrides].refl = b;
            overrides[n_overrides].coat = k < 0 ? 0 : k > 1 ? 1 : k;
            overrides[n_overrides].metal = c; overrides[n_overrides++].emis = e;
        }
    }
    fclose(f);
    rt_log("rt: %u material overrides read from wumrt_materials.ini", n_overrides);
}

static void load_surfaces(void)
{
    FILE *f = fopen("wumrt_surfaces.ini", "r");
    char line[256], kind[16];
    n_surfaces = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && n_surfaces < 1024) {
        u32 h; float a = 0, b = 0;
        int n = sscanf(line, " %8x %15[a-z] %f %f", &h, kind, &a, &b);
        int moss;
        if (n < 2) continue;
        if (!strcmp(kind, "rock") || !strcmp(kind, "sand")) {
            surfaces[n_surfaces].hash = h;
            surfaces[n_surfaces].kind = kind[0] == 'r' ? 3 : 4;
            surfaces[n_surfaces].h = 0;
            surfaces[n_surfaces++].d = n >= 3 && a > 0 ? a : 1.0f;
            continue;
        }
        if (strcmp(kind, "grass") && strcmp(kind, "moss")) continue;
        moss = !strcmp(kind, "moss");
        surfaces[n_surfaces].hash = h;
        surfaces[n_surfaces].kind = moss ? 2 : 1;
        surfaces[n_surfaces].h = n >= 3 && a > 0 ? a : moss ? 0.45f : 1.0f;
        surfaces[n_surfaces++].d = n >= 4 && b > 0 ? b : moss ? 1.6f : 1.0f;
    }
    fclose(f);
    rt_log("rt: %u terrain textures read from wumrt_surfaces.ini", n_surfaces);
}

static void layer_material_reset(u32 layer, u32 hash)
{
    struct layer_mat *m = &mats[layer];
    u32 i;
    if (!overrides_loaded) { load_overrides(); load_surfaces(); load_characters(); }
    memset(m, 0, sizeof *m);
    m->rough = 1.0f;
    m->hash = hash;
    for (i = 0; i < n_chars; i++)
        if (char_tex[i].hash == hash) { m->chr = char_tex[i].kind; break; }
    for (i = 0; i < n_surfaces; i++)
        if (surfaces[i].hash == hash) {
            m->kind = surfaces[i].kind;
            if (m->kind <= 2) { m->shell_h = surfaces[i].h; m->shell_d = surfaces[i].d; }
            else m->scat_d = surfaces[i].d;
            break;
        }
    for (i = 0; i < n_overrides; i++)
        if (overrides[i].hash == hash) {
            m->rough = overrides[i].rough; m->refl = overrides[i].refl; m->metal = overrides[i].metal;
            m->emis[0] = m->emis[1] = m->emis[2] = overrides[i].emis;
            m->coat = overrides[i].coat;
            m->from_file = 1;
            break;
        }
    mats_dirty = 1;
}

void rt_reload_materials(void)
{
    u32 i;
    load_overrides();
    load_surfaces();
    load_characters();
    for (i = 2; i < LY.used; i++) layer_material_reset(i, mats[i].hash);
    rt_reload_textures();
}

/* The game's material for a mesh that uses this layer's texture. */
static void layer_material_from_game(u32 layer, const float *mm)
{
    struct layer_mat *m = &mats[layer];
    float power = mm[13], refl = 0.3f * mm[0] + 0.59f * mm[1] + 0.11f * mm[2];
    float rough = power > 0.5f ? (float)pow(2.0 / (power + 2.0), 0.25) : 1.0f;
    if (m->from_file) return;
    if (refl > 1.0f) refl = 1.0f;
    if (rough < 0.08f) rough = 0.08f;
    if (refl < 0.02f) rough = 1.0f;         /* no highlight at all: matte, whatever the power says */
    if (fabs(rough - m->rough) + fabs(refl - m->refl) + fabs(mm[4] - m->emis[0]) + fabs(mm[5] - m->emis[1]) +
        fabs(mm[6] - m->emis[2]) < 0.004) return;
    m->rough = rough; m->refl = refl;
    m->emis[0] = mm[4]; m->emis[1] = mm[5]; m->emis[2] = mm[6];
    mats_dirty = 1;
    if (m->logged < 2) {
        m->logged++;
        rt_log("rt: material %08x (layer %u): specular %.2f %.2f %.2f power %.1f emissive %.2f %.2f %.2f fresnel %.2f %.2f %.2f "
               "-> roughness %.2f reflectivity %.2f", m->hash, layer, mm[0], mm[1], mm[2], power, mm[4], mm[5], mm[6],
               mm[8], mm[9], mm[10], rough, refl);
    }
}
static unsigned short layer_of_tex[65536];
static u8 tex_known[65536];         /* something is cached for this texture id */

static void layer_upload(u32 layer, u8 *rgba)     /* rgba: LAYER_SIZE^2 texels; overwritten */
{
    u32 level, sz = LAYER_SIZE, x, y, k;
    for (level = 0; level < LAYER_LEVELS; level++, sz /= 2) {
        gl.TexSubImage3D(GL_TEXTURE_2D_ARRAY, (GLint)level, 0, 0, (GLint)layer, (GLsizei)sz, (GLsizei)sz, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (sz == 1) break;
        for (y = 0; y < sz / 2; y++)            /* next level, in place */
            for (x = 0; x < sz / 2; x++)
                for (k = 0; k < 4; k++) {
                    const u8 *a = rgba + ((size_t)(2 * y) * sz + 2 * x) * 4 + k;
                    rgba[((size_t)y * (sz / 2) + x) * 4 + k] = (u8)((a[0] + a[4] + a[(size_t)sz * 4] + a[(size_t)sz * 4 + 4] + 2) / 4);
                }
    }
}

static int layers_ready(void)
{
    GLint prev = 0;
    u8 *px;
    if (LY.array) return 1;
    px = malloc(LAYER_SIZE * LAYER_SIZE * 4);
    if (!px) return 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev);
    glGenTextures(1, &LY.array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, LY.array);
    gl.TexStorage3D(GL_TEXTURE_2D_ARRAY, LAYER_LEVELS, GL_RGBA8, LAYER_SIZE, LAYER_SIZE, MAX_LAYERS);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    memset(px, 128, LAYER_SIZE * LAYER_SIZE * 4); layer_upload(0, px);
    memset(px, 255, LAYER_SIZE * LAYER_SIZE * 4); layer_upload(1, px);
    layer_material_reset(0, 0); layer_material_reset(1, 0);
    glPopClientAttrib();
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)prev);
    free(px);
    LY.used = 2; LY.nfree = 0;
    memset(layer_of_tex, 0, sizeof layer_of_tex);
    return 1;
}

/* ---- replacement textures (see rt.h) ---- */
static GLuint nrm_local[65536];
static GLuint *nrm_tex = nrm_local; /* game texture id -> our normal map for it (kept across core reloads) */
/* Per game texture, kept by the loader across core reloads: the hash of its original
   pixels (once a replacement is loaded the texture no longer has them) and whether the
   replacement colour image is already in it. */
struct tex_memory { u32 hash[65536]; u8 known[65536], replaced[65536]; GLuint nrm[65536]; };
static struct tex_memory *TM;
static u32 n_replaced, n_normals;

static u8 *read_wtx(const char *kind, u32 hash, u32 *w, u32 *h)
{
    char name[64];
    u32 hdr[3];
    u8 *px = NULL;
    FILE *f;
    sprintf(name, "wumrt_textures/%08x_%s.wtx", hash, kind);
    f = fopen(name, "rb");
    if (!f) return NULL;
    if (fread(hdr, 4, 3, f) == 3 && hdr[0] == 0x31585457 && hdr[1] && hdr[2] && hdr[1] <= 8192 && hdr[2] <= 8192) {
        size_t n = (size_t)hdr[1] * hdr[2] * 4;
        px = malloc(n);
        if (px && fread(px, 1, n, f) != n) { free(px); px = NULL; }
        *w = hdr[1]; *h = hdr[2];
    }
    fclose(f);
    return px;
}

/* How replacement textures go to the GPU (wumrt.ini `tex_compress`, `tex_max`). The game is
   32-bit: the driver keeps its own copies of textures in the game's address space, so a
   level of 1024 x 1024 RGBA8 maps came to ~1.3 GB virtual. tex_compress 1 lets the driver
   store colour maps S3TC-compressed (DXT1 for opaque ones: 1/8 the size; DXT5 with alpha:
   1/4), 2 does the same for normal maps (DXT5; the alpha holds the relief height).
   tex_max halves anything bigger than it on load (0 = no limit), so 2048 maps can be
   installed and still capped. */
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
static int tex_compress, tex_max;
static double tex_upload_us, tex_bytes;
static u32 n_packed;      /* replacements loaded from texpack.py's .wtc files */

void rt_set_textures(int compress, int max_size)
{
    tex_compress = compress < 0 ? 0 : compress > 2 ? 2 : compress;
    tex_max = max_size < 0 ? 0 : max_size;
}

/* Halve an RGBA8 picture in place (2 x 2 box) until it fits tex_max. */
static void shrink_to_max(u8 *px, u32 *w, u32 *h)
{
    while (tex_max > 0 && (*w > (u32)tex_max || *h > (u32)tex_max) && *w >= 2 && *h >= 2) {
        u32 nw = *w / 2, nh = *h / 2, x, y, k;
        for (y = 0; y < nh; y++)
            for (x = 0; x < nw; x++)
                for (k = 0; k < 4; k++) {
                    const u8 *a = px + (((size_t)(2 * y) * *w + 2 * x) * 4 + k), *b = a + (size_t)*w * 4;
                    px[((size_t)y * nw + x) * 4 + k] = (u8)((a[0] + a[4] + b[0] + b[4] + 2) / 4);
                }
        *w = nw; *h = nh;
    }
}

/* A replacement packed by texpack.py: HASH_KIND.wtc = 'WTC1', width, height, GL format (DXT1 or
   DXT5), number of mip levels, then per level its byte count and its blocks (rows bottom-up, as
   OpenGL wants them). Uploaded as it is: no compression work on the driver's side, which took
   ~60 ms per 1024 map and ~250 ms per 2048 map. Used only when it is at least as new as
   HASH_KIND.wtx (otherwise that was changed after packing, e.g. heights written into a normal
   map's alpha) and its mip chain reaches 1 x 1. tex_max skips the bigger levels. 1 if loaded. */
static int upload_packed(const char *kind, u32 hash)
{
    char name[64], wname[64];
    struct _stat a, b;
    u32 hdr[5], lv, size, skip = 0, w, h;
    u8 *buf = NULL;
    FILE *f;
    int ok = 0;
    double t0 = qpc_us(), bytes = 0;
    sprintf(name, "wumrt_textures/%08x_%s.wtc", hash, kind);
    sprintf(wname, "wumrt_textures/%08x_%s.wtx", hash, kind);
    if (_stat(name, &a) != 0 || (_stat(wname, &b) == 0 && b.st_mtime > a.st_mtime)) return 0;
    f = fopen(name, "rb");
    if (!f) return 0;
    if (fread(hdr, 4, 5, f) == 5 && hdr[0] == 0x31435457 && hdr[1] && hdr[2] && hdr[1] <= 8192 && hdr[2] <= 8192 &&
        (hdr[3] == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || hdr[3] == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) &&
        hdr[4] >= 1 && hdr[4] <= 14 && ((hdr[1] > hdr[2] ? hdr[1] : hdr[2]) >> (hdr[4] - 1)) == 1) {
        w = hdr[1]; h = hdr[2];
        while (tex_max > 0 && (w > (u32)tex_max || h > (u32)tex_max) && skip + 1 < hdr[4]) {
            skip++; w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
        }
        ok = 1;
        for (lv = 0; lv < hdr[4]; lv++) {
            u32 lw = hdr[1] >> lv ? hdr[1] >> lv : 1, lh = hdr[2] >> lv ? hdr[2] >> lv : 1;
            u32 want = ((lw + 3) / 4) * ((lh + 3) / 4) * (hdr[3] == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ? 8 : 16);
            u8 *nb;
            if (fread(&size, 4, 1, f) != 1 || size != want || !(nb = realloc(buf, size))) { ok = 0; break; }
            buf = nb;
            if (fread(buf, 1, size, f) != size) { ok = 0; break; }
            if (lv < skip) continue;
            gl.CompressedTexImage2D(GL_TEXTURE_2D, (GLint)(lv - skip), (GLenum)hdr[3], (GLsizei)lw, (GLsizei)lh, 0,
                                    (GLsizei)size, buf);
            bytes += size;
        }
    }
    fclose(f);
    free(buf);
    if (!ok) {
        rt_log("rt: %s is damaged; using the .wtx", name);
        return 0;
    }
    tex_bytes += bytes;
    tex_upload_us += qpc_us() - t0;
    n_packed++;
    return 1;
}

/* Load an RGBA8 picture into the bound texture (all mip levels). */
static void upload_replacement(u8 *px, u32 w, u32 h, int compress)
{
    double t0 = qpc_us();
    GLenum ifmt = GL_RGBA8;
    double bpp = 4.0;
    shrink_to_max(px, &w, &h);
    if (compress && (w % 4) == 0 && (h % 4) == 0) {
        size_t i, n = (size_t)w * h;
        int opaque = 1;
        for (i = 0; i < n; i++)
            if (px[i * 4 + 3] < 255) { opaque = 0; break; }
        ifmt = opaque ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        bpp = opaque ? 0.5 : 1.0;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gl.GenerateMipmap(GL_TEXTURE_2D);
    tex_bytes += (double)w * h * bpp * 4.0 / 3.0;
    tex_upload_us += qpc_us() - t0;
}

/* Called with the game's texture bound and byte-aligned unpacking set. */
static void load_replacements(u32 tex, u32 hash)
{
    u32 w = 0, h = 0;
    u8 *px = NULL;
    if (!(TM && tex < 65536 && TM->replaced[tex])) {
        if (tex_compress >= 1 && upload_packed("c", hash)) {
            if (TM && tex < 65536) TM->replaced[tex] = 1;
            n_replaced++;
        } else px = read_wtx("c", hash, &w, &h);
    }
    if (px) {
        if (TM && tex < 65536) TM->replaced[tex] = 1;
        upload_replacement(px, w, h, tex_compress >= 1);
        free(px);
        n_replaced++;
    }
    px = nrm_tex[tex] ? NULL : read_wtx("n", hash, &w, &h);     /* an earlier core may have made it */
    if (px) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        if (!(tex_compress >= 2 && upload_packed("n", hash))) upload_replacement(px, w, h, tex_compress >= 2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, tex);
        nrm_tex[tex] = t;
        free(px);
        n_normals++;
    }
    if ((n_replaced + n_normals) && ((n_replaced + n_normals) & (n_replaced + n_normals - 1)) == 0)
        rt_log("rt: replacement textures loaded so far: %u colour, %u normal maps, %u of them packed (%.0f MB on the GPU, "
               "%.0f ms uploading, compress %d, max %d)", n_replaced, n_normals, n_packed, tex_bytes / 1048576.0,
               tex_upload_us / 1000.0, tex_compress, tex_max);
}

/* Look in wumrt_textures again for every texture in use (files made while the game runs). */
static void rt_reload_textures(void)
{
    GLint prev = 0;
    u32 id;
    if (!LY.array || gl_state <= 0) return;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    for (id = 1; id < 65536; id++) {
        if (!layer_of_tex[id]) continue;
        if (nrm_tex[id]) { glDeleteTextures(1, &nrm_tex[id]); nrm_tex[id] = 0; }
        glBindTexture(GL_TEXTURE_2D, id);
        load_replacements(id, mats[layer_of_tex[id]].hash);
    }
    glPopClientAttrib();
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
}

GLuint rt_normal_map(u32 tex)
{
    return tex < 65536 ? nrm_tex[tex] : 0;
}

/* Called for every terrain draw. The first time a texture shows up on terrain its hash goes
   to the log, which is how the textures to list in wumrt_surfaces.ini are found. */
static u32 layer_for_texture(u32 tex);

int rt_shell(u32 tex, float *out)
{
    struct layer_mat *m;
    if (!tex || tex >= 65536) return 0;
    /* The tracer makes a texture's layer (and with it the hash) when it first needs its
       colours; with tracing off nobody else does. */
    if (!layer_of_tex[tex] && (!gl_init() || !layer_for_texture(tex))) return 0;
    m = &mats[layer_of_tex[tex]];
    if (!m->terrain_logged) {
        m->terrain_logged = 1;
        rt_log("rt: terrain texture %08x%s", m->hash, m->shell_h > 0 ? " (shell layers)" : "");
    }
    if (m->shell_h <= 0) return 0;
    out[0] = m->shell_h; out[1] = m->shell_d;
    return 1;
}

/* grade (post.c / grade.c): a terrain texture's hash, to recognise the level. */
u32 rt_terrain_hash(u32 tex)
{
    if (!tex || tex >= 65536) return 0;
    if (!layer_of_tex[tex] && (!gl_init() || !layer_for_texture(tex))) return 0;
    return mats[layer_of_tex[tex]].hash;
}

int rt_char_kind(u32 tex)
{
    if (!tex || tex >= 65536) return 0;
    if (!layer_of_tex[tex] && (!gl_init() || !layer_for_texture(tex))) return 0;
    return (int)mats[layer_of_tex[tex]].chr;
}

/* The layer holding a texture's colours; made on first use. 0 if there is none. */
static u32 layer_for_texture(u32 tex)
{
    GLint prev = 0, prev_arr = 0, w = 0, h = 0, level = 0;
    u32 layer, x, y;
    u8 *src, *dst;
    if (!tex || tex >= 65536 || gl_state <= 0) return 0;
    if (layer_of_tex[tex]) return layer_of_tex[tex];
    if (!layers_ready()) return 0;
    if (LY.nfree) layer = LY.free_list[--LY.nfree];
    else if (LY.used < MAX_LAYERS) layer = LY.used++;
    else {
        if (!LY.full_logged++) rt_log("rt: more than %d material textures; the rest are treated as grey", MAX_LAYERS);
        return 0;
    }
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* Read the smallest mip level that still has LAYER_SIZE texels a side. */
    for (level = 0; level < 14; level++) {
        GLint nw = 0, nh = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level + 1, GL_TEXTURE_WIDTH, &nw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level + 1, GL_TEXTURE_HEIGHT, &nh);
        if (nw < LAYER_SIZE || nh < LAYER_SIZE) break;
    }
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
    src = (w > 0 && h > 0 && w <= 4096 && h <= 4096) ? malloc((size_t)w * h * 4) : NULL;
    dst = src ? malloc(LAYER_SIZE * LAYER_SIZE * 4) : NULL;
    if (dst) {
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, src);
        for (y = 0; y < LAYER_SIZE; y++)
            for (x = 0; x < LAYER_SIZE; x++) {
                /* Average of the source texels this one covers (at least one). */
                u32 x0 = x * (u32)w / LAYER_SIZE, x1 = (x + 1) * (u32)w / LAYER_SIZE;
                u32 y0 = y * (u32)h / LAYER_SIZE, y1 = (y + 1) * (u32)h / LAYER_SIZE, xx, yy, n = 0, k;
                u32 sum[4] = { 0, 0, 0, 0 };
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                for (yy = y0; yy < y1; yy++)
                    for (xx = x0; xx < x1; xx++, n++)
                        for (k = 0; k < 4; k++) sum[k] += src[((size_t)yy * (u32)w + xx) * 4 + k];
                for (k = 0; k < 4; k++) dst[((size_t)y * LAYER_SIZE + x) * 4 + k] = (u8)(sum[k] / n);
            }
        {
            u32 hash = (u32)hash_bytes(0x811C9DC5u, dst, LAYER_SIZE * LAYER_SIZE * 4);
            if (TM) {                   /* an earlier core saw the original pixels */
                if (TM->known[tex]) hash = TM->hash[tex];
                else { TM->hash[tex] = hash; TM->known[tex] = 1; }
            }
            layer_material_reset(layer, hash);
        }
        load_replacements(tex, mats[layer].hash);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev_arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, LY.array);
        layer_upload(layer, dst);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)prev_arr);
        glPopClientAttrib();
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    free(src);
    if (!dst) { LY.free_list[LY.nfree++] = (unsigned short)layer; return 0; }
    free(dst);
    layer_of_tex[tex] = (unsigned short)layer;
    tex_known[tex] = 1;
    return layer;
}

/* Alpha channels of the textures those meshes use, reduced to at most 128 texels a side.
   alpha_words: per texture, width, height, then the alpha bytes packed four to a word.
   Offset 0 means "opaque". */
#define GL_TEXTURE_ALPHA_SIZE 0x805F
static struct { u32 tex, off; } alpha_texs[256];
static u32 n_alpha_texs;
static u32 *alpha_words;
static u32 alpha_count = 1, alpha_cap, alpha_uploaded;

static u32 alpha_texture(u32 tex)
{
    GLint prev = 0, w = 0, h = 0, abits = 0;
    u32 i, f = 1, ow, oh, off = 0, x, y, lo = 255;
    u8 *buf;

    for (i = 0; i < n_alpha_texs; i++)
        if (alpha_texs[i].tex == tex) return alpha_texs[i].off;
    if (n_alpha_texs == 256) return 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &abits);
    if (w <= 0 || h <= 0) {
        /* No image yet (seen right after a level change, when the id is about to be filled):
           decide later instead of remembering "opaque" for an id that may get an alpha mask. */
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        return 0;
    }
    buf = (w <= 8192 && h <= 8192 && abits > 0) ? malloc((size_t)w * h) : NULL;
    if (buf) {
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_ALPHA, GL_UNSIGNED_BYTE, buf);
        glPopClientAttrib();
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    if (buf) {
        while ((u32)w / f > 128 || (u32)h / f > 128) f *= 2;
        ow = (u32)w / f ? (u32)w / f : 1; oh = (u32)h / f ? (u32)h / f : 1;
        if (grow_pool((void **)&alpha_words, &alpha_cap, alpha_count + 2 + (ow * oh + 3) / 4, sizeof(u32))) {
            u32 *dst = alpha_words + alpha_count;
            if (alpha_count == 1) alpha_words[0] = 0;
            memset(dst, 0, (2 + (ow * oh + 3) / 4) * sizeof(u32));
            dst[0] = ow; dst[1] = oh;
            for (y = 0; y < oh; y++)
                for (x = 0; x < ow; x++) {
                    u32 sum = 0, n = 0, xx, yy, a;
                    for (yy = y * f; yy < (y + 1) * f && yy < (u32)h; yy++)
                        for (xx = x * f; xx < (x + 1) * f && xx < (u32)w; xx++) { sum += buf[yy * (u32)w + xx]; n++; }
                    a = n ? sum / n : 255;
                    if (a < lo) lo = a;
                    dst[2 + (y * ow + x) / 4] |= a << (((y * ow + x) & 3) * 8);
                }
            if (lo < 250) {         /* otherwise the texture is opaque after all */
                off = alpha_count;
                alpha_count += 2 + (ow * oh + 3) / 4;
            }
        }
        free(buf);
    }
    rt_log("rt: texture %u (%dx%d, %d alpha bits) -> %s", tex, w, h, abits, off ? "alpha mask" : "opaque");
    alpha_texs[n_alpha_texs].tex = tex;
    alpha_texs[n_alpha_texs++].off = off;
    if (tex < 65536) tex_known[tex] = 1;
    return off;
}

void rt_texture_gone(GLuint id)
{
    u32 i;
    fx_texture_gone(id);
    if (TM && id < 65536) { TM->known[id] = 0; TM->replaced[id] = 0; TM->hash[id] = 0; }
    if (id >= 65536 || !tex_known[id]) return;
    tex_known[id] = 0;
    for (i = 0; i < n_tex_colors; i++)
        if (tex_colors[i].tex == id) { tex_colors[i] = tex_colors[--n_tex_colors]; break; }
    for (i = 0; i < n_alpha_texs; i++)
        if (alpha_texs[i].tex == id) { alpha_texs[i] = alpha_texs[--n_alpha_texs]; break; }
    if (nrm_tex[id]) { glDeleteTextures(1, &nrm_tex[id]); nrm_tex[id] = 0; }
    if (layer_of_tex[id]) {
        if (LY.nfree < MAX_LAYERS) LY.free_list[LY.nfree++] = layer_of_tex[id];
        layer_of_tex[id] = 0;
    }
}

/* A unit vector as two 16-bit signed numbers (octahedral mapping); the shader decodes it. */
static u32 oct_pack(float x, float y, float z)
{
    float s = fabsf(x) + fabsf(y) + fabsf(z), a, b;
    int ia, ib;
    if (!(s > 1e-20f)) { x = 0; y = 0; z = 1; s = 1; }
    a = x / s; b = y / s;
    if (z < 0) { float ox = a, oy = b; a = (1 - fabsf(oy)) * (ox >= 0 ? 1.0f : -1.0f); b = (1 - fabsf(ox)) * (oy >= 0 ? 1.0f : -1.0f); }
    ia = (int)floorf(a * 32767.0f + 0.5f); ib = (int)floorf(b * 32767.0f + 0.5f);
    return ((u32)ia & 0xFFFFu) | ((u32)ib & 0xFFFFu) << 16;
}

static void oct_unpack(u32 p, float *n)
{
    float a = (float)(short)(p & 0xFFFFu) / 32767.0f, b = (float)(short)(p >> 16) / 32767.0f, z = 1 - fabsf(a) - fabsf(b), l;
    if (z < 0) { float oa = a; a = (1 - fabsf(b)) * (oa >= 0 ? 1.0f : -1.0f); b = (1 - fabsf(oa)) * (b >= 0 ? 1.0f : -1.0f); }
    l = sqrtf(a * a + b * b + z * z);
    n[0] = a / l; n[1] = b / l; n[2] = z / l;
}

/* Normals of the triangles just built into a pool, in the order the builder put them
   (order[i] = which input triangle output triangle i is); none if n4 is NULL. */
static void put_normals(NrmTri *dst, const NrmTri *n4, const u32 *order, u32 ntri)
{
    u32 i;
    if (!n4 || !order) { memset(dst, 0, (size_t)ntri * sizeof(NrmTri)); return; }
    for (i = 0; i < ntri; i++) dst[i] = n4[order[i]];
}

/* Smooth normals made from the mesh itself, for meshes whose own normals cannot be read when
   they are collected (the skinned characters' normal arrays are only filled for the main
   pass). Corners at the same position are welded; each corner gets the area-weighted mean of
   the face normals around its vertex, counting only faces within 60 degrees of its own, so
   hard edges (a hat's brim, a crate's corners) stay hard. tri9: ntri triangles, 9 floats. */
static struct { u32 *slot, *vid, *cnt, *start, *list; float *fn; u32 slot_cap, vid_cap, v_cap, start_cap, list_cap, fn_cap; } SN;

static int compute_normals(const float *tri9, u32 ntri, NrmTri *out)
{
    u32 nc = ntri * 3, cap = 64, nv = 0, i, f, k;
    if (!ntri) return 0;
    while (cap < nc * 2) cap *= 2;
    if (!grow_pool((void **)&SN.slot, &SN.slot_cap, cap, sizeof(u32)) ||
        !grow_pool((void **)&SN.vid, &SN.vid_cap, nc, sizeof(u32)) ||
        !grow_pool((void **)&SN.cnt, &SN.v_cap, nc + 1, sizeof(u32)) ||
        !grow_pool((void **)&SN.start, &SN.start_cap, nc + 1, sizeof(u32)) ||
        !grow_pool((void **)&SN.fn, &SN.fn_cap, ntri * 6, sizeof(float))) return 0;
    if (!grow_pool((void **)&SN.list, &SN.list_cap, nc + 1, sizeof(u32))) return 0;
    memset(SN.slot, 0xFF, (size_t)cap * sizeof(u32));
    /* weld: slot holds a corner index whose position is that vertex's */
    for (i = 0; i < nc; i++) {
        const u32 *p = (const u32 *)(tri9 + (size_t)i * 3);
        u32 h = (p[0] * 73856093u ^ p[1] * 19349663u ^ p[2] * 83492791u) & (cap - 1);
        for (;;) {
            u32 s = SN.slot[h];
            if (s == 0xFFFFFFFFu) { SN.slot[h] = i; SN.vid[i] = nv++; break; }
            if (!memcmp(tri9 + (size_t)s * 3, p, 12)) { SN.vid[i] = SN.vid[s]; break; }
            h = (h + 1) & (cap - 1);
        }
    }
    /* face normals: unnormalised (area weight) and unit */
    for (f = 0; f < ntri; f++) {
        const float *a = tri9 + (size_t)f * 9;
        float e1[3] = { a[3] - a[0], a[4] - a[1], a[5] - a[2] }, e2[3] = { a[6] - a[0], a[7] - a[1], a[8] - a[2] };
        float *n = SN.fn + (size_t)f * 6, l;
        n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        n[3] = l > 1e-30f ? n[0] / l : 0; n[4] = l > 1e-30f ? n[1] / l : 0; n[5] = l > 1e-30f ? n[2] / l : 0;
    }
    /* faces around each vertex */
    memset(SN.cnt, 0, (size_t)(nv + 1) * sizeof(u32));
    for (i = 0; i < nc; i++) SN.cnt[SN.vid[i]]++;
    SN.start[0] = 0;
    for (k = 0; k < nv; k++) SN.start[k + 1] = SN.start[k] + SN.cnt[k];
    memset(SN.cnt, 0, (size_t)(nv + 1) * sizeof(u32));
    for (i = 0; i < nc; i++) { u32 v = SN.vid[i]; SN.list[SN.start[v] + SN.cnt[v]++] = i / 3; }
    for (f = 0; f < ntri; f++) {
        const float *fu = SN.fn + (size_t)f * 6 + 3;
        u32 w[3];
        for (k = 0; k < 3; k++) {
            u32 v = SN.vid[f * 3 + k], j, guard = 0;
            float s[3] = { 0, 0, 0 };
            for (j = SN.start[v]; j < SN.start[v + 1] && guard < 256; j++, guard++) {
                const float *g = SN.fn + (size_t)SN.list[j] * 6;
                if (g[3] * fu[0] + g[4] * fu[1] + g[5] * fu[2] > 0.5f) { s[0] += g[0]; s[1] += g[1]; s[2] += g[2]; }
            }
            if (s[0] * s[0] + s[1] * s[1] + s[2] * s[2] < 1e-30f) { s[0] = fu[0]; s[1] = fu[1]; s[2] = fu[2]; }
            w[k] = oct_pack(s[0], s[1], s[2]);
        }
        out[f].a = w[0]; out[f].b = w[1]; out[f].c = w[2];
        out[f].flag = (fu[0] != 0 || fu[1] != 0 || fu[2] != 0) ? 1u : 0u;
    }
    return 1;
}

/* The same for an indexed mesh, welded by vertex index instead of position: a vertex index is
   what the game's own normals belong to, so its hard edges and seams (where the artist split
   vertices) come out as the game shades them, and nothing needs hashing. cidx: three vertex
   indices per triangle, all <= maxi. */
static float *vsum; static u32 vsum_cap;
static int compute_normals_indexed(const float *tri9, const u32 *cidx, u32 ntri, u32 maxi, NrmTri *out)
{
    u32 f, k;
    if (!ntri || !grow_pool((void **)&vsum, &vsum_cap, (maxi + 1) * 3, sizeof(float))) return 0;
    memset(vsum, 0, (size_t)(maxi + 1) * 3 * sizeof(float));
    for (f = 0; f < ntri; f++) {
        const float *a = tri9 + (size_t)f * 9;
        float e1[3] = { a[3] - a[0], a[4] - a[1], a[5] - a[2] }, e2[3] = { a[6] - a[0], a[7] - a[1], a[8] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        for (k = 0; k < 3; k++) {
            float *v = vsum + (size_t)cidx[f * 3 + k] * 3;
            v[0] += n[0]; v[1] += n[1]; v[2] += n[2];
        }
    }
    for (f = 0; f < ntri; f++) {
        const float *a = tri9 + (size_t)f * 9;
        float e1[3] = { a[3] - a[0], a[4] - a[1], a[5] - a[2] }, e2[3] = { a[6] - a[0], a[7] - a[1], a[8] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        u32 w[3];
        for (k = 0; k < 3; k++) {
            const float *v = vsum + (size_t)cidx[f * 3 + k] * 3;
            /* a vertex whose faces cancel out (a two-sided sheet) keeps the face's own normal */
            w[k] = v[0] * n[0] + v[1] * n[1] + v[2] * n[2] > 0 ? oct_pack(v[0], v[1], v[2]) : oct_pack(n[0], n[1], n[2]);
        }
        out[f].a = w[0]; out[f].b = w[1]; out[f].c = w[2];
        out[f].flag = n[0] != 0 || n[1] != 0 || n[2] != 0;
    }
    return 1;
}

static u32 pack_uv(float u, float v)
{
    int a = (int)(u * 1024.0f + 0.5f), b = (int)(v * 1024.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 16383) a = 16383;
    if (b < 0) b = 0;
    if (b > 16383) b = 16383;
    return (u32)a << 18 | (u32)b << 4;
}

static const u8 *vdata_hint(const struct rt_draw *d) { return (const u8 *)d->v_ptr; }

void rt_on_caster(const struct rt_draw *d, const float *model, const float *light_view)
{
    GLenum mode = d->mode;
    u32 first = d->first, count = d->count, arrays = (u32)d->arrays;
    u32 stride, vbuf = d->v_buf, ebuf = d->elem_buf, isz = 0, maxi = 0, vavail = 0xffffffffu, ntri = 0, i, nn;
    u32 alpha_off = 0, tc_stride = 0, tc_avail = 0, n_stride = 0, n_avail = 0xffffffffu, n_plain = 0;
    const u8 *vdata, *idata = NULL, *tcdata = NULL, *ndata = NULL;
    const void *indices = d->indices;
    u64 key = 0x243F6A8885A308D3ull;
    struct entry *e;
    struct inst *in;
    struct alpha_mesh *am = NULL;
    double t0;
    int promote, why = 0;
    u32 color = 0, layer = 0;
    const float *xf = NULL;

    if (!R.enabled || rt_stats.error || count < 3) return;
    if (mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP && mode != GL_TRIANGLE_FAN && mode != GL_QUADS) return;
    t0 = qpc_us();
    if (R.collect_frame != R.frame) {       /* first caster of a new shadow pass */
        R.collect_frame = R.frame;
        R.ninst = 0;
        R.fn_count = R.ft_count = 0;
    }
    R.finalized = 0;
    memcpy(R.light_view, light_view, sizeof R.light_view);
    if (!d->static_key) sc_inject(light_view);      /* the scattered stones and tufts, once per pass */

    if (!d->v_on) { why = 1; goto skip; }
    if (d->v_type != GL_FLOAT) { why = 2; goto skip; }
    if (d->v_size < 3) { why = 3; goto skip; }
    stride = d->v_stride ? d->v_stride : d->v_size * 4;

    if (d->static_key || d->own) {
        /* Geometry of our own (scatter.c, round.c): its texture and coordinates come with it.
           With a static key it never changes under that key: no hashing. */
        key = mix(key, d->static_key);
        if (d->texture) { color = texture_color(d->texture); layer = layer_for_texture(d->texture); }
        if (d->tc_on && d->tc_ptr && layer > 1) {
            static float sxf[6];
            tcdata = d->tc_ptr; tc_stride = d->tc_stride; tc_avail = 0xffffffffu;
            if (d->tex_xf) {
                const float *x = d->tex_xf;
                sxf[0] = x[0]; sxf[1] = x[4]; sxf[2] = x[12]; sxf[3] = x[1]; sxf[4] = x[5]; sxf[5] = x[13];
                xf = sxf;
            }
        }
    }
    am = (d->static_key || d->own) ? NULL : alpha_mesh_slot(mesh_identity(d), 0);     /* what the main pass said about this mesh */
    if (am) { am->frame = R.frame; color = texture_color(am->tex); layer = layer_for_texture(am->tex); }
    if (d->alpha_test) {
        /* Without the mesh's texture it would cast a solid shadow: better none. */
        if (!am || !am->has_tc) { R.f_alpha_skipped++; why = 20; goto skip; }
        alpha_off = alpha_texture(am->tex);
    }
    if (am && am->has_tc && (alpha_off || layer > 1)) {
        /* Texture coordinates: for the alpha test, and for the colour where a ray hits. */
        struct shadow_buf *b = am->tc_buf < MAX_BUF ? bufs[am->tc_buf] : NULL;
        if (!b || !b->data || (u32)am->tc_ptr >= b->size) {
            if (alpha_off) { R.f_alpha_skipped++; why = 21; goto skip; }
        } else {
            tcdata = b->data + (u32)am->tc_ptr;
            tc_avail = b->size - (u32)am->tc_ptr;
            tc_stride = am->tc_stride;
            if (am->has_xf) xf = am->xf;
            key = mix(mix(mix(mix(key, am->tc_buf), b->version), (u32)am->tc_ptr), 0xA1FAu);
        }
    }
    if (R.smooth && am && am->has_n) {
        /* Smooth normals, as the main pass gave them (the shadow pass binds none). */
        n_stride = am->n_stride;
        if (am->n_buf) {
            struct shadow_buf *b = am->n_buf < MAX_BUF ? bufs[am->n_buf] : NULL;
            if (b && b->data && (u32)am->n_ptr < b->size) {
                ndata = b->data + (u32)am->n_ptr;
                n_avail = b->size - (u32)am->n_ptr;
                key = mix(mix(mix(mix(key, am->n_buf), b->version), (u32)am->n_ptr), 0x4E4Du);
            }
        } else if (!vbuf && am->n_ptr) {
            /* Skinned meshes: positions and normals in plain memory, written by the game before
               the shadow pass. Interleaved with the positions they are covered by the
               positions' hash; a separate array is hashed itself below. */
            ndata = am->n_ptr;
            n_plain = (const u8 *)am->n_ptr >= vdata_hint(d) && (const u8 *)am->n_ptr < vdata_hint(d) + stride ? 1 : 2;
            key = mix(mix(key, n_plain), 0x4E4Eu);
        }
    }
    if (R.smooth && d->own && d->n_on && d->n_ptr && !d->n_buf && d->n_type == GL_FLOAT) {
        /* Our own mesh (round.c) brings its normals; with a static key they are covered by it. */
        ndata = d->n_ptr;
        n_stride = d->n_stride ? d->n_stride : 12;
        n_plain = d->static_key ? 1 : 2;
        key = mix(mix(key, n_plain), 0x4E4Fu);
    }

    if (vbuf) {
        struct shadow_buf *b = vbuf < MAX_BUF ? bufs[vbuf] : NULL;
        if (!b || !b->data || (u32)d->v_ptr >= b->size) { why = 4; goto skip; }
        vdata = b->data + (u32)d->v_ptr;
        vavail = b->size - (u32)d->v_ptr;
        key = mix(mix(mix(key, vbuf), b->version), (u32)d->v_ptr);
    } else {
        vdata = d->v_ptr;
        if (!vdata) { why = 5; goto skip; }
    }
    if (!arrays) {
        isz = d->itype == GL_UNSIGNED_SHORT ? 2 : d->itype == GL_UNSIGNED_INT ? 4 : d->itype == GL_UNSIGNED_BYTE ? 1 : 0;
        if (!isz) { why = 6; goto skip; }
        if (ebuf) {
            struct shadow_buf *b = ebuf < MAX_BUF ? bufs[ebuf] : NULL;
            if (!b || !b->data || (u32)indices > b->size || count * isz > b->size - (u32)indices) { why = 7; goto skip; }
            idata = b->data + (u32)indices;
            key = mix(mix(mix(key, ebuf), b->version), (u32)indices);
        } else {
            idata = indices;
            if (!idata) { why = 8; goto skip; }
            if (!d->static_key) {
                if (IsBadReadPtr(idata, count * isz)) { why = 8; goto skip; }
                { double h0 = qpc_us(); key = hash_bytes(key, idata, count * isz); R.p_hash += qpc_us() - h0; }
            }
        }
    }
    key = mix(mix(mix(mix(key, mode), count), first), stride | isz << 16 | arrays << 24);
    if (R.smooth) key = mix(key, 0x534D4F4Fu);      /* meshes carry smooth normals: built anew when it changes */

    /* Data that lives in buffers is identified without looking at it. */
    e = ((vbuf && (arrays || ebuf)) || d->static_key) ? table_find(key, 0) : NULL;
    if (!e || e->state != 2) {
        if (arrays) maxi = first + count - 1;
        else for (i = 0; i < count; i++) { u32 x = index_at(idata, isz, i); if (x > maxi) maxi = x; }
        if (maxi > 4000000) { why = 9; goto skip; }
        if (vbuf) {
            if ((u64)maxi * stride + 12 > vavail) { why = 10; goto skip; }
        } else if (!d->static_key) {
            if (IsBadReadPtr(vdata, maxi * stride + 12)) { why = 11; goto skip; }
            { double h0 = qpc_us(); key = hash_bytes(key, vdata, maxi * stride + 12); R.p_hash += qpc_us() - h0; }
        }
        if (ndata) {
            if (!n_plain) { if ((u64)maxi * n_stride + 12 > n_avail) ndata = NULL; }
            else if (IsBadReadPtr(ndata, maxi * n_stride + 12)) ndata = NULL;
            else if (n_plain == 2) { double h0 = qpc_us(); key = hash_bytes(key, ndata, maxi * n_stride + 12); R.p_hash += qpc_us() - h0; }
            if (!ndata) key = mix(key, 0xDEADu);
        }
        if (tcdata && (u64)maxi * tc_stride + 8 > tc_avail) {
            if (alpha_off) { R.f_alpha_skipped++; why = 22; goto skip; }
            tcdata = NULL;              /* no usable coordinates: one colour for the whole mesh */
        }
        e = table_find(key, 0);
    }

    if (e && (e->state == 2 || (e->state == 1 && e->last_frame == R.frame))) {
        e->last_frame = R.frame;        /* cached, or already built earlier in this frame */
    } else {
        /* Expand to a triangle list in object space. */
        u32 maxtri = mode == GL_TRIANGLES ? count / 3 : mode == GL_QUADS ? count / 2 : count - 2;
        float *t;
        u32 *uv = NULL;
        NrmTri *nt = NULL;
        u32 agree = 0, *ci = NULL;
        double x0 = qpc_us(), x1;
        if (!grow_pool((void **)&R.tri9, &R.tri9_cap, maxtri * 9, sizeof(float))) { why = 12; goto skip; }
        if (tcdata) {
            if (!grow_pool((void **)&R.uv3, &R.uv3_cap, maxtri * 3, sizeof(u32))) { why = 12; goto skip; }
            uv = R.uv3;
        }
        if (ndata) {
            if (!grow_pool((void **)&R.n4, &R.n4_cap, maxtri, sizeof(NrmTri))) { why = 12; goto skip; }
            nt = R.n4;
        }
        if (R.smooth && !arrays && grow_pool((void **)&R.cidx, &R.cidx_cap, maxtri * 3, sizeof(u32))) ci = R.cidx;
        t = R.tri9;
#define IDX(k) (arrays ? first + (k) : index_at(idata, isz, (k)))
#define EMIT(a, b, c) do { u32 ia = (a), ib = (b), ic = (c); \
        if (ia != ib && ib != ic && ia != ic) { \
            memcpy(t, vdata + (size_t)ia * stride, 12); memcpy(t + 3, vdata + (size_t)ib * stride, 12); \
            memcpy(t + 6, vdata + (size_t)ic * stride, 12); t += 9; \
            if (uv) { \
                float ta[2], tb[2], tc[2], bu, bv; \
                memcpy(ta, tcdata + (size_t)ia * tc_stride, 8); memcpy(tb, tcdata + (size_t)ib * tc_stride, 8); \
                memcpy(tc, tcdata + (size_t)ic * tc_stride, 8); \
                if (xf) { float q; \
                    q = ta[0] * xf[0] + ta[1] * xf[1] + xf[2]; ta[1] = ta[0] * xf[3] + ta[1] * xf[4] + xf[5]; ta[0] = q; \
                    q = tb[0] * xf[0] + tb[1] * xf[1] + xf[2]; tb[1] = tb[0] * xf[3] + tb[1] * xf[4] + xf[5]; tb[0] = q; \
                    q = tc[0] * xf[0] + tc[1] * xf[1] + xf[2]; tc[1] = tc[0] * xf[3] + tc[1] * xf[4] + xf[5]; tc[0] = q; } \
                bu = (float)floor(ta[0] < tb[0] ? (ta[0] < tc[0] ? ta[0] : tc[0]) : (tb[0] < tc[0] ? tb[0] : tc[0])); \
                bv = (float)floor(ta[1] < tb[1] ? (ta[1] < tc[1] ? ta[1] : tc[1]) : (tb[1] < tc[1] ? tb[1] : tc[1])); \
                uv[0] = pack_uv(ta[0] - bu, ta[1] - bv); uv[1] = pack_uv(tb[0] - bu, tb[1] - bv); \
                uv[2] = pack_uv(tc[0] - bu, tc[1] - bv); uv += 3; \
            } \
            if (nt) { \
                float na[3], nb[3], nc[3], fx, fy, fz, sx, sy, sz, dd, l1, l2; const float *p0 = t - 9; \
                memcpy(na, ndata + (size_t)ia * n_stride, 12); memcpy(nb, ndata + (size_t)ib * n_stride, 12); \
                memcpy(nc, ndata + (size_t)ic * n_stride, 12); \
                ((u32 *)nt)[0] = oct_pack(na[0], na[1], na[2]); ((u32 *)nt)[1] = oct_pack(nb[0], nb[1], nb[2]); \
                ((u32 *)nt)[2] = oct_pack(nc[0], nc[1], nc[2]); ((u32 *)nt)[3] = 1; nt++; \
                /* does the mean vertex normal point along the face normal (either side)? */ \
                fx = (p0[4] - p0[1]) * (p0[8] - p0[2]) - (p0[5] - p0[2]) * (p0[7] - p0[1]); \
                fy = (p0[5] - p0[2]) * (p0[6] - p0[0]) - (p0[3] - p0[0]) * (p0[8] - p0[2]); \
                fz = (p0[3] - p0[0]) * (p0[7] - p0[1]) - (p0[4] - p0[1]) * (p0[6] - p0[0]); \
                sx = na[0] + nb[0] + nc[0]; sy = na[1] + nb[1] + nc[1]; sz = na[2] + nb[2] + nc[2]; \
                dd = fx * sx + fy * sy + fz * sz; l1 = fx * fx + fy * fy + fz * fz; l2 = sx * sx + sy * sy + sz * sz; \
                if (dd * dd > 0.25f * l1 * l2) agree++; \
            } \
            if (ci) { ci[0] = ia; ci[1] = ib; ci[2] = ic; ci += 3; } \
            ntri++; } } while (0)
        if (mode == GL_TRIANGLES) for (i = 0; i + 2 < count; i += 3) EMIT(IDX(i), IDX(i + 1), IDX(i + 2));
        else if (mode == GL_TRIANGLE_STRIP) for (i = 0; i + 2 < count; i++) EMIT(IDX(i), IDX(i + 1), IDX(i + 2));
        else if (mode == GL_TRIANGLE_FAN) for (i = 1; i + 1 < count; i++) EMIT(IDX(0), IDX(i), IDX(i + 1));
        else for (i = 0; i + 3 < count; i += 4) { EMIT(IDX(i), IDX(i + 1), IDX(i + 2)); EMIT(IDX(i), IDX(i + 2), IDX(i + 3)); }
#undef IDX
#undef EMIT
        if (!ntri) { why = 13; goto skip; }
        uv = tcdata ? R.uv3 : NULL;
        if (nt && agree * 4 < ntri * 3) {
            /* Most triangles disagree with their normals: the layout is not what we think
               (or the data is last frame's). Flat normals are better than wrong ones. */
            static int logged;
            if (logged++ < 8) rt_log("rt: normals of a %u-triangle mesh rejected (%u agree with the faces; buffer %u, stride %u)",
                                     ntri, agree, am ? am->n_buf : 0, n_stride);
            nt = NULL;
        }
        nt = nt ? R.n4 : NULL;
        if (!nt && R.smooth) {
            double c0 = qpc_us();
            if (grow_pool((void **)&R.n4, &R.n4_cap, ntri, sizeof(NrmTri)) &&
                (ci ? compute_normals_indexed(R.tri9, R.cidx, ntri, maxi, R.n4) : compute_normals(R.tri9, ntri, R.n4))) nt = R.n4;
            R.p_normals += qpc_us() - c0;
        }
        x1 = qpc_us(); R.p_expand += x1 - x0;

        /* A mesh drawn unchanged for several frames in a row is worth a careful, cached
           build. Until then (and for anything animated) it is rebuilt quickly each frame. */
        promote = 0;
        if (e && e->state == 1) {
            e->streak = e->last_frame + 1 == R.frame ? e->streak + 1 : 0;
            /* Vertex data in plain memory is usually an animated mesh that happens to be
               standing still: wait much longer before caching it. */
            promote = e->streak >= ((vbuf || d->static_key) ? 6u : 120u);
        }
        if (!e) e = table_find(key, 1);
        if (promote) {
            if (!grow_pool((void **)&R.sn, &R.sn_cap, R.sn_count + 2 * ntri, sizeof(BvhNode)) ||
                !grow_pool((void **)&R.st, &R.st_cap, R.st_count + ntri, sizeof(BvhTri)) ||
                !grow_pool((void **)&R.sm, &R.sm_cap, R.st_count + ntri, sizeof(NrmTri))) { why = 14; goto skip; }
            nn = bvh_build(NULL, R.tri9, uv, ntri, 1, R.sn + R.sn_count, R.st + R.st_count);
            if (!nn) { why = 15; goto skip; }
            put_normals(R.sm + R.st_count, nt, bvh_order(NULL), ntri);
            e->state = 2;
            e->node_off = R.sn_count; e->nnodes = nn; e->tri_off = R.st_count; e->ntris = ntri;
            bvh_bounds(&R.sn[R.sn_count], e->lo, e->hi);
            R.sn_count += nn; R.st_count += ntri;
            R.f_built_static++;
        } else {
            struct entry tmp;
            u64 sid = mix(mix(mix(mix(0x452821E638D01377ull, ebuf), (u32)indices), count), first | mode << 24);
            struct slot *sl = slot_find(sid ? sid : 1, 0);
            if (!grow_pool((void **)&R.fn, &R.fn_cap, R.fn_count + 2 * ntri, sizeof(BvhNode)) ||
                !grow_pool((void **)&R.ft, &R.ft_cap, R.ft_count + ntri, sizeof(BvhTri)) ||
                !grow_pool((void **)&R.fm, &R.fm_cap, R.ft_count + ntri, sizeof(NrmTri))) { why = 16; goto skip; }
            if (sl && sl->nodes && sl->ntris == ntri && R.frame - sl->built < REBUILD_EVERY) {
                BvhTri *o = R.ft + R.ft_count;
                int a;
                for (i = 0; i < ntri; i++, o++) {
                    const float *t3 = R.tri9 + (size_t)sl->order[i] * 9;
                    const u32 *u3 = uv ? uv + (size_t)sl->order[i] * 3 : NULL;
                    for (a = 0; a < 3; a++) { o->v0[a] = t3[a]; o->e1[a] = t3[3 + a] - t3[a]; o->e2[a] = t3[6 + a] - t3[a]; }
                    o->uv0 = u3 ? u3[0] : 0; o->uv1 = u3 ? u3[1] : 0; o->uv2 = u3 ? u3[2] : 0;
                }
                put_normals(R.fm + R.ft_count, nt, sl->order, ntri);
                nn = sl->nnodes;
                memcpy(R.fn + R.fn_count, sl->nodes, (size_t)nn * sizeof(BvhNode));
                bvh_refit(R.fn + R.fn_count, nn, R.ft + R.ft_count);
                sl->used = R.frame;
                R.p_refit++;
            } else {
                nn = bvh_build(NULL, R.tri9, uv, ntri, 1, R.fn + R.fn_count, R.ft + R.ft_count);
                if (!nn) { why = 17; goto skip; }
                put_normals(R.fm + R.ft_count, nt, bvh_order(NULL), ntri);
                sl = slot_find(sid ? sid : 1, 1);
                free(sl->nodes); free(sl->order);
                sl->nodes = malloc((size_t)nn * sizeof(BvhNode));
                sl->order = malloc((size_t)ntri * sizeof(u32));
                if (sl->nodes && sl->order) {
                    memcpy(sl->nodes, R.fn + R.fn_count, (size_t)nn * sizeof(BvhNode));
                    memcpy(sl->order, bvh_order(NULL), (size_t)ntri * sizeof(u32));
                    sl->ntris = ntri; sl->nnodes = nn;
                    sl->built = R.frame - (u32)(sid % 97);     /* spread the periodic rebuilds over time */
                    sl->used = R.frame;
                } else { free(sl->nodes); free(sl->order); sl->nodes = NULL; sl->order = NULL; sl->id = 0; }
            }
            if (!e) { memset(&tmp, 0, sizeof tmp); e = &tmp; }     /* table full: use it this once */
            e->state = 1;
            e->node_off = R.fn_count; e->nnodes = nn; e->tri_off = R.ft_count; e->ntris = ntri;
            bvh_bounds(&R.fn[R.fn_count], e->lo, e->hi);
            R.fn_count += nn; R.ft_count += ntri;
            R.f_built_frame++;
            R.p_build += qpc_us() - x1; R.p_built++; R.p_tris += ntri;
            if (e == &tmp) {
                if (!grow_pool((void **)&R.inst, &R.inst_cap, R.ninst + 1, sizeof(struct inst))) { why = 18; goto skip; }
                in = &R.inst[R.ninst++];
                memcpy(in->model, model, sizeof in->model);
                in->node_off = tmp.node_off; in->tri_off = tmp.tri_off; in->in_frame_pool = 1;
                memcpy(in->lo, tmp.lo, 12); memcpy(in->hi, tmp.hi, 12);
                in->alpha_off = alpha_off; in->alpha_ref = am ? am->ref : 0;
                in->ntris = tmp.ntris; in->mergeable = 0; in->iid = 0; in->key = 0; in->covered = 0;
                in->color = color; in->layer = layer;
                if (alpha_off) R.f_alpha_inst++;
                R.f_collect_us += qpc_us() - t0;
                return;
            }
        }
        e->last_frame = R.frame;
    }

    if (!grow_pool((void **)&R.inst, &R.inst_cap, R.ninst + 1, sizeof(struct inst))) { why = 19; goto skip; }
    in = &R.inst[R.ninst++];
    memcpy(in->model, model, sizeof in->model);
    in->node_off = e->node_off; in->tri_off = e->tri_off; in->in_frame_pool = e->state == 1;
    memcpy(in->lo, e->lo, 12); memcpy(in->hi, e->hi, 12);
    in->alpha_off = alpha_off; in->alpha_ref = am ? am->ref : 0;
    in->ntris = e->ntris; in->covered = 0;
    in->color = color; in->layer = layer;
    in->key = e->key;
    in->mergeable = e->state == 2 && !alpha_off && (vbuf || d->static_key) && e->restless_until <= R.frame;
    in->iid = in->mergeable ? mix(e->key, hash_bytes(0x51EDull, model, 64)) : 0;
    if (alpha_off) R.f_alpha_inst++;
    R.f_collect_us += qpc_us() - t0;
    return;
skip:
    R.f_skipped++;
    if (why > 0 && why < 32 && !(R.skip_logged & (1u << why))) {
        R.skip_logged |= 1u << why;
        rt_log("rt: caster draw skipped, reason %d (mode %#x count %u first %u itype %#x arrays %u)", why, mode, count,
               first, d->itype, arrays);
    }
    R.f_collect_us += qpc_us() - t0;
}

/* ---- matrices (row vectors: p' = p * M, as the game's shaders use them) ------------ */
static int invert4(const float *m, double *out)
{
    double a[4][8];
    int i, j, k, piv;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][4 + j] = i == j; }
    for (i = 0; i < 4; i++) {
        double best = fabs(a[i][i]), f;
        piv = i;
        for (k = i + 1; k < 4; k++)
            if (fabs(a[k][i]) > best) { best = fabs(a[k][i]); piv = k; }
        if (best < 1e-30) return 0;
        if (piv != i)
            for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[piv][j]; a[piv][j] = t; }
        f = 1.0 / a[i][i];
        for (j = 0; j < 8; j++) a[i][j] *= f;
        for (k = 0; k < 4; k++)
            if (k != i && a[k][i] != 0) {
                f = a[k][i];
                for (j = 0; j < 8; j++) a[k][j] -= f * a[i][j];
            }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) out[i * 4 + j] = a[i][4 + j];
    return 1;
}

/* ---- merged static geometry -----------------------------------------------------------
   Cached meshes overlap each other a lot (terrain is drawn per material, not per area), so
   tracing them one by one makes every ray walk a dozen BVHs. Instead all cached, opaque
   instances are baked into world space and put in one BVH, built on a worker thread. The
   frame keeps using the previous merged BVH (plus individual instances for anything not
   in it) until a new one is ready. */
static struct {
    BvhNode *nodes; BvhTri *tris; u32 nnodes, ntris;
    NrmTri *nrm;                        /* smooth normals, world space, same order as tris */
    int dirty;                          /* not on the GPU yet */
    u32 cap_nodes, cap_tris;            /* room kept for it at the start of the GPU buffers */
    float lo[3], hi[3];
    u64 *set, *set_key; u32 *stamp; u32 set_cap, set_count;     /* ids (and mesh keys) of the instances it contains */
    u32 diff_frames, last_start, merges, last_ms;
} M;

static struct {
    HANDLE thread, wake;
    volatile LONG state;                /* 0 idle, 1 building, 2 done */
    float *tri9; u32 *uv3; u32 ntri;
    NrmTri *n4, *nrm;                   /* normals: input order, then the built order */
    BvhNode *nodes; BvhTri *tris; u32 nnodes;
    u64 *ids, *keys; u32 nids;
    struct bvh_ctx *ctx;
    double ms;
    volatile LONG quit;                 /* the core is being unloaded */
} job;

static DWORD WINAPI merge_worker(void *arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(job.wake, INFINITE);
        if (job.quit) return 0;
        if (job.state == 1) {
            double t0 = qpc_us();
            job.nodes = malloc((size_t)job.ntri * 2 * sizeof(BvhNode));
            job.tris = malloc((size_t)job.ntri * sizeof(BvhTri));
            job.nnodes = (job.nodes && job.tris)
                ? bvh_build(job.ctx, job.tri9, job.uv3, job.ntri, 1, job.nodes, job.tris) : 0;
            if (job.nnodes) {
                job.nrm = malloc((size_t)job.ntri * sizeof(NrmTri));
                if (job.nrm) put_normals(job.nrm, job.n4, job.n4 ? bvh_order(job.ctx) : NULL, job.ntri);
            }
            free(job.tri9); free(job.uv3); free(job.n4);
            job.tri9 = NULL; job.uv3 = NULL; job.n4 = NULL;
            job.ms = (qpc_us() - t0) / 1000.0;
            InterlockedExchange(&job.state, 2);
        }
    }
}

/* How many frames in a row an instance (mesh + placement) has been drawn. Things whose
   transform changes every frame never settle, and are left out of the merged BVH. */
#define PLACES 8192
#define SETTLED 30
#define RESTLESS_FRAMES 3600
static struct { u64 id; u32 frame, streak; } places[PLACES];

static u32 place_streak(u64 id)
{
    u32 i = (u32)(id >> 12) & (PLACES - 1), n, victim = i;
    for (n = 0; n < 8; n++) {
        u32 k = (i + n) & (PLACES - 1);
        if (places[k].id == id) {
            if (places[k].frame != R.frame) {
                places[k].streak = places[k].frame + 1 == R.frame ? places[k].streak + 1 : 1;
                places[k].frame = R.frame;
            }
            return places[k].streak;
        }
        if (places[k].frame < places[victim].frame) victim = k;
    }
    places[victim].id = id; places[victim].frame = R.frame; places[victim].streak = 1;
    return 1;
}

static int mset_find(u64 id)
{
    u32 i, n;
    if (!M.set_count) return -1;
    for (i = (u32)(id >> 16) & (M.set_cap - 1), n = 0; n < M.set_cap; n++, i = (i + 1) & (M.set_cap - 1)) {
        if (M.set[i] == id) return (int)i;
        if (!M.set[i]) return -1;
    }
    return -1;
}

static void merged_drop(void)
{
    M.set_count = 0;
    free(M.nodes); free(M.tris); free(M.nrm);
    M.nodes = NULL; M.tris = NULL; M.nrm = NULL;
    M.nnodes = M.ntris = 0;
}

/* The worker finished: make its BVH the merged one. */
static void merged_install(void)
{
    u32 i, cap = 64;
    merged_drop();
    if (job.nnodes && job.nids) {
        while (cap < job.nids * 2) cap *= 2;
        free(M.set); free(M.set_key); free(M.stamp);
        M.set = calloc(cap, sizeof(u64));
        M.set_key = calloc(cap, sizeof(u64));
        M.stamp = calloc(cap, sizeof(u32));
        if (M.set && M.set_key && M.stamp) {
            M.set_cap = cap;
            for (i = 0; i < job.nids; i++) {
                u32 k = (u32)(job.ids[i] >> 16) & (cap - 1);
                while (M.set[k]) k = (k + 1) & (cap - 1);
                M.set[k] = job.ids[i];
                M.set_key[k] = job.keys[i];
            }
            M.set_count = job.nids;
            M.nodes = job.nodes; M.tris = job.tris; M.nnodes = job.nnodes; M.ntris = job.ntri;
            M.nrm = job.nrm;
            job.nodes = NULL; job.tris = NULL; job.nrm = NULL;
            bvh_bounds(&M.nodes[0], M.lo, M.hi);
            if (M.nnodes > M.cap_nodes || M.ntris > M.cap_tris) {
                M.cap_nodes = M.nnodes + M.nnodes / 4 + 1024;
                M.cap_tris = M.ntris + M.ntris / 4 + 1024;
                R.gpu_node_cap = R.gpu_tri_cap = R.gpu_nrm_cap = 0;     /* lay the GPU buffers out again */
            }
            M.dirty = 1;
            M.merges++;
            M.last_ms = (u32)job.ms;
            rt_log("rt: merged %u static instances, %u triangles, built in %.0f ms", job.nids, M.ntris, job.ms);
        }
    }
    free(job.nodes); free(job.tris); free(job.ids); free(job.keys); free(job.nrm);
    job.nodes = NULL; job.tris = NULL; job.ids = NULL; job.keys = NULL; job.nrm = NULL;
    InterlockedExchange(&job.state, 0);
}

/* Hand the current frame's cached, opaque instances to the worker. */
static void merged_start(void)
{
    u32 i, j, ntri = 0, nids = 0, cap = 64, *slot_of;
    u64 *seen, *ids, *keys;
    float *t;
    u32 *col;
    NrmTri *nw;

    if (!job.thread) {
        job.ctx = bvh_ctx_new();
        job.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        job.thread = job.ctx && job.wake ? CreateThread(NULL, 0, merge_worker, NULL, 0, NULL) : NULL;
        if (!job.thread) return;
    }
    while (cap < R.ninst * 2) cap *= 2;
    seen = calloc(cap, sizeof(u64));
    ids = malloc((size_t)(R.ninst + 1) * sizeof(u64));
    keys = malloc((size_t)(R.ninst + 1) * sizeof(u64));
    slot_of = malloc((size_t)(R.ninst + 1) * sizeof(u32));
    if (!seen || !ids || !keys || !slot_of) { free(seen); free(ids); free(keys); free(slot_of); return; }
    for (i = 0; i < R.ninst; i++) {
        struct inst *in = &R.inst[i];
        u32 k;
        if (!(in->mergeable || in->covered) || in->in_frame_pool) continue;
        for (k = (u32)(in->iid >> 16) & (cap - 1); seen[k] && seen[k] != in->iid; k = (k + 1) & (cap - 1)) ;
        if (seen[k]) continue;          /* the same mesh in the same place twice */
        seen[k] = in->iid;
        slot_of[nids] = i;
        keys[nids] = in->key;
        ids[nids++] = in->iid;
        ntri += in->ntris;
    }
    free(seen);
    t = ntri >= 2000 ? malloc((size_t)ntri * 9 * sizeof(float)) : NULL;
    col = t ? calloc((size_t)ntri * 3, sizeof(u32)) : NULL;
    if (!t || !col) { free(t); free(col); free(ids); free(keys); free(slot_of); M.last_start = R.frame; return; }
    nw = R.sm ? malloc((size_t)ntri * sizeof(NrmTri)) : NULL;   /* without it: flat normals */
    job.tri9 = t;
    job.uv3 = col;
    job.n4 = nw;
    for (i = 0; i < nids; i++) {
        const struct inst *in = &R.inst[slot_of[i]];
        const float *m = in->model;
        const BvhTri *src = R.st + in->tri_off;
        const NrmTri *sn = R.sm ? R.sm + in->tri_off : NULL;
        double inv[16];
        int have_inv = invert4(m, inv);
        for (j = 0; j < in->ntris; j++, src++) {
            float p[3][3];
            int v, k;
            for (k = 0; k < 3; k++) { p[0][k] = src->v0[k]; p[1][k] = src->v0[k] + src->e1[k]; p[2][k] = src->v0[k] + src->e2[k]; }
            for (v = 0; v < 3; v++)
                for (k = 0; k < 3; k++)
                    *t++ = p[v][0] * m[0 * 4 + k] + p[v][1] * m[1 * 4 + k] + p[v][2] * m[2 * 4 + k] + m[3 * 4 + k];
            /* texture coordinates as they are, plus the instance's texture layer */
            col[0] = (src->uv0 & ~15u) | (in->layer & 15u);
            col[1] = (src->uv1 & ~15u) | (in->layer >> 4 & 15u);
            col[2] = (src->uv2 & ~15u) | (in->layer >> 8 & 15u);
            col += 3;
            if (nw) {
                /* normals to world space: by the inverse transpose of the model matrix */
                if (sn && sn[j].flag && have_inv) {
                    u32 q[3] = { sn[j].a, sn[j].b, sn[j].c }, w[3];
                    for (v = 0; v < 3; v++) {
                        float o[3], r[3];
                        oct_unpack(q[v], o);
                        for (k = 0; k < 3; k++)
                            r[k] = (float)(o[0] * inv[k * 4 + 0] + o[1] * inv[k * 4 + 1] + o[2] * inv[k * 4 + 2]);
                        w[v] = oct_pack(r[0], r[1], r[2]);
                    }
                    nw->a = w[0]; nw->b = w[1]; nw->c = w[2]; nw->flag = 1;
                } else memset(nw, 0, sizeof *nw);
                nw++;
            }
        }
    }
    free(slot_of);
    job.ntri = ntri; job.ids = ids; job.keys = keys; job.nids = nids;
    M.last_start = R.frame;
    InterlockedExchange(&job.state, 1);
    SetEvent(job.wake);
}

/* ---- per-frame finalisation: top level, uploads -------------------------------------
   GPU buffer layout: [merged BVH | cached meshes | this frame's meshes]. */
static void upload_pool(GLuint buf, u32 *gpu_cap, u32 mcap, const void *merged, u32 mcount, int mdirty,
                        const void *stat, u32 stat_count, u32 *uploaded, const void *frame, u32 frame_count,
                        size_t elem)
{
    u32 need = mcap + stat_count + frame_count;
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    if (need > *gpu_cap) {
        *gpu_cap = need + need / 2 + 4096;
        gl.BufferData(GL_SHADER_STORAGE_BUFFER, (ptrdiff_t)((size_t)*gpu_cap * elem), NULL, GL_DYNAMIC_DRAW);
        *uploaded = 0;
        mdirty = 1;
    }
    if (mdirty && merged && mcount)
        gl.BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (ptrdiff_t)((size_t)mcount * elem), merged);
    if (stat_count > *uploaded)
        gl.BufferSubData(GL_SHADER_STORAGE_BUFFER, (ptrdiff_t)((size_t)(mcap + *uploaded) * elem),
                         (ptrdiff_t)((size_t)(stat_count - *uploaded) * elem), (const u8 *)stat + (size_t)*uploaded * elem);
    *uploaded = stat_count;
    if (frame_count)
        gl.BufferSubData(GL_SHADER_STORAGE_BUFFER, (ptrdiff_t)((size_t)(mcap + stat_count) * elem),
                         (ptrdiff_t)((size_t)frame_count * elem), frame);
}

static int finalize(void)
{
    u32 i, n = R.ninst, out = 0, uncovered = 0, covered = 0, mergeable_tris = 0;
    int j, k, c, differs;
    double t0 = qpc_us();

    if (!n) return 0;
    if (job.state == 2) merged_install();

    /* Which instances does the merged BVH already contain? */
    for (i = 0; i < n; i++) {
        struct inst *in = &R.inst[i];
        int slot = in->mergeable ? mset_find(in->iid) : -1;
        in->covered = slot >= 0;
        if (slot >= 0) {
            if (M.stamp[slot] != R.frame + 1) { M.stamp[slot] = R.frame + 1; covered++; }
        } else if (in->mergeable) {
            if (place_streak(in->iid) >= SETTLED) {
                uncovered++;
                mergeable_tris += in->ntris;
            } else {
                in->mergeable = 0;      /* still moving: trace it on its own */
            }
        }
    }
    if (M.set_count && covered * 2 < M.set_count) {
        /* Most of it is gone (another level, a big change): better none than a wrong one. */
        merged_drop();
        for (i = 0; i < n; i++) R.inst[i].covered = 0;
        covered = 0;
    }
    differs = uncovered > 0 || covered != M.set_count;
    M.diff_frames = differs ? M.diff_frames + 1 : 0;
    if (M.diff_frames == 1 && covered != M.set_count) {
        /* Something left the merged BVH. A mesh that does that is probably animated by its
           transform: keep it out for a while so it does not go in and out for ever. */
        for (i = 0; i < M.set_cap; i++)
            if (M.set[i] && M.stamp[i] != R.frame + 1) {
                struct entry *e = table_find(M.set_key[i], 0);
                if (e) e->restless_until = R.frame + RESTLESS_FRAMES;
            }
    }
    /* Something missing from it means a wrong shadow on screen: rebuild at once. Additions
       are already traced correctly on their own, so they can wait until things calm down. */
    if (differs && job.state == 0 &&
        ((covered != M.set_count && R.frame - M.last_start >= 8) ||
         (M.diff_frames >= 4 && (R.frame - M.last_start >= 30 || !M.merges)))) {
        if (M.merges < 40)
            rt_log("rt: merge wanted at frame %u: %u settled instances outside it (%u triangles), %u of its %u present",
                   R.frame, uncovered, mergeable_tris, covered, M.set_count);
        merged_start();
    }

    if (n + 1 > R.tlas_cap) {
        u32 cap = n + n / 2 + 64;
        free(R.tlas); free(R.boxes); free(R.inst_gpu);
        R.tlas = malloc((size_t)cap * 2 * sizeof(BvhNode));
        R.boxes = malloc((size_t)cap * 6 * sizeof(float));
        R.inst_gpu = malloc((size_t)cap * 20 * sizeof(float));
        R.tlas_cap = (R.tlas && R.boxes && R.inst_gpu) ? cap : 0;
        if (!R.tlas_cap) return 0;
    }
    if (M.set_count) {
        float *g = R.inst_gpu, *box = R.boxes;
        memset(g, 0, 20 * sizeof(float));
        g[0] = g[5] = g[10] = 1.0f;         /* identity; node and triangle base 0 */
        g[16] = g[17] = g[18] = 0.5f;
        g[19] = -1.0f;                      /* texture layer: per triangle */
        memcpy(box, M.lo, 12); memcpy(box + 3, M.hi, 12);
        out = 1;
    }
    for (i = 0; i < n; i++) {
        struct inst *in = &R.inst[i];
        const float *m = in->model;
        double inv[16];
        float *g = &R.inst_gpu[out * 20], *box = &R.boxes[out * 6];
        u32 nb = M.cap_nodes + in->node_off + (in->in_frame_pool ? R.sn_count : 0);
        u32 tb = M.cap_tris + in->tri_off + (in->in_frame_pool ? R.st_count : 0);
        if (in->covered) continue;
        if (!invert4(m, inv)) memset(inv, 0, sizeof inv);
        for (j = 0; j < 3; j++) {           /* row j of world -> object, for column vectors */
            g[j * 4 + 0] = (float)inv[0 * 4 + j]; g[j * 4 + 1] = (float)inv[1 * 4 + j];
            g[j * 4 + 2] = (float)inv[2 * 4 + j]; g[j * 4 + 3] = (float)inv[3 * 4 + j];
        }
        memcpy(&g[12], &nb, 4); memcpy(&g[13], &tb, 4);
        memcpy(&g[14], &in->alpha_off, 4);
        g[15] = in->alpha_ref;
        if (in->color) {
            g[16] = (float)(in->color & 255) / 255.0f; g[17] = (float)(in->color >> 8 & 255) / 255.0f;
            g[18] = (float)(in->color >> 16 & 255) / 255.0f;
        } else g[16] = g[17] = g[18] = 0.5f;
        g[19] = (float)in->layer;
        box[0] = box[1] = box[2] = 1e30f; box[3] = box[4] = box[5] = -1e30f;
        for (c = 0; c < 8; c++) {
            float p[3] = { c & 1 ? in->hi[0] : in->lo[0], c & 2 ? in->hi[1] : in->lo[1], c & 4 ? in->hi[2] : in->lo[2] };
            for (k = 0; k < 3; k++) {
                float w = p[0] * m[0 * 4 + k] + p[1] * m[1 * 4 + k] + p[2] * m[2 * 4 + k] + m[3 * 4 + k];
                if (w < box[k]) box[k] = w;
                if (w > box[3 + k]) box[3 + k] = w;
            }
        }
        out++;
    }
    if (!out) return 0;
    R.tlas_count = bvh_build_boxes(R.boxes, out, R.tlas);
    if (!R.tlas_count) return 0;

    upload_pool(R.buf_nodes, &R.gpu_node_cap, M.cap_nodes, M.nodes, M.nnodes, M.dirty,
                R.sn, R.sn_count, &R.sn_uploaded, R.fn, R.fn_count, sizeof(BvhNode));
    upload_pool(R.buf_tris, &R.gpu_tri_cap, M.cap_tris, M.tris, M.ntris, M.dirty,
                R.st, R.st_count, &R.st_uploaded, R.ft, R.ft_count, sizeof(BvhTri));
    /* The normals mirror the triangle buffer index for index; meshes without them have
       zero entries. Pools that were never grown (nothing had normals yet) are zero-filled. */
    if (grow_pool((void **)&R.sm, &R.sm_cap, R.st_count, sizeof(NrmTri)) &&
        grow_pool((void **)&R.fm, &R.fm_cap, R.ft_count, sizeof(NrmTri))) {
        upload_pool(R.buf_nrm, &R.gpu_nrm_cap, M.cap_tris, M.nrm, M.nrm ? M.ntris : 0, M.dirty,
                    R.sm, R.st_count, &R.sm_uploaded, R.fm, R.ft_count, sizeof(NrmTri));
    }
    M.dirty = 0;
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, R.buf_tlas);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, (ptrdiff_t)((size_t)R.tlas_count * sizeof(BvhNode)), R.tlas, GL_STREAM_DRAW);
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, R.buf_inst);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, (ptrdiff_t)((size_t)out * 80), R.inst_gpu, GL_STREAM_DRAW);
    if (alpha_uploaded != alpha_count) {
        static const u32 none[4];
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, R.buf_alpha);
        gl.BufferData(GL_SHADER_STORAGE_BUFFER, (ptrdiff_t)((size_t)(alpha_words ? alpha_count : 4) * sizeof(u32)),
                      alpha_words ? alpha_words : none, GL_DYNAMIC_DRAW);
        alpha_uploaded = alpha_count;
    }
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    R.finalized = 1;
    rt_stats.tlas_instances = out;
    rt_stats.merged_instances = M.set_count;
    rt_stats.merged_tris = M.set_count ? M.ntris : 0;
    rt_stats.merge_ms = M.last_ms;
    rt_stats.merges = M.merges;
    R.f_finalize_us += qpc_us() - t0;
    return 1;
}

/* ---- the compute program ---------------------------------------------------------- */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *s;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    s = malloc((size_t)n + 1);
    if (s) { n = (long)fread(s, 1, (size_t)n, f); s[n] = 0; }
    fclose(f);
    return s;
}

/* Compute programs survive a core reload when their source has not changed (compiling the
   tracer takes the driver several seconds): the old core hands them to the loader, keyed by
   a hash of source and defines, and the new core takes them back. */
struct kept_programs { struct { u64 hash; GLuint prog; } e[8]; };
static struct kept_programs *KP;
static struct { GLuint prog; u64 hash; } made[16];
static u32 nmade;

/* defines: extra #define lines, inserted after the #version line (or NULL). */
static GLuint compile_program(const char *path, const char *defines)
{
    char *src = read_file(path), logbuf[4096];
    const char *srcs[3];
    GLint lens[3];
    char *eol;
    GLuint sh, prog;
    GLint ok = 0;

    if (!src) { rt_log("rt: %s not found", path); return 0; }
    eol = strchr(src, '\n');
    if (!eol) { free(src); return 0; }
    {
        u64 h = hash_bytes(hash_bytes(0x5EED, src, (u32)strlen(src)), defines ? defines : "", defines ? (u32)strlen(defines) : 0);
        GLboolean (APIENTRY *is_program)(GLuint) = (void *)rt_real_proc("glIsProgram");
        u32 k;
        for (k = 0; KP && is_program && k < 8; k++)
            if (KP->e[k].prog && KP->e[k].hash == h && is_program(KP->e[k].prog)) {
                prog = KP->e[k].prog;
                KP->e[k].prog = 0;
                free(src);
                made[nmade++ & 15].prog = prog; made[(nmade - 1) & 15].hash = h;
                rt_log("rt: %s unchanged, program taken over from the previous core", path);
                return prog;
            }
        made[nmade & 15].hash = h;          /* the program id is filled in below */
    }
    srcs[0] = src; lens[0] = (GLint)(eol + 1 - src);
    srcs[1] = defines ? defines : ""; lens[1] = (GLint)strlen(srcs[1]);
    srcs[2] = eol + 1; lens[2] = (GLint)strlen(eol + 1);
    sh = gl.CreateShader(GL_COMPUTE_SHADER);
    gl.ShaderSource(sh, 3, srcs, lens);
    gl.CompileShader(sh);
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    free(src);
    if (!ok) {
        logbuf[0] = 0;
        gl.GetShaderInfoLog(sh, sizeof logbuf, NULL, logbuf);
        rt_log("rt: %s failed to compile:\n%s", path, logbuf);
        gl.DeleteShader(sh);
        return 0;
    }
    prog = gl.CreateProgram();
    gl.AttachShader(prog, sh);
    gl.LinkProgram(prog);
    gl.DeleteShader(sh);
    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        logbuf[0] = 0;
        gl.GetProgramInfoLog(prog, sizeof logbuf, NULL, logbuf);
        rt_log("rt: %s failed to link:\n%s", path, logbuf);
        gl.DeleteProgram(prog);
        return 0;
    }
    made[nmade++ & 15].prog = prog;
    return prog;
}

/* For fx.c: a compute program (kept across core reloads like ours when unchanged). */
GLuint rt_compile_compute(const char *path, const char *defines)
{
    return compile_program(path, defines);
}

static int load_program(void)
{
    GLuint prog = compile_program("wumrt_trace.comp",
                                  R.big_stacks ? "#define MESH_STACK 56\n#define TOP_STACK 32\n" : NULL);
    GLuint fprog = compile_program("wumrt_filter.comp", NULL);
    GLuint tprog = compile_program("wumrt_temporal.comp", NULL);
    GLuint aprog = compile_program("wumrt_atrous.comp", NULL);
    R.want_reload = 0;
    if (!prog || !fprog || !tprog || !aprog) {
        if (aprog) gl.DeleteProgram(aprog);
        if (prog) gl.DeleteProgram(prog);
        if (fprog) gl.DeleteProgram(fprog);
        if (tprog) gl.DeleteProgram(tprog);
        rt_stats.error = 2;
        return 0;
    }
    if (R.prog) gl.DeleteProgram(R.prog);
    if (R.fprog) gl.DeleteProgram(R.fprog);
    if (R.tprog) gl.DeleteProgram(R.tprog);
    if (R.aprog) gl.DeleteProgram(R.aprog);
    R.prog = prog;
    R.fprog = fprog;
    R.tprog = tprog;
    R.aprog = aprog;
    if (KP) {                           /* what the previous core left and we did not take */
        u32 k;
        for (k = 0; k < 8; k++)
            if (KP->e[k].prog) { gl.DeleteProgram(KP->e[k].prog); KP->e[k].prog = 0; }
    }
    R.a_eye = gl.GetUniformLocation(aprog, "uEye");
    R.a_base = gl.GetUniformLocation(aprog, "uBase");
    R.a_dx = gl.GetUniformLocation(aprog, "uDx");
    R.a_dy = gl.GetUniformLocation(aprog, "uDy");
    R.a_step = gl.GetUniformLocation(aprog, "uStep");
    R.u_reflect = gl.GetUniformLocation(prog, "uReflect");
    R.u_char = gl.GetUniformLocation(prog, "uChar");
    R.u_caps = gl.GetUniformLocation(prog, "uCaps");
    R.u_leaf = gl.GetUniformLocation(prog, "uLeaf");
    R.u_ncaps = gl.GetUniformLocation(prog, "uNCaps");
    R.u_eye = gl.GetUniformLocation(prog, "uEye");
    R.u_base = gl.GetUniformLocation(prog, "uBase");
    R.u_dx = gl.GetUniformLocation(prog, "uDx");
    R.u_dy = gl.GetUniformLocation(prog, "uDy");
    R.u_sun = gl.GetUniformLocation(prog, "uSun");
    R.u_params = gl.GetUniformLocation(prog, "uParams");
    R.u_counts = gl.GetUniformLocation(prog, "uCounts");
    R.u_sky = gl.GetUniformLocation(prog, "uSkyParams");
    R.u_sun_col = gl.GetUniformLocation(prog, "uSunCol");
    R.u_sky_gain = gl.GetUniformLocation(prog, "uSkyGain");
    R.u_fallback = gl.GetUniformLocation(prog, "uFallback");
    R.u_gi = gl.GetUniformLocation(prog, "uGi");
    R.u_gi_rate = gl.GetUniformLocation(prog, "uGiRate");
    R.u_cloud = gl.GetUniformLocation(prog, "uCloud");
    R.u_prev_vp = gl.GetUniformLocation(prog, "uPrevVP");
    R.u_prev_eye = gl.GetUniformLocation(prog, "uPrevEye");
    R.u_albedo = gl.GetUniformLocation(prog, "uAlbedo");
    R.u_sky_atlas = gl.GetUniformLocation(prog, "uSkyAtlas");
    R.u_sky_tint = gl.GetUniformLocation(prog, "uSkyTint");
    R.f_eye = gl.GetUniformLocation(fprog, "uEye");
    R.f_base = gl.GetUniformLocation(fprog, "uBase");
    R.f_dx = gl.GetUniformLocation(fprog, "uDx");
    R.f_dy = gl.GetUniformLocation(fprog, "uDy");
    R.f_params = gl.GetUniformLocation(fprog, "uParams");
    R.t_eye = gl.GetUniformLocation(tprog, "uEye");
    R.t_base = gl.GetUniformLocation(tprog, "uBase");
    R.t_dx = gl.GetUniformLocation(tprog, "uDx");
    R.t_dy = gl.GetUniformLocation(tprog, "uDy");
    R.t_hist = gl.GetUniformLocation(tprog, "uHist");
    R.t_prev_vp = gl.GetUniformLocation(tprog, "uPrevVP");
    R.t_prev_eye = gl.GetUniformLocation(tprog, "uPrevEye");
    rt_stats.error = 0;
    rt_log("rt: compute programs ready");
    return 1;
}

/* ---- the sky, copied into six square views (see rt.h) ---------------------------------
   One texture, three views across and two up: +X -X +Y / -Y +Z -Z. */
#define SKY_FACE 256
static struct {
    GLuint tex, fbo;
    u32 valid, want, frame, next;       /* valid: views drawn at least once (bit mask) */
    int active, dirty;
    GLint save_draw, save_read, save_vp[4]; GLboolean save_scissor;
    float mean[3]; u32 mean_frame;      /* cosine-weighted mean colour of the upper half */
} SK;
static const float sky_fwd[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float sky_up[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };

u32 rt_sky_wanted(void)
{
    if (!R.enabled || !((R.gi && R.sky_rays > 0) || R.reflect || R.specular) || gl_state <= 0 || rt_stats.error) return 0;
    if (SK.frame != R.frame) {
        SK.frame = R.frame;
        SK.want = SK.valid != 63 ? 63 : 1u << (SK.next++ % 6);
    }
    return SK.want;
}

int rt_sky_begin(u32 face, const float *game_view, float *view, float *proj)
{
    const float *f = sky_fwd[face], *u = sky_up[face];
    float r[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
    float eye[3];
    const float zn = 1.0f, zf = 200000.0f;
    int i;
    if (face > 5 || gl_state <= 0) return 0;
    if (!SK.tex) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glGenTextures(1, &SK.tex);
        glBindTexture(GL_TEXTURE_2D, SK.tex);
        gl.TexStorage2D(GL_TEXTURE_2D, 6, GL_RGBA8, SKY_FACE * 3, SKY_FACE * 2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        gl.GenFramebuffers(1, &SK.fbo);
        SK.valid = 0;
    }
    if (!SK.active) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &SK.save_draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &SK.save_read);
        glGetIntegerv(GL_VIEWPORT, SK.save_vp);
        SK.save_scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (SK.save_scissor) glDisable(GL_SCISSOR_TEST);
        gl.BindFramebuffer(GL_FRAMEBUFFER, SK.fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, SK.tex, 0);
        SK.active = 1;
    }
    glViewport((GLint)(face % 3) * SKY_FACE, (GLint)(face / 3) * SKY_FACE, SKY_FACE, SKY_FACE);
    /* Where the game's sky camera stands (row vectors: eye * view = 0). */
    for (i = 0; i < 3; i++)
        eye[i] = -(game_view[12] * game_view[i * 4] + game_view[13] * game_view[i * 4 + 1] + game_view[14] * game_view[i * 4 + 2]);
    memset(view, 0, 16 * sizeof(float));
    for (i = 0; i < 3; i++) { view[i * 4] = r[i]; view[i * 4 + 1] = u[i]; view[i * 4 + 2] = -f[i]; }
    view[12] = -(eye[0] * r[0] + eye[1] * r[1] + eye[2] * r[2]);
    view[13] = -(eye[0] * u[0] + eye[1] * u[1] + eye[2] * u[2]);
    view[14] = eye[0] * f[0] + eye[1] * f[1] + eye[2] * f[2];
    view[15] = 1.0f;
    memset(proj, 0, 16 * sizeof(float));
    proj[0] = 1.0f; proj[5] = 1.0f;
    proj[10] = -(zf + zn) / (zf - zn); proj[11] = -1.0f;
    proj[14] = -2.0f * zf * zn / (zf - zn);
    SK.valid |= 1u << face;
    SK.dirty = 1;
    return 1;
}

void rt_sky_end(void)
{
    if (!SK.active) return;
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)SK.save_draw);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)SK.save_read);
    glViewport(SK.save_vp[0], SK.save_vp[1], SK.save_vp[2], SK.save_vp[3]);
    if (SK.save_scissor) glEnable(GL_SCISSOR_TEST);
    SK.active = 0;
}

/* Before tracing: mip levels for the sky (diffuse rays read a blurred one), and now and
   then its mean colour as an upward-facing surface sees it. Texture bound by the caller. */
static void sky_refresh(void)
{
    if (SK.dirty) { gl.GenerateMipmap(GL_TEXTURE_2D); SK.dirty = 0; }
    if (SK.valid == 63 && (R.frame - SK.mean_frame >= 30 || !SK.mean_frame)) {
        enum { N = SKY_FACE >> 4 };                 /* mip level 4: 16 x 16 per view */
        static float px[N * 3 * N * 2 * 3];
        double sum[3] = { 0, 0, 0 }, wsum = 0;
        u32 face, x, y;
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glGetTexImage(GL_TEXTURE_2D, 4, GL_RGB, GL_FLOAT, px);
        glPopClientAttrib();
        for (face = 0; face < 6; face++) {
            const float *f = sky_fwd[face], *u = sky_up[face];
            float r[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
            for (y = 0; y < N; y++)
                for (x = 0; x < N; x++) {
                    double a = (x + 0.5) * 2.0 / N - 1.0, b = (y + 0.5) * 2.0 / N - 1.0;
                    double len2 = 1.0 + a * a + b * b, len = sqrt(len2);
                    double dy = (f[1] + a * r[1] + b * u[1]) / len;      /* how far up the direction points */
                    double w = dy > 0 ? dy / (len2 * len) : 0;          /* cosine x solid angle */
                    const float *c = px + ((size_t)(face / 3 * N + y) * (N * 3) + face % 3 * N + x) * 3;
                    sum[0] += c[0] * w; sum[1] += c[1] * w; sum[2] += c[2] * w; wsum += w;
                }
        }
        if (wsum > 0) { SK.mean[0] = (float)(sum[0] / wsum); SK.mean[1] = (float)(sum[1] / wsum); SK.mean[2] = (float)(sum[2] / wsum); }
        if (!SK.mean_frame) rt_log("rt: sky captured, mean colour from above %.2f %.2f %.2f", SK.mean[0], SK.mean[1], SK.mean[2]);
        SK.mean_frame = R.frame ? R.frame : 1;
    }
}

/* Bounding-box diagonal of the vertex buffer a draw reads its positions from. */
int rt_draw_bounds(const struct rt_draw *d, const float *model, float *wlo, float *whi)
{
    const u8 *base, *ind = NULL;
    size_t size = (size_t)-1;
    u32 stride, i, n = d->count, idx;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, c[3];
    int k, got = 0;
    if (!d->v_on || d->v_type != GL_FLOAT || d->v_size < 3 || !n) return 0;
    if (d->v_buf) {
        struct shadow_buf *b = d->v_buf < MAX_BUF ? bufs[d->v_buf] : NULL;
        if (!b || !b->data || (u32)d->v_ptr >= b->size) return 0;
        base = b->data + (u32)d->v_ptr;
        size = b->size - (u32)d->v_ptr;
    } else base = (const u8 *)d->v_ptr;
    if (!base) return 0;
    if (!d->arrays) {
        if (d->elem_buf) {
            struct shadow_buf *e = d->elem_buf < MAX_BUF ? bufs[d->elem_buf] : NULL;
            if (!e || !e->data || (u32)d->indices >= e->size) return 0;
            ind = e->data + (u32)d->indices;
            if (n > (e->size - (u32)d->indices) / (d->itype == GL_UNSIGNED_INT ? 4 : d->itype == GL_UNSIGNED_SHORT ? 2 : 1))
                return 0;
        } else ind = (const u8 *)d->indices;
        if (!ind) return 0;
    }
    stride = d->v_stride ? d->v_stride : d->v_size * 4;
    if (n > 65536) n = 65536;
    for (i = 0; i < n; i++) {
        float v[3];
        idx = d->arrays ? d->first + i : d->itype == GL_UNSIGNED_INT ? ((const u32 *)ind)[i] :
              d->itype == GL_UNSIGNED_SHORT ? ((const unsigned short *)ind)[i] : ind[i];
        if ((size_t)idx * stride + 12 > size) continue;
        memcpy(v, base + (size_t)idx * stride, 12);
        for (k = 0; k < 3; k++) { if (v[k] < lo[k]) lo[k] = v[k]; if (v[k] > hi[k]) hi[k] = v[k]; }
        got = 1;
    }
    if (!got) return 0;
    for (k = 0; k < 3; k++) { wlo[k] = 1e30f; whi[k] = -1e30f; }
    for (i = 0; i < 8; i++) {           /* the box's corners into world space */
        c[0] = i & 1 ? hi[0] : lo[0]; c[1] = i & 2 ? hi[1] : lo[1]; c[2] = i & 4 ? hi[2] : lo[2];
        for (k = 0; k < 3; k++) {
            float w = c[0] * model[k] + c[1] * model[4 + k] + c[2] * model[8 + k] + model[12 + k];
            if (w < wlo[k]) wlo[k] = w;
            if (w > whi[k]) whi[k] = w;
        }
    }
    return 1;
}

int rt_draw_center(const struct rt_draw *d, const float *model, float *out)
{
    float lo[3], hi[3];
    int k;
    if (!rt_draw_bounds(d, model, lo, hi)) return 0;
    for (k = 0; k < 3; k++) out[k] = 0.5f * (lo[k] + hi[k]);
    return 1;
}

/* Characters as capsules for the contact shadow (see wumrt_trace.comp, uCaps). */
static float caps_data[16 * 8];
static int caps_n;
void rt_set_capsules(const float *caps, int n)
{
    if (n > 16) n = 16;
    if (n < 0) n = 0;
    memcpy(caps_data, caps, (size_t)n * 8 * sizeof(float));
    caps_n = n;
    {
        static u32 calls;
        if (n && (calls++ % 900) == 0)
            rt_log("rt: %d character capsules (first: %.1f %.1f %.1f - %.1f %.1f %.1f r %.1f), contact %.2f", n,
                   n ? caps[0] : 0.0f, n ? caps[1] : 0.0f, n ? caps[2] : 0.0f, n ? caps[4] : 0.0f, n ? caps[5] : 0.0f, n ? caps[6] : 0.0f,
                   n ? caps[3] : 0.0f, R.char_contact);
    }
}

float rt_mesh_extent(const struct rt_draw *d)
{
    static struct { u32 buf, version, ptr; float size; } cache[64];
    struct shadow_buf *b = d->v_buf && d->v_buf < MAX_BUF ? bufs[d->v_buf] : NULL;
    u32 slot = (d->v_buf * 31u + (u32)d->v_ptr) & 63, stride, n, i;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, dx, dy, dz;
    if (!b || !b->data || !d->v_on || d->v_type != GL_FLOAT || d->v_size < 3 || (u32)d->v_ptr >= b->size) return 0;
    if (cache[slot].buf == d->v_buf && cache[slot].version == b->version && cache[slot].ptr == (u32)d->v_ptr)
        return cache[slot].size;
    stride = d->v_stride ? d->v_stride : d->v_size * 4;
    n = (b->size - (u32)d->v_ptr) / stride;
    for (i = 0; i < n; i++) {
        float v[3];
        int k;
        memcpy(v, b->data + (u32)d->v_ptr + (size_t)i * stride, 12);
        for (k = 0; k < 3; k++) { if (v[k] < lo[k]) lo[k] = v[k]; if (v[k] > hi[k]) hi[k] = v[k]; }
    }
    dx = hi[0] - lo[0]; dy = hi[1] - lo[1]; dz = hi[2] - lo[2];
    cache[slot].buf = d->v_buf; cache[slot].version = b->version; cache[slot].ptr = (u32)d->v_ptr;
    cache[slot].size = n ? (float)sqrt(dx * dx + dy * dy + dz * dz) : 0;
    return cache[slot].size;
}

/* The game made a new OpenGL context (it can when the display mode changes): everything we
   created in the old one is gone, and texture ids now mean other textures. Forget it all;
   it is created again on demand. */
static void forget_gl_objects(void)
{
    u32 i;
    R.prog = R.fprog = R.tprog = R.aprog = 0;
    fx_forget();
    R.buf_nodes = R.buf_tris = R.buf_tlas = R.buf_inst = R.buf_alpha = R.buf_stat = R.query = 0;
    R.buf_mat = 0; mats_dirty = 1;
    R.buf_nrm = 0;
    R.gpu_node_cap = R.gpu_tri_cap = R.gpu_nrm_cap = 0;
    R.sn_uploaded = R.st_uploaded = R.sm_uploaded = 0;
    R.query_pending = 0;
    R.finalized = 0;
    M.dirty = 1;
    sc_forget();
    for (i = 0; i < MAX_CAMS; i++) {
        struct cam *c = &R.cams[i];
        c->raw = c->sky = c->mask[0] = c->mask[1] = c->bounce[0] = c->bounce[1] = c->out = c->out_bounce = 0;
        c->acc[0] = c->acc[1] = c->mom[0] = c->mom[1] = c->tmp[0] = c->tmp[1] = c->refl = 0;
        c->w = c->h = 0;
        c->ok = 0;
    }
    R.ncams = 0;
    n_alpha_texs = 0; alpha_count = 1; alpha_uploaded = 0;
    n_tex_colors = 0;
    memset(alpha_meshes, 0, sizeof alpha_meshes);
    memset(&LY, 0, sizeof LY);
    memset(nrm_tex, 0, 65536 * sizeof(GLuint));
    memset(layer_of_tex, 0, sizeof layer_of_tex);
    memset(tex_known, 0, sizeof tex_known);
    memset(&SK, 0, sizeof SK);
    rt_log("rt: new OpenGL context, GPU objects will be created again");
}

static int ensure_gpu(void)
{
    static HGLRC context;
    HGLRC now = wglGetCurrentContext();
    if (!now) return 0;
    if (context && now != context) forget_gl_objects();
    context = now;
    if (!gl_init()) return 0;
    if (!R.buf_nodes) {
        GLuint b[5];
        gl.GenBuffers(5, b);
        R.buf_nodes = b[0]; R.buf_tris = b[1]; R.buf_tlas = b[2]; R.buf_inst = b[3];
        R.buf_alpha = b[4];
        gl.GenBuffers(1, &R.buf_nrm);
        {
            static const u32 zero[4];
            gl.GenBuffers(1, &R.buf_stat);
            gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, R.buf_stat);
            gl.BufferData(GL_SHADER_STORAGE_BUFFER, sizeof zero, zero, GL_DYNAMIC_DRAW);
            gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        gl.GenQueries(1, &R.query);
    }
    if (!R.prog || R.want_reload) {
        if (rt_stats.error == 2 && !R.want_reload) return 0;
        if (!load_program()) return 0;
    }
    return 1;
}

static GLuint make_texture(GLenum format, u32 w, u32 h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    gl.TexStorage2D(GL_TEXTURE_2D, 1, format, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int ensure_mask(struct cam *c, u32 w, u32 h)
{
    GLint prev = 0;
    if (c->raw && c->w == w && c->h == h) return 1;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    if (c->raw) {
        glDeleteTextures(1, &c->raw);
        if (c->sky != c->tmp[1]) glDeleteTextures(1, &c->sky);
        glDeleteTextures(2, c->mask); glDeleteTextures(2, c->bounce);
        glDeleteTextures(2, c->acc); glDeleteTextures(2, c->mom); glDeleteTextures(2, c->tmp);
        glDeleteTextures(1, &c->refl);
    }
    c->raw = make_texture(GL_RGBA16F, w, h);
    c->bounce[0] = make_texture(GL_RGBA16F, w, h); c->bounce[1] = make_texture(GL_RGBA16F, w, h);
    c->mask[0] = make_texture(GL_RGBA16F, w, h); c->mask[1] = make_texture(GL_RGBA16F, w, h);
    c->acc[0] = make_texture(GL_RGBA16F, w, h); c->acc[1] = make_texture(GL_RGBA16F, w, h);
    c->mom[0] = make_texture(GL_RGBA16F, w, h); c->mom[1] = make_texture(GL_RGBA16F, w, h);
    c->tmp[0] = make_texture(GL_RGBA16F, w, h); c->tmp[1] = make_texture(GL_RGBA16F, w, h);
    /* This frame's sky light (written by the tracer, read only by the temporal pass) shares
       tmp[1], which the indirect-light filter first writes after that pass: one screen-sized
       texture less per camera. */
    c->sky = c->tmp[1];
    c->refl = make_texture(GL_RGBA16F, w, h);
    c->hist_frame = 0xfffffff0u;        /* nothing to accumulate from yet */
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    c->w = w; c->h = h;
    rt_log("rt: mask textures %ux%u for camera %d", w, h, (int)(c - R.cams));
    return 1;
}

static int trace(struct cam *c)
{
    const float *view = c->view, *proj = c->proj;
    GLint vp[4], prev_prog = 0;
    double iv[16], ivp[16], eye[3], base[3], px[3], py[3], sun[3], len;
    float vpm[16];
    u32 w, h;
    int i, j, k;
    double t0 = qpc_us();

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] < 64 || vp[3] < 64 || vp[2] > 16384 || vp[3] > 16384) return 0;
    c->vw = (u32)vp[2]; c->vh = (u32)vp[3];
    {
        float sc = R.scale, px = (float)vp[2] * (float)vp[3] * sc * sc;
        if (R.max_pixels > 0 && px > R.max_pixels) sc *= (float)sqrt(R.max_pixels / px);
        w = (u32)((float)vp[2] * sc + 0.5f); h = (u32)((float)vp[3] * sc + 0.5f);
    }
    if (w < 16 || h < 16 || w > 16384 || h > 16384) return 0;
    if (!R.finalized && !finalize()) return 0;
    if (!ensure_mask(c, w, h)) return 0;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double s = 0;
            for (k = 0; k < 4; k++) s += (double)view[i * 4 + k] * proj[k * 4 + j];
            vpm[i * 4 + j] = (float)s;
        }
    if (!invert4(view, iv) || !invert4(vpm, ivp) || fabs(iv[15]) < 1e-12) return 0;
    for (k = 0; k < 3; k++) eye[k] = iv[12 + k] / iv[15];
#define UNPROJECT(out, x, y) do { double nx = 2 * ((x) + 0.5) / w - 1, ny = 2 * ((y) + 0.5) / h - 1, q[4]; \
        for (k = 0; k < 4; k++) q[k] = nx * ivp[0 * 4 + k] + ny * ivp[1 * 4 + k] - ivp[2 * 4 + k] + ivp[3 * 4 + k]; \
        for (k = 0; k < 3; k++) (out)[k] = q[k] / q[3]; } while (0)
    UNPROJECT(base, 0, 0);
    UNPROJECT(px, 1, 0);
    UNPROJECT(py, 0, 1);
#undef UNPROJECT
    sun[0] = R.light_view[2]; sun[1] = R.light_view[6]; sun[2] = R.light_view[10];
    len = sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (len < 1e-9) return 0;
    for (k = 0; k < 3; k++) { sun[k] /= len; rt_stats.sun[k] = (float)sun[k]; }
    if (c == &R.cams[R.time_cam])
        for (k = 0; k < 3; k++) { rt_stats.cam_eye[k] = (float)eye[k]; rt_stats.cam_fwd[k] = -view[k * 4 + 2]; }

    if (!R.big_stacks && (R.frame & 15) == 0) {
        /* Did a ray run out of traversal stack in the last frames? */
        u32 flag = 0;
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, R.buf_stat);
        gl.GetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &flag);
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (flag) {
            rt_log("rt: a ray overflowed the traversal stack; switching to large stacks (slower)");
            R.big_stacks = 1;
            R.want_reload = 1;
        }
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    gl.UseProgram(R.prog);
    gl.Uniform3f(R.u_eye, (float)eye[0], (float)eye[1], (float)eye[2]);
    gl.Uniform3f(R.u_base, (float)base[0], (float)base[1], (float)base[2]);
    gl.Uniform3f(R.u_dx, (float)(px[0] - base[0]), (float)(px[1] - base[1]), (float)(px[2] - base[2]));
    gl.Uniform3f(R.u_dy, (float)(py[0] - base[0]), (float)(py[1] - base[1]), (float)(py[2] - base[2]));
    gl.Uniform3f(R.u_sun, (float)sun[0], (float)sun[1], (float)sun[2]);
    for (k = 0; k < 3; k++) R.water_sun[k] = (float)sun[k];
    /* The sun's sample pattern changes every frame when the result is accumulated over time. */
    gl.Uniform4f(R.u_params, (float)R.samples, (float)tan(R.radius * 3.14159265358979 / 180.0),
                 R.accum > 0 && R.samples > 1 && R.filter_radius > 0 ? (float)(R.frame & 63) : 0.0f, 0.0f);
    gl.Uniform1i(R.u_counts, (GLint)R.tlas_count);
    {
        int tex = ((R.gi && R.sky_rays > 0) || R.reflect || R.specular) && layers_ready();
        int gi = tex && R.gi && R.sky_rays > 0;
        int hist = gi && c->hist_frame + 1 == R.frame;
        int sky_ok = tex && SK.tex && SK.valid == 63;
        int light_ok = R.frame - R.light_frame < 600;
        float sun_col[3] = { 1.0f, 1.0f, 1.0f }, amb[3] = { 0.45f, 0.45f, 0.45f }, gain, mean_lum, amb_lum, fb[3];
        GLint act = 0, prev_arr = 0, prev_2d = 0;
        if (light_ok) { memcpy(sun_col, R.sun_col, 12); memcpy(amb, R.amb_col, 12); }
        gl.Uniform4f(R.u_sky, (float)R.sky_rays, gi ? 1e30f : R.sky_dist, gi ? 2.0f : R.bounce > 0 ? 1.0f : 0.0f,
                     R.accum > 0 ? (float)(R.frame & 63) : 0.0f);
        gl.Uniform4f(R.u_gi_rate, 1.0f, 0.0f, 0.0f, 0.0f);    /* set again below when there is history */
        {   /* moss: cloud shadows drift with the time (wrapped so the floats keep their precision) */
            double secs = qpc_us() * 1e-6, drift = R.cloud[2] * secs;
            drift -= floor(drift / 100000.0) * 100000.0;
            gl.Uniform4f(R.u_cloud, R.cloud[0], R.cloud[1], (float)drift, (float)(drift * 0.45));
        }
        c->gi = gi;
        c->tex_bound = tex;
        c->has_refl = tex && R.reflect && R.frame - R.water_frame < 8;
        gl.Uniform4f(R.u_reflect, c->has_refl ? 1.0f : 0.0f, tex && R.specular ? 1.0f : 0.0f, R.specular_strength, R.smooth ? 1.0f : 0.0f);
        /* Characters need the material table (which textures are skin), so only with it. */
        gl.Uniform4f(R.u_char, tex && R.char_on ? 1.0f : 0.0f, R.char_depth > 0.01f ? R.char_depth : 0.01f,
                     tex ? R.char_contact : 0.0f, R.char_reach > 0.1f ? R.char_reach : 0.1f);
        /* two capsules per matrix (column-major: A0, B0, A1, B1) */
        gl.UniformMatrix4fv(R.u_caps, 8, GL_FALSE, caps_data);
        gl.Uniform4f(R.u_leaf, tex && R.leaf_on ? 1.0f : 0.0f, R.leaf_depth > 0.01f ? R.leaf_depth : 0.01f, 0.0f, 0.0f);
        gl.Uniform1i(R.u_ncaps, tex && R.char_contact != 0 ? caps_n : 0);
        if (tex) {
            if (!R.buf_mat) { gl.GenBuffers(1, &R.buf_mat); mats_dirty = 1; }
            if (mats_dirty) {
                static float gpu[MAX_LAYERS * 8];
                u32 l;
                for (l = 0; l < MAX_LAYERS; l++) {
                    float *g = &gpu[l * 8];
                    g[0] = l < LY.used ? mats[l].rough : 1.0f; g[1] = mats[l].refl; g[2] = mats[l].metal; g[3] = (float)mats[l].chr;
                    g[4] = mats[l].emis[0]; g[5] = mats[l].emis[1]; g[6] = mats[l].emis[2]; g[7] = mats[l].coat * R.clearcoat;
                }
                gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, R.buf_mat);
                gl.BufferData(GL_SHADER_STORAGE_BUFFER, sizeof gpu, gpu, GL_DYNAMIC_DRAW);
                gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                mats_dirty = 0;
            }
            gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, R.buf_mat);
        }
        gl.BindImageTexture(6, c->refl, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        if (tex) {
            glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
            gl.ActiveTexture(GL_TEXTURE0 + 10);
            glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev_arr);
            glBindTexture(GL_TEXTURE_2D_ARRAY, LY.array);
            gl.ActiveTexture(GL_TEXTURE0 + 11);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_2d);
            if (sky_ok) { glBindTexture(GL_TEXTURE_2D, SK.tex); sky_refresh(); }
            c->tex_restore[0] = (GLuint)act; c->tex_restore[1] = (GLuint)prev_arr; c->tex_restore[2] = (GLuint)prev_2d;
            /* The sky's brightness is set so that open ground gets as much light from it as
               the game's ambient colour gave; the sky then only decides its tint. */
            mean_lum = 0.3f * SK.mean[0] + 0.59f * SK.mean[1] + 0.11f * SK.mean[2];
            amb_lum = 0.3f * amb[0] + 0.59f * amb[1] + 0.11f * amb[2];
            gain = sky_ok && SK.mean_frame && mean_lum > 0.02f ? amb_lum / mean_lum : 1.0f;
            gain *= R.gi_sky;
            for (k = 0; k < 3; k++) {
                /* light assumed to arrive where nothing is known yet: a bit over half the open sky's */
                float open = amb[k] * R.gi_sky;
                if (sky_ok && SK.mean_frame) open += (SK.mean[k] * gain - open) * R.gi_tint;
                fb[k] = open * 0.6f;
            }
            gl.Uniform4f(R.u_sky_tint, R.gi_tint, 0.0f, 0.0f, 0.0f);
            gl.Uniform1i(R.u_albedo, 10);
            gl.Uniform1i(R.u_sky_atlas, 11);
            gl.Uniform3f(R.u_sun_col, sun_col[0], sun_col[1], sun_col[2]);
            gl.Uniform4f(R.u_sky_gain, gain, sky_ok ? 1.0f : 0.0f, amb[0] * R.gi_sky, amb[1] * R.gi_sky);
            gl.Uniform4f(R.u_fallback, fb[0], fb[1], fb[2], amb[2] * R.gi_sky);
            gl.Uniform4f(R.u_gi, hist ? 1.0f : 0.0f, R.water_y, R.frame - R.water_frame < 8 ? 1.0f : 0.0f, R.gi_bounce);
            /* Skipping sky rays needs last frame's result, both here and in the temporal pass. */
            gl.Uniform4f(R.u_gi_rate, hist && R.accum > 0 ? (float)R.gi_rate : 1.0f, (float)(R.frame & 0xffff), R.gi_far, 0.0f);
            gl.UniformMatrix4fv(R.u_prev_vp, 1, GL_FALSE, c->prev_vp);
            gl.Uniform3f(R.u_prev_eye, c->prev_eye[0], c->prev_eye[1], c->prev_eye[2]);
            gl.BindImageTexture(4, c->mask[c->cur], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
            gl.BindImageTexture(5, c->bounce[c->cur], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        }
    }
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, R.buf_nodes);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, R.buf_tris);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, R.buf_tlas);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, R.buf_inst);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, R.buf_alpha);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, R.buf_stat);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, R.buf_nrm);
    gl.BindImageTexture(0, c->raw, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    gl.BindImageTexture(2, c->sky, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    i = !R.query_pending && c == &R.cams[R.time_cam];
    if (i) gl.BeginQuery(GL_TIME_ELAPSED, R.query);
    gl.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
    c->out = c->raw;
    c->has_sky = 0;
    if ((R.samples > 1 && R.filter_radius > 0) || R.sky_rays > 0) {
        int nxt = c->cur ^ 1, it, passes = R.sky_rays > 0 ? R.denoise : 0;
        int hist = R.accum > 0 && c->hist_frame + 1 == R.frame;
        float e3[3] = { (float)eye[0], (float)eye[1], (float)eye[2] };
        float b3[3] = { (float)base[0], (float)base[1], (float)base[2] };
        float x3[3] = { (float)(px[0] - base[0]), (float)(px[1] - base[1]), (float)(px[2] - base[2]) };
        float y3[3] = { (float)(py[0] - base[0]), (float)(py[1] - base[1]), (float)(py[2] - base[2]) };
        GLuint from, to;
#define RAYS(E, B, X, Y) do { gl.Uniform3f(E, e3[0], e3[1], e3[2]); gl.Uniform3f(B, b3[0], b3[1], b3[2]); \
        gl.Uniform3f(X, x3[0], x3[1], x3[2]); gl.Uniform3f(Y, y3[0], y3[1], y3[2]); } while (0)
        /* 1. Accumulate over time, unfiltered; the spatial passes then work on the steadier result. */
        gl.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        gl.UseProgram(R.tprog);
        RAYS(R.t_eye, R.t_base, R.t_dx, R.t_dy);
        gl.Uniform4f(R.t_hist, 1.0f - R.accum, hist ? 1.0f : 0.0f, R.accum > 0 ? 0.2f : 1.0f, 0.0f);
        gl.UniformMatrix4fv(R.t_prev_vp, 1, GL_FALSE, c->prev_vp);
        gl.Uniform3f(R.t_prev_eye, c->prev_eye[0], c->prev_eye[1], c->prev_eye[2]);
        gl.BindImageTexture(1, c->raw, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        gl.BindImageTexture(2, c->sky, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        gl.BindImageTexture(4, c->acc[c->cur], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        gl.BindImageTexture(5, c->mask[c->cur], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        gl.BindImageTexture(6, c->mom[c->cur], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        gl.BindImageTexture(0, c->acc[nxt], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        gl.BindImageTexture(3, c->mom[nxt], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        gl.BindImageTexture(7, c->tmp[0], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        gl.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
        /* 2. Sun and sky visibility: a small blur along surfaces. */
        gl.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        gl.UseProgram(R.fprog);
        RAYS(R.f_eye, R.f_base, R.f_dx, R.f_dy);
        gl.Uniform4f(R.f_params, R.samples > 1 ? (float)R.filter_radius : 0.0f, R.sky_rays > 0 ? 2.0f : 0.0f, 0.0f, 0.0f);
        gl.BindImageTexture(2, c->acc[nxt], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        gl.BindImageTexture(4, c->mom[nxt], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        gl.BindImageTexture(0, c->mask[nxt], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        gl.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
        /* 3. Indirect light: several passes, each reaching twice as far, steered by the variance. */
        if (passes > 0) {
            gl.UseProgram(R.aprog);
            RAYS(R.a_eye, R.a_base, R.a_dx, R.a_dy);
            from = c->tmp[0];
            for (it = 0; it < passes; it++) {
                to = it == passes - 1 ? c->bounce[nxt] : c->tmp[(it + 1) & 1];
                gl.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                gl.Uniform1i(R.a_step, 1 << it);
                gl.BindImageTexture(2, from, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                gl.BindImageTexture(0, to, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
                gl.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
                from = to;
            }
            if (c->gi) {
                /* Light from effects (fx.c): shadow rays to the brightest, added to the indirect light. */
                struct fx_view fv;
                memset(&fv, 0, sizeof fv);
                fv.cam = (int)(c - R.cams); fv.w = w; fv.h = h;
                fv.geom = c->raw; fv.prev_mask = c->mask[c->cur]; fv.light = c->bounce[nxt];
                fv.hist = c->hist_frame + 1 == R.frame;
                memcpy(fv.eye, e3, 12); memcpy(fv.base, b3, 12); memcpy(fv.dx, x3, 12); memcpy(fv.dy, y3, 12);
                memcpy(fv.prev_vp, c->prev_vp, 64); memcpy(fv.prev_eye, c->prev_eye, 12);
                fv.tlas_count = R.tlas_count; fv.frame = R.frame; fv.main_prog = R.prog;
                fv.defines = R.big_stacks ? "#define MESH_STACK 56\n#define TOP_STACK 32\n" : "";
                fx_trace(&fv);
            }
        }
#undef RAYS
        c->has_sky = R.sky_rays > 0;
        c->cur = nxt;
        c->out = c->mask[nxt];
        c->out_bounce = c->bounce[nxt];
        memcpy(c->prev_vp, vpm, sizeof c->prev_vp);
        for (k = 0; k < 3; k++) c->prev_eye[k] = (float)eye[k];
        c->hist_frame = R.frame;
    }
    if (i) { gl.EndQuery(GL_TIME_ELAPSED); R.query_pending = 1; }
    if (c->tex_bound) {                     /* texture units as the game left them */
        gl.ActiveTexture(GL_TEXTURE0 + 11);
        glBindTexture(GL_TEXTURE_2D, c->tex_restore[2]);
        gl.ActiveTexture(GL_TEXTURE0 + 10);
        glBindTexture(GL_TEXTURE_2D_ARRAY, c->tex_restore[1]);
        gl.ActiveTexture(c->tex_restore[0]);
    }
    gl.MemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gl.UseProgram((GLuint)prev_prog);
    R.f_trace_us += qpc_us() - t0;
    return 1;
}

GLuint rt_prepare_main(const float *view, const float *proj, int landscape, float *inv_size, float *mask_size,
                       GLuint *bounce_texture)
{
    struct cam *c = NULL;
    u32 i;
    if (!R.enabled || !R.ninst) return 0;
    /* No shadow pass for a few frames (a menu, another scene): the casters we hold belong
       to something else. */
    if (R.frame - R.collect_frame > 2) return 0;
    if (R.cam_frame != R.frame) { R.cam_frame = R.frame; R.ncams = 0; }
    for (i = 0; i < R.ncams; i++)
        if (!memcmp(view, R.cams[i].view, 64) && !memcmp(proj, R.cams[i].proj, 64)) { c = &R.cams[i]; break; }
    if (!c) {
        /* A camera counts as a scene camera once its projection has been seen drawing
           the landscape. That keeps out the HUD's 3D models, which have their own. */
        for (i = 0; i < R.nscene_proj; i++)
            if (!memcmp(proj, R.scene_proj[i], 64)) break;
        if (i == R.nscene_proj) {
            if (!landscape) return 0;
            if (R.nscene_proj == 8) R.nscene_proj = 0;
            memcpy(R.scene_proj[R.nscene_proj++], proj, 64);
        }
        if (R.ncams == MAX_CAMS) return 0;
        c = &R.cams[R.ncams++];
        memcpy(c->view, view, 64);
        memcpy(c->proj, proj, 64);
        c->ok = ensure_gpu() && trace(c);
    }
    if (!c->ok) return 0;
    R.last_cam = (u32)(c - R.cams); R.last_cam_frame = R.frame;
    inv_size[0] = 1.0f / (float)c->vw;
    inv_size[1] = 1.0f / (float)c->vh;
    mask_size[0] = (float)c->w; mask_size[1] = (float)c->h;
    mask_size[2] = c->has_sky ? R.sky_strength : 0.0f;      /* how much sky occlusion to apply; 0 = none */
    if (c->has_sky && c->gi) mask_size[2] = -1.0f;          /* negative: the second texture is all the indirect light */
    mask_size[3] = c->has_sky ? R.bounce : 0.0f;            /* how much bounced sunlight to add */
    *bounce_texture = c->has_sky ? c->out_bounce : c->out;  /* something valid to sample either way */
    return c->out;
}

int rt_is_main_view(const float *view)
{
    u32 i, best = 0;
    if (R.cam_frame != R.frame || !R.ncams) return 0;
    for (i = 1; i < R.ncams; i++)
        if (R.cams[i].w * R.cams[i].h > R.cams[best].w * R.cams[best].h) best = i;
    if (memcmp(view, R.cams[best].view, 64)) {
        static u32 logged;
        if (logged++ < 3) {
            const float *a = view, *b = R.cams[best].view;
            rt_log("fx debug: view %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.2f %.2f %.2f %.2f", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
            rt_log("fx debug: cam  %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.2f %.2f %.2f %.2f (cam %u of %u)", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], best, R.ncams);
        }
        return 0;
    }
    return 1;
}

void rt_frame_end(void)
{
    rt_stats.enabled = (u32)R.enabled;
    rt_stats.ok = R.cam_frame == R.frame && R.ncams && R.cams[0].ok;
    rt_stats.instances = R.ninst;
    rt_stats.static_tris = R.st_count; rt_stats.frame_tris = R.ft_count;
    rt_stats.static_nodes = R.sn_count; rt_stats.frame_nodes = R.fn_count;
    rt_stats.cache_entries = table_used;
    rt_stats.built_static = R.f_built_static; rt_stats.built_frame = R.f_built_frame;
    rt_stats.skipped = R.f_skipped;
    rt_stats.alpha_instances = R.f_alpha_inst; rt_stats.alpha_skipped = R.f_alpha_skipped;
    rt_stats.collect_us = (u32)R.f_collect_us; rt_stats.finalize_us = (u32)R.f_finalize_us;
    rt_stats.trace_cpu_us = (u32)R.f_trace_us;
    rt_stats.traces = R.cam_frame == R.frame ? R.ncams : 0;
    /* Report (and time) the largest mask: that is the main view. */
    {
        u32 i, best = 0;
        for (i = 1; i < rt_stats.traces; i++)
            if (R.cams[i].w * R.cams[i].h > R.cams[best].w * R.cams[best].h) best = i;
        R.time_cam = best;
        rt_stats.mask_w = R.cams[best].w; rt_stats.mask_h = R.cams[best].h;
    }
    rt_stats.pool_bytes = (u32)((size_t)R.sn_cap * sizeof(BvhNode) + (size_t)R.st_cap * sizeof(BvhTri) +
                                (size_t)R.fn_cap * sizeof(BvhNode) + (size_t)R.ft_cap * sizeof(BvhTri));
    rt_stats.shadow_bytes = shadow_bytes;
    rt_stats.samples = (u32)R.samples;
    rt_stats.sun_radius = R.radius;
    if (R.query_pending && gl_state > 0) {
        GLuint avail = 0, ns = 0;
        gl.GetQueryObjectuiv(R.query, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (avail) {
            gl.GetQueryObjectuiv(R.query, GL_QUERY_RESULT, &ns);
            rt_stats.trace_gpu_us = ns / 1000;
            R.query_pending = 0;
        }
    }
    R.p_total += R.f_collect_us;
    if (++R.p_frames == 600) {
        rt_log("rt: collect profile per frame: total %.0f us = hashing %.0f + expanding %.0f + building %.0f + rest %.0f; "
               "%.1f meshes (%.0f triangles) per frame, %.1f of them refitted; smooth normals made %.0f us", R.p_total / 600, R.p_hash / 600,
               R.p_expand / 600, R.p_build / 600, (R.p_total - R.p_hash - R.p_expand - R.p_build) / 600,
               R.p_built / 600.0, R.p_tris / 600.0, R.p_refit / 600.0, R.p_normals / 600);
        R.p_total = R.p_hash = R.p_expand = R.p_build = R.p_normals = 0; R.p_frames = R.p_built = R.p_tris = R.p_refit = 0;
    }
    R.f_built_static = R.f_built_frame = R.f_skipped = R.f_alpha_inst = R.f_alpha_skipped = 0;
    R.f_collect_us = R.f_finalize_us = R.f_trace_us = 0;
    R.frame++;
    if ((R.frame & 31) == 0) table_sweep();
}

void rt_set_enabled(int on)
{
    R.enabled = on;
    if (!R.samples) { R.samples = 1; R.radius = 1.0f; R.filter_radius = 2; }
    if (R.scale <= 0) R.scale = 1.0f;
    if (on && rt_stats.error == 2) R.want_reload = 1;
}

void rt_set_soft(int samples, float radius_degrees, int filter_radius)
{
    R.samples = samples < 1 ? 1 : samples > 64 ? 64 : samples;
    R.radius = radius_degrees;
    R.filter_radius = filter_radius < 0 ? 0 : filter_radius > 4 ? 4 : filter_radius;
}

void rt_set_accumulate(float weight)
{
    R.accum = weight < 0 ? 0 : weight > 0.99f ? 0.99f : weight;
}

void rt_set_sky(int rays, float distance, float strength, float bounce)
{
    R.bounce = bounce < 0 ? 0 : bounce > 4 ? 4 : bounce;
    R.sky_rays = rays < 0 ? 0 : rays > 32 ? 32 : rays;
    R.sky_dist = distance > 1.0f ? distance : 1.0f;
    R.sky_strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
}

GLuint rt_water_reflection(float *info)
{
    struct cam *c = &R.cams[R.last_cam < MAX_CAMS ? R.last_cam : 0];
    if (!R.enabled || !R.reflect || R.last_cam_frame != R.frame || !c->ok || !c->has_refl || !c->refl) return 0;
    info[0] = 1.0f / (float)c->vw; info[1] = 1.0f / (float)c->vh; info[2] = R.reflect_strength;
    return c->refl;
}

GLuint rt_last_reflection(void)
{
    struct cam *c = &R.cams[R.last_cam < MAX_CAMS ? R.last_cam : 0];
    return R.last_cam_frame == R.frame && c->ok ? c->refl : 0;
}

void rt_set_smooth(int on)
{
    R.smooth = on;
}

void rt_set_characters(int on, float depth, float contact, float reach)
{
    R.char_on = on; R.char_depth = depth; R.char_contact = contact; R.char_reach = reach;
}

void rt_set_clearcoat(float strength)
{
    strength = strength < 0 ? 0 : strength > 8 ? 8 : strength;
    if (strength != R.clearcoat) { R.clearcoat = strength; mats_dirty = 1; }
}

void rt_set_foliage(int on, float depth)
{
    R.leaf_on = on; R.leaf_depth = depth;
}

void rt_set_specular(int on, float strength)
{
    R.specular = on;
    R.specular_strength = strength < 0 ? 0 : strength > 8 ? 8 : strength;
}

int rt_dump_layers(const char *path)
{
    enum { LEVEL = 1, EDGE = LAYER_SIZE >> LEVEL };
    GLint prev = 0;
    u8 *px;
    FILE *f;
    u32 hdr[3], l;
    if (!LY.array || gl_state <= 0) return 7;
    px = malloc((size_t)MAX_LAYERS * EDGE * EDGE * 4);
    if (!px) return 3;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev);
    glBindTexture(GL_TEXTURE_2D_ARRAY, LY.array);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, LEVEL, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPopClientAttrib();
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)prev);
    f = fopen(path, "wb");
    if (!f) { free(px); return 4; }
    hdr[0] = 0x4C545257; hdr[1] = LY.used; hdr[2] = EDGE;      /* 'WRTL' */
    fwrite(hdr, 4, 3, f);
    for (l = 0; l < LY.used; l++) {
        float v[4] = { mats[l].rough, mats[l].refl, mats[l].metal, mats[l].emis[0] };
        u32 flag = (u32)mats[l].from_file;
        fwrite(&mats[l].hash, 4, 1, f); fwrite(v, 4, 4, f); fwrite(&flag, 4, 1, f);
        fwrite(px + (size_t)l * EDGE * EDGE * 4, 1, EDGE * EDGE * 4, f);
    }
    fclose(f);
    free(px);
    return 0;
}

int rt_dump_textures(const char *prefix)
{
    GLint prev = 0;
    u32 id, done = 0;
    if (!LY.array || gl_state <= 0) return 7;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    for (id = 1; id < 65536; id++) {
        GLint w = 0, h = 0;
        char name[320];
        u32 hdr[3];
        u8 *px;
        FILE *f;
        if (!layer_of_tex[id]) continue;
        glBindTexture(GL_TEXTURE_2D, id);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) continue;
        px = malloc((size_t)w * h * 4);
        if (!px) continue;
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        _snprintf(name, sizeof name, "%s%08x.wtx", prefix, mats[layer_of_tex[id]].hash);
        name[sizeof name - 1] = 0;
        f = fopen(name, "wb");
        if (f) {
            hdr[0] = 0x31585457; hdr[1] = (u32)w; hdr[2] = (u32)h;      /* 'WTX1' */
            fwrite(hdr, 4, 3, f);
            fwrite(px, 1, (size_t)w * h * 4, f);
            fclose(f);
            done++;
        }
        free(px);
    }
    glPopClientAttrib();
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    return done ? 0 : 4;
}

void rt_set_reflections(int on, float strength)
{
    R.reflect = on;
    R.reflect_strength = strength < 0 ? 0 : strength > 8 ? 8 : strength;
}

void rt_set_denoise(int passes)
{
    R.denoise = passes < 1 ? 1 : passes > 5 ? 5 : passes;
}

/* moss: cloud shadows over everything the sun lights: strength 0..1 (0 = off), size of a cloud
   in world units, drift speed in world units a second. */
void rt_set_clouds(float strength, float size, float speed)
{
    R.cloud[0] = strength < 0 ? 0 : strength > 1 ? 1 : strength;
    R.cloud[1] = size > 1 ? size : 1;
    R.cloud[2] = speed;
}

void rt_set_gi_rate(int rate, float far_dist)
{
    R.gi_rate = rate >= 4 ? 4 : rate >= 2 ? 2 : 1;
    R.gi_far = far_dist > 0 ? far_dist : 0;
}

void rt_set_gi(int on, float sky_gain, float bounce_gain, float tint)
{
    R.gi_tint = tint < 0 ? 0 : tint > 1 ? 1 : tint;
    R.gi = on;
    R.gi_sky = sky_gain < 0 ? 0 : sky_gain > 8 ? 8 : sky_gain;
    R.gi_bounce = bounce_gain < 0 ? 0 : bounce_gain > 8 ? 8 : bounce_gain;
}

void rt_set_light(const float *sun_rgb, const float *ambient_rgb)
{
    if (fabs(sun_rgb[0] - R.sun_col[0]) + fabs(sun_rgb[1] - R.sun_col[1]) + fabs(sun_rgb[2] - R.sun_col[2]) +
        fabs(ambient_rgb[0] - R.amb_col[0]) + fabs(ambient_rgb[1] - R.amb_col[1]) + fabs(ambient_rgb[2] - R.amb_col[2]) > 0.02)
        rt_log("rt: sun colour %.2f %.2f %.2f, ambient colour %.2f %.2f %.2f", sun_rgb[0], sun_rgb[1], sun_rgb[2],
               ambient_rgb[0], ambient_rgb[1], ambient_rgb[2]);
    memcpy(R.sun_col, sun_rgb, 12);
    memcpy(R.amb_col, ambient_rgb, 12);
    R.light_frame = R.frame;
}

void rt_water_sun(float *s)
{
    s[0] = R.water_sun[0]; s[1] = R.water_sun[1]; s[2] = R.water_sun[2];
}

void rt_set_water(float height)
{
    R.water_y = height;
    R.water_frame = R.frame;
}

void rt_set_scale(float scale, float max_pixels)
{
    R.scale = scale < 0.125f ? 0.125f : scale > 8.0f ? 8.0f : scale;
    if (max_pixels > 0) R.max_pixels = max_pixels;
}

void rt_reload_shader(void)
{
    R.want_reload = 1;
}

int rt_dump_mask(const char *path, u32 cam)
{
    GLint prev = 0;
    float *buf;
    FILE *f;
    u32 hdr[3];
    struct cam *c = &R.cams[(cam & 0xff) < MAX_CAMS ? cam & 0xff : 0];
    /* 0x100: what the tracer wrote, before filtering. 0x400: the bounce texture.
       0x200: write the texture's third and fourth channels instead of the first two. */
    GLuint tex = (cam & 0x400) ? c->out_bounce : (cam & 0x100) ? c->raw : c->out;
    u32 i, npix;
    if (!tex || !c->w) return 7;
    buf = malloc((size_t)c->w * c->h * 4 * sizeof(float));
    if (!buf) return 3;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, buf);
    npix = c->w * c->h;
    for (i = 0; i < npix; i++) {            /* keep two of the four channels */
        buf[i * 2] = buf[i * 4 + ((cam & 0x200) ? 2 : 0)];
        buf[i * 2 + 1] = buf[i * 4 + ((cam & 0x200) ? 3 : 1)];
    }
    glPopClientAttrib();
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    f = fopen(path, "wb");
    if (!f) { free(buf); return 4; }
    hdr[0] = 0x46545257; hdr[1] = c->w; hdr[2] = c->h;     /* 'WRTF' */
    fwrite(hdr, 4, 3, f);
    fwrite(buf, sizeof(float), (size_t)c->w * c->h * 2, f);
    fclose(f);
    free(buf);
    return 0;
}

/* ---- start and end of a core (see rt.h) --------------------------------------------- */
#define GL_BUFFER_MAPPED 0x88BC
#ifndef GL_BUFFER_SIZE
#define GL_BUFFER_SIZE 0x8764
#endif

/* Copy every vertex/index buffer the engine already has into the CPU mirror: static level
   geometry is uploaded once, long before a reloaded core could have seen it. */
static void mirror_existing_buffers(void)
{
    GLboolean (APIENTRY *is_buffer)(GLuint) = (void *)rt_real_proc("glIsBuffer");
    void (APIENTRY *bind)(GLenum, GLuint) = (void *)rt_real_proc("glBindBuffer");
    void (APIENTRY *param)(GLenum, GLenum, GLint *) = (void *)rt_real_proc("glGetBufferParameteriv");
    void (APIENTRY *get)(GLenum, ptrdiff_t, ptrdiff_t, void *) = (void *)rt_real_proc("glGetBufferSubData");
    GLint prev = 0;
    u32 id, n = 0, last = 0;
    double t0 = qpc_us();
    if (!is_buffer || !bind || !param || !get || !wglGetCurrentContext()) return;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev);
    for (id = 1; id < MAX_BUF; id++) {
        GLint size = 0, mapped = 0;
        if (!is_buffer(id)) {
            if (id > last + 4096) break;        /* ids are handed out in order */
            continue;
        }
        last = id;
        bind(GL_ARRAY_BUFFER, id);
        param(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
        param(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &mapped);
        if (size <= 0 || mapped) continue;
        if (!bufs[id]) bufs[id] = calloc(1, sizeof(struct shadow_buf));
        if (!bufs[id]) break;
        bufs[id]->data = malloc((size_t)size);
        if (!bufs[id]->data) { free(bufs[id]); bufs[id] = NULL; continue; }
        bufs[id]->size = (u32)size;
        bufs[id]->version = 1;
        get(GL_ARRAY_BUFFER, 0, size, bufs[id]->data);
        shadow_bytes += (u32)size;
        n++;
    }
    bind(GL_ARRAY_BUFFER, (GLuint)prev);
    while (glGetError() != GL_NO_ERROR) ;
    rt_log("rt: %u existing buffers copied into the mirror (%.1f MB) in %.0f ms", n, shadow_bytes / 1e6,
           (qpc_us() - t0) / 1000.0);
}

void rt_start(void *(*persist)(const char *name, u32 size, int *created))
{
    TM = persist ? persist("textures", sizeof(struct tex_memory), NULL) : NULL;
    if (TM) nrm_tex = TM->nrm;
    KP = persist ? persist("programs", sizeof(struct kept_programs), NULL) : NULL;
    mirror_existing_buffers();
}

void rt_shutdown(void)
{
    void (APIENTRY *del_bufs)(GLsizei, const GLuint *) = (void *)rt_real_proc("glDeleteBuffers");
    void (APIENTRY *del_fbos)(GLsizei, const GLuint *) = (void *)rt_real_proc("glDeleteFramebuffers");
    void (APIENTRY *del_queries)(GLsizei, const GLuint *) = (void *)rt_real_proc("glDeleteQueries");
    u32 i, ntex = 0;
    if (job.thread) {
        InterlockedExchange(&job.quit, 1);
        SetEvent(job.wake);
        if (WaitForSingleObject(job.thread, 10000) != WAIT_OBJECT_0) rt_log("rt: the merge worker did not stop");
        CloseHandle(job.thread);
        CloseHandle(job.wake);
        job.thread = NULL;
    }
    if (gl_state <= 0 || !wglGetCurrentContext()) return;
    sc_shutdown();
    {
        GLuint mine[6];
        u32 k, j, slot = 0;
        mine[0] = R.prog; mine[1] = R.fprog; mine[2] = R.tprog; mine[3] = R.aprog;
        mine[4] = fx_program(0); mine[5] = fx_program(1);
        for (k = 0; k < 6; k++) {
            int kept = 0;
            if (!mine[k]) continue;
            for (j = 0; KP && j < 16 && !kept; j++)
                if (made[j].prog == mine[k] && slot < 8) {
                    if (KP->e[slot].prog) gl.DeleteProgram(KP->e[slot].prog);
                    KP->e[slot].prog = mine[k]; KP->e[slot].hash = made[j].hash;
                    slot++; kept = 1;
                }
            if (!kept) gl.DeleteProgram(mine[k]);
        }
    }
    if (del_bufs) {
        GLuint b[8];
        b[0] = R.buf_nodes; b[1] = R.buf_tris; b[2] = R.buf_tlas; b[3] = R.buf_inst;
        b[4] = R.buf_alpha; b[5] = R.buf_stat; b[6] = R.buf_mat; b[7] = R.buf_nrm;
        del_bufs(8, b);
    }
    if (del_queries && R.query) del_queries(1, &R.query);
    fx_shutdown();
    for (i = 0; i < MAX_CAMS; i++) {
        struct cam *c = &R.cams[i];
        GLuint t[13];
        if (!c->raw) continue;
        t[0] = c->raw; t[1] = c->sky; t[2] = c->mask[0]; t[3] = c->mask[1]; t[4] = c->bounce[0]; t[5] = c->bounce[1];
        t[6] = c->acc[0]; t[7] = c->acc[1]; t[8] = c->mom[0]; t[9] = c->mom[1]; t[10] = c->tmp[0]; t[11] = c->tmp[1];
        t[12] = c->refl;
        glDeleteTextures(13, t);
    }
    if (LY.array) glDeleteTextures(1, &LY.array);
    if (SK.tex) glDeleteTextures(1, &SK.tex);
    if (SK.fbo && del_fbos) del_fbos(1, &SK.fbo);
    for (i = 1; i < 65536; i++)        /* normal maps stay for the next core when the loader keeps them */
        if (nrm_tex[i] && nrm_tex == nrm_local) { glDeleteTextures(1, &nrm_tex[i]); nrm_tex[i] = 0; ntex++; }
    while (glGetError() != GL_NO_ERROR) ;
    rt_log("rt: GPU objects deleted (%u normal maps)", ntex);
}

float rt_center_distance(void)
{
    struct cam *c = &R.cams[R.time_cam < MAX_CAMS ? R.time_cam : 0];
    GLint prev = 0;
    float *buf, d;
    if (!c->out || !c->w || !c->h || gl_state <= 0) return 0;
    buf = malloc((size_t)c->w * c->h * 4 * sizeof(float));
    if (!buf) return 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, c->out);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, buf);
    glPopClientAttrib();
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    d = buf[((size_t)(c->h / 2) * c->w + c->w / 2) * 4 + 1];
    free(buf);
    return d > 0 && d < 59999 ? d : 0;
}

/* The film post-processing's depth of field (post.c) reads the eye distance of the main
   view, the scene camera traced last this frame. */
GLuint rt_post_mask(u32 *mask_w, u32 *mask_h, u32 *view_w, u32 *view_h)
{
    struct cam *c = &R.cams[R.last_cam < MAX_CAMS ? R.last_cam : 0];
    if (!R.enabled || R.last_cam_frame != R.frame || !c->ok || !c->out) return 0;
    *mask_w = c->w; *mask_h = c->h; *view_w = c->vw; *view_h = c->vh;
    return c->out;
}

/* ... and its camera, the direction towards the sun (world) and the sun's colour, for the sun glare. */
int rt_post_camera(float *view, float *proj, float *sun, float *sun_col)
{
    struct cam *c = &R.cams[R.last_cam < MAX_CAMS ? R.last_cam : 0];
    if (!R.enabled || R.last_cam_frame != R.frame || !c->ok) return 0;
    memcpy(view, c->view, 64); memcpy(proj, c->proj, 64);
    memcpy(sun, rt_stats.sun, 12); memcpy(sun_col, R.sun_col, 12);
    return 1;
}

#include "scatter.c"
