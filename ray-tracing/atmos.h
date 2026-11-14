/* Atmospheric depth (atmos.c): aerial perspective / height haze on the scene texture, and
   optional sun shafts, before post.c's tone curve. Called from post.c's run_post. */
#ifndef ATMOS_H
#define ATMOS_H
#include "rt.h"

void atmos_setting(const char *name, float v);  /* name NULL: back to the defaults */
/* scene: the game's scene texture (sRGB-encoded colour; RGBA16F with hdr = 1, else RGBA8). */
void atmos_apply(GLuint scene, u32 w, u32 h, int rgba8);
void atmos_frame_end(void);
void atmos_forget(void);                        /* GL context gone */

#endif
