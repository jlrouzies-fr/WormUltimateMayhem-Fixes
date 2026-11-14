/*
 * Motion blur (part of wumrt_core.dll): when the camera pans, flies or shakes, the scene smears
 * softly along the motion like a film camera's shutter (motion_blur = 0.5 is a 180 degree
 * shutter: half of the frame-to-frame motion); a camera that holds still changes nothing.
 * With motion_blur_objects = 1, moving rigid objects (shells, flying barrels, sheep, debris)
 * smear along their own motion too, and streak over the background behind them.
 *
 * Runs on the game's scene texture from post.c's run_dof, right after the lit scene (before the
 * depth of field, bloom and the tone curve; particles, name labels and the HUD come later and
 * stay sharp).
 *
 * Camera: each pixel's surface point (tracer mask, eye distance in .y) projected with this and
 * last frame's camera. A surface that was not there last frame (something farther was, in last
 * frame's mask) moved by itself: without an object velocity for it, it is not smeared.
 *
 * Objects: every sun-lit draw of the main view passes motion_object_begin (wumrt.c). A draw is
 * known by its buffers, count and texture; last frame's draw with the same key and the nearest
 * position gives last frame's model matrix. When it changed, the mesh is drawn once more, right
 * then, with our own small program into a velocity buffer (screen motion in pixels from this and
 * last frame's model x view x projection, and the eye distance). The blur pass uses that
 * velocity where the tracer sees the same surface. CPU-skinned worms keep their vertices'
 * motion in the vertices, so they are not covered (their model matrix does not move them).
 *
 * Settings (wumrt.ini; motion_blur = 0 in code):
 *   motion_blur          shutter fraction, 0 = off, 0.5 = 180 degree shutter
 *   motion_blur_objects  1 = moving rigid objects too (default 0)
 *   motion_blur_max      longest smear, share of the view height, default 0.04
 *   motion_blur_taps     taps along the smear, 2..16, default 12
 *   motion_blur_cut      camera turns more than this (degrees) or jumps farther than
 *                        motion_blur_jump world units in a frame: a cut, no blur; defaults 20, 400
 *   motion_blur_obj_jump an object moving farther than this in a frame is new (no blur), default 300
 *   motion_blur_log      1 = log runs and the GPU time every 300 frames
 *   motion_blur_split    1 = only the right half is smeared (A/B in one moving frame)
 *   motion_blur_debug    1 = show the blur vectors (red/green), object pixels (blue), self-moved (grey)
 * Needs post = 1 (it runs inside post's pass) and the tracer (the mask).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "post.h"
#include "motion.h"

#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
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
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define MB_TIMESTAMP 0x8E28
#define MB_QUERY_RESULT 0x8866
#define MB_QUERY_RESULT_AVAILABLE 0x8867
#define MB_UNIT 26              /* texture units 26..28 (post uses 12.., atmos 24) */
#define TILE 16                 /* = TILE in wumrt_motion.comp */

GLuint rt_compile_compute(const char *path, const char *defines);

static struct {
    float shutter, max, cut, jump, obj_jump;
    int taps, log, split, objects, debug;
} S;

static void defaults(void)
{
    S.shutter = 0.0f; S.max = 0.04f; S.taps = 12; S.cut = 20.0f; S.jump = 400.0f; S.obj_jump = 300.0f;
    S.log = 0; S.split = 0; S.objects = 0; S.debug = 0;
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
    else if (!strcmp(name, "motion_blur_obj_jump")) S.obj_jump = v;
    else if (!strcmp(name, "motion_blur_objects")) S.objects = v != 0;
    else if (!strcmp(name, "motion_blur_log")) S.log = (int)v;
    else if (!strcmp(name, "motion_blur_split")) S.split = v != 0;
    else if (!strcmp(name, "motion_blur_debug")) S.debug = v != 0;
}

int motion_wanted(void) { return S.shutter > 0 && S.max > 0; }
int motion_objects_wanted(void) { return motion_wanted() && S.objects; }

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
    /* for the objects' velocity re-draw */
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
    void (APIENTRY *DeleteProgram)(GLuint);
    void (APIENTRY *GenFramebuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindFramebuffer)(GLenum, GLuint);
    void (APIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (APIENTRY *GenRenderbuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindRenderbuffer)(GLenum, GLuint);
    void (APIENTRY *RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (APIENTRY *FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum (APIENTRY *CheckFramebufferStatus)(GLenum);
    void (APIENTRY *DeleteFramebuffers)(GLsizei, const GLuint *);
    void (APIENTRY *DeleteRenderbuffers)(GLsizei, const GLuint *);
} G;
static int g_state;             /* 0 not tried, 1 ready, -1 unavailable */

static struct {
    HGLRC ctx;
    int failed;
    GLuint prog[4][2];          /* [pass - 1][8-bit output] */
    GLuint vel, tmp, tiles; u32 w, h, tw, th;
    /* last frame's camera and mask */
    int have_prev; u32 prev_frame;
    float prev_vp[16], prev_eye[3], prev_fwd[3];
    GLuint prev_mask; u32 prev_mw, prev_mh;
    u32 frame, frames, runs, still, cuts, obj_runs;
    GLuint q[4][2]; int q_pending[4], q_next, q_made;
    double gpu_ms; u32 gpu_n;
    /* objects: velocity buffer and its program */
    GLuint oprog, ofbo, otex, odepth; u32 ow, oh; int ofailed;
    GLint u_mvp, u_prev, u_mv, u_vp;
} X;

/* ---- objects ------------------------------------------------------------------------- */
#define MAXOBJ 512
struct obj { u32 key; float model[16]; };
static struct {
    struct obj cur[MAXOBJ], prev[MAXOBJ];
    unsigned char taken[MAXOBJ];
    int ncur, nprev;
    u32 drawn_frame; int drawn;             /* the velocity buffer holds this frame's moving objects */
    u32 draws, seen;                        /* for the log */
    /* GL state saved around one velocity draw */
    GLint fbo, prog, vp[4], depth_func; GLboolean depth_mask, color_mask[4];
    int blend, depth_test, alpha_test, scissor, stencil;
    int active;
    u32 dbg, applied_frame, after;
} O;

static const char *VS =
    "#version 330 compatibility\n"
    "uniform mat4 uMVP, uPrevMVP, uMV;\n"
    "out vec4 vCur; out vec4 vPrev; out vec3 vEye;\n"
    "void main() {\n"
    "    vCur = uMVP * gl_Vertex; vPrev = uPrevMVP * gl_Vertex; vEye = (uMV * gl_Vertex).xyz;\n"
    "    gl_Position = vCur;\n"
    "}\n";
static const char *FS =
    "#version 330 compatibility\n"
    "uniform vec4 uVp;\n"
    "in vec4 vCur; in vec4 vPrev; in vec3 vEye;\n"
    "layout(location = 0) out vec4 oVel;\n"
    "void main() {\n"
    "    vec2 v = vec2(0.0);\n"
    "    if (vPrev.w > 1e-4) v = (vCur.xy / vCur.w - vPrev.xy / vPrev.w) * 0.5 * uVp.xy;\n"
    "    oVel = vec4(v, length(vEye), 1.0);\n"
    "}\n";

static int init(void)
{
    static const char *names[] = { "glUseProgram", "glGetUniformLocation", "glUniform1i", "glUniform3f", "glUniform4f",
        "glUniformMatrix4fv", "glDispatchCompute", "glMemoryBarrier", "glBindImageTexture", "glTexStorage2D",
        "glActiveTexture", "glGenQueries", "glQueryCounter", "glGetQueryObjectuiv", "glGetQueryObjectui64v",
        "glCreateShader", "glShaderSource", "glCompileShader", "glGetShaderiv", "glGetShaderInfoLog", "glDeleteShader",
        "glCreateProgram", "glAttachShader", "glLinkProgram", "glGetProgramiv", "glDeleteProgram",
        "glGenFramebuffers", "glBindFramebuffer", "glFramebufferTexture2D", "glGenRenderbuffers", "glBindRenderbuffer",
        "glRenderbufferStorage", "glFramebufferRenderbuffer", "glCheckFramebufferStatus", "glDeleteFramebuffers",
        "glDeleteRenderbuffers" };
    PROC *slot = (PROC *)&G;
    int i, a, b;
    HGLRC ctx = wglGetCurrentContext();
    if (g_state < 0) return 0;
    if (!g_state) {
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
            if (!(slot[i] = rt_real_proc(names[i]))) { g_state = -1; rt_log("motion: %s missing; off", names[i]); return 0; }
        g_state = 1;
    }
    if (X.ctx != ctx) { memset(&X, 0, sizeof X); X.ctx = ctx; O.ncur = O.nprev = 0; O.drawn = 0; }
    if (X.failed) return 0;
    if (!X.prog[0][0]) {
        static const char *defs[4][2] = { { "#define PASS 1\n", NULL }, { "#define PASS 2\n", NULL },
                                          { "#define PASS 3\n", "#define PASS 3\n#define OUT_RGBA8 1\n" },
                                          { "#define PASS 4\n", NULL } };
        for (a = 0; a < 4; a++)
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

static GLuint make_tex(GLenum fmt, u32 w, u32 h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    G.TexStorage2D(GL_TEXTURE_2D, 1, fmt, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

static void mul4(const float *a, const float *b, float *o)  /* row vectors: o = a @ b */
{
    int i, j, k;
    float t[16];
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double s = 0;
            for (k = 0; k < 4; k++) s += (double)a[i * 4 + k] * b[k * 4 + j];
            t[i * 4 + j] = (float)s;
        }
    memcpy(o, t, sizeof t);
}

static GLuint compile_stage(GLenum type, const char *src)
{
    GLint ok = 0;
    char logbuf[2048];
    GLuint s = G.CreateShader(type);
    G.ShaderSource(s, 1, &src, NULL);
    G.CompileShader(s);
    G.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        logbuf[0] = 0;
        G.GetShaderInfoLog(s, sizeof logbuf, NULL, logbuf);
        rt_log("motion: velocity %s shader failed to compile:\n%s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", logbuf);
        G.DeleteShader(s);
        return 0;
    }
    return s;
}

/* The velocity program and a buffer of w x h (once per size). */
static int objects_ready(u32 w, u32 h)
{
    GLint prev_tex = 0, prev_rb = 0;
    if (X.ofailed) return 0;
    if (!X.oprog) {
        GLuint vs = compile_stage(GL_VERTEX_SHADER, VS), fs = vs ? compile_stage(GL_FRAGMENT_SHADER, FS) : 0;
        GLint ok = 0;
        if (!vs || !fs) { if (vs) G.DeleteShader(vs); X.ofailed = 1; return 0; }
        X.oprog = G.CreateProgram();
        G.AttachShader(X.oprog, vs); G.AttachShader(X.oprog, fs);
        G.LinkProgram(X.oprog);
        G.DeleteShader(vs); G.DeleteShader(fs);
        G.GetProgramiv(X.oprog, GL_LINK_STATUS, &ok);
        if (!ok) { rt_log("motion: velocity program failed to link"); G.DeleteProgram(X.oprog); X.oprog = 0; X.ofailed = 1; return 0; }
        X.u_mvp = G.GetUniformLocation(X.oprog, "uMVP");
        X.u_prev = G.GetUniformLocation(X.oprog, "uPrevMVP");
        X.u_mv = G.GetUniformLocation(X.oprog, "uMV");
        X.u_vp = G.GetUniformLocation(X.oprog, "uVp");
    }
    if (X.ofbo && X.ow == w && X.oh == h) return 1;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(0x8CA7 /* GL_RENDERBUFFER_BINDING */, &prev_rb);
    if (X.otex) glDeleteTextures(1, &X.otex);
    if (X.odepth) G.DeleteRenderbuffers(1, &X.odepth);
    if (!X.ofbo) G.GenFramebuffers(1, &X.ofbo);
    X.otex = make_tex(GL_RGBA32F, w, h);
    G.GenRenderbuffers(1, &X.odepth);
    G.BindRenderbuffer(GL_RENDERBUFFER, X.odepth);
    G.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, X.ofbo);
    G.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, X.otex, 0);
    G.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, X.odepth);
    if (G.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        rt_log("motion: velocity framebuffer incomplete; objects off");
        X.ofailed = 1;
    }
    G.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    X.ow = w; X.oh = h;
    rt_log("motion: object velocity buffer %ux%u", w, h);
    return !X.ofailed;
}

static u32 draw_key(const struct rt_draw *d)
{
    u32 k = 2166136261u, v[8];
    int i;
    v[0] = d->v_buf; v[1] = d->v_buf ? (u32)(uintptr_t)d->v_ptr : 0; v[2] = d->elem_buf;
    v[3] = d->elem_buf ? (u32)(uintptr_t)d->indices : 0; v[4] = d->count; v[5] = d->first;
    v[6] = (u32)d->mode; v[7] = d->texture;
    for (i = 0; i < 8; i++) { k ^= v[i]; k *= 16777619u; }
    return k;
}

int motion_object_begin(const struct rt_draw *d, const float *model, const float *view, const float *proj)
{
    struct obj *best = NULL;
    float bd = 1e30f, diff = 0, mvp[16], prev[16], mv[16];
    int i, bi = -1;
    u32 key;
    GLint vp[4];
    if (!motion_objects_wanted() || !d->v_on || !post_main_view()) return 0;
    if (g_state <= 0 || X.ctx != wglGetCurrentContext() || X.failed || X.ofailed) return 0;
    key = draw_key(d);
    O.seen++;
    if (O.applied_frame == X.frame + 1) O.after++;     /* lit draws after the blur pass this frame */
    if (O.ncur < MAXOBJ) { O.cur[O.ncur].key = key; memcpy(O.cur[O.ncur].model, model, 64); O.ncur++; }
    if (!X.have_prev || X.prev_frame + 1 != X.frame) return 0;          /* no camera from last frame */
    for (i = 0; i < O.nprev; i++) {
        float dx, dy, dz, dd;
        if (O.prev[i].key != key || O.taken[i]) continue;
        dx = O.prev[i].model[12] - model[12]; dy = O.prev[i].model[13] - model[13]; dz = O.prev[i].model[14] - model[14];
        dd = dx * dx + dy * dy + dz * dz;
        if (dd < bd) { bd = dd; best = &O.prev[i]; bi = i; }
    }
    if (!best || sqrtf(bd) > S.obj_jump) return 0;
    O.taken[bi] = 1;
    for (i = 0; i < 16; i++) { float e = fabsf(model[i] - best->model[i]); if (e > diff) diff = e; }
    if (diff < 1e-4f) return 0;                                          /* did not move */
    if (S.log >= 2 && O.dbg < 40) {
        O.dbg++;
        rt_log("motion: object key %08x tex %u count %u buf %u moved %.2f (largest matrix change %.4f), pos %.1f %.1f %.1f",
               key, d->texture, d->count, d->v_buf, sqrtf(bd), diff, model[12], model[13], model[14]);
    }
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] < 16 || vp[3] < 16 || vp[0] < 0 || vp[1] < 0) return 0;
    if (!objects_ready((u32)(vp[0] + vp[2]), (u32)(vp[1] + vp[3]))) return 0;

    mul4(model, view, mv);
    mul4(mv, proj, mvp);
    mul4(best->model, X.prev_vp, prev);
    /* save what we change */
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &O.fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &O.prog);
    glGetIntegerv(GL_DEPTH_FUNC, &O.depth_func);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &O.depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, O.color_mask);
    O.blend = glIsEnabled(GL_BLEND); O.depth_test = glIsEnabled(GL_DEPTH_TEST); O.alpha_test = glIsEnabled(GL_ALPHA_TEST);
    O.scissor = glIsEnabled(GL_SCISSOR_TEST); O.stencil = glIsEnabled(GL_STENCIL_TEST);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, X.ofbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    if (O.drawn_frame != X.frame || !O.drawn) {                         /* first moving object this frame */
        GLfloat cc[4]; GLfloat cd = 1;
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &cd);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0); glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(cc[0], cc[1], cc[2], cc[3]); glClearDepth(cd);
        if (O.scissor) glEnable(GL_SCISSOR_TEST);
        O.drawn_frame = X.frame; O.drawn = 1;
    }
    glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    G.UseProgram(X.oprog);
    G.UniformMatrix4fv(X.u_mvp, 1, GL_FALSE, mvp);
    G.UniformMatrix4fv(X.u_prev, 1, GL_FALSE, prev);
    G.UniformMatrix4fv(X.u_mv, 1, GL_FALSE, mv);
    G.Uniform4f(X.u_vp, (float)vp[2], (float)vp[3], 0, 0);
    O.active = 1;
    O.draws++;
    return 1;
}

void motion_object_end(void)
{
    if (!O.active) return;
    O.active = 0;
    G.UseProgram((GLuint)O.prog);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)O.fbo);
    glDepthFunc((GLenum)O.depth_func);
    glDepthMask(O.depth_mask);
    glColorMask(O.color_mask[0], O.color_mask[1], O.color_mask[2], O.color_mask[3]);
    if (O.blend) glEnable(GL_BLEND);
    if (O.alpha_test) glEnable(GL_ALPHA_TEST);
    if (O.stencil) glEnable(GL_STENCIL_TEST);
    if (!O.depth_test) glDisable(GL_DEPTH_TEST);
}

/* ---- the blur pass ------------------------------------------------------------------- */
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
    int i, k, c, have_prev, still = 1, cut = 0, objs;
    GLint prev_unit = 0, prev_tex[3] = { 0, 0, 0 }, prev_prog = 0;
    if (!motion_wanted() || !scene || !w || !h) return;
    if (!init()) return;
    mask = rt_post_mask(&mw, &mh, &vw, &vh);
    if (!mask || !mw || !mh || !vw || !vh || !rt_post_camera(view, proj, sun, scol)) return;
    if (fabsf(proj[0]) < 1e-6f || fabsf(proj[5]) < 1e-6f) return;

    mul4(view, proj, vp);
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
    objs = S.objects && O.drawn && O.drawn_frame == X.frame && X.otex && !X.ofailed;
    if (have_prev) {
        float dot = 0, dist = 0;
        for (i = 0; i < 16; i++) if (fabsf(vp[i] - X.prev_vp[i]) > 1e-6f * (1.0f + fabsf(vp[i]))) { still = 0; break; }
        for (k = 0; k < 3; k++) { dot += fwd[k] * X.prev_fwd[k]; dist += (eye[k] - X.prev_eye[k]) * (eye[k] - X.prev_eye[k]); }
        if (dot < cosf(S.cut * 3.14159265f / 180.0f) || sqrtf(dist) > S.jump) cut = 1;
    }
    if (!have_prev) ;
    else if (cut) X.cuts++;
    else if (still && !objs) X.still++;
    if (have_prev && !cut && (!still || objs || S.debug)) {
        static const int order[4] = { 0, 3, 1, 2 };     /* PASS 1, 4, 2, 3 */
        int prev_ok = X.prev_mask && X.prev_mask != mask && X.prev_mw == mw && X.prev_mh == mh && glIsTexture(X.prev_mask);
        int oi;
        pw = vw < w ? vw : w; ph = vh < h ? vh : h;
        if (objs && (X.ow < pw || X.oh < ph)) objs = 0;

        glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
        for (i = 0; i < 3; i++) {
            G.ActiveTexture(GL_TEXTURE0 + MB_UNIT + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex[i]);
        }
        if (X.w != w || X.h != h || !X.vel) {
            if (X.vel) glDeleteTextures(1, &X.vel);
            if (X.tmp) glDeleteTextures(1, &X.tmp);
            if (X.tiles) glDeleteTextures(1, &X.tiles);
            X.tw = (w + TILE - 1) / TILE; X.th = (h + TILE - 1) / TILE;
            X.vel = make_tex(GL_RGBA16F, w, h); X.tmp = make_tex(GL_RGBA16F, w, h); X.tiles = make_tex(GL_RGBA16F, X.tw, X.th);
            X.w = w; X.h = h;
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
        for (oi = 0; oi < 4; oi++) {
            int pi = order[oi];
            u32 tw = (pw + TILE - 1) / TILE, th = (ph + TILE - 1) / TILE;
            GLuint p = X.prog[pi][pi == 2 && rgba8 ? 1 : 0];
            G.UseProgram(p);
            set4(p, "uView", (float)pw, (float)ph, (float)mw, (float)mh);
            set4(p, "uBlur", S.shutter, S.max * (float)ph, (float)S.taps, prev_ok ? 1.0f : 0.0f);
            set4(p, "uFrame", (float)(X.frame & 1023), S.split ? 1.0f : 0.0f, S.debug ? 1.0f : 0.0f, 0);
            set4(p, "uObj", objs ? 1.0f : 0.0f, 0, (float)tw, (float)th);
            if (pi == 0) {
                setm(p, "uVP", vp); setm(p, "uPrevVP", X.prev_vp);
                set3(p, "uEye", eye); set3(p, "uPrevEye", X.prev_eye);
                set3(p, "uBase", base); set3(p, "uDx", dx); set3(p, "uDy", dy);
                bind_sampler(p, "uMaskTex", MB_UNIT, mask);
                bind_sampler(p, "uPrevMaskTex", MB_UNIT + 1, prev_ok ? X.prev_mask : mask);
                bind_sampler(p, "uObjTex", MB_UNIT + 2, objs ? X.otex : mask);
                G.BindImageTexture(0, X.vel, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            } else if (pi == 3) {
                G.BindImageTexture(1, X.vel, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(3, X.tiles, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            } else if (pi == 1) {
                bind_sampler(p, "uScene", MB_UNIT, scene);
                G.BindImageTexture(1, X.vel, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(3, X.tiles, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(0, X.tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            } else {
                G.BindImageTexture(1, X.tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(2, X.vel, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(3, X.tiles, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
                G.BindImageTexture(0, scene, 0, GL_FALSE, 0, GL_WRITE_ONLY, rgba8 ? GL_RGBA8 : GL_RGBA16F);
            }
            if (pi == 3) G.DispatchCompute((tw + 7) / 8, (th + 7) / 8, 1);
            else G.DispatchCompute((pw + 7) / 8, (ph + 7) / 8, 1);
            G.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        }
        G.MemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
        for (i = 0; i < 4; i++) G.BindImageTexture((GLuint)i, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        for (i = 0; i < 3; i++) {
            G.ActiveTexture(GL_TEXTURE0 + MB_UNIT + i);
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex[i]);
        }
        G.ActiveTexture((GLenum)prev_unit);
        G.UseProgram((GLuint)prev_prog);
        G.QueryCounter(X.q[X.q_next][1], MB_TIMESTAMP);
        X.q_pending[X.q_next] = 1;
        X.q_next = (X.q_next + 1) & 3;
        X.runs++;
        if (objs) X.obj_runs++;
    }
    /* this frame's camera and mask are next frame's "last" */
    memcpy(X.prev_vp, vp, sizeof vp);
    memcpy(X.prev_eye, eye, sizeof eye);
    memcpy(X.prev_fwd, fwd, sizeof fwd);
    X.prev_mask = mask; X.prev_mw = mw; X.prev_mh = mh;
    X.have_prev = 1; X.prev_frame = X.frame;
    O.applied_frame = X.frame + 1;
}

void motion_frame_end(void)
{
    /* this frame's object draws become last frame's */
    if (S.objects || O.ncur) {
        memcpy(O.prev, O.cur, sizeof(struct obj) * (size_t)O.ncur);
        O.nprev = O.ncur; O.ncur = 0;
        memset(O.taken, 0, sizeof O.taken);
    }
    X.frame++;
    if (!motion_wanted() || !S.log || g_state <= 0 || X.ctx != wglGetCurrentContext()) return;
    if (++X.frames < 300) return;
    rt_log("motion: %u blurred (%u with moving objects), %u still, %u cuts in %u frames; GPU %.3f ms per run (%u timed); "
           "objects: %u lit draws seen (%u after the blur pass), %u velocity re-draws",
           X.runs, X.obj_runs, X.still, X.cuts, X.frames, X.gpu_n ? X.gpu_ms / X.gpu_n : 0.0, X.gpu_n, O.seen, O.after, O.draws);
    X.frames = X.runs = X.obj_runs = X.still = X.cuts = X.gpu_n = 0; X.gpu_ms = 0;
    O.seen = O.draws = 0; O.dbg = 0; O.after = 0;
}

void motion_forget(void)
{
    /* programs are dropped (re-read from wumrt_motion.comp on next use); textures kept */
    int a, b;
    if (g_state > 0 && X.ctx == wglGetCurrentContext()) {
        for (a = 0; a < 4; a++)
            for (b = 0; b < 2; b++)
                if (X.prog[a][b]) G.DeleteProgram(X.prog[a][b]);
    }
    memset(X.prog, 0, sizeof X.prog);
    X.failed = 0; X.ofailed = 0;
}
