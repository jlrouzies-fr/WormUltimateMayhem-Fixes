/*
 * Film post-processing (part of wumrt_core.dll): tone curve, colour grade, bloom, depth of
 * field and anti-aliasing on the game's scene texture, before the HUD is drawn on it.
 *
 * Where in the frame: the engine draws the main view into a framebuffer of its own (FBO 2 in
 * the captures; the picture-in-picture view goes to another one first), then particles and
 * the worms' name labels with the same camera, then the 2D HUD and the HUD's 3D models with a
 * camera of their own, and finally copies the texture to the window. wumrt.c tells us about
 * every framebuffer bind and, before every draw, what kind of program draws it:
 *   - depth of field runs at the first unlit draw after the lit scene (particles, labels,
 *     the worm silhouettes), so labels and effects stay sharp;
 *   - everything else runs at the first draw with a camera that is not the scene's (the
 *     HUD), or when the engine leaves the scene's framebuffer, whichever comes first.
 * Only the framebuffer that held the last scene of the previous frame (the main view) is
 * processed.
 *
 * HDR (hdr = 1): the scene texture is given 16-bit float storage at the end of a frame (its
 * content is not needed then), and patch_shaders.py has the lit shaders stop clamping their
 * output to 1, so bright sunlit surfaces, highlights and stacked additive effects keep their
 * values for the tone curve and bloom. With an 8-bit texture the hardware clamps exactly as
 * the stock shaders did. Switching hdr off (or unloading the core) gives it 8 bits again.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "rt.h"
#include "post.h"
#include "atmos.h"
#include "motion.h"         /* motion-blur stream: camera motion blur */

#define GL_COMPUTE_SHADER 0x91B9
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_RGBA16F 0x881A
#define GL_R32F 0x822E
#define GL_HALF_FLOAT 0x140B
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x20
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x8
#define GL_FRAMEBUFFER_BARRIER_BIT 0x400
#define GL_TEXTURE_UPDATE_BARRIER_BIT 0x100
#define GL_READ_WRITE 0x88BA
#define GL_WRITE_ONLY 0x88B9
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_IMAGE_BINDING_NAME 0x8F3A
#define GL_RGBA32F 0x8814

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
    void (APIENTRY *Uniform4f)(GLint, float, float, float, float);
    void (APIENTRY *DispatchCompute)(GLuint, GLuint, GLuint);
    void (APIENTRY *MemoryBarrier)(GLbitfield);
    void (APIENTRY *BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    void (APIENTRY *TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (APIENTRY *ActiveTexture)(GLenum);
    void (APIENTRY *GetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
    void (APIENTRY *GenQueries)(GLsizei, GLuint *);
    void (APIENTRY *BeginQuery)(GLenum, GLuint);
    void (APIENTRY *EndQuery)(GLenum);
    void (APIENTRY *GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
    void (APIENTRY *DeleteQueries)(GLsizei, const GLuint *);
    void (APIENTRY *Uniform1i)(GLint, GLint);
} pg;
static int pg_state;        /* 0 not tried, 1 ready, -1 unavailable */

static int pg_init(void)
{
    static const char *names[] = {
        "glCreateShader", "glShaderSource", "glCompileShader", "glGetShaderiv", "glGetShaderInfoLog",
        "glDeleteShader", "glCreateProgram", "glAttachShader", "glLinkProgram", "glGetProgramiv",
        "glGetProgramInfoLog", "glDeleteProgram", "glUseProgram", "glGetUniformLocation", "glUniform4f",
        "glDispatchCompute", "glMemoryBarrier", "glBindImageTexture", "glTexStorage2D", "glActiveTexture",
        "glGetFramebufferAttachmentParameteriv", "glGenQueries", "glBeginQuery", "glEndQuery",
        "glGetQueryObjectuiv", "glDeleteQueries", "glUniform1i" };
    PROC *slot = (PROC *)&pg;
    int i;
    if (pg_state) return pg_state > 0;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
        slot[i] = rt_real_proc(names[i]);
        if (!slot[i]) { rt_log("post: %s is not available; film post-processing disabled", names[i]); pg_state = -1; return 0; }
    }
    pg_state = 1;
    return 1;
}

/* ---- settings ------------------------------------------------------------------------ */
static struct {
    int on, hdr, aa, dither;
    float exposure, contrast, knee, saturation, warm, vignette;
    float bloom, bloom_threshold, bloom_knee;
    int bloom_levels;
    float dof, dof_range, dof_near, dof_speed, dof_focus;
    float glare, glare_size;
    float auto_exposure, auto_key; int grade_auto;
    int timer;
} S = { 0, 0, 0, 1, 1.0f, 1.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 5, 0.0f, 1.0f, 0.3f, 0.15f, 0 };

#include "grade.c"      /* grade stream: per-level colour script (wumrt_grades.ini, grade_levels) */

void post_settings_begin(void)
{
    /* every read of wumrt.ini starts from the defaults, so a deleted line means "default" */
    S.on = 0; S.hdr = 0; S.aa = 0; S.dither = 1;
    S.exposure = 1.0f; S.contrast = 1.0f; S.knee = 0.6f; S.saturation = 1.0f; S.warm = 0.0f; S.vignette = 0.0f;
    S.bloom = 0.0f; S.bloom_threshold = 1.0f; S.bloom_knee = 0.5f; S.bloom_levels = 5;
    S.dof = 0.0f; S.dof_range = 1.0f; S.dof_near = 0.15f; S.dof_speed = 0.15f; S.dof_focus = 0.0f;
    S.glare = 0.0f; S.glare_size = 1.0f;
    S.auto_exposure = 0.0f; S.auto_key = 0.35f; S.grade_auto = 0;
    S.timer = 0;
    gr_on = 0; gr_speed = 0.04f; gr_loaded = 0;     /* grade.c: wumrt_grades.ini is read again too */
}

int post_setting(const char *name, float v)
{
    if (!strcmp(name, "post")) S.on = v != 0;
    else if (!strcmp(name, "hdr")) S.hdr = v != 0;
    else if (!strcmp(name, "exposure")) S.exposure = v;
    else if (!strcmp(name, "tone_contrast")) S.contrast = v;
    else if (!strcmp(name, "tone_knee")) S.knee = v;
    else if (!strcmp(name, "saturation")) S.saturation = v;
    else if (!strcmp(name, "grade_warm")) S.warm = v;
    else if (!strcmp(name, "vignette")) S.vignette = v;
    else if (!strcmp(name, "bloom")) S.bloom = v;
    else if (!strcmp(name, "bloom_threshold")) S.bloom_threshold = v;
    else if (!strcmp(name, "bloom_knee")) S.bloom_knee = v;
    else if (!strcmp(name, "bloom_levels")) S.bloom_levels = (int)v;
    else if (!strcmp(name, "dof")) S.dof = v;
    else if (!strcmp(name, "dof_range")) S.dof_range = v;
    else if (!strcmp(name, "dof_near")) S.dof_near = v;
    else if (!strcmp(name, "dof_speed")) S.dof_speed = v;
    else if (!strcmp(name, "dof_focus")) S.dof_focus = v;
    else if (!strcmp(name, "glare")) S.glare = v;
    else if (!strcmp(name, "glare_size")) S.glare_size = v;
    else if (!strcmp(name, "auto_exposure")) S.auto_exposure = v;
    else if (!strcmp(name, "auto_key")) S.auto_key = v;
    else if (!strcmp(name, "grade_auto")) S.grade_auto = v != 0;
    else if (!strcmp(name, "aa")) S.aa = v != 0;
    else if (!strcmp(name, "dither")) S.dither = v != 0;
    else if (!strcmp(name, "post_timer")) S.timer = v != 0;
    else if (!strcmp(name, "grade_levels")) gr_on = v != 0;                    /* grade.c */
    else if (!strcmp(name, "grade_speed")) gr_speed = v > 0 && v <= 1 ? v : 1;  /* grade.c */
    else return 0;
    return 1;
}

void post_settings_end(void)
{
    if (S.bloom_levels < 1) S.bloom_levels = 1;
    if (S.bloom_levels > 7) S.bloom_levels = 7;
    if (S.knee < 0.05f) S.knee = 0.05f;
    if (S.knee > 0.98f) S.knee = 0.98f;
    if (S.dof < 0) S.dof = 0;
    if (S.dof > 0.05f) S.dof = 0.05f;
    if (S.dof_speed <= 0 || S.dof_speed > 1) S.dof_speed = 1;
    rt_log("post: on=%d hdr=%d exposure=%.2f contrast=%.2f knee=%.2f saturation=%.2f warm=%.2f vignette=%.2f bloom=%.2f "
           "(threshold %.2f, knee %.2f, %d levels) dof=%.4f (range %.2f, near %.2f) aa=%d", S.on, S.hdr, S.exposure,
           S.contrast, S.knee, S.saturation, S.warm, S.vignette, S.bloom, S.bloom_threshold, S.bloom_knee,
           S.bloom_levels, S.dof, S.dof_range, S.dof_near, S.aa);
}

/* ---- GPU resources ------------------------------------------------------------------- */
#define NPASS 9
static struct {
    HGLRC ctx;
    GLuint prog[NPASS][2];          /* [pass][1 = 8-bit output variant] */
    GLuint prog_ldr;                /* composite into the AA input */
    int compiled, failed;
    u32 w, h;                       /* size the textures below were made for */
    GLuint tmp, ldr, focus;
    GLuint bloom[8]; u32 bw[8], bh[8]; int nbloom;
    GLuint query; int query_pending; u32 gpu_us; u32 timed;
    GLuint qdof; int qdof_pending; u32 dof_us; u32 dof_timed;
    /* the game's scene texture this frame (the engine alternates between two from frame to frame) */
    GLuint scene_tex; u32 sw, sh; GLint sfmt;
} G;

/* Every texture the scene framebuffer has drawn into lately. */
#define NST 4
static struct scene_tex {
    GLuint id; u32 w, h; GLint fmt;
    GLint orig_fmt; int converted;  /* we gave it float storage; orig_fmt is what it had */
    u32 checked, used;              /* frames: when its size and format were last asked, last used */
} ST[NST];

/* ---- what happens in the frame ------------------------------------------------------- */
static struct {
    GLuint cur_fbo;                 /* the engine's draw framebuffer, as far as we know */
    int fbo_known;
    GLuint scene_fbo; int scene_seen;
    float scene_view[16];
    int lit_after;                  /* a lit draw happened since the scene's landscape */
    int dof_done, post_done;
    GLuint main_fbo; int main_known;    /* from the previous frame */
    u32 frame;
    u32 runs, dof_runs, last_log;
    u32 binds, fbo_mismatch, scenes, worm_runs;
    int dof_ran;
    int worm_found; float worm_dist, worm_score;   /* the worm nearest the middle of the main view */
    float night; float light_col[3]; int light_known;   /* level mood from the game's sun colour */
    int sun_on; float sun_uv[2], sun_col[3];   /* the sun on screen this frame, for the glare */
} F;

static char *read_text(const char *path)
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

static GLuint compile(const char *src, const char *defines)
{
    const char *eol = strchr(src, '\n');
    const char *srcs[3];
    GLint lens[3], ok = 0;
    char logbuf[4096];
    GLuint sh, prog;
    if (!eol) return 0;
    srcs[0] = src; lens[0] = (GLint)(eol + 1 - src);
    srcs[1] = defines; lens[1] = (GLint)strlen(defines);
    srcs[2] = eol + 1; lens[2] = (GLint)strlen(eol + 1);
    sh = pg.CreateShader(GL_COMPUTE_SHADER);
    pg.ShaderSource(sh, 3, srcs, lens);
    pg.CompileShader(sh);
    pg.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        logbuf[0] = 0;
        pg.GetShaderInfoLog(sh, sizeof logbuf, NULL, logbuf);
        rt_log("post: wumrt_post.comp (%s) failed to compile:\n%s", defines, logbuf);
        pg.DeleteShader(sh);
        return 0;
    }
    prog = pg.CreateProgram();
    pg.AttachShader(prog, sh);
    pg.LinkProgram(prog);
    pg.DeleteShader(sh);
    pg.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        logbuf[0] = 0;
        pg.GetProgramInfoLog(prog, sizeof logbuf, NULL, logbuf);
        rt_log("post: wumrt_post.comp (%s) failed to link:\n%s", defines, logbuf);
        pg.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void delete_programs(void)
{
    int i, k;
    for (i = 0; i < NPASS; i++)
        for (k = 0; k < 2; k++)
            if (G.prog[i][k]) { pg.DeleteProgram(G.prog[i][k]); G.prog[i][k] = 0; }
    if (G.prog_ldr) { pg.DeleteProgram(G.prog_ldr); G.prog_ldr = 0; }
    G.compiled = 0;
}

static int compile_all(void)
{
    char *src = read_text("wumrt_post.comp");
    char def[96];
    int i, bad = 0;
    double t0 = (double)GetTickCount();
    if (!src) { rt_log("post: wumrt_post.comp not found"); G.failed = 1; return 0; }
    delete_programs();
    for (i = 1; i < NPASS; i++) {
        sprintf(def, "#define PASS %d\n", i);
        if (i == 7) continue;           /* the composite only ever writes ldr (prog_ldr) */
        G.prog[i][0] = compile(src, def);
        if (!G.prog[i][0]) bad++;
        if (i == 3 || i == 8) {
            sprintf(def, "#define PASS %d\n#define OUT_RGBA8\n", i);
            G.prog[i][1] = compile(src, def);
            if (!G.prog[i][1]) bad++;
        }
    }
    G.prog_ldr = compile(src, "#define PASS 7\n#define TO_LDR\n");
    if (!G.prog_ldr) bad++;
    free(src);
    if (bad) { delete_programs(); G.failed = 1; return 0; }
    G.compiled = 1; G.failed = 0;
    rt_log("post: programs compiled in %.0f ms", (double)GetTickCount() - t0);
    return 1;
}

static GLuint make_tex(GLenum fmt, u32 w, u32 h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    pg.TexStorage2D(GL_TEXTURE_2D, 1, fmt, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void delete_textures(void)
{
    int i;
    if (G.tmp) glDeleteTextures(1, &G.tmp);
    if (G.ldr) glDeleteTextures(1, &G.ldr);
    for (i = 0; i < G.nbloom; i++) if (G.bloom[i]) glDeleteTextures(1, &G.bloom[i]);
    G.tmp = G.ldr = 0; memset(G.bloom, 0, sizeof G.bloom); G.nbloom = 0; G.w = G.h = 0;
}

/* Textures for a scene of w x h (the caller saves and restores the texture binding). */
static void ensure_textures(u32 w, u32 h)
{
    u32 bw = (w + 1) / 2, bh = (h + 1) / 2;
    if (G.w == w && G.h == h && G.tmp) return;
    delete_textures();
    G.tmp = make_tex(GL_RGBA16F, w, h);
    G.ldr = make_tex(GL_RGBA16F, w, h);
    while (G.nbloom < 7 && bw >= 2 && bh >= 2) {
        G.bw[G.nbloom] = bw; G.bh[G.nbloom] = bh;
        G.bloom[G.nbloom] = make_tex(GL_RGBA16F, bw, bh);
        G.nbloom++;
        bw = (bw + 1) / 2; bh = (bh + 1) / 2;
    }
    if (!G.focus) {
        float zero = 0;
        float zero4[4] = { 0, 0, 0, 0 };
        G.focus = make_tex(GL_RGBA32F, 1, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, zero4);
        (void)zero;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    G.w = w; G.h = h;
    rt_log("post: textures for %ux%u, %d bloom levels", w, h, G.nbloom);
}

static void query_texture(struct scene_tex *t)
{
    GLint prev = 0, v = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &v); t->w = (u32)v;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &v); t->h = (u32)v;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &t->fmt);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    t->checked = F.frame;
}

/* The colour texture of the engine's framebuffer that is bound now, its size and format. */
static int find_scene_texture(void)
{
    GLint type = 0, name = 0;
    struct scene_tex *t = NULL;
    int i;
    pg.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type != GL_TEXTURE) {
        static u32 logged;
        if (!logged++) rt_log("post: the scene framebuffer has no colour texture (type %#x)", type);
        return 0;
    }
    pg.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    if (name <= 0) return 0;
    for (i = 0; i < NST; i++) if (ST[i].id == (GLuint)name) { t = &ST[i]; break; }
    if (!t) {                       /* new: take the slot used longest ago */
        t = &ST[0];
        for (i = 1; i < NST; i++) if (ST[i].used < t->used) t = &ST[i];
        memset(t, 0, sizeof *t);
        t->id = (GLuint)name;
        query_texture(t);
        rt_log("post: scene texture %u, %ux%u, format %#x (framebuffer %u)", t->id, t->w, t->h, t->fmt, F.cur_fbo);
    } else if (F.frame - t->checked >= 120) query_texture(t);
    t->used = F.frame ? F.frame : 1;
    G.scene_tex = t->id; G.sw = t->w; G.sh = t->h; G.sfmt = t->fmt;
    return G.sw > 0 && G.sh > 0 && G.sw <= 8192 && G.sh <= 8192;
}

static int is_float_fmt(GLint f) { return f == GL_RGBA16F || f == 0x8814 /* RGBA32F */ || f == 0x881B /* RGB16F */; }

/* ---- running the passes -------------------------------------------------------------- */
struct saved {
    GLint prog, active, tex[4];
};
static void save_state(struct saved *s)
{
    int i;
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    for (i = 0; i < 4; i++) {
        pg.ActiveTexture(GL_TEXTURE0 + 12 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex[i]);
    }
}
static void restore_state(const struct saved *s)
{
    int i;
    for (i = 0; i < 4; i++) {
        pg.ActiveTexture(GL_TEXTURE0 + 12 + i);
        glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex[i]);
    }
    pg.ActiveTexture((GLenum)s->active);
    pg.UseProgram((GLuint)s->prog);
}

/* Sampler bindings: unit 12 + i, matched to `layout(binding = i)` by setting the uniform. */
static void bind_sampler(GLuint prog, const char *name, int i, GLuint tex)
{
    GLint loc = pg.GetUniformLocation(prog, name);
    pg.ActiveTexture(GL_TEXTURE0 + 12 + i);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (loc >= 0) pg.Uniform1i(loc, 12 + i);
}

static void set4(GLuint prog, const char *name, float a, float b, float c, float d)
{
    GLint loc = pg.GetUniformLocation(prog, name);
    if (loc >= 0) pg.Uniform4f(loc, a, b, c, d);
}

/* The grade for the level's mood: with grade_auto = 1 a dim or bluish sun (night levels) turns
   the warm light into a cool moonlit tint and eases the saturation. */
static float mood_warm(void) { return S.grade_auto ? S.warm * (1.0f - F.night) - 0.5f * F.night : S.warm; }
static float mood_sat(void) { return S.grade_auto ? S.saturation + (1.0f - S.saturation) * 0.5f * F.night : S.saturation; }

static void common_uniforms(GLuint prog, u32 dw, u32 dh, u32 sw, u32 sh)
{
    set4(prog, "uSize", (float)dw, (float)dh, 1.0f / dw, 1.0f / dh);
    set4(prog, "uSrc", (float)sw, (float)sh, 1.0f / sw, 1.0f / sh);
    set4(prog, "uTone", S.exposure, S.contrast, S.knee, mood_sat());
    set4(prog, "uGrade", mood_warm(), S.vignette, S.bloom, S.dither ? 1.0f : 0.0f);
    set4(prog, "uAuto", S.auto_exposure, S.auto_key > 0.01f ? S.auto_key : 0.01f, 0.04f, 0);
    set4(prog, "uBloom", S.bloom_threshold, S.bloom_knee, 0, 0);
    set4(prog, "uFrame", S.dof_speed, (float)(F.frame & 1023), 0, 0);
}

/* Where the sun is on screen (main camera), for the glare. */
static void find_sun(void)
{
    float v[16], p[16], sun[3], col[3], e[3], clip[4], m;
    int c, k;
    F.sun_on = 0;
    if (S.glare <= 0 || !rt_post_camera(v, p, sun, col)) return;
    for (c = 0; c < 3; c++) e[c] = sun[0] * v[c] + sun[1] * v[4 + c] + sun[2] * v[8 + c];      /* a direction: no translation */
    for (c = 0; c < 4; c++) { clip[c] = 0; for (k = 0; k < 3; k++) clip[c] += e[k] * p[k * 4 + c]; }
    if (clip[3] <= 1e-4f) return;                                                               /* behind the camera */
    F.sun_uv[0] = clip[0] / clip[3] * 0.5f + 0.5f;
    F.sun_uv[1] = clip[1] / clip[3] * 0.5f + 0.5f;
    if (F.sun_uv[0] < -0.5f || F.sun_uv[0] > 1.5f || F.sun_uv[1] < -0.5f || F.sun_uv[1] > 1.5f) return;
    m = col[0] > col[1] ? col[0] : col[1]; if (col[2] > m) m = col[2];
    if (m <= 0) { col[0] = 1; col[1] = 0.9f; col[2] = 0.75f; m = 1; }
    for (c = 0; c < 3; c++) F.sun_col[c] = col[c] / m;
    F.sun_on = 1;
}

static void sun_uniforms(GLuint prog)
{
    set4(prog, "uSun", F.sun_uv[0], F.sun_uv[1], F.sun_on ? 1.0f : 0.0f, F.sun_on ? S.glare : 0.0f);
    set4(prog, "uSunCol", F.sun_col[0], F.sun_col[1], F.sun_col[2], S.glare_size > 0.05f ? S.glare_size : 0.05f);
}

static void dispatch(u32 w, u32 h)
{
    pg.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
    pg.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

static int ready(void)
{
    HGLRC ctx = wglGetCurrentContext();
    if (!pg_init()) return 0;
    if (G.ctx != ctx) {             /* a new GL context: what we made is gone with the old one */
        memset(G.prog, 0, sizeof G.prog); G.prog_ldr = 0; G.compiled = 0; G.failed = 0;
        G.tmp = G.ldr = G.focus = 0; memset(G.bloom, 0, sizeof G.bloom); G.nbloom = 0; G.w = G.h = 0;
        G.query = 0; G.query_pending = 0; G.qdof = 0; G.qdof_pending = 0; G.scene_tex = 0;
        memset(ST, 0, sizeof ST);
        G.ctx = ctx;
    }
    if (G.failed) return 0;
    if (!G.compiled && !compile_all()) return 0;
    return 1;
}

/* Depth of field, on the scene texture as it is now. */
static void run_dof(void)
{
    struct saved sv;
    GLuint mask;
    u32 mw = 0, mh = 0, vw = 0, vh = 0;
    int o8;
    F.dof_done = 1;
    if (motion_wanted() && ready() && find_scene_texture())         /* motion-blur stream: camera motion blur */
        motion_apply(G.scene_tex, G.sw, G.sh, !is_float_fmt(G.sfmt));  /* (motion.c), before the depth of field */
    if (S.dof <= 0 || !ready() || !find_scene_texture()) return;
    mask = rt_post_mask(&mw, &mh, &vw, &vh);
    if (!mask || !mw || !mh || !vw || !vh) return;
    o8 = !is_float_fmt(G.sfmt);
    save_state(&sv);
    ensure_textures(G.sw, G.sh);
    if (S.timer && !G.qdof) pg.GenQueries(1, &G.qdof);
    if (S.timer && G.qdof && !G.qdof_pending) pg.BeginQuery(GL_TIME_ELAPSED, G.qdof); else if (S.timer) G.qdof_pending |= 2;
    find_sun();
    /* focus: where the centre of the view is (and how much of the sun is open sky) */
    pg.UseProgram(G.prog[1][0]);
    bind_sampler(G.prog[1][0], "uMaskTex", 0, mask);
    common_uniforms(G.prog[1][0], G.sw, G.sh, mw, mh);
    set4(G.prog[1][0], "uDof", 0, 0, 0, S.dof_focus > 0 ? S.dof_focus : F.worm_found ? F.worm_dist : 0.0f);
    if (F.worm_found) F.worm_runs++;
    sun_uniforms(G.prog[1][0]);
    set4(G.prog[1][0], "uFrame", S.dof_speed, (float)(F.frame & 1023), F.sun_on ? 3.0f : 1.0f, 0);
    set4(G.prog[1][0], "uMask", (float)mw, (float)mh, (float)mw / (float)vw, (float)mh / (float)vh);
    pg.BindImageTexture(0, G.focus, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
    pg.DispatchCompute(1, 1, 1);
    pg.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    /* gather blur -> tmp */
    pg.UseProgram(G.prog[2][0]);
    bind_sampler(G.prog[2][0], "uScene", 0, G.scene_tex);
    bind_sampler(G.prog[2][0], "uMaskTex", 1, mask);
    bind_sampler(G.prog[2][0], "uFocusTex", 2, G.focus);
    common_uniforms(G.prog[2][0], G.sw, G.sh, G.sw, G.sh);
    set4(G.prog[2][0], "uDof", S.dof * (float)G.sh, S.dof_range, S.dof_near, S.dof_focus);
    set4(G.prog[2][0], "uMask", (float)mw, (float)mh, (float)mw / (float)vw, (float)mh / (float)vh);
    pg.BindImageTexture(0, G.tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    dispatch(G.sw, G.sh);
    /* tmp -> scene */
    pg.UseProgram(G.prog[3][o8]);
    bind_sampler(G.prog[3][o8], "uSrcTex", 0, G.tmp);
    common_uniforms(G.prog[3][o8], G.sw, G.sh, G.sw, G.sh);
    pg.BindImageTexture(0, G.scene_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, o8 ? GL_RGBA8 : GL_RGBA16F);
    dispatch(G.sw, G.sh);
    pg.MemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    pg.BindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
    if (S.timer && G.qdof) { if (G.qdof_pending & 2) G.qdof_pending &= ~2; else { pg.EndQuery(GL_TIME_ELAPSED); G.qdof_pending = 1; } }
    restore_state(&sv);
    F.dof_runs++;
    F.dof_ran = 1;
}

/* Tone curve, grade, bloom, anti-aliasing. */
static void run_post(void)
{
    struct saved sv;
    int o8, i, nb;
    F.post_done = 1;
    if (!F.dof_done) run_dof();
    if (!ready() || !find_scene_texture()) return;
    o8 = !is_float_fmt(G.sfmt);
    save_state(&sv);
    ensure_textures(G.sw, G.sh);
    atmos_apply(G.scene_tex, G.sw, G.sh, o8);       /* atmos.c: haze before bloom and the tone curve */
    if (S.timer && !G.query) pg.GenQueries(1, &G.query);
    if (S.timer && G.query && !G.query_pending) pg.BeginQuery(GL_TIME_ELAPSED, G.query);
    {
        /* level mood from the game's sun colour (night: dim or bluish) */
        float v[16], pm[16], sun[3], col[3];
        if (S.grade_auto && rt_post_camera(v, pm, sun, col)) {
            float l = 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2], blue = col[2] - col[0];
            float dim = l < 0.2f ? 1.0f : l > 0.4f ? 0.0f : (0.4f - l) / 0.2f;
            float bl = blue <= 0 ? 0.0f : blue > 0.1f ? 1.0f : blue / 0.1f;
            F.night = dim > bl ? dim : bl;
            memcpy(F.light_col, col, sizeof col); F.light_known = 1;
        } else F.night = 0;
    }
    if (S.auto_exposure > 0) {
        /* the scene's brightness, for the automatic exposure (before the bloom, which uses it) */
        pg.UseProgram(G.prog[1][0]);
        bind_sampler(G.prog[1][0], "uMaskTex", 0, G.scene_tex);         /* unused in this mode, but valid */
        bind_sampler(G.prog[1][0], "uScene", 1, G.scene_tex);
        common_uniforms(G.prog[1][0], G.sw, G.sh, G.sw, G.sh);
        set4(G.prog[1][0], "uFrame", S.dof_speed, (float)(F.frame & 1023), 4.0f, 0);
        set4(G.prog[1][0], "uSun", 0, 0, 0, 0);
        pg.BindImageTexture(0, G.focus, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
        pg.DispatchCompute(1, 1, 1);
        pg.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    if (S.glare > 0 && !(S.dof > 0 && F.dof_ran)) {
        /* the sun's visibility, without the focus (depth of field is off) */
        GLuint mask;
        u32 mw = 0, mh = 0, vw = 0, vh = 0;
        find_sun();
        mask = F.sun_on ? rt_post_mask(&mw, &mh, &vw, &vh) : 0;
        if (mask && mw && mh && vw && vh) {
            pg.UseProgram(G.prog[1][0]);
            bind_sampler(G.prog[1][0], "uMaskTex", 0, mask);
            common_uniforms(G.prog[1][0], G.sw, G.sh, mw, mh);
            sun_uniforms(G.prog[1][0]);
            set4(G.prog[1][0], "uDof", 0, 0, 0, S.dof_focus);
            set4(G.prog[1][0], "uFrame", S.dof_speed, (float)(F.frame & 1023), 2.0f, 0);
            set4(G.prog[1][0], "uMask", (float)mw, (float)mh, (float)mw / (float)vw, (float)mh / (float)vh);
            pg.BindImageTexture(0, G.focus, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            pg.DispatchCompute(1, 1, 1);
            pg.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        } else F.sun_on = 0;
    }
    nb = S.bloom > 0 ? (S.bloom_levels < G.nbloom ? S.bloom_levels : G.nbloom) : 0;
    if (nb > 0) {
        /* bright part, half size */
        pg.UseProgram(G.prog[4][0]);
        bind_sampler(G.prog[4][0], "uSrcTex", 0, G.scene_tex);
        bind_sampler(G.prog[4][0], "uFocusTex", 1, G.focus);
        common_uniforms(G.prog[4][0], G.bw[0], G.bh[0], G.sw, G.sh);
        pg.BindImageTexture(0, G.bloom[0], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        dispatch(G.bw[0], G.bh[0]);
        for (i = 1; i < nb; i++) {
            pg.UseProgram(G.prog[5][0]);
            bind_sampler(G.prog[5][0], "uSrcTex", 0, G.bloom[i - 1]);
            common_uniforms(G.prog[5][0], G.bw[i], G.bh[i], G.bw[i - 1], G.bh[i - 1]);
            pg.BindImageTexture(0, G.bloom[i], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            dispatch(G.bw[i], G.bh[i]);
        }
        for (i = nb - 2; i >= 0; i--) {
            pg.UseProgram(G.prog[6][0]);
            bind_sampler(G.prog[6][0], "uSrcTex", 0, G.bloom[i + 1]);
            common_uniforms(G.prog[6][0], G.bw[i], G.bh[i], G.bw[i + 1], G.bh[i + 1]);
            pg.BindImageTexture(0, G.bloom[i], 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
            dispatch(G.bw[i], G.bh[i]);
        }
    }
    {
        /* tone curve and grade -> ldr; then ldr -> scene, through FXAA or as it is */
        GLuint p = G.prog_ldr, q = G.prog[S.aa ? 8 : 3][o8];
        float strength = nb > 0 ? S.bloom / (float)nb : 0.0f;     /* the levels add up */
        pg.UseProgram(p);
        bind_sampler(p, "uScene", 0, G.scene_tex);
        bind_sampler(p, "uBloomTex", 1, nb > 0 ? G.bloom[0] : G.tmp);
        bind_sampler(p, "uFocusTex", 2, G.focus);
        common_uniforms(p, G.sw, G.sh, G.sw, G.sh);
        sun_uniforms(p);
        set4(p, "uGrade", mood_warm(), S.vignette, strength, S.dither ? 1.0f : 0.0f);
        grade_uniforms(p, S.grade_auto ? F.night : 0.0f);                                  /* grade.c: the level's own grade */
        pg.BindImageTexture(0, G.ldr, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        dispatch(G.sw, G.sh);
        pg.UseProgram(q);
        bind_sampler(q, "uSrcTex", 0, G.ldr);
        common_uniforms(q, G.sw, G.sh, G.sw, G.sh);
        pg.BindImageTexture(0, G.scene_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, o8 ? GL_RGBA8 : GL_RGBA16F);
        dispatch(G.sw, G.sh);
    }
    pg.MemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    pg.BindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
    if (S.timer && G.query && !G.query_pending) { pg.EndQuery(GL_TIME_ELAPSED); G.query_pending = 1; }
    restore_state(&sv);
    F.runs++;
}

/* ---- hooks from wumrt.c ---------------------------------------------------------------- */
static int same_rotation(const float *a, const float *b)
{
    int r, c;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            if (fabs(a[r * 4 + c] - b[r * 4 + c]) > 1e-4f) return 0;
    return 1;
}

static int in_main(void)
{
    return S.on && F.scene_seen && F.main_known && F.cur_fbo == F.main_fbo && F.scene_fbo == F.cur_fbo;
}

void post_bind_fbo(GLenum target, GLuint id)
{
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER) return;
    if (F.fbo_known && id != F.cur_fbo && in_main() && !F.post_done) run_post();   /* leaving the scene: last chance */
    F.cur_fbo = id;
    F.fbo_known = 1;
    F.binds++;
}

void post_before_draw(u32 cls, const float *view)
{
    if (!F.fbo_known) {
        GLint v = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &v);
        F.cur_fbo = (GLuint)v; F.fbo_known = 1;
    }
    if (cls & POST_CASTER) return;
    if (cls & POST_LANDSCAPE) {
        GLint v = 0;                /* ask, rather than trust the bind hooks alone (a few dozen draws a frame) */
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &v);
        if ((GLuint)v != F.cur_fbo) F.fbo_mismatch++;
        F.cur_fbo = (GLuint)v;
        F.scene_fbo = F.cur_fbo; F.scene_seen = 1; F.lit_after = 0;
        F.dof_done = F.post_done = 0;
        if (view) memcpy(F.scene_view, view, sizeof F.scene_view);
        return;
    }
    if (!in_main()) return;
    if (!F.post_done && (cls & POST_HAS_VIEW) && view && !same_rotation(view, F.scene_view)) { run_post(); return; }
    if (cls & POST_LIT) { F.lit_after = 1; return; }
    if (!F.dof_done && F.lit_after && (cls & (POST_UNLIT | POST_EFFECT)) && !(cls & POST_WATER)) run_dof();
}

int post_wants_worms(void)
{
    return S.on && S.dof > 0 && S.dof_focus <= 0 && in_main() && !F.dof_done;
}

void post_note_worm(const float *c, const float *v, const float *p)
{
    float e[4], clip[4], eye[3], d, score;
    int i, k;
    for (i = 0; i < 4; i++) e[i] = c[0] * v[i] + c[1] * v[4 + i] + c[2] * v[8 + i] + v[12 + i];
    for (i = 0; i < 4; i++) { clip[i] = 0; for (k = 0; k < 4; k++) clip[i] += e[k] * p[k * 4 + i]; }
    if (clip[3] <= 1e-3f) return;
    /* how far from the middle of the view, a bit more forgiving below it (the camera looks
       down on the worm it follows) */
    {
        float x = clip[0] / clip[3], y = clip[1] / clip[3];
        if (y < 0) y *= 0.7f;
        score = x * x + y * y;
    }
    if (score > 0.36f) return;                  /* only the middle 60% */
    for (k = 0; k < 3; k++) eye[k] = -(v[12] * v[k * 4 + 0] + v[13] * v[k * 4 + 1] + v[14] * v[k * 4 + 2]);
    d = (float)sqrt((c[0] - eye[0]) * (c[0] - eye[0]) + (c[1] - eye[1]) * (c[1] - eye[1]) + (c[2] - eye[2]) * (c[2] - eye[2]));
    if (!F.worm_found || score < F.worm_score) { F.worm_found = 1; F.worm_score = score; F.worm_dist = d; }
}

/* Give a scene texture float storage, or take it back: only between frames. */
static void set_scene_format(struct scene_tex *t, int want_float)
{
    GLint prev = 0, now = 0;
    GLenum fmt, e;
    if (!t->id || !t->w || !t->h || pg_state <= 0) return;
    if (want_float == is_float_fmt(t->fmt)) return;
    if (!want_float && !t->converted) return;           /* the game made it float itself: leave it */
    if (!glIsTexture(t->id)) { t->id = 0; return; }
    fmt = want_float ? GL_RGBA16F : (t->orig_fmt ? (GLenum)t->orig_fmt : GL_RGBA8);
    if (want_float) t->orig_fmt = t->fmt;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, t->id);
    while (glGetError() != GL_NO_ERROR) {}
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, (GLsizei)t->w, (GLsizei)t->h, 0, GL_RGBA,
                 want_float ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
    e = glGetError();
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &now);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    rt_log("post: scene texture %u given format %#x (%s), error %#x", t->id, now,
           want_float ? "float, for HDR" : "back to the game's", e);
    t->fmt = now;
    t->converted = want_float && is_float_fmt(now);
}

void post_frame_end(void)
{
    if (F.scene_seen) { F.main_fbo = F.scene_fbo; F.main_known = 1; F.scenes++; }
    else F.main_known = 0;
    F.scene_seen = 0; F.dof_done = F.post_done = 0; F.lit_after = 0; F.dof_ran = 0; F.worm_found = 0;
    F.frame++;
    if (pg_state > 0 && G.ctx == wglGetCurrentContext()) {
        int i;
        for (i = 0; i < NST; i++)
            if (ST[i].id && F.frame - ST[i].used < 600) set_scene_format(&ST[i], S.on && S.hdr);
    }
    if (G.qdof_pending == 1) {
        GLuint avail = 0;
        pg.GetQueryObjectuiv(G.qdof, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (avail) {
            GLuint ns = 0;
            pg.GetQueryObjectuiv(G.qdof, GL_QUERY_RESULT, &ns);
            G.dof_us += ns / 1000; G.dof_timed++;
            G.qdof_pending = 0;
        }
    }
    if (G.query_pending) {
        GLuint avail = 0;
        pg.GetQueryObjectuiv(G.query, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (avail) {
            GLuint ns = 0;
            pg.GetQueryObjectuiv(G.query, GL_QUERY_RESULT, &ns);
            G.gpu_us += ns / 1000; G.timed++;
            G.query_pending = 0;
        }
    }
    if (S.on && F.frame - F.last_log >= 300) {
        F.last_log = F.frame;
        if (S.timer && G.timed) {
            rt_log("post: %u runs, %u with depth of field in 300 frames; GPU %.3f ms per run (tone, bloom, AA; %u timed)",
                   F.runs, F.dof_runs, G.gpu_us / 1000.0 / G.timed, G.timed);
            G.gpu_us = 0; G.timed = 0;
            if (G.dof_timed) rt_log("post: depth of field GPU %.3f ms per run (%u timed)", G.dof_us / 1000.0 / G.dof_timed, G.dof_timed);
            G.dof_us = 0; G.dof_timed = 0;
        } else rt_log("post: %u runs, %u with depth of field in 300 frames", F.runs, F.dof_runs);
        rt_log("post: %u frames with a scene, main framebuffer %u, %u framebuffer binds seen, %u times the binding differed from the hooks; "
               "focus on a worm in %u frames (last at distance %.1f)", F.scenes, F.main_fbo, F.binds, F.fbo_mismatch, F.worm_runs, F.worm_dist);
        if (S.grade_auto || S.auto_exposure > 0) {
            float fx[4] = { 0, 0, 0, 0 };
            GLint prev = 0;
            if (G.focus && pg_state > 0) {
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
                glBindTexture(GL_TEXTURE_2D, G.focus);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, fx);
                glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
            }
            rt_log("post: mood: sun colour %.2f %.2f %.2f, night %.2f; scene mean luminance %.3f (log2 %.2f), auto exposure x%.2f",
                   F.light_col[0], F.light_col[1], F.light_col[2], F.night, pow(2.0, fx[2]), fx[2],
                   S.auto_exposure > 0 && fx[3] > 0.5f ? fmin(fmax(pow(2.0, (log2(S.auto_key) - fx[2]) * S.auto_exposure), 0.8), 1.25) : 1.0);
        }
        F.runs = F.dof_runs = 0; F.scenes = F.binds = F.fbo_mismatch = F.worm_runs = 0;
    }
}

void post_shutdown(void)
{
    if (pg_state <= 0 || G.ctx != wglGetCurrentContext()) return;
    {
        int i;
        for (i = 0; i < NST; i++) if (ST[i].id) set_scene_format(&ST[i], 0);
    }
    delete_programs();
    delete_textures();
    if (G.focus) { glDeleteTextures(1, &G.focus); G.focus = 0; }
    if (G.query) { pg.DeleteQueries(1, &G.query); G.query = 0; }
    if (G.qdof) { pg.DeleteQueries(1, &G.qdof); G.qdof = 0; }
}

void post_reload(void)
{
    if (pg_state > 0 && G.ctx == wglGetCurrentContext()) delete_programs();
    G.failed = 0;
}
