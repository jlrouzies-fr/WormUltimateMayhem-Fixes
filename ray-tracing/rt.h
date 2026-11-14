/* Shared declarations for the real-time sun shadow tracer inside wumrt. */
#ifndef RT_H
#define RT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;
typedef unsigned long long u64;

/* The core (wumrt_core.dll, built with WUMRT_CORE) allocates from a heap of its own that
   goes away with it, so a core can be unloaded without leaking what it held. */
#ifdef WUMRT_CORE
#include <stdlib.h>
void *core_malloc(size_t n);
void *core_calloc(size_t n, size_t m);
void *core_realloc(void *p, size_t n);
void core_free(void *p);
#define malloc(n) core_malloc(n)
#define calloc(n, m) core_calloc(n, m)
#define realloc(p, n) core_realloc(p, n)
#define free(p) core_free(p)
#endif

/* ---- BVH (bvh.c) ---------------------------------------------------------------
   A node holds the boxes of its two children. Child i is a leaf if n_i > 0 (primitives
   c_i .. c_i + n_i - 1), otherwise c_i is the index of another node (never 0). A BVH
   that is a single leaf repeats that leaf as both children. Node 0 is the root; indices are relative to the start of the
   BVH they belong to. */
typedef struct { float lo0[3]; u32 c0; float hi0[3]; u32 n0; float lo1[3]; u32 c1; float hi1[3]; u32 n1; } BvhNode;   /* 64 bytes */
/* uv0..2: texture coordinates of the three corners, each packed as u << 18 | v << 4 (14 bits
   each, 10 of them fraction). The low 4 bits of the three words together hold a texture
   layer (uv0 bits 0-3, uv1 4-7, uv2 8-11), used by triangles baked into the merged BVH.
   Three equal coordinates mean the triangle has none. */
typedef struct { float v0[3]; u32 uv0; float e1[3]; u32 uv1; float e2[3]; u32 uv2; } BvhTri;   /* 48 bytes */
/* Smooth normals of one triangle's corners (octahedral, 16 + 16 bits each); flag 0 = none,
   use the flat normal. Kept in a buffer parallel to the triangles, index for index. */
typedef struct { u32 a, b, c, flag; } NrmTri;   /* 16 bytes */

/* Builds a BVH over n triangles (9 floats each; uv3: 3 packed UVs each, or NULL). nodes must
   hold n entries, tris n. quality 0 = fast (per-frame meshes), 1 = binned SAH (cached
   meshes). Returns node count. */
struct bvh_ctx;                             /* a builder's scratch memory; NULL = the main thread's */
struct bvh_ctx *bvh_ctx_new(void);
u32 bvh_build(struct bvh_ctx *ctx, const float *tri9, const u32 *uv3, u32 n, int quality, BvhNode *nodes, BvhTri *tris);
/* Builds a BVH over n boxes (lo xyz, hi xyz); every leaf holds one box index. nodes: n. */
u32 bvh_build_boxes(const float *box6, u32 n, BvhNode *nodes);
void bvh_bounds(const BvhNode *root, float *lo, float *hi);     /* box around a whole BVH */
/* After bvh_build: for each output triangle, which input triangle it was. */
const u32 *bvh_order(struct bvh_ctx *ctx);
/* Recompute every box of a BVH whose triangles moved (same tree, same triangle order). */
void bvh_refit(BvhNode *nodes, u32 nnodes, const BvhTri *tris);

/* ---- tracer (rt_core.c) ------------------------------------------------------- */
struct rt_stats {
    u32 enabled, ok;                /* ok: last frame produced a mask */
    u32 instances, static_tris, frame_tris, static_nodes, frame_nodes;
    u32 cache_entries, built_static, built_frame;       /* last frame */
    u32 skipped;                    /* caster draws we could not use, last frame */
    u32 collect_us, finalize_us, trace_cpu_us, trace_gpu_us;
    u32 traces, mask_w, mask_h;
    u32 pool_bytes, shadow_bytes;
    u32 error;                      /* 0 ok, 1 no compute support, 2 shader failed */
    u32 samples;
    u32 alpha_instances, alpha_skipped;     /* alpha-tested casters traced / left out, last frame */
    u32 merged_instances, merged_tris, merge_ms, merges;    /* static geometry merged into one BVH */
    u32 tlas_instances;                     /* what the top level holds after merging */
    float sun[3];
    float sun_radius;               /* degrees */
    float cam_eye[3], cam_fwd[3];   /* main camera, world space */
};
extern struct rt_stats rt_stats;

void rt_log(const char *fmt, ...);                      /* provided by wumrt.c */
PROC rt_real_proc(const char *name);                    /* provided by wumrt.c: real wglGetProcAddress */
void rt_buffer_deleted(GLuint id);                      /* provided by wumrt.c: keeps its tracked bindings right */

/* Wraps the buffer functions the engine fetches, so vertex data can be mirrored. */
PROC rt_wrap_proc(const char *name, PROC real);

/* One draw call, with the GL state that matters to the tracer (tracked by wumrt.c). */
struct rt_draw {
    GLenum mode; u32 first, count; GLenum itype; const void *indices; int arrays;
    int v_on; u32 v_size, v_stride, v_buf; GLenum v_type; const void *v_ptr;       /* positions */
    int tc_on; u32 tc_size, tc_stride, tc_buf; GLenum tc_type; const void *tc_ptr; /* texture unit 0 */
    int n_on; u32 n_stride, n_buf; GLenum n_type; const void *n_ptr;               /* vertex normals */
    u32 elem_buf;
    int alpha_test, blend; float alpha_ref;
    u32 texture;            /* main pass only: the material's texture */
    const float *tex_xf;    /* main pass only: the vertex program's texture matrix, or NULL */
    const float *material;  /* main pass only: the game's material matrix (rows: specular colour, emissive
                               colour, fresnel colour, then fresnel power and specular power), or NULL */
    u64 static_key;         /* nonzero: our own geometry in plain memory that never changes under this key */
    int own;                /* our own geometry in plain memory (round.c), with its texture, texture coordinates
                               and normals attached (the normals are used as given); static_key may be 0 */
};

/* The CPU mirror of an engine buffer: its bytes, size and a version that changes with every
   write (NULL if the buffer is not mirrored). */
const u8 *rt_buffer_data(u32 id, u32 *size, u32 *version);

void rt_set_enabled(int on);
void rt_set_soft(int samples, float radius_degrees, int filter_radius);
/* Mask resolution relative to the viewport, and an upper limit on its pixel count (0 = keep
   the current limit). */
void rt_set_scale(float scale, float max_pixels);
/* Sky visibility (ambient occlusion): rays per pixel (0 = off), their reach in world units,
   and how strongly the result dims ambient light (0..1). bounce > 0 also gathers sunlight
   reflected by nearby surfaces with those rays; its value scales the light added. */
void rt_set_sky(int rays, float distance, float strength, float bounce);
/* Blend this share of last frame's sky/bounce result into the new one (0 = off), which
   averages the noise away over time where the view holds still. */
void rt_set_accumulate(float weight);
/* Traced sky light: the sky rays return the colour of the sky where they get away and the
   lit colour of what they hit otherwise, and the result replaces the game's constant
   ambient light. sky_gain and bounce_gain scale the two parts; tint (0..1) is how much of
   the sky's own colour its light keeps, the rest taking the hue of the game's ambient
   colour. Needs sky rays. */
void rt_set_gi(int on, float sky_gain, float bounce_gain, float tint);
/* Cheaper indirect light: rate 1, 2 or 4 = every pixel block traces its sky rays every frame,
   every 2nd frame, every 4th (the rest keep their accumulated value); far_dist > 0 halves the
   sky rays of pixels farther than that (world units). */
void rt_set_gi_rate(int rate, float far_dist);
void rt_set_clouds(float strength, float size, float speed);   /* moss: cloud shadows (strength 0 = off) */
/* Trace what the sea mirrors, for the water shader; strength scales how reflective it is. */
void rt_set_reflections(int on, float strength);
/* How many filter passes the indirect light gets (each reaches twice as far as the last). */
void rt_set_denoise(int passes);
/* For the sea's draw: the reflection texture of the scene camera being drawn (0 = none).
   info: 1/width, 1/height of the view, strength. */
GLuint rt_water_reflection(float *info);
/* Smooth normals: trace from the curved surface the meshes' vertex normals describe (see
   the README's "Smooth normals"). */
void rt_set_smooth(int on);
/* Replacement textures: compress 0 none, 1 colour maps S3TC, 2 also normal maps; max_size
   halves bigger ones on load (0 = no limit). Applies to textures loaded from then on. */
void rt_set_textures(int compress, int max_size);
/* Trace what glossy surfaces mirror (one ray per pixel whose material is shiny enough). */
void rt_set_specular(int on, float strength);
/* The reflection texture of the scene camera being drawn (always a valid texture once that
   camera has a mask): rgb = mirrored light for the surface at each pixel. */
GLuint rt_last_reflection(void);
/* Writes every material texture layer for labelling: 'WRTL', count, edge, then per layer
   its content hash, 4 floats (roughness, reflectivity, metal, emissive), a flag (1 = set by
   the materials file) and edge*edge RGBA bytes. */
int rt_dump_layers(const char *path);
/* Replacement textures live in the folder wumrt_textures, one file per texture and kind,
   named by the same hash as in wumrt_materials.ini: HASH_n.wtx is a tangent-space normal
   map, HASH_c.wtx a better colour image that is loaded into the game's own texture. A .wtx
   file is 'WTX1', width, height, then RGBA bytes, bottom row first.
   rt_normal_map: the normal map for a game texture, or 0. rt_dump_textures: write every
   material texture in use, full size, as PREFIX + HASH.wtx. */
GLuint rt_normal_map(u32 tex);
int rt_dump_textures(const char *prefix);
/* Shell layers: whether a terrain texture carries grass or moss (wumrt_surfaces.ini, keyed by
   the same hash). Returns 1 and fills out[0] = height and out[1] = blade density, both
   relative to the settings, if it does. */
int rt_shell(u32 tex, float *out);
/* Characters (wumrt_characters.ini, same hash): what a texture is on the worms. 0 = nothing,
   1 = skin, 2 = skin whose texture also holds the eyes, 3 = an eye. */
int rt_char_kind(u32 tex);
/* The centre of the vertices a draw uses, in world space (row-vector model matrix applied).
   Returns 0 if it cannot tell. */
int rt_draw_center(const struct rt_draw *d, const float *model, float *out);
/* The same draw's world-space bounding box. */
int rt_draw_bounds(const struct rt_draw *d, const float *model, float *lo, float *hi);
/* Characters as capsules for the contact shadow: n of them (at most 16), 8 floats each
   (end A xyz, radius, end B xyz, unused), world space; used from the next trace on. */
void rt_set_capsules(const float *caps, int n);
/* Characters in the tracer: on = trace how much body lies between skin turned away from the
   sun and the sun (for light through thin parts, and the sun's visibility there for the
   light wrap); depth = world units of body that let 1/e of the light through; contact =
   how much darker the sky light gets where it is blocked by a character, within reach. */
void rt_set_characters(int on, float depth, float contact, float reach);
/* Foliage (wumrt_characters.ini kind 4): trace light through leaves turned away from the sun. */
void rt_set_foliage(int on, float depth);
/* Scattered stones and tufts (scatter.c): on/off, how many and how big, relative. */
void rt_set_scatter(int on, float density, float size);
/* After a sun-lit terrain draw (its programs still bound): draw its stones and tufts.
   Returns 1 if it drew anything. */
int rt_scatter_draw(const struct rt_draw *d, const float *model);
/* Read wumrt_materials.ini and wumrt_surfaces.ini again (keyed by content hash). */
void rt_reload_materials(void);
/* The sun's colour and the game's ambient colour, as its shaders get them. */
void rt_set_light(const float *sun_rgb, const float *ambient_rgb);
/* The sea: a horizontal plane at this height (world y). Called when the game draws it. */
void rt_set_water(float height);
/* Size (bounding-box diagonal, object space) of the vertex buffer a draw uses; 0 if unknown. */
float rt_mesh_extent(const struct rt_draw *d);
/* The sky is copied into six square views around the camera by drawing the game's own sky
   draws again. rt_sky_wanted: which of the six to refresh this frame (bit mask, 0 = none).
   rt_sky_begin: point rendering at one of them; fills the view and projection matrices to
   draw with (game_view: the view matrix the game uses for its sky). rt_sky_end: back to
   where the game was rendering. */
u32 rt_sky_wanted(void);
int rt_sky_begin(u32 face, const float *game_view, float *view, float *proj);
void rt_sky_end(void);
/* A texture was deleted or given new contents: forget what was derived from it. */
void rt_texture_gone(GLuint id);
void rt_reload_shader(void);
/* A shadow-pass draw: collect it as a shadow caster. `light_view` is the light's view matrix. */
void rt_on_caster(const struct rt_draw *d, const float *model, const float *light_view);
/* A sun-lit main-pass draw: remember its texture (for the colour of light it bounces) and,
   if it is alpha tested, its UV layout. The shadow pass sets up neither, so this is what the
   same mesh uses when it comes by as a caster. */
void rt_on_main_alpha(const struct rt_draw *d);
/* A sun-lit main-pass draw with this camera is about to happen. Returns the mask texture
   to use (0 = none); fills inv_size with 1/width, 1/height of the view and mask_size with
   the mask's width and height, the sky occlusion strength and the bounce strength (4 floats). `landscape` says
   the draw is terrain, which is what marks a camera as a scene camera. */
GLuint rt_prepare_main(const float *view, const float *proj, int landscape, float *inv_size, float *mask_size,
                       GLuint *bounce_texture);
void rt_frame_end(void);
/* A core starts (at the end of a frame, GL context current): `persist` is the loader's
   memory that outlives cores (see wumrt_api.h). The engine's vertex buffers that already
   exist are copied into the CPU mirror. */
void rt_start(void *(*persist)(const char *name, u32 size, int *created));
/* Before the core is unloaded: stop the worker thread, delete the GL objects we made. */
void rt_shutdown(void);
/* Distance from the eye to what the main view's centre pixel shows, from the last mask
   (0 = unknown). */
float rt_center_distance(void);
/* Writes the last mask as 'WRTF', w, h, then w*h*2 floats (visibility, distance). */
int rt_dump_mask(const char *path, u32 cam);

#endif
