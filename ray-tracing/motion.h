/* Camera motion blur (motion.c): a film shutter's smear along the camera's motion on the scene
   texture, right after the lit scene. Called from post.c's run_dof (motion-blur stream). */
#ifndef MOTION_H
#define MOTION_H
#include "rt.h"

void motion_setting(const char *name, float v);  /* name NULL: back to the defaults */
int motion_wanted(void);
int motion_objects_wanted(void);
/* A sun-lit draw (wumrt.c before_draw). 1: the caller makes the same draw call now (our velocity
   program and buffer are bound), then calls motion_object_end(). */
int motion_object_begin(const struct rt_draw *d, const float *model, const float *view, const float *proj);
void motion_object_end(void);
/* scene: the game's scene texture (sRGB-encoded; RGBA16F with hdr = 1, else RGBA8). */
void motion_apply(GLuint scene, u32 w, u32 h, int rgba8);
void motion_frame_end(void);
void motion_forget(void);                        /* GL context gone or shaders reloaded */

#endif
