/*
 * Camera motion blur (part of wumrt_core.dll): when the camera pans, flies or shakes, the scene
 * smears softly along the motion like a film camera's shutter (motion_blur = 0.5 is a 180 degree
 * shutter: half of the frame-to-frame motion); a camera that holds still changes nothing.
 *
 * Runs on the game's scene texture from post.c's run_dof, right after the lit scene (before the
 * depth of field, bloom and the tone curve; particles, name labels and the HUD come later and
 * stay sharp). Motion vectors are the camera's only: each pixel's surface point (tracer mask,
 * eye distance in .y) projected with this and last frame's camera. A surface that was not there
 * last frame (something farther was, in last frame's mask) moved by itself, a worm or shell the
 * camera follows: it is not smeared, and does not bleed into its neighbours.
 *
 * Settings (wumrt.ini; motion_blur = 0 in code):
 *   motion_blur          shutter fraction, 0 = off, 0.5 = 180 degree shutter
 *   motion_blur_max      longest smear, share of the view height, default 0.04
 *   motion_blur_taps     taps along the smear, 2..16, default 12
 *   motion_blur_cut      camera turns more than this (degrees) or jumps farther than
 *                        motion_blur_jump world units in a frame: a cut, no blur; defaults 20, 400
 *   motion_blur_log      1 = log runs and the GPU time every 300 frames
 *   motion_blur_split    1 = only the right half is smeared (A/B in one moving frame)
 * Needs post = 1 (it runs inside post's pass) and the tracer (the mask).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "post.h"
#include "motion.h"

#define GL_RGBA16F 0x881A
#define GL_READ_ONLY 0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x20
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x8
#define GL_FRAMEBUFFER_BARRIER_BIT 0x400
#define GL_TEXTURE_UPDATE_BARRIER_BIT 0x100
#define GL_CLAMP_TO_EDGE 0x812F
#define MB_TIMESTAMP 0x8E28
#define MB_QUERY_RESULT 0x8866
#define MB_QUERY_RESULT_AVAILABLE 0x8867
#define MB_UNIT 26              /* texture units 26, 27 (post uses 12.., atmos 24) */

GLuint rt_compile_compute(const char *path, const char *defines);

static struct {
    float shutter, max, cut, jump;
    int taps, log, split;
} S;

static void defaults(void)
{
    S.shutter = 0.0f; S.max = 0.04f; S.taps = 12; S.cut = 20.0f; S.jump = 400.0f; S.log = 0; S.split = 0;
}

void motion_setting(const char *name, float v)
{
    static int started;
    if (!started) { defaults(); started = 1; }
    if (!name) { defaults(); return; }
    if (!strcmp(name, "motion_blur")) S.shutter = v < 0 ? 0 : v > 1 ? 1 : v;
    else if (!strcmp(name, "motion_blur_max")) S.max = v < 0 ? 0 : v > 0.15f ? 0.15f : v;
    else if (!strcmp(name, "motion_blur_taps")) S.taps = v < 2 ? 2 : v > 16 ? 16 : (int)v;
    else if (!strcmp(name, "motion_blur_cut")) S.cut = v;
    else if (!strcmp(name, "motion_blur_jump")) S.jump = v;
    else if (!strcmp(name, "motion_blur_log")) S.log = v != 0;
    else if (!strcmp(name, "motion_blur_split")) S.split = v != 0;
}

int motion_wanted(void) { return S.shutter > 0 && S.max > 0; }

static struct {
    void (APIENTRY *UseProgram)(GLuint);
    GLint (APIENTRY *GetUniformLocation)(GLuint, const char *);
    void (APIENTRY *Uniform1i)(GLint, GLint);
    void (APIENTRY *Uniform3f)(GLint, float, float, float);
    void (APIENTRY *Uniform4f)(GLint, float, float, float, float);
    void (APIENTRY *UniformMatrix4fv)(GLint, GLsizei, GLboolean, const float *);
    void (APIENTRY *DispatchCompute)(GLuint, GLuint, GLuint);
    void (APIENTRY *MemoryBarrier)(GLbitfield);
    void (APIENTRY *BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    void (APIENTRY *TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (APIENTRY *ActiveTexture)(GLenum);
    void (APIENTRY *GenQueries)(GLsizei, GLuint *);
    void (APIENTRY *QueryCounter)(GLuint, GLenum);
    void (APIENTRY *GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
    void (APIENTRY *GetQueryObjectui64v)(GLuint, GLenum, unsigned long long *);
} G;
static int g_state;             /* 0 not tried, 1 ready, -1 unavailable */

static struct {
    HGLRC ctx;
    int failed;
    GLuint prog[3][2];          /* [pass - 1][8-bit output] */
    GLuint vel, tmp; u32 w, h;
    /* last frame's camera and mask */
    int have_prev; u32 prev_frame;
    float prev_vp[16], prev_eye[3], prev_fwd[3];
    GLuint prev_mask; u32 prev_mw, prev_mh;
    u32 frame, frames, runs, still, cuts, moved_frames;
    GLuint q[4][2]; int q_pending[4], q_next, q_made;
    double gpu_ms; u32 gpu_n;
} X;

static int init(void)
{
    static const char *names[] = { "glUseProgram", "glGetUniformLocation", "glUniform1i", "glUniform3f", "glUniform4f",
        "glUniformMatrix4fv", "glDispatchCompute", "glMemoryBarrier", "glBindImageTexture", "glTexStorage2D",
        "glActiveTexture", "glGenQueries", "glQueryCounter", "glGetQueryObjectuiv", "glGetQueryObjectui64v" };
    PROC *slot = (PROC *)&G;
    int i, a, b;
    HGLRC ctx = wglGetCurrentContext();
    if (g_state < 0) return 0;
    if (!g_state) {
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
            if (!(slot[i] = rt_real_proc(names[i]))) { g_state = -1; rt_log("motion: %s missing; off", names[i]); return 0; }
        g_state = 1;
    }
    if (X.ctx != ctx) { memset(&X, 0, sizeof X); X.ctx = ctx; }
    if (X.failed) return 0;
    if (!X.prog[0][0]) {
        static const char *defs[3][2] = { { "#define PASS 1\n", NULL }, { "#define PASS 2\n", NULL },
                                          { "#define PASS 3\n", "#define PASS 3\n#define OUT_RGBA8 1\n" } };
        for (a = 0; a < 3; a++)
            for (b = 0; b < 2; b++) {
                if (!defs[a][b]) continue;
                if (!(X.prog[a][b] = rt_compile_compute("wumrt_motion.comp", defs[a][b]))) {
                    rt_log("motion: wumrt_motion.comp failed to compile; off until reload");
                    X.failed = 1;
                    return 0;
                }
            }
        rt_log("motion: programs ready");
    }
    if (!X.q_made) { G.GenQueries(8, &X.q[0][0]); X.q_made = 1; }
    return 1;
}

static GLuint make_tex(u32 w, u32 h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    G.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

static void set4(GLuint p, const char *n, float a, float b, float c, float d)
{
    GLint l = G.GetUniformLocation(p, n);
    if (l >= 0) G.Uniform4f(l, a, b, c, d);
}
static void set3(GLuint p, const char *n, const float *v)
{
    GLint l = G.GetUniformLocation(p, n);
    if (l >= 0) G.Uniform3f(l, v[0], v[1], v[2]);
}
static void setm(GLuint p, const char *n, const float *m)
{
    GLint l = G.GetUniformLocation(p, n);
    if (l >= 0) G.UniformMatrix4fv(l, 1, GL_FALSE, m);
}
static void bind_sampler(GLuint p, const char *n, int unit, GLuint tex)
{
    GLint l = G.GetUniformLocation(p, n);
    G.ActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (l >= 0) G.Uniform1i(l, unit);
}

void motion_apply(GLuint scene, u32 w, u32 h, int rgba8)
{
    float view[16], proj[16], sun[3], scol[3], vp[16], eye[3], fwd[3], base[3], dx[3], dy[3], bv[3];
    GLuint mask;
    u32 mw = 0, mh = 0, vw = 0, vh = 0, pw, ph;
    int i, j, k, c, have_prev, still = 1, cut = 0;
    GLint prev_unit = 0, prev_tex[2] = { 0, 0 }, prev_prog = 0;
    if (!motion_wanted() || !scene || !w || !h) return;
    if (!init()) return;
    mask = rt_post_mask(&mw, &mh, &vw, &vh);
    if (!mask || !mw || !mh || !vw || !vh || !rt_post_camera(view, proj, sun, scol)) return;
    if (fabsf(proj[0]) < 1e-6f || fabsf(proj[5]) < 1e-6f) return;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double s = 0;
            for (k = 0; k < 4; k++) s += (double)view[i * 4 + k] * proj[k * 4 + j];
            vp[i * 4 + j] = (float)s;
        }
    /* camera: rows of view are world axes in eye space (row vectors, translation in 12..14) */
    for (k = 0; k < 3; k++) {
        eye[k] = 0;
        for (c = 0; c < 3; c++) eye[k] -= view[12 + c] * view[k * 4 + c];
        fwd[k] = -view[k * 4 + 2];
    }
    bv[0] = (-1.0f + proj[8]) / proj[0]; bv[1] = (-1.0f + proj[9]) / proj[5]; bv[2] = -1.0f;
    for (k = 0; k < 3; k++) {
        base[k] = bv[0] * view[k * 4 + 0] + bv[1] * view[k * 4 + 1] + bv[2] * view[k * 4 + 2];
        dx[k] = 2.0f / proj[0] * view[k * 4 + 0];
        dy[k] = 2.0f / proj[5] * view[k * 4 + 1];
    }

    have_prev = X.have_prev && X.prev_frame + 1 == X.frame;
    if (have_prev) {
        float dot = 0, dist = 0;
        for (i = 0; i < 16; i++) if (fabsf(vp[i] - X.prev_vp[i]) > 1e-6f * (1.0f + fabsf(vp[i]))) { still = 0; break; }
        for (k = 0; k < 3; k++) { dot += fwd[k] * X.prev_fwd[k]; dist += (eye[k] - X.prev_eye[k]) * (eye[k] - X.prev_eye[k]); }
        if (dot < cosf(S.cut * 3.14159265f / 180.0f) || sqrtf(dist) > S.jump) cut = 1;
    }
    if (!have_prev) ;
    else if (still) X.still++;
    else if (cut) X.cuts++;
    if (have_prev && !still && !cut) {
        int prev_ok = X.prev_mask && X.prev_mask != mask && X.prev_mw == mw && X.prev_mh == mh && glIsTexture(X.prev_mask);
        pw = vw < w ? vw : w; ph = vh < h ? vh : h;

        glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
        for (i = 0; i < 2; i++) {
            G.ActiveTexture(GL_TEXTURE0 + MB_UNIT + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex[i]);
        }
        if (X.w != w || X.h != h || !X.vel) {
            if (X.vel) glDeleteTextures(1, &X.vel);
            if (X.tmp) glDeleteTextures(1, &X.tmp);
            X.vel = make_tex(w, h); X.tmp = make_tex(w, h); X.w = w; X.h = h;
            rt_log("motion: textures for %ux%u", w, h);
        }
        /* timer: read the oldest pair, start a new one */
        if (X.q_pending[X.q_next]) {
            GLuint avail = 0;
            G.GetQueryObjectuiv(X.q[X.q_next][1], MB_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) {
                unsigned long long t0 = 0, t1 = 0;
                G.GetQueryObjectui64v(X.q[X.q_next][0], MB_QUERY_RESULT, &t0);
                G.GetQueryObjectui64v(X.q[X.q_next][1], MB_QUERY_RESULT, &t1);
                if (t1 > t0) { X.gpu_ms += (double)(t1 - t0) / 1e6; X.gpu_n++; }
            }
            X.q_pending[X.q_next] = 0;
        }
        G.QueryCounter(X.q[X.q_next][0], MB_TIMESTAMP);
        for (i = 0; i < 3; i++) {
            GLuint p = X.prog[i][i == 2 && rgba8 ? 1 : 0];
            G.UseProgram(p);
            set4(p, "uView", (float)pw, (float)ph, (float)mw, (float)mh);
            set4(p, "uBlur", S.shutter, S.max * (float)ph, (float)S.taps, prev_ok ? 1.0f : 0.0f);
            set4(p, "uFrame", (float)(X.frame & 1023), S.split ? 1.0f : 0.0f, 0, 0);
            if (i == 0) {
                setm(p, "uVP", vp); setm(p, "uPrevVP", X.prev_vp);
                set3(p, "uEye", eye); set3(p, "uPrevEye", X.prev_eye);
                set3(p, "uBase", base); set3(p, "uDx", dx); set3(p, "uDy", dy);
                bind_sampler(p, "uMaskTex", MB_UNIT, mask);
                bind_sampler(p, "uPrevMaskTex", MB_UNIT + 1, prev_ok ? X.prev_mask : mask);
                G.BindImageTexture(0, X.vel, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            } else if (i == 1) {
                bind_sampler(p, "uScene", MB_UNIT, scene);
                G.BindImageTexture(1, X.vel, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(0, X.tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            } else {
                G.BindImageTexture(1, X.tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(2, X.vel, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(0, scene, 0, GL_FALSE, 0, GL_WRITE_ONLY, rgba8 ? GL_RGBA8 : GL_RGBA16F);
            }
            G.DispatchCompute((pw + 7) / 8, (ph + 7) / 8, 1);
            G.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        }
        G.MemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
        G.BindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        G.BindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        G.BindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        for (i = 0; i < 2; i++) {
            G.ActiveTexture(GL_TEXTURE0 + MB_UNIT + i);
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex[i]);
        }
        G.ActiveTexture((GLenum)prev_unit);
        G.UseProgram((GLuint)prev_prog);
        G.QueryCounter(X.q[X.q_next][1], MB_TIMESTAMP);
        X.q_pending[X.q_next] = 1;
        X.q_next = (X.q_next + 1) & 3;
        X.runs++;
    }
    /* this frame's camera and mask are next frame's "last" */
    memcpy(X.prev_vp, vp, sizeof vp);
    memcpy(X.prev_eye, eye, sizeof eye);
    memcpy(X.prev_fwd, fwd, sizeof fwd);
    X.prev_mask = mask; X.prev_mw = mw; X.prev_mh = mh;
    X.have_prev = 1; X.prev_frame = X.frame;
}

void motion_frame_end(void)
{
    X.frame++;
    if (!motion_wanted() || !S.log || g_state <= 0 || X.ctx != wglGetCurrentContext()) return;
    if (++X.frames < 300) return;
    rt_log("motion: %u blurred, %u still, %u cuts in %u frames; GPU %.3f ms per run (%u timed)",
           X.runs, X.still, X.cuts, X.frames, X.gpu_n ? X.gpu_ms / X.gpu_n : 0.0, X.gpu_n);
    X.frames = X.runs = X.still = X.cuts = X.gpu_n = 0; X.gpu_ms = 0;
}

void motion_forget(void)
{
    /* programs are dropped (re-read from wumrt_motion.comp on next use); textures kept */
    int a, b;
    if (g_state > 0 && X.ctx == wglGetCurrentContext()) {
        void (APIENTRY *del)(GLuint) = (void *)rt_real_proc("glDeleteProgram");
        for (a = 0; a < 3; a++)
            for (b = 0; b < 2; b++)
                if (X.prog[a][b] && del) del(X.prog[a][b]);
    }
    memset(X.prog, 0, sizeof X.prog);
    X.failed = 0;
}
