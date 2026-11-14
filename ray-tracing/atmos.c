/*
 * Atmospheric depth (part of wumrt_core.dll): distant surfaces fade towards the sky's own
 * colour, more so low down (aerial perspective + height haze), so the backgrounds separate in
 * layers; optional screen-space sun shafts. Runs on the game's scene texture from post.c's
 * run_post (before bloom, auto exposure and the tone curve, before the HUD), using the tracer's
 * mask of the main view (eye distance in .y; nothing hit = sky).
 *
 * Settings (wumrt.ini; atmos = 0 in code):
 *   atmos            0/1
 *   atmos_strength   most haze a far pixel gets (0..1), default 0.5
 *   atmos_start      distance (world units; a worm is ~25 tall) where it starts, default 150
 *   atmos_range      distance scale of the build-up, default 500
 *   atmos_height     height scale: the haze thins over this much above atmos_base, default 250
 *   atmos_base       height where the haze is thickest, default 0
 *   atmos_low        how much the height matters (0 = only distance, 1 = all), default 0.6
 *   atmos_shafts     sun shaft strength, default 0 (off)
 *   atmos_shaft_len  share of the way to the sun the shaft taps cover, default 0.5
 *   atmos_log        1 = log the haze colour and the GPU time every 300 frames
 * Needs post = 1 (it runs inside post's pass) and the tracer (the mask).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "post.h"
#include "atmos.h"

#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
#define GL_READ_ONLY 0x88B8
#define GL_READ_WRITE 0x88BA
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x20
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x8
#define GL_CLAMP_TO_EDGE 0x812F
#define AT_TIMESTAMP 0x8E28
#define AT_QUERY_RESULT 0x8866
#define AT_QUERY_RESULT_AVAILABLE 0x8867
#define AT_UNIT 24              /* texture unit for the mask sampler (post uses 12..) */

GLuint rt_compile_compute(const char *path, const char *defines);

static struct {
    int on, log;
    float strength, start, range, height, base, low, shafts, shaft_len;
} S;

static void defaults(void)
{
    S.on = 0; S.log = 0;
    S.strength = 0.5f; S.start = 150.0f; S.range = 500.0f; S.height = 250.0f; S.base = 0.0f; S.low = 0.6f;
    S.shafts = 0.0f; S.shaft_len = 0.5f;
}

void atmos_setting(const char *name, float v)
{
    static int started;
    if (!started) { defaults(); started = 1; }
    if (!name) { defaults(); return; }
    if (!strcmp(name, "atmos")) S.on = v != 0;
    else if (!strcmp(name, "atmos_log")) S.log = (int)v;
    else if (!strcmp(name, "atmos_strength")) S.strength = v < 0 ? 0 : v > 1 ? 1 : v;
    else if (!strcmp(name, "atmos_start")) S.start = v;
    else if (!strcmp(name, "atmos_range")) S.range = v;
    else if (!strcmp(name, "atmos_height")) S.height = v;
    else if (!strcmp(name, "atmos_base")) S.base = v;
    else if (!strcmp(name, "atmos_low")) S.low = v;
    else if (!strcmp(name, "atmos_shafts")) S.shafts = v;
    else if (!strcmp(name, "atmos_shaft_len")) S.shaft_len = v;
}

static struct {
    void (APIENTRY *UseProgram)(GLuint);
    GLint (APIENTRY *GetUniformLocation)(GLuint, const char *);
    void (APIENTRY *Uniform1i)(GLint, GLint);
    void (APIENTRY *Uniform3f)(GLint, float, float, float);
    void (APIENTRY *Uniform4f)(GLint, float, float, float, float);
    void (APIENTRY *DispatchCompute)(GLuint, GLuint, GLuint);
    void (APIENTRY *MemoryBarrier)(GLbitfield);
    void (APIENTRY *BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    void (APIENTRY *TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (APIENTRY *ActiveTexture)(GLenum);
    void (APIENTRY *DeleteProgram)(GLuint);
    void (APIENTRY *GenQueries)(GLsizei, GLuint *);
    void (APIENTRY *QueryCounter)(GLuint, GLenum);
    void (APIENTRY *GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
    void (APIENTRY *GetQueryObjectui64v)(GLuint, GLenum, unsigned long long *);
} G;
static int g_state;             /* 0 not tried, 1 ready, -1 unavailable */
static struct {
    HGLRC ctx;
    GLuint prog[2][2];          /* [pass 1 sky, pass 2 apply][rgba16f, rgba8] */
    GLuint fog;                 /* 1x1 RGBA32F haze colour */
    int failed;
    GLuint q[4][2]; int q_pending[4], q_next, q_made;
    double gpu_ms; u32 gpu_n, runs, frames;
} X;

static int init(void)
{
    static const char *names[] = { "glUseProgram", "glGetUniformLocation", "glUniform1i", "glUniform3f", "glUniform4f",
        "glDispatchCompute", "glMemoryBarrier", "glBindImageTexture", "glTexStorage2D", "glActiveTexture", "glDeleteProgram",
        "glGenQueries", "glQueryCounter", "glGetQueryObjectuiv", "glGetQueryObjectui64v" };
    PROC *slot = (PROC *)&G;
    int i;
    HGLRC ctx = wglGetCurrentContext();
    if (g_state < 0) return 0;
    if (!g_state) {
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
            if (!(slot[i] = rt_real_proc(names[i]))) { g_state = -1; rt_log("atmos: %s missing; off", names[i]); return 0; }
        g_state = 1;
    }
    if (X.ctx != ctx) { memset(&X, 0, sizeof X); X.ctx = ctx; }
    if (X.failed) return 0;
    if (!X.prog[0][0]) {
        static const char *defs[2][2] = { { "#define PASS 1\n", "#define PASS 1\n#define OUT_RGBA8 1\n" },
                                          { "#define PASS 2\n", "#define PASS 2\n#define OUT_RGBA8 1\n" } };
        int a, b;
        for (a = 0; a < 2; a++)
            for (b = 0; b < 2; b++)
                if (!(X.prog[a][b] = rt_compile_compute("wumrt_atmos.comp", defs[a][b]))) {
                    rt_log("atmos: wumrt_atmos.comp failed to compile; off until reload");
                    X.failed = 1;
                    return 0;
                }
        rt_log("atmos: programs ready");
    }
    if (!X.fog) {
        GLint prev = 0;
        static const float zero[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glGenTextures(1, &X.fog);
        glBindTexture(GL_TEXTURE_2D, X.fog);
        G.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 1, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, zero);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    }
    if (!X.q_made) { G.GenQueries(8, &X.q[0][0]); X.q_made = 1; }
    return 1;
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

void atmos_apply(GLuint scene, u32 w, u32 h, int rgba8)
{
    float view[16], proj[16], sun[3], scol[3], eye[3], base[3], dx[3], dy[3], bv[3], sun_uv[2] = { 0, 0 };
    GLuint mask;
    u32 mw = 0, mh = 0, vw = 0, vh = 0;
    int k, c, sun_on = 0, a;
    GLint prev_unit = 0, prev_tex = 0;
    if (!S.on || !scene || !w || !h || (S.strength <= 0 && S.shafts <= 0)) return;
    if (!init()) return;
    mask = rt_post_mask(&mw, &mh, &vw, &vh);
    if (!mask || !mw || !mh || !rt_post_camera(view, proj, sun, scol)) return;
    if (fabsf(proj[0]) < 1e-6f || fabsf(proj[5]) < 1e-6f) return;

    /* camera: rows of view are world axes in eye space (row vectors, translation in 12..14) */
    for (k = 0; k < 3; k++) {
        eye[k] = 0;
        for (c = 0; c < 3; c++) eye[k] -= view[12 + c] * view[k * 4 + c];
    }
    /* eye-space ray at uv: ((2u - 1 + P8) / P0, (2v - 1 + P9) / P5, -1) */
    bv[0] = (-1.0f + proj[8]) / proj[0]; bv[1] = (-1.0f + proj[9]) / proj[5]; bv[2] = -1.0f;
    for (k = 0; k < 3; k++) {
        base[k] = bv[0] * view[k * 4 + 0] + bv[1] * view[k * 4 + 1] + bv[2] * view[k * 4 + 2];
        dx[k] = 2.0f / proj[0] * view[k * 4 + 0];
        dy[k] = 2.0f / proj[5] * view[k * 4 + 1];
    }
    if (S.shafts > 0) {
        float e[3], clip[4], m;
        for (c = 0; c < 3; c++) e[c] = sun[0] * view[c] + sun[1] * view[4 + c] + sun[2] * view[8 + c];
        for (c = 0; c < 4; c++) { clip[c] = 0; for (k = 0; k < 3; k++) clip[c] += e[k] * proj[k * 4 + c]; }
        if (clip[3] > 1e-4f) {
            sun_uv[0] = clip[0] / clip[3] * 0.5f + 0.5f; sun_uv[1] = clip[1] / clip[3] * 0.5f + 0.5f;
            sun_on = sun_uv[0] > -0.5f && sun_uv[0] < 1.5f && sun_uv[1] > -0.5f && sun_uv[1] < 1.5f;
        }
        m = scol[0] > scol[1] ? scol[0] : scol[1]; if (scol[2] > m) m = scol[2];
        if (m <= 0) { scol[0] = 1; scol[1] = 0.9f; scol[2] = 0.75f; m = 1; }
        for (c = 0; c < 3; c++) scol[c] /= m;
    }

    /* timer: read the oldest pair, start a new one */
    if (X.q_pending[X.q_next]) {
        GLuint avail = 0;
        G.GetQueryObjectuiv(X.q[X.q_next][1], AT_QUERY_RESULT_AVAILABLE, &avail);
        if (avail) {
            unsigned long long t0 = 0, t1 = 0;
            G.GetQueryObjectui64v(X.q[X.q_next][0], AT_QUERY_RESULT, &t0);
            G.GetQueryObjectui64v(X.q[X.q_next][1], AT_QUERY_RESULT, &t1);
            if (t1 > t0) { X.gpu_ms += (double)(t1 - t0) / 1e6; X.gpu_n++; }
        }
        X.q_pending[X.q_next] = 0;
    }
    G.QueryCounter(X.q[X.q_next][0], AT_TIMESTAMP);

    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
    G.ActiveTexture(GL_TEXTURE0 + AT_UNIT);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glBindTexture(GL_TEXTURE_2D, mask);
    rgba8 = rgba8 ? 1 : 0;
    for (a = 0; a < 2; a++) {
        GLuint p = X.prog[a][rgba8];
        G.UseProgram(p);
        { GLint l = G.GetUniformLocation(p, "uMaskTex"); if (l >= 0) G.Uniform1i(l, AT_UNIT); }
        set4(p, "uSize", (float)w, (float)h, 1.0f / w, 1.0f / h);
        set4(p, "uMask", (float)mw, (float)mh, 0, 0);
        set3(p, "uEye", eye); set3(p, "uBase", base); set3(p, "uDx", dx); set3(p, "uDy", dy);
        set4(p, "uAtmos", S.strength, S.start, S.range, S.height);
        set4(p, "uAtmos2", S.base, S.low, S.shafts, 0.05f);
        set4(p, "uSun", sun_uv[0], sun_uv[1], sun_on ? 1.0f : 0.0f, S.shaft_len);
        set4(p, "uSunCol", scol[0], scol[1], scol[2], 0);
        set4(p, "uFallback", 0.55f, 0.65f, 0.8f, 0);
        G.BindImageTexture(0, scene, 0, GL_FALSE, 0, a == 0 ? GL_READ_ONLY : GL_READ_WRITE, rgba8 ? GL_RGBA8 : GL_RGBA16F);
        G.BindImageTexture(2, X.fog, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
        if (a == 0) G.DispatchCompute(1, 1, 1);
        else G.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
        G.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    G.BindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    G.ActiveTexture((GLenum)prev_unit);
    G.QueryCounter(X.q[X.q_next][1], AT_TIMESTAMP);
    X.q_pending[X.q_next] = 1;
    X.q_next = (X.q_next + 1) & 3;
    X.runs++;
}

void atmos_frame_end(void)
{
    if (!S.on || !S.log || g_state <= 0 || X.ctx != wglGetCurrentContext()) return;
    if (++X.frames < 300) return;
    {
        float fog[4] = { 0, 0, 0, 0 };
        GLint prev = 0;
        if (X.fog) {
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
            glBindTexture(GL_TEXTURE_2D, X.fog);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, fog);
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        }
        rt_log("atmos: %u runs in %u frames; haze colour (linear) %.3f %.3f %.3f%s; GPU %.3f ms per run (%u timed)",
               X.runs, X.frames, fog[0], fog[1], fog[2], fog[3] > 0.5f ? "" : " (fallback, no sky seen)",
               X.gpu_n ? X.gpu_ms / X.gpu_n : 0.0, X.gpu_n);
    }
    X.frames = X.runs = X.gpu_n = 0; X.gpu_ms = 0;
}

void atmos_forget(void)
{
    memset(&X, 0, sizeof X);
}
