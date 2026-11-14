/* Light from effects (fx.c): explosions, fire, glowing debris and other glowing effects
   become point lights with a size, traced with shadows and added to the indirect light. */
#ifndef FX_H
#define FX_H
#include "rt.h"

/* A wumrt.ini setting whose name starts with "fx" (name NULL: back to the defaults, before
   the file is read). */
void fx_setting(const char *name, float v);

/* An unlit, textured main-pass draw (the game draws its particles with FFVertexMainTexCol):
   if it glows (additive blending) its light is collected. model: the draw's model matrix
   (row vectors). col_*: the vertex colour array, if one is enabled. */
struct fx_draw_info {
    const struct rt_draw *d;
    const float *model, *view;
    u32 blend_src, blend_dst;
    int col_on; u32 col_size, col_stride, col_buf; GLenum col_type; const void *col_ptr;
};
void fx_on_draw(const struct fx_draw_info *f);
/* fx_log = 2: log every new kind of unlit main-pass draw (programs, blending, texture), to find
   out how the game draws a new effect. */
void fx_census(const char *vp, const char *fp, const float *view, int blend, u32 src, u32 dst, u32 tex,
               GLenum mode, u32 count, int vbo);

/* End of a frame: what was collected becomes the light list the next frame traces. */
void fx_frame_end(void);

/* After the tracer's own passes for one camera (rt_core.c trace()): shadow rays towards the
   effect lights, their accumulation, and the light added to the indirect light texture. */
struct fx_view {
    int cam; u32 w, h;
    GLuint geom;            /* the trace's output: sun, distance, normal */
    GLuint prev_mask;       /* last frame's filtered mask (y distance, w frames) */
    GLuint light;           /* the indirect light texture the game's shaders read (and the next frame's sky rays) */
    int hist;               /* 1 if prev_mask and the reprojection below belong to the last frame */
    float eye[3], base[3], dx[3], dy[3], prev_vp[16], prev_eye[3];
    u32 tlas_count, frame;
    GLuint main_prog;       /* the tracer's program: when it changes, the effect programs are compiled again */
    const char *defines;    /* the tracer's extra defines (bigger stacks) */
};
void fx_trace(const struct fx_view *v);

void fx_texture_gone(GLuint id);
int fx_smoke_texture(GLuint tex);
int fx_smoke_light(float *pos_r, float *rgb);  /* brightest effect light: world pos + radius, power */    /* a soft white/grey sprite texture (lit smoke, wumrt.c) */
void fx_forget(void);           /* new GL context: our GL objects are gone */
void fx_shutdown(void);         /* core unload: delete our GL objects (programs: see fx_program) */
GLuint fx_program(int i);       /* our compute programs (0, 1), for rt_shutdown to keep or delete */

/* rt_core.c */
GLuint rt_compile_compute(const char *path, const char *defines);
int rt_is_main_view(const float *view);
float rt_center_distance(void);

#endif
