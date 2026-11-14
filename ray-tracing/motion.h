/* Camera motion blur (motion.c): a film shutter's smear along the camera's motion on the scene
   texture, right after the lit scene. Called from post.c's run_dof (motion-blur stream). */
#ifndef MOTION_H
#define MOTION_H
#include "rt.h"

void motion_setting(const char *name, float v);  /* name NULL: back to the defaults */
int motion_wanted(void);
/* scene: the game's scene texture (sRGB-encoded; RGBA16F with hdr = 1, else RGBA8). */
void motion_apply(GLuint scene, u32 w, u32 h, int rgba8);
void motion_frame_end(void);
void motion_forget(void);                        /* GL context gone or shaders reloaded */

#endif
