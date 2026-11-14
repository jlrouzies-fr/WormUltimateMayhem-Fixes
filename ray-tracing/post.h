/* Film post-processing (post.c): tone curve, grade, bloom, depth of field, anti-aliasing on
   the scene before the HUD. Called from wumrt.c. */
#ifndef POST_H
#define POST_H

/* What kind of draw is coming (post_before_draw's cls). */
#define POST_CASTER    1u       /* shadow-map pass */
#define POST_LANDSCAPE 2u       /* terrain of a scene camera */
#define POST_LIT       4u       /* other sun-lit object */
#define POST_UNLIT     8u       /* FixedFunction.cg without lighting: sky, particles, labels, HUD */
#define POST_EFFECT    16u      /* PostProcess.cg (worm silhouettes) */
#define POST_WATER     32u
#define POST_HAS_VIEW  64u      /* the vertex program has a view matrix (passed as view) */

void post_settings_begin(void);                 /* before wumrt.ini is read */
int post_setting(const char *name, float v);    /* 1 if the setting is ours */
void post_settings_end(void);
void post_bind_fbo(GLenum target, GLuint id);   /* before the engine binds a framebuffer */
void post_before_draw(u32 cls, const float *view);
void post_frame_end(void);                      /* before SwapBuffers */
void post_shutdown(void);
void post_reload(void);                         /* rt.py reload: compile wumrt_post.comp again */
/* Depth of field focuses on the worm nearest the middle of the main view: wumrt.c reports the
   centre of every worm draw (world space) with the camera it is drawn with. */
int post_wants_worms(void);
void post_note_worm(const float *centre, const float *view, const float *proj);

/* rt_core.c: the main view's traced mask this frame (eye distance in .y), its size and the
   size of the view it was traced for; 0 if there is none. */
GLuint rt_post_mask(u32 *mask_w, u32 *mask_h, u32 *view_w, u32 *view_h);
/* The same camera's view and projection (row vectors), the direction towards the sun and its colour. */
int rt_post_camera(float *view, float *proj, float *sun, float *sun_col);

#endif
