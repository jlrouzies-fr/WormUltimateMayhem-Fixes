/*
 * wumrt core - research harness for the ray-tracing spike, the swappable part. The loader
 * (loader.c, the proxy glut32.dll that the SANDBOX copy of Worms Ultimate Mayhem loads)
 * copies wumrt_core.dll and loads the copy at the end of the first frame, and again on
 * `rt.py reload-core`. Through the loader's import-table thunks this hooks the game to:
 *
 *   - take screenshots and dump one frame's draw calls (geometry, matrices, GL state)
 *     on request, through a shared-memory control block (see rt.py)
 *   - collect shadow casters and give the edited shaders the traced lighting (rt_core.c)
 *   - draw shell layers, override the scene camera (free camera)
 *
 * Input isolation, the control block itself and the Cg handle translation live in the
 * loader. 32-bit, WormsMayhem.exe build 1077 only (import table addresses are fixed).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "rt.h"
#include "ctl.h"
#include "wumrt_api.h"
#include "fx.h"
#include "atmos.h"
#include "motion.h"
#include "post.h"

/* ---- import table slots in WormsMayhem.exe ---------------------------------- */
#define IAT_SwapBuffers      0x815024
#define IAT_glDrawArrays     0x81542c
#define IAT_glEnableClientState 0x815430
#define IAT_glTexCoordPointer 0x815440
#define IAT_glVertexPointer  0x815444
#define IAT_glNormalPointer  0x815448
#define IAT_glAlphaFunc      0x815470
#define IAT_glDisableClientState 0x8154dc
#define IAT_glEnable         0x8154e0
#define IAT_glDisable        0x8154e4
#define IAT_glPopAttrib     0x8154f0
#define IAT_glPopClientAttrib 0x8154fc
#define IAT_glDrawElements   0x815434
#define IAT_glCallList       0x81543c
#define IAT_wglGetProcAddress 0x8154ac
#define IAT_glBegin          0x8154bc
#define IAT_glDeleteTextures 0x815538
#define IAT_glTexImage2D     0x81553c
#define IAT_glBlendFunc      0x815530
#define IAT_glColorPointer   0x81544c
#define IAT_cgGetProfileString 0x81566c
#define IAT_cgGetParameterName 0x815678
#define IAT_cgGetNextParameter 0x815684
#define IAT_cgGetFirstParameter 0x81569c
#define IAT_cgGLSetParameter3f 0x8156d0
#define IAT_cgGLSetMatrixParameterfr 0x8156d4
#define IAT_cgGLBindProgram  0x8156ec
#define IAT_cgUpdateProgramParameters 0x8156a8
#define IAT_cgGLSetParameter4f 0x8156cc
#define IAT_cgGLEnableTextureParameter 0x8156dc
#define IAT_cgGLSetTextureParameter 0x8156e0
#define IAT_cgGLDisableProfile 0x8156c0
#define IAT_cgGLEnableProfile 0x8156d8

#pragma pack(push, 1)
struct draw_rec {
    u32 magic;                  /* 'DRAW' */
    u32 kind;                   /* 0 elements, 1 arrays, 2 range elements, 3 begin, 4 call list */
    u32 mode, first, count, index_type;
    u32 fbo;
    i32 viewport[4];
    u32 depth_test, blend, cull, alpha_test, depth_mask, color_mask, depth_func, cull_mode, front_face;
    u32 blend_src, blend_dst;
    u32 tex0;
    u32 vp_index, fp_index;     /* into the program table, 0xffffffff = none */
    u32 have;                   /* bit 0 model, 1 view, 2 projection, 3 shadowMatrix, 4 light dir */
    float model[16], view[16], proj[16], shadow[16], light[4];
    float gl_modelview[16], gl_projection[16];
    u32 v_size, v_type, v_stride, v_ptr, v_buffer, elem_buffer;
    u32 n_size, n_type_stride, n_ptr, n_buffer;     /* normal array, if enabled (n_size 3 or 0) */
    u32 nverts, vbytes;         /* vbytes of raw vertex data follow (stride as recorded) */
    u32 nidx;                   /* then nidx u32 indices (elements only) */
};

struct prog_rec {
    u32 magic;                  /* 'PROG' */
    u32 index, is_vertex;
    char file[64], entry[64], profile[16];
};
#pragma pack(pop)

static struct ctl *ctl;
static struct ctl dummy_ctl;
static struct wumrt_loader *L;      /* the permanent loader (glut32.dll) we live in */
static HWND game_hwnd;
static void settings_poll(void);
static u32 freecam_command(u32 mode);
static u32 frame_cap;
static u32 *frame_cap_kept;         /* the loader keeps it across core reloads */

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!L) return;
    va_start(ap, fmt);
    L->vlog(fmt, ap);
    va_end(ap);
}

static void *hook_iat(u32 slot, void *fn)
{
    return L->hook_iat(slot, fn);
}

/* ---- Cg program and parameter tracking ------------------------------------------ */
#define MAXPROG 512
struct prog {
    void *h;                         /* the handle in use (changes when a Cg reload replaces the program) */
    void *orig_h;                    /* the handle the engine created and holds */
    int is_vertex;
    char file[64], entry[64], profile[16];
    u32 have;
    float model[16], view[16], proj[16], shadow[16], light[4];
    void *p_rt_refl; int refl_last;  /* the sea's fragment program: our reflection sampler */
    void *p_rt_normal, *p_rt_params3;    /* normal map sampler and its strength */
    void *p_rt_char, *p_rt_char2;        /* characters: what the draw is, strengths (see before_draw) */
    void *p_rt_mask, *p_rt_bounce, *p_rt_params, *p_rt_params2;  /* our extra uniforms, if the (edited) shader has them */
    int sun_lit;                     /* main-pass vertex program lit by the sun */
    int caster;                      /* shadow-map pass vertex program: its draws cast shadows */
    int landscape;                   /* main-pass terrain program */
    int profile_id;
    u32 tex0;                        /* fragment program: texture last given to its texture0 sampler */
    float texxf[16];                 /* vertex program: its texture matrix (have bit 6) */
    float dif[3], amb[3];            /* fragment program: light colours last given to it (have bits 7, 8) */
    float mat[16];                   /* fragment program: material matrix last given to it (have bit 9) */
    void *p_view, *p_proj;           /* vertex program: handles of its view and projection matrices */
    int unlit_tex;                   /* textured, unlit vertex program: what the sky is drawn with */
    int water;                       /* the sea's main-pass vertex program */
    int effect;                      /* PostProcess.cg (worm silhouettes; film post, post.c) */
    float rt_last;
    void *p_rt_shell;                /* terrain programs (vertex and fragment): the shell layer uniform */
    int shell_on;                    /* fragment program: rtShell was left non-zero */
    void *p_rt_relief;               /* lit fragment programs: the parallax relief uniform */
    void *p_rt_smoke[6];             /* FFFragmentMainTexCol: lit smoke uniforms (see smoke_draw) */
    int smoke_last;                  /* rtSmoke was left on */
    void *p_rt_wind;                 /* terrain fragment programs: wind and colour patches on the grass */
    void *p_rt_water;                /* the sea's fragment program: stylised water (water_style) and the sun */
    void *p_rt_blade;                /* terrain fragment programs: backlit blades (characters stream) */
    void *p_mvp, *p_mv, *p_campos;   /* the sea's vertex program: its combined matrices and camera position */
};
static struct prog progs[MAXPROG];
static int nprogs;
static int cur_vp = -1, cur_fp = -1;
static int vertex_profile = -1, fragment_profile = -1, vp_on, fp_on;

#define PCACHE 4096
static struct { void *param; short prog; signed char slot; } pcache[PCACHE];

static void (__cdecl *real_cgGLBindProgram)(void *);
static void (__cdecl *real_cgGLEnableProfile)(int);
static void (__cdecl *real_cgGLDisableProfile)(int);
static const char *(__cdecl *p_cgGetProfileString)(int);
static const char *(__cdecl *p_cgGetParameterName)(void *);
static void *(__cdecl *p_cgGetParameterProgram)(void *);
static void *(__cdecl *p_cgGetNamedParameter)(void *, const char *);

/* Our extra shader uniforms. The engine walks every program's parameters on each draw and
   writes a log line for any it has no handler for, which is very slow; so it is not shown ours. */
#define MAX_RT_PARAMS 1024
static void *rt_param_handles[MAX_RT_PARAMS];
static float water_style;   /* stylised sea in water.cg: depth colour, shore foam, sun glints (0 = off) */
static int n_rt_param_handles;
static void *(__cdecl *real_cgGetFirstParameter)(void *, int);
static void *(__cdecl *real_cgGetNextParameter)(void *);

static int is_rt_param(void *p)
{
    int i;
    for (i = 0; i < n_rt_param_handles; i++)
        if (rt_param_handles[i] == p) return 1;
    return 0;
}

static void *__cdecl my_cgGetFirstParameter(void *prog, int name_space)
{
    void *p = real_cgGetFirstParameter(prog, name_space);
    while (p && is_rt_param(p)) p = real_cgGetNextParameter(p);
    return p;
}

static void *__cdecl my_cgGetNextParameter(void *param)
{
    void *p = real_cgGetNextParameter(param);
    while (p && is_rt_param(p)) p = real_cgGetNextParameter(p);
    return p;
}

static int find_prog(void *h)
{
    int i;
    for (i = 0; i < nprogs; i++)
        if (progs[i].h == h) return i;
    return -1;
}

static const char *basename_of(const char *s)
{
    const char *b = s;
    for (; *s; s++)
        if (*s == '\\' || *s == '/') b = s + 1;
    return b;
}

/* Fill a program slot from the loader's record of it: classify it by its entry point and
   look up its matrices and our uniforms by name. Used when the program is first seen and
   again when a Cg reload replaced it (everything tracked about it starts over). */
static void setup_program(struct prog *p, const struct cg_record *r)
{
    void *h = r->cur;
    const char *file = r->file, *entry = r->entry;
    int profile = r->profile;
    {
        const char *ps = r->profile_name;
        memset(p, 0, sizeof *p);
        p->h = h;
        p->orig_h = r->orig;
        strncpy(p->file, file ? basename_of(file) : "", sizeof p->file - 1);
        strncpy(p->entry, entry ? entry : "", sizeof p->entry - 1);
        strncpy(p->profile, ps ? ps : "", sizeof p->profile - 1);
        p->is_vertex = strstr(p->profile, "vp") != NULL || strstr(p->profile, "glslv") != NULL;
        p->profile_id = profile;
        p->landscape = p->is_vertex && (!strncmp(p->entry, "Landscape", 9) || !strncmp(p->entry, "HeightMap", 9));
        if (p->is_vertex) vertex_profile = profile; else fragment_profile = profile;
        p->caster = p->is_vertex && strstr(p->entry, "Lighting") && !strstr(p->entry, "Water") &&
                    !strstr(p->entry, "Skinned");
        p->unlit_tex = p->is_vertex && !_stricmp(p->file, "FixedFunction.cg") && strstr(p->entry, "Tex") &&
                       !strstr(p->entry, "Lit") && !strstr(p->entry, "Lighting");
        p->water = p->is_vertex && !strcmp(p->entry, "WaterVertexMain");
        p->effect = !_stricmp(p->file, "PostProcess.cg");
        if (p->water && p_cgGetNamedParameter) {
            p->p_mvp = p_cgGetNamedParameter(h, "modelViewProjection");
            p->p_mv = p_cgGetNamedParameter(h, "modelView");
            p->p_campos = p_cgGetNamedParameter(h, "cameraPos");
        }
        if (p->is_vertex && p_cgGetNamedParameter) {
            p->p_view = p_cgGetNamedParameter(h, "view");
            p->p_proj = p_cgGetNamedParameter(h, "projection");
        }
        p->sun_lit = p->is_vertex && !strstr(p->entry, "Lighting") &&
                     (strstr(p->entry, "Directional") || !strncmp(p->entry, "Landscape", 9) ||
                      !strncmp(p->entry, "HeightMap", 9));
        if (!p->is_vertex && p_cgGetNamedParameter) {
            p->p_rt_params = p_cgGetNamedParameter(h, "rtParams");
            p->p_rt_mask = p_cgGetNamedParameter(h, "rtMask");
            p->p_rt_params2 = p_cgGetNamedParameter(h, "rtParams2");
            p->p_rt_bounce = p_cgGetNamedParameter(h, "rtBounce");
            p->p_rt_refl = p_cgGetNamedParameter(h, "rtRefl");
            p->p_rt_normal = p_cgGetNamedParameter(h, "rtNormal");
            p->p_rt_params3 = p_cgGetNamedParameter(h, "rtParams3");
            p->p_rt_char = p_cgGetNamedParameter(h, "rtChar");
            p->p_rt_char2 = p_cgGetNamedParameter(h, "rtChar2");
            p->p_rt_relief = p_cgGetNamedParameter(h, "rtRelief");
            p->p_rt_wind = p_cgGetNamedParameter(h, "rtWind");
            p->p_rt_smoke[0] = p_cgGetNamedParameter(h, "rtSmoke");
            p->p_rt_smoke[1] = p_cgGetNamedParameter(h, "rtSmokeSun");
            p->p_rt_smoke[2] = p_cgGetNamedParameter(h, "rtSmokeSunCol");
            p->p_rt_smoke[3] = p_cgGetNamedParameter(h, "rtSmokeAmb");
            p->p_rt_smoke[4] = p_cgGetNamedParameter(h, "rtSmokeFx");
            p->p_rt_smoke[5] = p_cgGetNamedParameter(h, "rtSmokeFxCol");
            if (!p->p_rt_smoke[1] || !p->p_rt_smoke[2] || !p->p_rt_smoke[3]) p->p_rt_smoke[0] = NULL;
            p->p_rt_water = p_cgGetNamedParameter(h, "rtWater");
            p->p_rt_blade = p_cgGetNamedParameter(h, "rtBlade");
        }
        if (p_cgGetNamedParameter) {
            p->p_rt_shell = p_cgGetNamedParameter(h, "rtShell");
        }
    }
}

/* Hide this program's extra uniforms from the engine's parameter walks. */
static void add_rt_params(const struct prog *p)
{
    void *h[20];
    int i;
    h[8] = p->p_rt_char; h[9] = p->p_rt_char2;
    h[0] = p->p_rt_normal; h[1] = p->p_rt_params3; h[2] = p->p_rt_refl; h[3] = p->p_rt_bounce;
    h[4] = p->p_rt_params2; h[5] = p->p_rt_params; h[6] = p->p_rt_mask; h[7] = p->p_rt_shell;
    h[10] = p->p_rt_relief; h[11] = p->p_rt_wind; h[12] = p->p_rt_water; h[13] = p->p_rt_blade;
    h[14] = p->p_rt_smoke[0]; h[15] = p->p_rt_smoke[1]; h[16] = p->p_rt_smoke[2]; h[17] = p->p_rt_smoke[3];
    h[18] = p->p_rt_smoke[4]; h[19] = p->p_rt_smoke[5];
    for (i = 0; i < 20; i++)
        if (h[i] && n_rt_param_handles < MAX_RT_PARAMS) rt_param_handles[n_rt_param_handles++] = h[i];
}

/* A program the engine created (now, or before this core was loaded). */
static void register_program(const struct cg_record *r)
{
    const char *const *args = r->has_args ? r->args : NULL;
    struct prog *p;
    if (!r->cur || nprogs >= MAXPROG) return;
    p = &progs[nprogs++];
    setup_program(p, r);
    add_rt_params(p);
    logmsg("cg program %d: %s %s (%s)%s%s%s", nprogs - 1, p->file, p->entry, p->profile,
           args && args[0] ? " +args" : "", p->p_rt_params ? " [rt uniforms]" : "", p->p_rt_shell ? " [shells]" : "");
    if (args) {
        int i;
        for (i = 0; args[i] && i < 16; i++) logmsg("    arg %s", args[i]);
    }
}

/* A Cg reload replaced some programs: take up the new handles. */
static void programs_replaced(void)
{
    int i, changed = 0;
    u32 k;
    for (i = 0; i < nprogs; i++) {
        void *cur = L->cg_current(progs[i].orig_h);
        if (!cur || cur == progs[i].h) continue;
        for (k = 0; k < L->nprog; k++)
            if (L->prog[k].orig == progs[i].orig_h) { setup_program(&progs[i], &L->prog[k]); changed++; break; }
    }
    n_rt_param_handles = 0;
    for (i = 0; i < nprogs; i++) add_rt_params(&progs[i]);
    memset(pcache, 0, sizeof pcache);
    logmsg("core: %d programs taken over from the Cg reload, %d hidden uniforms", changed, n_rt_param_handles);
}

/* ---- free camera ------------------------------------------------------------------
   rt.py freecam replaces the view matrix of the scene cameras (the ones that draw the
   landscape: the main view and the picture-in-picture view; not the shadow pass, not the
   HUD's models) with one built from a position, yaw and pitch, at the moment the engine
   gives it to a program, so the tracer and the shaders all see the same camera. What the
   engine passes in eye space (light directions and positions), view-projection products,
   the sky's view and the sea's matrices are corrected to match. The engine still culls
   with its own camera, so things outside its view are missing. The state lives in the
   loader and survives a core reload. */
struct freecam {
    u32 on;
    float eye[3], yaw, pitch;           /* degrees; yaw 0 looks along -z, pitch up is positive */
    float view[16];                     /* built from the pose */
    float game[4][16]; u32 ngame;       /* the engine's scene views seen lately, newest first */
    float proj[16]; u32 have_proj;      /* the projection of a scene camera */
    float fix[16];                      /* game[0]^-1 @ view: eye space of the game -> ours */
    u32 overrides;
};
static struct freecam *FC;

static void mat_mul(const float *a, const float *b, float *out)     /* out = a @ b (row-major) */
{
    float t[16];
    int r, c, k;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            float s = 0;
            for (k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            t[r * 4 + c] = s;
        }
    memcpy(out, t, sizeof t);
}

/* Inverse of a view matrix (rotation and translation; row vectors). */
static void view_inverse(const float *m, float *out)
{
    int r, c;
    memset(out, 0, 16 * sizeof(float));
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) out[r * 4 + c] = m[c * 4 + r];
    for (c = 0; c < 3; c++) out[12 + c] = -(m[12] * out[0 * 4 + c] + m[13] * out[1 * 4 + c] + m[14] * out[2 * 4 + c]);
    out[15] = 1;
}

static void fc_build(void)
{
    double y = FC->yaw * 3.14159265358979 / 180.0, p = FC->pitch * 3.14159265358979 / 180.0;
    float f[3], r[3], u[3], b[3], len;
    float *m = FC->view;
    f[0] = (float)(sin(y) * cos(p)); f[1] = (float)sin(p); f[2] = (float)(-cos(y) * cos(p));
    r[0] = -f[2]; r[1] = 0; r[2] = f[0];
    len = (float)sqrt(r[0] * r[0] + r[2] * r[2]);
    if (len < 1e-6f) { r[0] = 1; r[2] = 0; len = 1; }
    r[0] /= len; r[2] /= len;
    u[0] = r[1] * f[2] - r[2] * f[1]; u[1] = r[2] * f[0] - r[0] * f[2]; u[2] = r[0] * f[1] - r[1] * f[0];
    b[0] = -f[0]; b[1] = -f[1]; b[2] = -f[2];
    m[0] = r[0]; m[1] = u[0]; m[2] = b[0]; m[3] = 0;
    m[4] = r[1]; m[5] = u[1]; m[6] = b[1]; m[7] = 0;
    m[8] = r[2]; m[9] = u[2]; m[10] = b[2]; m[11] = 0;
    m[12] = -(FC->eye[0] * r[0] + FC->eye[1] * r[1] + FC->eye[2] * r[2]);
    m[13] = -(FC->eye[0] * u[0] + FC->eye[1] * u[1] + FC->eye[2] * u[2]);
    m[14] = -(FC->eye[0] * b[0] + FC->eye[1] * b[1] + FC->eye[2] * b[2]);
    m[15] = 1;
    if (FC->ngame) {
        float inv[16];
        view_inverse(FC->game[0], inv);
        mat_mul(inv, FC->view, FC->fix);
    }
}

/* The pose of a view matrix: eye = -(t @ R^T), forward = -(third column). */
static void fc_pose_of(const float *m, float *eye, float *yaw, float *pitch)
{
    float fx = -m[2], fy = -m[6], fz = -m[10];
    int k;
    for (k = 0; k < 3; k++) eye[k] = -(m[12] * m[k * 4 + 0] + m[13] * m[k * 4 + 1] + m[14] * m[k * 4 + 2]);
    *yaw = (float)(atan2(fx, -fz) * 180.0 / 3.14159265358979);
    *pitch = (float)(asin(fy < -1 ? -1 : fy > 1 ? 1 : fy) * 180.0 / 3.14159265358979);
}

static void fc_note_game_view(const float *m)
{
    u32 i;
    for (i = 0; i < FC->ngame; i++)
        if (!memcmp(FC->game[i], m, 64)) break;
    if (i == 0 && FC->ngame) return;
    if (i == FC->ngame) i = FC->ngame < 4 ? FC->ngame++ : 3;
    memmove(FC->game[1], FC->game[0], (size_t)i * 64);
    memcpy(FC->game[0], m, 64);
    if (FC->on) fc_build();             /* fix depends on game[0] */
}

/* Replace a matrix the engine is giving a program, if it is a scene camera's. */
static int fc_matrix(const struct prog *p, int slot, const float *m, float *out)
{
    u32 i;
    if (p->caster) return 0;
    if (slot == 1) {
        for (i = 0; i < FC->ngame; i++)
            if (!memcmp(FC->game[i], m, 64)) break;
        if (i < FC->ngame) { memcpy(out, FC->view, 64); return 1; }
        for (i = 0; i < FC->ngame; i++) {           /* the sky: the same rotation, its own eye */
            const float *g = FC->game[i];
            if (m[0] == g[0] && m[1] == g[1] && m[2] == g[2] && m[4] == g[4] && m[5] == g[5] && m[6] == g[6] &&
                m[8] == g[8] && m[9] == g[9] && m[10] == g[10]) {
                float eye[3], y, pt;
                int k;
                fc_pose_of(m, eye, &y, &pt);
                memcpy(out, FC->view, 64);
                for (k = 0; k < 3; k++)
                    out[12 + k] = -(eye[0] * out[0 * 4 + k] + eye[1] * out[1 * 4 + k] + eye[2] * out[2 * 4 + k]);
                return 1;
            }
        }
        return 0;
    }
    if (slot == 12 && FC->have_proj) {             /* viewProjection */
        for (i = 0; i < FC->ngame; i++) {
            float g[16];
            int k;
            float worst = 0, scale = 1e-6f;
            mat_mul(FC->game[i], FC->proj, g);
            for (k = 0; k < 16; k++) {
                float d = (float)fabs(g[k] - m[k]);
                if (d > worst) worst = d;
                if (fabs(g[k]) > scale) scale = (float)fabs(g[k]);
            }
            if (worst <= scale * 1e-4f) { mat_mul(FC->view, FC->proj, out); return 1; }
        }
    }
    return 0;
}

/* Light directions (w = 0) and positions (w = 1) in the engine's eye space -> ours. */
static void fc_vector(float *v, float w)
{
    float r[3];
    int c;
    for (c = 0; c < 3; c++) r[c] = v[0] * FC->fix[c] + v[1] * FC->fix[4 + c] + v[2] * FC->fix[8 + c] + w * FC->fix[12 + c];
    memcpy(v, r, sizeof r);
}

static void (__cdecl *real_cgGLSetParameter3f)(void *, float, float, float);
static void (__cdecl *real_cgGLSetMatrixParameterfr)(void *, const float *);

/* The sea's vertex program gets combined matrices and a camera position instead of a view. */
static void fc_water(struct prog *vp)
{
    float mv[16], mvp[16];
    if (!FC->on || !FC->have_proj || !(vp->have & 1)) return;
    mat_mul(vp->model, FC->view, mv);
    mat_mul(mv, FC->proj, mvp);
    if (vp->p_mv) real_cgGLSetMatrixParameterfr(vp->p_mv, mv);
    if (vp->p_mvp) real_cgGLSetMatrixParameterfr(vp->p_mvp, mvp);
    if (vp->p_campos) real_cgGLSetParameter3f(vp->p_campos, FC->eye[0], FC->eye[1], FC->eye[2]);
    (*(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters)(vp->h);
}

/* slot: 0 model, 1 view, 2 projection, 3 shadowMatrix, 4 globalLightDir, 5 texture0,
   6 texTransform, 7 the sun's colour, 8 the ambient colour, -1 other */
static int param_slot(void *param, int *prog_out)
{
    u32 hsh = ((u32)param >> 2) * 2654435761u % PCACHE;
    int n;
    for (n = 0; n < PCACHE; n++, hsh = (hsh + 1) % PCACHE) {
        if (pcache[hsh].param == param) {
            *prog_out = pcache[hsh].prog;
            return pcache[hsh].slot;
        }
        if (!pcache[hsh].param) break;
    }
    if (n == PCACHE) return -1;
    {
        const char *name = p_cgGetParameterName ? p_cgGetParameterName(param) : NULL;
        void *ph = p_cgGetParameterProgram ? p_cgGetParameterProgram(param) : NULL;
        int slot = -1;
        if (name) {
            if (!strcmp(name, "model")) slot = 0;
            else if (!strcmp(name, "view")) slot = 1;
            else if (!strcmp(name, "projection")) slot = 2;
            else if (!strcmp(name, "shadowMatrix")) slot = 3;
            else if (!strcmp(name, "globalLightDir")) slot = 4;
            else if (!strcmp(name, "texture0")) slot = 5;
            else if (!strcmp(name, "texTransform")) slot = 6;
            else if (!strcmp(name, "materialMatrix")) slot = 9;
            else if (!strcmp(name, "globalDiffuse") || !strcmp(name, "lightDiffuseCol")) slot = 7;
            else if (!strcmp(name, "globalAmbient") || !strcmp(name, "lightAmbientCol")) slot = 8;
            else if (!strcmp(name, "lightDirection")) slot = 10;
            else if (!strcmp(name, "lightPosition")) slot = 11;
            else if (!strcmp(name, "viewProjection")) slot = 12;
        }
        pcache[hsh].param = param;
        pcache[hsh].prog = (short)find_prog(ph);
        pcache[hsh].slot = (signed char)slot;
        *prog_out = pcache[hsh].prog;
        return slot;
    }
}

static void __cdecl my_cgGLSetMatrixParameterfr(void *param, const float *m)
{
    int pi = -1, slot = param_slot(param, &pi);
    float fcm[16];
    if (pi >= 0 && m && progs[pi].landscape) {      /* the landscape's camera is a scene camera */
        if (slot == 1) fc_note_game_view(m);
        if (slot == 2 && memcmp(FC->proj, m, 64)) { memcpy(FC->proj, m, 64); FC->have_proj = 1; }
    }
    if (FC->on && pi >= 0 && m && fc_matrix(&progs[pi], slot, m, fcm)) { m = fcm; FC->overrides++; }
    if (slot >= 0 && slot <= 3 && pi >= 0 && m) {
        struct prog *p = &progs[pi];
        float *dst = slot == 0 ? p->model : slot == 1 ? p->view : slot == 2 ? p->proj : p->shadow;
        memcpy(dst, m, 16 * sizeof(float));
        p->have |= 1u << slot;
    }
    if (slot == 9 && pi >= 0 && m) {
        memcpy(progs[pi].mat, m, 16 * sizeof(float));
        progs[pi].have |= 1u << 9;
    }
    if (slot == 6 && pi >= 0 && m) {
        memcpy(progs[pi].texxf, m, 16 * sizeof(float));
        progs[pi].have |= 1u << 6;
    }
    real_cgGLSetMatrixParameterfr(param, m);
}

static int fc_check_left;            /* log the corrected sun direction this many more times */

static void __cdecl my_cgGLSetParameter3f(void *param, float x, float y, float z)
{
    int pi = -1, slot = param_slot(param, &pi);
    if (FC->on && FC->ngame && pi >= 0 && (slot == 4 || slot == 10 || slot == 11) && !progs[pi].caster) {
        float v[3]; v[0] = x; v[1] = y; v[2] = z;
        fc_vector(v, slot == 11 ? 1.0f : 0.0f);
        x = v[0]; y = v[1]; z = v[2];
        if (slot == 4 && fc_check_left > 0) {   /* compare with the tracer's world sun seen through our view */
            const float *s = rt_stats.sun;
            float e[3], le, lv, d;
            int c;
            fc_check_left--;
            for (c = 0; c < 3; c++) e[c] = s[0] * FC->view[c] + s[1] * FC->view[4 + c] + s[2] * FC->view[8 + c];
            le = (float)sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]); lv = (float)sqrt(x * x + y * y + z * z);
            d = le > 0 && lv > 0 ? (e[0] * x + e[1] * y + e[2] * z) / (le * lv) : 0;
            logmsg("free camera check: light direction given %.3f %.3f %.3f, world sun through the free view %.3f %.3f %.3f, cosine %.4f",
                   x / lv, y / lv, z / lv, e[0] / le, e[1] / le, e[2] / le, d);
        }
    }
    if (slot == 4 && pi >= 0) {
        progs[pi].light[0] = x; progs[pi].light[1] = y; progs[pi].light[2] = z;
        progs[pi].have |= 1u << 4;
    }
    if ((slot == 7 || slot == 8) && pi >= 0) {
        float *dst = slot == 7 ? progs[pi].dif : progs[pi].amb;
        dst[0] = x; dst[1] = y; dst[2] = z;
        progs[pi].have |= 1u << slot;
    }
    real_cgGLSetParameter3f(param, x, y, z);
}

static void (__cdecl *real_cgGLSetTextureParameter)(void *, GLuint);
static void (__cdecl *real_cgGLSetParameter4f)(void *, float, float, float, float);

/* The engine gives its float3 colour uniforms four values. */
static void __cdecl my_cgGLSetParameter4f(void *param, float x, float y, float z, float w)
{
    int pi = -1, slot = param_slot(param, &pi);
    if (FC->on && FC->ngame && pi >= 0 && (slot == 4 || slot == 10 || slot == 11) && !progs[pi].caster) {
        float v[3]; v[0] = x; v[1] = y; v[2] = z;
        fc_vector(v, slot == 11 ? 1.0f : 0.0f);
        x = v[0]; y = v[1]; z = v[2];
    }
    if ((slot == 7 || slot == 8) && pi >= 0) {
        float *dst = slot == 7 ? progs[pi].dif : progs[pi].amb;
        dst[0] = x; dst[1] = y; dst[2] = z;
        progs[pi].have |= 1u << slot;
    }
    real_cgGLSetParameter4f(param, x, y, z, w);
}

static void __cdecl my_cgGLSetTextureParameter(void *param, GLuint tex)
{
    int pi = -1, slot = param_slot(param, &pi);
    if (slot == 5 && pi >= 0) progs[pi].tex0 = tex;
    real_cgGLSetTextureParameter(param, tex);
}

static void __cdecl my_cgGLBindProgram(void *h)
{
    int i = find_prog(h);
    if (i >= 0) {
        if (progs[i].is_vertex) cur_vp = i; else cur_fp = i;
    }
    real_cgGLBindProgram(h);
}

static void __cdecl my_cgGLEnableProfile(int profile)
{
    if (profile == vertex_profile) vp_on = 1;
    if (profile == fragment_profile) fp_on = 1;
    real_cgGLEnableProfile(profile);
}

static void __cdecl my_cgGLDisableProfile(int profile)
{
    if (profile == vertex_profile) vp_on = 0;
    if (profile == fragment_profile) fp_on = 0;
    real_cgGLDisableProfile(profile);
}

/* ---- GL state we follow instead of querying it on every draw ------------------------ */
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_TEXTURE0 0x84C0
struct arr_state { u32 size, stride, buf; GLenum type; const void *ptr; };
static struct {
    u32 array_buf, elem_buf, client_unit;
    struct arr_state vtx, tc0, nrm;
    int vtx_on, tc0_on, nrm_on, alpha_test, blend;
    float alpha_ref;
    struct arr_state col; int col_on;       /* vertex colours (the effects' fade) */
    GLenum blend_src, blend_dst;
} gs;

static void (APIENTRY *real_glVertexPointer)(GLint, GLenum, GLsizei, const void *);
static void (APIENTRY *real_glTexCoordPointer)(GLint, GLenum, GLsizei, const void *);
static void (APIENTRY *real_glNormalPointer)(GLenum, GLsizei, const void *);
static void (APIENTRY *real_glEnableClientState)(GLenum);
static void (APIENTRY *real_glDisableClientState)(GLenum);
static void (APIENTRY *real_glEnable)(GLenum);
static void (APIENTRY *real_glDisable)(GLenum);
static void (APIENTRY *real_glAlphaFunc)(GLenum, GLclampf);
static void (APIENTRY *real_glPopAttrib)(void);
static void (APIENTRY *real_glPopClientAttrib)(void);
static void (APIENTRY *real_glBindBuffer)(GLenum, GLuint);
static void (APIENTRY *real_glClientActiveTexture)(GLenum);
static void (APIENTRY *real_glBindFramebuffer)(GLenum, GLuint);
static void (APIENTRY *real_glBindFramebufferEXT)(GLenum, GLuint);

static void APIENTRY my_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    gs.vtx.size = (u32)size; gs.vtx.type = type; gs.vtx.stride = (u32)stride; gs.vtx.ptr = ptr; gs.vtx.buf = gs.array_buf;
    real_glVertexPointer(size, type, stride, ptr);
}

static void APIENTRY my_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    if (gs.client_unit == 0) {
        gs.tc0.size = (u32)size; gs.tc0.type = type; gs.tc0.stride = (u32)stride; gs.tc0.ptr = ptr; gs.tc0.buf = gs.array_buf;
    }
    real_glTexCoordPointer(size, type, stride, ptr);
}

static void (APIENTRY *real_glColorPointer)(GLint, GLenum, GLsizei, const void *);
static void (APIENTRY *real_glBlendFunc)(GLenum, GLenum);

static void APIENTRY my_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    gs.col.size = (u32)size; gs.col.type = type; gs.col.stride = (u32)stride; gs.col.ptr = ptr; gs.col.buf = gs.array_buf;
    real_glColorPointer(size, type, stride, ptr);
}

static void APIENTRY my_glBlendFunc(GLenum src, GLenum dst)
{
    gs.blend_src = src; gs.blend_dst = dst;
    real_glBlendFunc(src, dst);
}

static void APIENTRY my_glNormalPointer(GLenum type, GLsizei stride, const void *ptr)
{
    gs.nrm.size = 3; gs.nrm.type = type; gs.nrm.stride = (u32)stride; gs.nrm.ptr = ptr; gs.nrm.buf = gs.array_buf;
    real_glNormalPointer(type, stride, ptr);
}

static void APIENTRY my_glEnableClientState(GLenum cap)
{
    if (cap == GL_VERTEX_ARRAY) gs.vtx_on = 1;
    else if (cap == GL_NORMAL_ARRAY) gs.nrm_on = 1;
    else if (cap == GL_COLOR_ARRAY) gs.col_on = 1;
    else if (cap == GL_TEXTURE_COORD_ARRAY && gs.client_unit == 0) gs.tc0_on = 1;
    real_glEnableClientState(cap);
}

static void APIENTRY my_glDisableClientState(GLenum cap)
{
    if (cap == GL_VERTEX_ARRAY) gs.vtx_on = 0;
    else if (cap == GL_NORMAL_ARRAY) gs.nrm_on = 0;
    else if (cap == GL_COLOR_ARRAY) gs.col_on = 0;
    else if (cap == GL_TEXTURE_COORD_ARRAY && gs.client_unit == 0) gs.tc0_on = 0;
    real_glDisableClientState(cap);
}

static void APIENTRY my_glEnable(GLenum cap)
{
    if (cap == GL_ALPHA_TEST) gs.alpha_test = 1;
    else if (cap == GL_BLEND) gs.blend = 1;
    real_glEnable(cap);
}

static void APIENTRY my_glDisable(GLenum cap)
{
    if (cap == GL_ALPHA_TEST) gs.alpha_test = 0;
    else if (cap == GL_BLEND) gs.blend = 0;
    real_glDisable(cap);
}

static void APIENTRY my_glAlphaFunc(GLenum func, GLclampf ref)
{
    gs.alpha_ref = ref;
    real_glAlphaFunc(func, ref);
}

static void APIENTRY my_glPopAttrib(void)
{
    real_glPopAttrib();
    gs.alpha_test = glIsEnabled(GL_ALPHA_TEST);     /* whatever was pushed is back: ask */
    gs.blend = glIsEnabled(GL_BLEND);
    glGetFloatv(GL_ALPHA_TEST_REF, &gs.alpha_ref);
    {
        GLint v = 0;
        glGetIntegerv(GL_BLEND_SRC, &v); gs.blend_src = (GLenum)v;
        glGetIntegerv(GL_BLEND_DST, &v); gs.blend_dst = (GLenum)v;
    }
}

static void read_array_state(void)
{
    GLint v = 0;
    void *ptr = NULL;
    gs.vtx_on = glIsEnabled(GL_VERTEX_ARRAY);
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &v); gs.vtx.size = (u32)v;
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &v); gs.vtx.type = (GLenum)v;
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &v); gs.vtx.stride = (u32)v;
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &ptr); gs.vtx.ptr = ptr;
    glGetIntegerv(0x8896, &v); gs.vtx.buf = (u32)v;                 /* GL_VERTEX_ARRAY_BUFFER_BINDING */
    gs.nrm_on = glIsEnabled(GL_NORMAL_ARRAY);
    glGetIntegerv(GL_NORMAL_ARRAY_TYPE, &v); gs.nrm.type = (GLenum)v;
    glGetIntegerv(GL_NORMAL_ARRAY_STRIDE, &v); gs.nrm.stride = (u32)v;
    glGetPointerv(GL_NORMAL_ARRAY_POINTER, &ptr); gs.nrm.ptr = ptr;
    glGetIntegerv(0x8897, &v); gs.nrm.buf = (u32)v;                 /* GL_NORMAL_ARRAY_BUFFER_BINDING */
    gs.nrm.size = 3;
    gs.col_on = glIsEnabled(GL_COLOR_ARRAY);
    glGetIntegerv(GL_COLOR_ARRAY_SIZE, &v); gs.col.size = (u32)v;
    glGetIntegerv(GL_COLOR_ARRAY_TYPE, &v); gs.col.type = (GLenum)v;
    glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &v); gs.col.stride = (u32)v;
    glGetPointerv(GL_COLOR_ARRAY_POINTER, &ptr); gs.col.ptr = ptr;
    glGetIntegerv(0x8898, &v); gs.col.buf = (u32)v;                 /* GL_COLOR_ARRAY_BUFFER_BINDING */
    glGetIntegerv(0x8894, &v); gs.array_buf = (u32)v;               /* GL_ARRAY_BUFFER_BINDING */
    glGetIntegerv(0x8895, &v); gs.elem_buf = (u32)v;                /* GL_ELEMENT_ARRAY_BUFFER_BINDING */
    glGetIntegerv(0x84E1, &v); gs.client_unit = (u32)v - GL_TEXTURE0;   /* GL_CLIENT_ACTIVE_TEXTURE */
    if (gs.client_unit == 0) {
        gs.tc0_on = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &v); gs.tc0.size = (u32)v;
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &v); gs.tc0.type = (GLenum)v;
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &v); gs.tc0.stride = (u32)v;
        glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &ptr); gs.tc0.ptr = ptr;
        glGetIntegerv(0x889A, &v); gs.tc0.buf = (u32)v;             /* GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING */
    }
}

static void APIENTRY my_glPopClientAttrib(void)
{
    real_glPopClientAttrib();
    read_array_state();
}

static void APIENTRY my_glBindBuffer(GLenum target, GLuint id)
{
    if (target == GL_ARRAY_BUFFER) gs.array_buf = id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) gs.elem_buf = id;
    real_glBindBuffer(target, id);
}

static void APIENTRY my_glClientActiveTexture(GLenum unit)
{
    gs.client_unit = unit - GL_TEXTURE0;
    real_glClientActiveTexture(unit);
}

/* Framebuffer binds, for the film post-processing (post.c), which runs before the HUD, and
   for hide_hud (draw_fbo: the framebuffer draws go to). */
static GLuint draw_fbo;

static void track_draw_fbo(GLenum target, GLuint id)
{
    if (target == 0x8D40 /* GL_FRAMEBUFFER */ || target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */) draw_fbo = id;
}

static void APIENTRY my_glBindFramebuffer(GLenum target, GLuint id)
{
    post_bind_fbo(target, id);
    track_draw_fbo(target, id);
    real_glBindFramebuffer(target, id);
}

static void APIENTRY my_glBindFramebufferEXT(GLenum target, GLuint id)
{
    post_bind_fbo(target, id);
    track_draw_fbo(target, id);
    real_glBindFramebufferEXT(target, id);
}

void rt_buffer_deleted(GLuint id)       /* deleting a bound buffer unbinds it */
{
    if (!id) return;
    if (gs.array_buf == id) gs.array_buf = 0;
    if (gs.elem_buf == id) gs.elem_buf = 0;
}

/* ---- draw call capture ---------------------------------------------------------- */
static void (APIENTRY *real_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (APIENTRY *real_glDrawArrays)(GLenum, GLint, GLsizei);
static void (APIENTRY *real_glDrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void (APIENTRY *real_glBegin)(GLenum);
static void (APIENTRY *real_glCallList)(GLuint);
static PROC (WINAPI *real_wglGetProcAddress)(LPCSTR);
static BOOL (WINAPI *real_SwapBuffers)(HDC);

static void (APIENTRY *p_glGetBufferSubData)(GLenum, int, int, void *);
static void (APIENTRY *p_glBindBuffer)(GLenum, GLuint);
static void (APIENTRY *p_glGetBufferParameteriv)(GLenum, GLenum, GLint *);
static void (APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);

#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_VERTEX_ARRAY_BUFFER_BINDING 0x8896
#define GL_NORMAL_ARRAY_BUFFER_BINDING 0x8897
#define GL_BUFFER_SIZE 0x8764
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_BGR_FMT 0x80E0

static void (APIENTRY *p_glActiveTexture)(GLenum);
#define GL_ACTIVE_TEXTURE 0x84E0

static FILE *capf;
static int capturing;
static u32 stat_cur[8];
static int ext_ready;

static void resolve_ext(void)
{
    if (ext_ready || !real_wglGetProcAddress) return;
    p_glGetBufferSubData = (void *)real_wglGetProcAddress("glGetBufferSubData");
    p_glBindBuffer = (void *)real_wglGetProcAddress("glBindBuffer");
    p_glGetBufferParameteriv = (void *)real_wglGetProcAddress("glGetBufferParameteriv");
    p_glBindFramebuffer = (void *)real_wglGetProcAddress("glBindFramebuffer");
    p_glActiveTexture = (void *)real_wglGetProcAddress("glActiveTexture");
    ext_ready = 1;
}

static u32 type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}

/* Copy `bytes` at `ptr` (a client pointer, or an offset into `buffer` if nonzero). */
static int fetch(GLenum target, u32 buffer, const void *ptr, u32 bytes, void *dst)
{
    if (!bytes) return 1;
    if (buffer) {
        GLint prev = 0, size = 0;
        GLenum binding = target == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : GL_ELEMENT_ARRAY_BUFFER_BINDING;
        if (!p_glGetBufferSubData || !p_glBindBuffer) return 0;
        glGetIntegerv(binding, &prev);
        p_glBindBuffer(target, buffer);
        if (p_glGetBufferParameteriv) p_glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        if (size && (u32)ptr + bytes > (u32)size) {
            p_glBindBuffer(target, prev);
            return 0;
        }
        p_glGetBufferSubData(target, (int)ptr, (int)bytes, dst);
        p_glBindBuffer(target, prev);
        return 1;
    }
    if (!ptr || IsBadReadPtr(ptr, bytes)) return 0;
    memcpy(dst, ptr, bytes);
    return 1;
}

static void rec_draw(u32 kind, GLenum mode, u32 first, u32 count, GLenum itype, const void *indices)
{
    struct draw_rec r;
    GLint v[4];
    GLboolean b[4];
    void *vptr = NULL, *nptr = NULL;
    u32 *idx = NULL;
    u8 *vdata = NULL;
    u32 i, maxi = 0, stride;

    resolve_ext();
    memset(&r, 0, sizeof r);
    r.magic = 0x57415244;   /* 'DRAW' */
    r.kind = kind; r.mode = mode; r.first = first; r.count = count; r.index_type = itype;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, v); r.fbo = v[0];
    glGetIntegerv(GL_VIEWPORT, r.viewport);
    r.depth_test = glIsEnabled(GL_DEPTH_TEST); r.blend = glIsEnabled(GL_BLEND);
    r.cull = glIsEnabled(GL_CULL_FACE); r.alpha_test = glIsEnabled(GL_ALPHA_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, b); r.depth_mask = b[0];
    glGetBooleanv(GL_COLOR_WRITEMASK, b); r.color_mask = b[0] | b[1] << 1 | b[2] << 2 | b[3] << 3;
    glGetIntegerv(GL_DEPTH_FUNC, v); r.depth_func = v[0];
    glGetIntegerv(GL_CULL_FACE_MODE, v); r.cull_mode = v[0];
    glGetIntegerv(GL_FRONT_FACE, v); r.front_face = v[0];
    glGetIntegerv(GL_BLEND_SRC, v); r.blend_src = v[0];
    glGetIntegerv(GL_BLEND_DST, v); r.blend_dst = v[0];
    glGetIntegerv(GL_TEXTURE_BINDING_2D, v); r.tex0 = v[0];
    glGetFloatv(GL_MODELVIEW_MATRIX, r.gl_modelview);
    glGetFloatv(GL_PROJECTION_MATRIX, r.gl_projection);
    r.vp_index = (u32)cur_vp; r.fp_index = (u32)cur_fp;
    if (cur_vp >= 0) {
        struct prog *p = &progs[cur_vp];
        r.have = p->have;
        memcpy(r.model, p->model, sizeof r.model); memcpy(r.view, p->view, sizeof r.view);
        memcpy(r.proj, p->proj, sizeof r.proj); memcpy(r.shadow, p->shadow, sizeof r.shadow);
        memcpy(r.light, p->light, sizeof r.light);
    }

    if (kind <= 2 && glIsEnabled(GL_VERTEX_ARRAY)) {
        glGetIntegerv(GL_VERTEX_ARRAY_SIZE, v); r.v_size = v[0];
        glGetIntegerv(GL_VERTEX_ARRAY_TYPE, v); r.v_type = v[0];
        glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, v); r.v_stride = v[0];
        glGetPointerv(GL_VERTEX_ARRAY_POINTER, &vptr); r.v_ptr = (u32)vptr;
        glGetIntegerv(GL_VERTEX_ARRAY_BUFFER_BINDING, v); r.v_buffer = v[0];
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, v); r.elem_buffer = v[0];
        if (glIsEnabled(GL_NORMAL_ARRAY)) {
            r.n_size = 3;
            glGetIntegerv(GL_NORMAL_ARRAY_TYPE, v); r.n_type_stride = (u32)v[0] << 16;
            glGetIntegerv(GL_NORMAL_ARRAY_STRIDE, v); r.n_type_stride |= (u32)v[0] & 0xffff;
            glGetPointerv(GL_NORMAL_ARRAY_POINTER, &nptr); r.n_ptr = (u32)nptr;
            glGetIntegerv(GL_NORMAL_ARRAY_BUFFER_BINDING, v); r.n_buffer = v[0];
        }

        if (kind == 1) {
            maxi = count ? first + count - 1 : 0;
        } else if (count && count < 30000000) {
            u32 isz = type_size(itype);
            u8 *raw = malloc(count * isz);
            idx = malloc(count * sizeof(u32));
            if (raw && idx && fetch(GL_ELEMENT_ARRAY_BUFFER, r.elem_buffer, indices, count * isz, raw)) {
                for (i = 0; i < count; i++) {
                    u32 x = isz == 1 ? raw[i] : isz == 2 ? ((unsigned short *)raw)[i] : ((u32 *)raw)[i];
                    idx[i] = x;
                    if (x > maxi) maxi = x;
                }
                r.nidx = count;
            } else {
                free(idx);
                idx = NULL;
            }
            free(raw);
        }
        stride = r.v_stride ? r.v_stride : r.v_size * type_size(r.v_type);
        if (count && stride && (kind == 1 || idx) && maxi < 20000000) {
            u32 bytes = maxi * stride + r.v_size * type_size(r.v_type);
            vdata = malloc(bytes);
            if (vdata && fetch(GL_ARRAY_BUFFER, r.v_buffer, vptr, bytes, vdata)) {
                r.nverts = maxi + 1;
                r.vbytes = bytes;
            } else {
                free(vdata);
                vdata = NULL;
            }
        }
    }
    fwrite(&r, sizeof r, 1, capf);
    if (vdata) fwrite(vdata, 1, r.vbytes, capf);
    if (idx && r.nidx) fwrite(idx, sizeof(u32), r.nidx, capf);
    free(vdata);
    free(idx);
}


static u32 verify_left;
static float bump_strength;         /* strength of normal maps, 0 = off */
static int smooth_on = 1;           /* correct the traced light for smooth-shaded low-polygon shapes */
/* Characters (wumrt_characters.ini): strengths of the light wrap, the rim light, the light
   through thin parts and the catchlights in the eyes; 0 = off. */
static float char_wrap, char_rim, char_trans, char_catch;
static float leaf_glow, leaf_wrap;      /* foliage (kind 4): light through leaves, soft wrap */

/* Where the worms are: the centres of their face draws (world space), logged every few seconds
   so that a free camera can be aimed at one (lookat.py). */
static float face_pos[8][3];
static int n_faces;
static DWORD face_logged;
/* Every worm draw (skin) becomes a capsule along its bounding box's longest side, for the
   contact shadow; the list of a frame is handed to the tracer at its end (so the trace uses
   the previous frame's, one frame late, which does not show). Duplicates (the picture-in-
   picture view draws the same worms again) are dropped. */
static float cap_build[16 * 8];
static int cap_count;
static void note_char(const struct rt_draw *d, const float *model)
{
    float lo[3], hi[3], e[3], c[3], r, half, *o;
    int k, ax = 0, i;
    if (!rt_draw_bounds(d, model, lo, hi)) return;
    for (k = 0; k < 3; k++) { e[k] = hi[k] - lo[k]; c[k] = 0.5f * (lo[k] + hi[k]); }
    if (e[1] > e[ax]) ax = 1;
    if (e[2] > e[ax]) ax = 2;
    r = 0.25f * (e[0] + e[1] + e[2] - e[ax]);      /* half the mean of the two shorter sides */
    if (r <= 0.01f || e[ax] > 200.0f) return;
    half = 0.5f * e[ax] - r;
    if (half < 0) half = 0;
    for (i = 0; i < cap_count; i++) {
        o = &cap_build[i * 8];
        if (fabs(0.5f * (o[0] + o[4]) - c[0]) + fabs(0.5f * (o[1] + o[5]) - c[1]) + fabs(0.5f * (o[2] + o[6]) - c[2]) < 1.0f)
            return;
    }
    if (cap_count >= 16) return;
    o = &cap_build[cap_count++ * 8];
    for (k = 0; k < 3; k++) { o[k] = c[k]; o[4 + k] = c[k]; }
    o[ax] -= half; o[4 + ax] += half;
    o[3] = r; o[7] = 0;
}

static void chars_frame_end(void)
{
    rt_set_capsules(cap_build, cap_count);
    cap_count = 0;
}

static void note_face(const struct rt_draw *d, const float *model)
{
    float c[3];
    int i;
    if (!rt_draw_center(d, model, c)) return;
    for (i = 0; i < n_faces; i++)
        if (fabs(face_pos[i][0] - c[0]) + fabs(face_pos[i][1] - c[1]) + fabs(face_pos[i][2] - c[2]) < 1.0) break;
    if (i == n_faces && n_faces < 8) { memcpy(face_pos[n_faces], c, sizeof c); n_faces++; }
    if (GetTickCount() - face_logged > 3000) {
        char buf[400];
        int len = 0;
        face_logged = GetTickCount();
        buf[0] = 0;
        for (i = 0; i < n_faces && len < (int)sizeof buf - 40; i++)
            len += _snprintf(buf + len, sizeof buf - len, " (%.1f %.1f %.1f)", face_pos[i][0], face_pos[i][1], face_pos[i][2]);
        logmsg("characters: %d faces at%s", n_faces, buf);
        n_faces = 0;
    }
}

/* Texture ids get reused: tell the tracer when one is deleted or given a new image. */
static void (APIENTRY *real_glDeleteTextures)(GLsizei, const GLuint *);
static void (APIENTRY *real_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);

static void APIENTRY my_glDeleteTextures(GLsizei n, const GLuint *ids)
{
    GLsizei i;
    for (i = 0; ids && i < n; i++) rt_texture_gone(ids[i]);
    real_glDeleteTextures(n, ids);
}

static void APIENTRY my_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                                     GLenum fmt, GLenum type, const void *data)
{
    if (target == GL_TEXTURE_2D && level == 0) {
        GLint id = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
        if (id > 0) rt_texture_gone((GLuint)id);
    }
    real_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
}

/* Debugging aid: compare what we tracked with what OpenGL says. */
static void verify_state(const struct rt_draw *d)
{
    GLint v = 0;
    void *ptr = NULL;
    int bad = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &v); bad |= (u32)v != d->v_size;
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &v); bad |= ((GLenum)v != d->v_type) << 1;
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &v); bad |= ((u32)v != d->v_stride) << 2;
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &ptr); bad |= (ptr != d->v_ptr) << 3;
    glGetIntegerv(0x8896, &v); bad |= ((u32)v != d->v_buf) << 4;
    if (!d->arrays) { glGetIntegerv(0x8895, &v); bad |= ((u32)v != d->elem_buf) << 5; }
    bad |= ((glIsEnabled(GL_VERTEX_ARRAY) != 0) != (d->v_on != 0)) << 6;
    bad |= ((glIsEnabled(GL_ALPHA_TEST) != 0) != (d->alpha_test != 0)) << 7;
    bad |= ((glIsEnabled(GL_BLEND) != 0) != (d->blend != 0)) << 8;
    if (d->tc_on) {
        glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &ptr); bad |= (ptr != d->tc_ptr) << 9;
        glGetIntegerv(0x889A, &v); bad |= ((u32)v != d->tc_buf) << 10;
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &v); bad |= ((u32)v != d->tc_stride) << 11;
    }
    bad |= ((glIsEnabled(GL_NORMAL_ARRAY) != 0) != (d->n_on != 0)) << 12;
    if (d->n_on) {
        glGetPointerv(GL_NORMAL_ARRAY_POINTER, &ptr); bad |= (ptr != d->n_ptr) << 13;
        glGetIntegerv(0x8897, &v); bad |= ((u32)v != d->n_buf) << 14;
        glGetIntegerv(GL_NORMAL_ARRAY_STRIDE, &v); bad |= ((u32)v != d->n_stride) << 15;
        glGetIntegerv(GL_NORMAL_ARRAY_TYPE, &v); bad |= ((GLenum)v != d->n_type) << 16;
    }
    ctl->verify_checked++;
    if (bad) {
        if (ctl->verify_bad < 10) logmsg("tracked GL state differs from the real one, bits %#x", bad);
        ctl->verify_bad++;
    }
    verify_left--;
}

/* A sky draw is about to happen: draw it into the tracer's views of the sky first, with
   the game's own shaders and state and only the camera changed. */
static void capture_sky(struct prog *vp, u32 kind, GLenum mode, u32 first, u32 count, GLenum itype, const void *indices)
{
    u32 want = rt_sky_wanted(), f, drawn = 0;
    float v[16], pm[16];
    if (!want || (vp->have & 6) != 6 || !vp->p_view || !vp->p_proj) return;
    for (f = 0; f < 6; f++) {
        if (!(want >> f & 1)) continue;
        if (!rt_sky_begin(f, vp->view, v, pm)) break;
        real_cgGLSetMatrixParameterfr(vp->p_view, v);
        real_cgGLSetMatrixParameterfr(vp->p_proj, pm);
        (*(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters)(vp->h);
        if (kind == 1) real_glDrawArrays(mode, (GLint)first, (GLsizei)count);
        else real_glDrawElements(mode, (GLsizei)count, itype, indices);
        drawn++;
    }
    rt_sky_end();
    if (drawn) {
        real_cgGLSetMatrixParameterfr(vp->p_view, vp->view);
        real_cgGLSetMatrixParameterfr(vp->p_proj, vp->proj);
        (*(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters)(vp->h);
        stat_cur[7] += drawn;
    }
}

#include "round.c"

static float relief_depth, relief_shadow = 6.0f;     /* parallax relief: depth scale (0 = off), self-shadow strength */
/* edges: sculpted soil lips on terrain cut faces (Landscape.cg RtEdge, rtRelief.zw);
   strength 0 = off (negative = debug view), radius = lip size in world units */
static float edges_strength, edges_radius = 8.0f;
static struct rt_draw scat_draw;          /* the last sun-lit terrain draw, for draw_scatter */
static int scat_ok, scatter_on;
static float shell_ground[4];             /* rtShell of the game's own grass draw (see draw_shells) */

/* Called before every draw. Shadow-pass draws are handed to the tracer as casters.
   Sun-lit main-pass draws get the traced mask: rtParams = (enabled, 1/width, 1/height,
   bounce strength) and the mask texture, for shaders that have those uniforms. */
/* ---- Lit smoke ----------------------------------------------------------------------
   The game draws smoke puffs as unlit alpha-blended sprites (FFVertexMainTexCol /
   FFFragmentMainTexCol), so at night they glow cream. For main-view, alpha-blended draws with
   a grey/white texture (fx_smoke_texture) the patched fragment program gets rtSmoke on and
   shades the puff with the sun (direction, colour) and the ambient colour the game's lit
   shaders get, with a normal from the puff's alpha slope (soft terminator, darker underside).
   Settings: smoke (0/1), smoke_strength, smoke_step (texture step of the slope),
   smoke_gain (slope -> normal; negative flips v), smoke_under (what the underside keeps),
   smoke_sun, smoke_ambient (scales of the two lights). */
static struct { int on; float strength, step, gain, under, sun, amb, fx; } SM;
static float smoke_sun_rgb[3] = { 1, 1, 1 }, smoke_amb_rgb[3] = { 0.4f, 0.4f, 0.4f };
static void smoke_settings_begin(void)
{
    SM.on = 0; SM.strength = 1.0f; SM.step = 1.0f / 32.0f; SM.gain = 6.0f; SM.under = 0.55f; SM.sun = 1.0f; SM.amb = 1.0f; SM.fx = 1.0f;
}
static void smoke_setting(const char *name, float v)
{
    if (!strcmp(name, "smoke")) SM.on = v != 0;
    else if (!strcmp(name, "smoke_strength")) SM.strength = v;
    else if (!strcmp(name, "smoke_step")) SM.step = v;
    else if (!strcmp(name, "smoke_gain")) SM.gain = v;
    else if (!strcmp(name, "smoke_under")) SM.under = v;
    else if (!strcmp(name, "smoke_sun")) SM.sun = v;
    else if (!strcmp(name, "smoke_ambient")) SM.amb = v;
    else if (!strcmp(name, "smoke_fx")) SM.fx = v;
}
static void smoke_draw(struct prog *vp, struct prog *fp)
{
    int on = SM.on && vp && vp->unlit_tex && (vp->have & 2) && gs.blend &&
             gs.blend_src == 0x0302 && gs.blend_dst == 0x0303 && rt_is_main_view(vp->view) && fx_smoke_texture(fp->tex0);
    if (!on && !fp->smoke_last) return;
    if (on) {
        /* the direction towards the sun, in view space (row vectors: v * view) */
        const float *s = rt_stats.sun, *m = vp->view;
        float l[3], len;
        int k;
        for (k = 0; k < 3; k++) l[k] = s[0] * m[k] + s[1] * m[4 + k] + s[2] * m[8 + k];
        if ((vp->have & 4) && vp->proj[11] > 0) l[2] = -l[2];   /* left-handed view: the eye looks along +z */
        len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        if (len > 1e-6f) { l[0] /= len; l[1] /= len; l[2] /= len; }
        real_cgGLSetParameter4f(fp->p_rt_smoke[0], 1.0f, SM.strength, SM.step, SM.gain);
        real_cgGLSetParameter4f(fp->p_rt_smoke[1], l[0], l[1], l[2], 1.0f);
        real_cgGLSetParameter4f(fp->p_rt_smoke[2], smoke_sun_rgb[0] * SM.sun, smoke_sun_rgb[1] * SM.sun, smoke_sun_rgb[2] * SM.sun, 0);
        real_cgGLSetParameter4f(fp->p_rt_smoke[3], smoke_amb_rgb[0] * SM.amb, smoke_amb_rgb[1] * SM.amb, smoke_amb_rgb[2] * SM.amb, SM.under);
        if (fp->p_rt_smoke[4] && fp->p_rt_smoke[5]) {
            /* the brightest explosion/fire light (fx.c), in view space */
            float pr[4], rgb[3], v[3];
            if (SM.fx > 0 && fx_smoke_light(pr, rgb)) {
                for (k = 0; k < 3; k++) v[k] = pr[0] * m[k] + pr[1] * m[4 + k] + pr[2] * m[8 + k] + m[12 + k];
                real_cgGLSetParameter4f(fp->p_rt_smoke[4], v[0], v[1], v[2], pr[3]);
                real_cgGLSetParameter4f(fp->p_rt_smoke[5], rgb[0] * SM.fx, rgb[1] * SM.fx, rgb[2] * SM.fx,
                                        (vp->have & 4) && vp->proj[11] > 0 ? -1.0f : 1.0f);
            } else {
                real_cgGLSetParameter4f(fp->p_rt_smoke[4], 0, 0, 0, 1);
                real_cgGLSetParameter4f(fp->p_rt_smoke[5], 0, 0, 0, 1);
            }
        }
    } else {
        real_cgGLSetParameter4f(fp->p_rt_smoke[0], 0, 0, 0, 0);
    }
    fp->smoke_last = on;
    (*(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters)(fp->h);
}

static void before_draw(u32 kind, GLenum mode, u32 first, u32 count, GLenum itype, const void *indices)
{
    struct prog *vp = (cur_vp >= 0 && vp_on) ? &progs[cur_vp] : NULL;
    struct prog *fp = (cur_fp >= 0 && fp_on) ? &progs[cur_fp] : NULL;
    struct rt_draw d;
    float inv[2] = { 0, 0 }, msize[4] = { 0, 0, 0, 0 };
    GLuint tex = 0, btex = 0;
    int caster = vp && vp->caster, lit = vp && vp->sun_lit;
    static int sky_phase;       /* between the shadow pass and the first lit draw: where the sky is drawn */

    if (fp && fp->p_rt_smoke[0]) smoke_draw(vp, fp);
    if (vp) {                   /* film post-processing: where the scene ends (post.c) */
        u32 cls = caster ? POST_CASTER : vp->landscape && lit ? POST_LANDSCAPE : lit ? POST_LIT : 0;
        if (!caster && !lit) cls = vp->water ? POST_WATER : vp->effect ? POST_EFFECT : POST_UNLIT;
        if (vp->p_view && (vp->have & 2)) cls |= POST_HAS_VIEW;
        post_before_draw(cls, (vp->have & 2) ? vp->view : NULL);
    }
    if (caster) sky_phase = 1;
    else if (lit) sky_phase = 0;
    else if (vp && kind <= 2) {
        if (vp->water && (vp->have & 1)) rt_set_water(vp->model[13]);
        if (vp->water && FC->on) fc_water(vp);
        if (vp->water && fp && fp->p_rt_refl && fp->p_rt_params) {
            /* The sea: give its shader what the tracer saw mirrored in it. */
            float info[3] = { 0, 0, 0 };
            GLuint rtex = rt_water_reflection(info);
            if (rtex || fp->refl_last) {
                real_cgGLSetParameter4f(fp->p_rt_params, rtex ? 1.0f : 0.0f, info[0], info[1], info[2]);
                if (fp->p_rt_water) {
                    float s[3];
                    rt_water_sun(s);
                    real_cgGLSetParameter4f(fp->p_rt_water, rtex ? water_style : 0.0f, s[0], s[1], s[2]);
                }
                if (rtex && p_glActiveTexture) {
                    GLint act = 0;
                    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
                    real_cgGLSetTextureParameter(fp->p_rt_refl, rtex);
                    (*(void (__cdecl **)(void *))IAT_cgGLEnableTextureParameter)(fp->p_rt_refl);
                    p_glActiveTexture((GLenum)act);
                }
                fp->refl_last = rtex != 0;
                (*(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters)(fp->h);
            }
            return;
        }
        if (!sky_phase && (vp->have & 2)) fx_census(vp->entry, fp ? fp->entry : "", vp->view, gs.blend, gs.blend_src, gs.blend_dst,
                                       fp ? fp->tex0 : 0, mode, count, gs.vtx.buf != 0);
        if (!sky_phase && vp->unlit_tex && (vp->have & 3) == 3 && gs.blend) {
            /* Possibly an effect (explosion, fire, sparks): its light for the next frame (fx.c). */
            struct fx_draw_info fi;
            memset(&d, 0, sizeof d);
            d.mode = mode; d.first = first; d.count = count; d.itype = itype; d.indices = indices; d.arrays = kind == 1;
            d.v_on = gs.vtx_on; d.v_size = gs.vtx.size; d.v_stride = gs.vtx.stride; d.v_buf = gs.vtx.buf;
            d.v_type = gs.vtx.type; d.v_ptr = gs.vtx.ptr; d.elem_buf = gs.elem_buf;
            d.texture = fp ? fp->tex0 : 0;
            fi.d = &d; fi.model = vp->model; fi.view = vp->view;
            fi.blend_src = gs.blend_src; fi.blend_dst = gs.blend_dst;
            fi.col_on = gs.col_on; fi.col_buf = gs.col.buf; fi.col_size = gs.col.size; fi.col_stride = gs.col.stride;
            fi.col_type = gs.col.type; fi.col_ptr = gs.col.ptr;
            fx_on_draw(&fi);
        }
        if (sky_phase && vp->unlit_tex && rt_sky_wanted()) {
            memset(&d, 0, sizeof d);
            d.v_on = gs.vtx_on; d.v_size = gs.vtx.size; d.v_stride = gs.vtx.stride; d.v_buf = gs.vtx.buf;
            d.v_type = gs.vtx.type; d.v_ptr = gs.vtx.ptr;
            if (rt_mesh_extent(&d) > 4000.0f) capture_sky(vp, kind, mode, first, count, itype, indices);
        }
    }
    if (kind > 2 || !(caster || lit)) {
        if (!fp || !fp->p_rt_params || fp->rt_last == 0) return;
    }
    if (caster || lit || verify_left) {
        d.mode = mode; d.first = first; d.count = count; d.itype = itype; d.indices = indices; d.arrays = kind == 1;
        d.v_on = gs.vtx_on; d.v_size = gs.vtx.size; d.v_stride = gs.vtx.stride; d.v_buf = gs.vtx.buf;
        d.v_type = gs.vtx.type; d.v_ptr = gs.vtx.ptr;
        d.tc_on = gs.tc0_on; d.tc_size = gs.tc0.size; d.tc_stride = gs.tc0.stride; d.tc_buf = gs.tc0.buf;
        d.tc_type = gs.tc0.type; d.tc_ptr = gs.tc0.ptr;
        d.n_on = gs.nrm_on; d.n_stride = gs.nrm.stride; d.n_buf = gs.nrm.buf; d.n_type = gs.nrm.type; d.n_ptr = gs.nrm.ptr;
        d.elem_buf = gs.elem_buf;
        d.alpha_test = gs.alpha_test; d.blend = gs.blend; d.alpha_ref = gs.alpha_ref;
        d.texture = fp ? fp->tex0 : 0;
        d.tex_xf = (vp && (vp->have & 64)) ? vp->texxf : NULL;
        d.material = (fp && (fp->have & 512)) ? fp->mat : NULL;
        d.static_key = 0;
        d.own = 0;
        if (verify_left && kind <= 2) verify_state(&d);
        if (caster) {
            if (kind <= 2 && (vp->have & 3) == 3) {
                struct rt_draw rd;      /* the rounded mesh (round.c), if this one has one */
                rt_on_caster(round_caster(&d, &rd) ? &rd : &d, vp->model, vp->view);
            }
            return;
        }
        if (lit && kind <= 2) rt_on_main_alpha(&d);
        if (lit && kind <= 2 && vp && (vp->have & 7) == 7 && !vp->landscape && motion_objects_wanted()
            && motion_object_begin(&d, vp->model, vp->view, vp->proj)) {    /* motion-blur stream: a moving object's */
            if (kind == 1) real_glDrawArrays(mode, (GLint)first, (GLsizei)count);      /* screen velocity (motion.c) */
            else real_glDrawElements(mode, (GLsizei)count, itype, indices);
            motion_object_end();
        }
        if (lit && kind <= 2 && vp->landscape) { scat_draw = d; scat_ok = 1; }
        /* The terrain's shader is only ever given the sun's colours. */
        if (lit && vp->landscape && fp && (fp->have & 0x180) == 0x180) {
            rt_set_light(fp->dif, fp->amb);
            memcpy(smoke_sun_rgb, fp->dif, 12); memcpy(smoke_amb_rgb, fp->amb, 12);
        }
    }
    if (!fp || !fp->p_rt_params) return;
    if (lit && (vp->have & 6) == 6) tex = rt_prepare_main(vp->view, vp->proj, vp->landscape, inv, msize, &btex);
    if (!tex && fp->rt_last == 0) return;
    if (tex) {
        GLint act = 0;
        resolve_ext();
        real_cgGLSetParameter4f(
            fp->p_rt_params, 1.0f, inv[0], inv[1], msize[3]);
        if (fp->p_rt_params2)
            real_cgGLSetParameter4f(
                fp->p_rt_params2, msize[0] * inv[0] < 0.999f ? 1.0f : 0.0f, msize[0], msize[1], msize[2]);
        if (fp->p_rt_char) {
            /* rtChar = (what this draw is on the worms: 0 nothing, 1 skin, 2 skin holding the eyes,
               3 an eye; light wrap; rim light; light through thin parts), rtChar2 = (catchlight
               strength, the world's up direction in eye space = row 1 of the view matrix). */
            int kind = (char_wrap > 0 || char_rim > 0 || char_trans > 0 || char_catch > 0 || leaf_glow != 0 || leaf_wrap > 0)
                       ? rt_char_kind(fp->tex0) : 0;
            /* foliage: (4, wrap, 0, light through the leaf) */
            if (kind == 4)
                real_cgGLSetParameter4f(fp->p_rt_char, leaf_glow != 0 || leaf_wrap > 0 ? 4.0f : 0.0f, leaf_wrap, 0.0f, leaf_glow);
            else
                real_cgGLSetParameter4f(fp->p_rt_char, (float)kind, char_wrap, char_rim, char_trans);
            if (kind == 2 && (vp->have & 1)) note_face(&d, vp->model);
            if ((kind == 1 || kind == 2) && (vp->have & 1)) note_char(&d, vp->model);
            if (fp->p_rt_char2) real_cgGLSetParameter4f(fp->p_rt_char2, char_catch, vp->view[4], vp->view[5], vp->view[6]);
        }
        if ((vp->have & 1) && post_wants_worms()) {          /* depth of field: focus on the worm */
            int wk = rt_char_kind(fp->tex0);
            float wc[3];
            if ((wk == 1 || wk == 2) && rt_draw_center(&d, vp->model, wc)) post_note_worm(wc, vp->view, vp->proj);
        }
        if (fp->p_rt_mask && p_glActiveTexture) {
            glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
            real_cgGLSetTextureParameter(fp->p_rt_mask, tex);
            (*(void (__cdecl **)(void *))IAT_cgGLEnableTextureParameter)(fp->p_rt_mask);
            if (fp->p_rt_bounce) {
                real_cgGLSetTextureParameter(fp->p_rt_bounce, btex);
                (*(void (__cdecl **)(void *))IAT_cgGLEnableTextureParameter)(fp->p_rt_bounce);
            }
            if (fp->p_rt_params3) {
                /* The normal map that goes with this draw's colour texture, if there is one. */
                GLuint nm = bump_strength != 0 ? rt_normal_map(fp->tex0) : 0;
                /* yzw: the world's up direction in eye space (row 1 of the view matrix), for the
                   sky light's share of the normal mapping */
                real_cgGLSetParameter4f(fp->p_rt_params3, nm ? bump_strength : 0.0f, smooth_on ? vp->view[4] : 0.0f, smooth_on ? vp->view[5] : 0.0f, smooth_on ? vp->view[6] : 0.0f);
                if (nm && fp->p_rt_normal) {
                    real_cgGLSetTextureParameter(fp->p_rt_normal, nm);
                    (*(void (__cdecl **)(void *))IAT_cgGLEnableTextureParameter)(fp->p_rt_normal);
                }
                /* Parallax relief from the height in the normal map's alpha (relief.py): x = how deep
                   height 0 is, in texture repeats (0.04 as relief.py writes them, times the setting) */
                if (fp->p_rt_relief)
                    real_cgGLSetParameter4f(fp->p_rt_relief, nm ? 0.04f * relief_depth : 0.0f, relief_shadow, edges_strength, edges_radius);
            }
            if (fp->p_rt_refl) {
                GLuint rtex = rt_last_reflection();
                real_cgGLSetTextureParameter(fp->p_rt_refl, rtex ? rtex : btex);
                (*(void (__cdecl **)(void *))IAT_cgGLEnableTextureParameter)(fp->p_rt_refl);
            }
            p_glActiveTexture((GLenum)act);
        }
        stat_cur[6]++;
    } else {
        real_cgGLSetParameter4f(fp->p_rt_params, 0, 0, 0, 0);
    }
    fp->rt_last = tex ? 1.0f : 0.0f;
    (*(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters)(fp->h);
}

/* ---- shell layers: grass and moss with thickness ---------------------------------
   A terrain draw whose texture is listed as grass or moss (wumrt_surfaces.ini) is drawn
   `shells` more times, each copy pushed further out along the vertex normals (rtShell.x of
   the vertex program), and the fragment program keeps only the pixels inside a blade at
   that height (its rtShell = cells per world unit, height share, blade radius, height). The copies are
   drawn before the game's own draw, outermost first, so that pixels hidden under blades
   are rejected by the depth test instead of being shaded. */
static int shell_count;                 /* layers per grass draw, 0 = off */
static float shell_height = 3.5f;       /* height of the outermost layer, world units */
static float shell_density = 0.3f;      /* blade cells per world unit */
static float shell_width = 0.45f;       /* radius of a blade's foot, in cells (at most 0.45) */
static int shell_shadow;                 /* 1 = the blades shade each other and the ground from the sun */
static float grass_wind;                 /* wind sway of shells and border tufts (0 = still) */
static float blade_glow, blade_tip;      /* backlit blades (characters stream): glow, tip highlight */
static float grass_patches;              /* painted colour patches over the grass (0 = none) */

/* Seconds, for the wind: wrapped every 2000 pi seconds (about 1.7 hours) so the shaders' sines
   keep their precision. */
static float wind_time(void)
{
    LARGE_INTEGER c, f;
    double t;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    t = (double)c.QuadPart / (double)f.QuadPart;
    return (float)(t - floor(t / 6283.1853) * 6283.1853);
}
static double shell_cpu_us;             /* time spent in draw_shells this frame */
static u32 shell_draws, shell_verts, shell_draws_last, shell_verts_last, shell_draws_frame;
static double shell_cpu_frame;

static void draw_shells(u32 kind, GLenum mode, u32 first, u32 start, u32 end, u32 count, GLenum itype, const void *indices)
{
    struct prog *vp = (cur_vp >= 0 && vp_on) ? &progs[cur_vp] : NULL;
    struct prog *fp = (cur_fp >= 0 && fp_on) ? &progs[cur_fp] : NULL;
    void (__cdecl *update)(void *) = *(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters;
    float sh[2] = { 0, 0 }, density, full, width = shell_width > 0.45f ? 0.45f : shell_width;
    int i, n = shell_count > 64 ? 64 : shell_count;
    LARGE_INTEGER t0, t1, freq;
    if (!vp || !fp || !vp->landscape || !vp->p_rt_shell || !fp->p_rt_shell) return;
    if (post_wants_terrain()) post_note_terrain(rt_terrain_hash(fp->tex0));   /* grade.c: which level */
    if (n <= 0 || !rt_shell(fp->tex0, sh)) {
        if (fp->shell_on) {
            real_cgGLSetParameter4f(fp->p_rt_shell, 0, 0, 0, 0);
            update(fp->h);
            fp->shell_on = 0;
        }
        return;
    }
    if (width <= 0) width = 0.01f;
    QueryPerformanceCounter(&t0);
    density = shell_density * sh[1];
    full = shell_height * sh[0];
    if (!shell_shadow) full = -full;         /* the shader reads the sign: negative = no sun test */
    if (fp->p_rt_wind) real_cgGLSetParameter4f(fp->p_rt_wind, wind_time(), grass_wind, grass_patches, 0);
    if (fp->p_rt_blade) real_cgGLSetParameter4f(fp->p_rt_blade, blade_glow, blade_tip, 0, 0);
    for (i = n; i >= 1; i--) {
        float t = (float)i / (float)n;
        real_cgGLSetParameter4f(vp->p_rt_shell, t * shell_height * sh[0], 0, 0, 0);
        update(vp->h);
        real_cgGLSetParameter4f(fp->p_rt_shell, density, t, width, full);
        update(fp->h);
        if (kind == 1) real_glDrawArrays(mode, (GLint)first, (GLsizei)count);
        else if (kind == 2) real_glDrawRangeElements(mode, start, end, (GLsizei)count, itype, indices);
        else real_glDrawElements(mode, (GLsizei)count, itype, indices);
        shell_draws++; shell_verts += count;
    }
    /* The game's own draw follows: no offset, and marked as the ground under the blades. */
    real_cgGLSetParameter4f(vp->p_rt_shell, 0, 0, 0, 0);
    update(vp->h);
    real_cgGLSetParameter4f(fp->p_rt_shell, density, 0, width, full);
    update(fp->h);
    fp->shell_on = 1;
    shell_ground[0] = density; shell_ground[1] = 0; shell_ground[2] = width; shell_ground[3] = full;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    shell_cpu_us += (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)freq.QuadPart;
}

/* Scattered stones and tufts (scatter.c in the core), drawn after a sun-lit terrain draw with
   the same programs. On grass the shell uniform is cleared for them and put back after. */
static void draw_scatter(void)
{
    struct prog *vp = (cur_vp >= 0 && vp_on) ? &progs[cur_vp] : NULL;
    struct prog *fp = (cur_fp >= 0 && fp_on) ? &progs[cur_fp] : NULL;
    void (__cdecl *update)(void *) = *(void (__cdecl **)(void *))IAT_cgUpdateProgramParameters;
    int shell = 0;
    if (!scat_ok) return;
    scat_ok = 0;
    if (!scatter_on || !vp || !fp || !vp->landscape || !vp->p_rt_shell || !fp->p_rt_shell) return;
    if (fp->shell_on) { real_cgGLSetParameter4f(fp->p_rt_shell, 0, 0, 0, 0); update(fp->h); shell = 1; }
    /* the vertex program sways the tufts' tips (see patch_shaders.py) and keeps their alpha at 1 */
    real_cgGLSetParameter4f(vp->p_rt_shell, 0, wind_time(), grass_wind, 1);
    update(vp->h);
    rt_scatter_draw(&scat_draw, vp->model);
    real_cgGLSetParameter4f(vp->p_rt_shell, 0, 0, 0, 0);
    update(vp->h);
    if (shell) {
        real_cgGLSetParameter4f(fp->p_rt_shell, shell_ground[0], shell_ground[1], shell_ground[2], shell_ground[3]);
        update(fp->h);
    }
}

/* ---- hide_hud: clean review shots of a paused match -------------------------------------
 * The engine draws the HUD, the pause box and the menus into its scene framebuffer after the
 * scene, starting with the first fixed-function draw (no vertex program) after the sun-lit
 * main pass; then the frame is copied to the window's framebuffer (0). With hide_hud = 1
 * every draw from that point on is skipped except those into framebuffer 0, which leaves the
 * scene alone on screen and in screenshots. hide_hud = 2 does the same and logs the frame's
 * draws after the shadow pass once (to check the split). Default 0. Immediate-mode
 * (glBegin) draws are never skipped. */
static int hide_hud, hud_logged, draw_index, lit_seen, hud_part;

static int hud_skip(int kind, GLsizei count)
{
    struct prog *vp = (cur_vp >= 0 && vp_on) ? &progs[cur_vp] : NULL;
    int skip;
    if (!hide_hud) return 0;
    if (vp && vp->sun_lit) lit_seen = 1;
    if (!vp && lit_seen && draw_fbo != 0) hud_part = 1;
    skip = hud_part && draw_fbo != 0;
    if (hide_hud == 2 && !hud_logged && draw_index < 6000 && !(vp && vp->caster))
        logmsg("hud log: draw %d kind %d n %d fbo %u vp %s -> %s", draw_index, kind, (int)count, draw_fbo,
               vp ? vp->entry : "-", skip ? "skipped" : "kept");
    draw_index++;
    return skip;
}

static void APIENTRY my_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (hud_skip(0, count)) return;
    stat_cur[0]++; stat_cur[5] += count;
    before_draw(0, mode, 0, count, type, indices);
    if (capturing) rec_draw(0, mode, 0, count, type, indices);
    draw_shells(0, mode, 0, 0, 0, count, type, indices);
    if (!round_main(0, mode, 0, (u32)count, type, indices)) real_glDrawElements(mode, count, type, indices);
    draw_scatter();
}

static void APIENTRY my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (hud_skip(1, count)) return;
    stat_cur[1]++; stat_cur[5] += count;
    before_draw(1, mode, first, count, 0, NULL);
    if (capturing) rec_draw(1, mode, first, count, 0, NULL);
    draw_shells(1, mode, first, 0, 0, count, 0, NULL);
    if (!round_main(1, mode, (u32)first, (u32)count, 0, NULL)) real_glDrawArrays(mode, first, count);
    draw_scatter();
}

static void APIENTRY my_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                            GLenum type, const void *indices)
{
    if (hud_skip(2, count)) return;
    stat_cur[2]++; stat_cur[5] += count;
    before_draw(2, mode, 0, count, type, indices);
    if (capturing) rec_draw(2, mode, 0, count, type, indices);
    draw_shells(2, mode, 0, start, end, count, type, indices);
    if (!round_main(2, mode, 0, (u32)count, type, indices)) real_glDrawRangeElements(mode, start, end, count, type, indices);
    draw_scatter();
}

static void APIENTRY my_glBegin(GLenum mode)
{
    if (hide_hud == 2 && !hud_logged) logmsg("hud log: glBegin in fbo %u (never skipped)", draw_fbo);
    stat_cur[3]++;
    {
        struct prog *vp = (cur_vp >= 0 && vp_on) ? &progs[cur_vp] : NULL;
        if (vp && vp->effect) post_before_draw(POST_EFFECT, NULL);
    }
    if (capturing) rec_draw(3, mode, 0, 0, 0, NULL);
    real_glBegin(mode);
}

static void APIENTRY my_glCallList(GLuint list)
{
    stat_cur[4]++;
    if (capturing) rec_draw(4, 0, list, 0, 0, NULL);
    real_glCallList(list);
}

/* The loader asks this for every function the engine fetches with wglGetProcAddress (the
   engine keeps the loader's thunk; this decides where the thunk goes while we are loaded). */
static PROC core_wrap_proc(const char *name, PROC p)
{
    if (name && p && (!strcmp(name, "glDrawRangeElements") || !strcmp(name, "glDrawRangeElementsEXT"))) {
        real_glDrawRangeElements = (void *)p;
        logmsg("wglGetProcAddress %s -> wrapped", name);
        return (PROC)my_glDrawRangeElements;
    }
    if (name && (strstr(name, "Draw") || strstr(name, "Buffer") || strstr(name, "ArrayRange") ||
                 strstr(name, "AllocateMemory")))
        logmsg("wglGetProcAddress %s -> %p", name, (void *)p);
    if (name && p && (!strcmp(name, "glBindBuffer") || !strcmp(name, "glBindBufferARB"))) {
        real_glBindBuffer = (void *)p;
        return (PROC)my_glBindBuffer;
    }
    if (name && p && !strcmp(name, "glBindFramebuffer")) {
        real_glBindFramebuffer = (void *)p;
        return (PROC)my_glBindFramebuffer;
    }
    if (name && p && !strcmp(name, "glBindFramebufferEXT")) {
        real_glBindFramebufferEXT = (void *)p;
        return (PROC)my_glBindFramebufferEXT;
    }
    if (name && p && (!strcmp(name, "glClientActiveTexture") || !strcmp(name, "glClientActiveTextureARB"))) {
        real_glClientActiveTexture = (void *)p;
        return (PROC)my_glClientActiveTexture;
    }
    return rt_wrap_proc(name, p);
}

void rt_log(const char *fmt, ...)
{
    va_list ap;
    if (!L) return;
    va_start(ap, fmt);
    L->vlog(fmt, ap);
    va_end(ap);
}

PROC rt_real_proc(const char *name)
{
    return real_wglGetProcAddress ? real_wglGetProcAddress(name) : NULL;
}

/* ---- frame end: screenshots, capture, commands ---------------------------------- */
static void screenshot(HDC hdc, const char *path)
{
    RECT rc;
    GLint read_fbo = 0, read_buf = 0;
    u32 w, h, hdr[3];
    u8 *buf;
    FILE *f;

    GetClientRect(WindowFromDC(hdc), &rc);
    w = rc.right - rc.left; h = rc.bottom - rc.top;
    if (!w || !h || w > 8192 || h > 8192) { ctl->result = 2; return; }
    buf = malloc(w * h * 3);
    if (!buf) { ctl->result = 3; return; }
    resolve_ext();
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    if (read_fbo && p_glBindFramebuffer) p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR_FMT, GL_UNSIGNED_BYTE, buf);
    glPopClientAttrib();
    if (read_fbo && p_glBindFramebuffer) p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    else glReadBuffer(read_buf);
    f = fopen(path, "wb");
    if (f) {
        hdr[0] = 0x53545257; hdr[1] = w; hdr[2] = h;   /* 'WRTS', rows bottom-up, BGR */
        fwrite(hdr, 4, 3, f);
        fwrite(buf, 1, w * h * 3, f);
        fclose(f);
        ctl->shot_w = w; ctl->shot_h = h; ctl->result = 0;
    } else ctl->result = 4;
    free(buf);
}

static void begin_capture(const char *path)
{
    int i;
    capf = fopen(path, "wb");
    if (!capf) { ctl->result = 4; ctl->ack_seq = ctl->cmd_seq; return; }
    setvbuf(capf, NULL, _IOFBF, 1 << 20);
    for (i = 0; i < nprogs; i++) {
        struct prog_rec pr;
        memset(&pr, 0, sizeof pr);
        pr.magic = 0x474F5250;  /* 'PROG' */
        pr.index = i; pr.is_vertex = progs[i].is_vertex;
        memcpy(pr.file, progs[i].file, sizeof pr.file);
        memcpy(pr.entry, progs[i].entry, sizeof pr.entry);
        memcpy(pr.profile, progs[i].profile, sizeof pr.profile);
        fwrite(&pr, sizeof pr, 1, capf);
    }
    capturing = 1;
}

static void on_frame_end(HDC hdc)
{
    if (hide_hud == 2 && draw_index && !hud_logged) {
        logmsg("hide_hud: %d draws this frame", draw_index);
        hud_logged = 1;
    }
    draw_index = 0;
    lit_seen = hud_part = 0;
    if (!game_hwnd) {
        game_hwnd = WindowFromDC(hdc);
        ctl->hwnd = (u32)game_hwnd;
        logmsg("first swap, window %p", (void *)game_hwnd);
    }
    if (capturing) {
        u32 end = 0x21444E45;   /* 'END!' */
        fwrite(&end, 4, 1, capf);
        fclose(capf);
        capf = NULL;
        capturing = 0;
        ctl->result = 0;
        ctl->ack_seq = ctl->cmd_seq;
    } else if (ctl->cmd_seq != ctl->ack_seq) {
        char path[260];
        memcpy(path, ctl->path, sizeof path);
        path[sizeof path - 1] = 0;
        if (ctl->cmd == CMD_SCREENSHOT) {
            screenshot(hdc, path);
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_RT) {
            if (ctl->arg[1]) rt_set_soft((int)ctl->arg[1], (float)ctl->arg[2] / 1000.0f, ctl->arg[3] ? (int)ctl->arg[3] - 1 : 2);
            rt_set_enabled(ctl->arg[0] != 0);
            ctl->result = 0;
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_DUMP_TEXTURES) {
            ctl->result = (u32)rt_dump_textures(path);
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_DUMP_LAYERS) {
            ctl->result = (u32)rt_dump_layers(path);
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_RT_RELOAD) {
            rt_reload_shader();
            rt_reload_materials();
            post_reload();
            atmos_forget();
            motion_forget();
            ctl->result = 0;
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_PACING) {
            BOOL (WINAPI *swap_interval)(int) = (void *)real_wglGetProcAddress("wglSwapIntervalEXT");
            if (swap_interval) swap_interval((int)ctl->arg[0]);
            frame_cap = ctl->arg[1];
            if (frame_cap_kept) *frame_cap_kept = frame_cap;
            ctl->result = swap_interval ? 0 : 1;
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_RT_SKY) {
            rt_set_sky((int)ctl->arg[0], (float)ctl->arg[1], (float)ctl->arg[2] / 1000.0f, (float)ctl->arg[3] / 1000.0f);
            ctl->result = 0;
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_RT_SCALE) {
            rt_set_scale((float)ctl->arg[0] / 1000.0f, 0.0f);
            ctl->result = 0;
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_VERIFY) {
            verify_left = ctl->arg[0];
            ctl->verify_checked = ctl->verify_bad = 0;
            ctl->result = 0;
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_DUMP_MASK) {
            ctl->result = (u32)rt_dump_mask(path, ctl->arg[0]);
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd == CMD_CAPTURE) {
            begin_capture(path);        /* acknowledged at the next swap */
        } else if (ctl->cmd == CMD_FREECAM) {
            ctl->result = freecam_command(ctl->arg[0]);
            ctl->ack_seq = ctl->cmd_seq;
        } else if (ctl->cmd >= CMD_LOADER_FIRST) {
            /* the loader's: it answers after this frame */
        } else {
            ctl->result = 1;
            ctl->ack_seq = ctl->cmd_seq;
        }
    }
    settings_poll();
    if ((shell_draws != shell_draws_last && (ctl->frame % 30) == 0) || (shell_draws && (ctl->frame % 1800) == 0)) {
        logmsg("shells: %u extra terrain draws, %u extra indices this frame (%d layers, height %.2f, density %.0f)",
               shell_draws, shell_verts, shell_count, shell_height, shell_density);
        shell_draws_last = shell_draws; shell_verts_last = shell_verts;
    }
    shell_draws_frame = shell_draws;
    shell_cpu_frame = shell_cpu_us;
    shell_cpu_us = 0;
    shell_draws = shell_verts = 0;
    fx_frame_end();
    atmos_frame_end();
    motion_frame_end();
    post_frame_end();
    round_frame_end();
    chars_frame_end();
    rt_frame_end();
    ctl->rt = rt_stats;
    memcpy(ctl->stat, stat_cur, sizeof stat_cur);
    memset(stat_cur, 0, sizeof stat_cur);
    ctl->swap_tick = GetTickCount();
    ctl->frame++;
}

/* frame_timer = 1 in wumrt.ini: GPU time from the end of one buffer swap to the start of the
   next (two timestamp queries per frame, which do not disturb the tracer's own timer), averaged
   and written to the log every 60 measured frames. On a GPU that another program is using it
   includes the time spent waiting for that program. */
static int frame_timer_on;

static void frame_timer(int end)
{
    static void (APIENTRY *gen)(GLsizei, GLuint *);
    static void (APIENTRY *counter)(GLuint, GLenum);
    static void (APIENTRY *getu)(GLuint, GLenum, GLuint *);
    static void (APIENTRY *get64)(GLuint, GLenum, u64 *);
    static GLuint q[2];
    static int state;           /* 0 idle, 1 start issued, 2 both issued */
    static u64 sum_ns, min_ns;
    static double cpu_us;
    static u32 n;
    if (!frame_timer_on) { state = 0; n = 0; sum_ns = 0; min_ns = 0; cpu_us = 0; return; }
    if (!gen) {
        gen = (void *)real_wglGetProcAddress("glGenQueries");
        counter = (void *)real_wglGetProcAddress("glQueryCounter");
        getu = (void *)real_wglGetProcAddress("glGetQueryObjectuiv");
        get64 = (void *)real_wglGetProcAddress("glGetQueryObjectui64v");
        if (!gen || !counter || !getu || !get64) { frame_timer_on = 0; logmsg("frame timer: timestamp queries not available"); return; }
    }
    if (!q[0]) gen(2, q);
    if (!end) {
        if (state == 2) {
            GLuint ok = 0;
            getu(q[1], 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &ok);
            if (!ok) return;
            {
                u64 t0 = 0, t1 = 0;
                get64(q[0], 0x8866 /* GL_QUERY_RESULT */, &t0);
                get64(q[1], 0x8866, &t1);
                if (t1 > t0) {
                    sum_ns += t1 - t0; n++; cpu_us += shell_cpu_frame;
                    if (!min_ns || t1 - t0 < min_ns) min_ns = t1 - t0;
                }
                if (n == 60) {
                    logmsg("frame timer: GPU %.2f ms per frame, fastest %.2f ms (60 frames; shells %d, %u shell draws, "
                           "%.2f ms of CPU in them)", (double)sum_ns / n / 1e6, (double)min_ns / 1e6, shell_count,
                           shell_draws_frame, cpu_us / n / 1e3);
                    n = 0; sum_ns = 0; min_ns = 0; cpu_us = 0;
                }
            }
            state = 0;
        }
        if (state == 0) { counter(q[0], 0x8E28 /* GL_TIMESTAMP */); state = 1; }
    } else if (state == 1) {
        counter(q[1], 0x8E28);
        state = 2;
    }
}

static BOOL WINAPI my_SwapBuffers(HDC hdc)
{
    BOOL r;
    on_frame_end(hdc);
    frame_timer(1);
    r = real_SwapBuffers(hdc);
    frame_timer(0);
    if (frame_cap) {            /* crude limiter for when V-Sync is off in the sandbox */
        static DWORD last;
        DWORD now = GetTickCount(), want = 1000 / frame_cap;
        if (now - last < want) Sleep(want - (now - last));
        last = GetTickCount();
    }
    return r;
}

/* ---- start-up --------------------------------------------------------------------- */
/* wumrt.ini, next to the game: one `name = value` per line. rt (0/1), samples, radius
   (degrees), filter (pixels), scale (mask size relative to the render size), sky (sky
   occlusion rays per pixel, 0 = off), sky_reach (world units), sky_strength (0..1), bounce
   (strength of sunlight reflected by nearby surfaces, 0 = off; needs sky rays), sandbox
   (1 = cut the game off from keyboard, mouse, pads and focus, for unattended testing),
   max_megapixels (the traced textures are never larger than this, whatever the render size),
   shells (layers drawn over grass and moss terrain, 0 = off), shell_height (world units),
   shell_density (blade cells per world unit), shell_width (radius of a blade's foot in
   cells, up to 0.45), shell_shadow (1 = the blades shade each other and the ground from the
   sun), grass_wind (sway of shells and border tufts, 0 = still), grass_patches (painted colour
   patches over the grass, 0 = none), frame_timer (1 = log the GPU time per frame).
   The file is read again whenever it changes. */
static int sandbox_mode;            /* from the loader */

static FILETIME settings_time;
static int settings_started, rt_wanted;

static void read_settings(void)
{
    FILE *f = fopen("wumrt.ini", "r");
    WIN32_FILE_ATTRIBUTE_DATA info;
    char line[128], name[32];
    float v, radius = 1.0f, scale = 1.0f, sky_reach = 80.0f, sky_strength = 0.7f, bounce = 0.0f, max_mp = 4.0f, accum = 0.95f;
    float gi_sky = 1.0f, gi_bounce = 0.7f, gi_tint = 0.5f, refl_strength = 1.0f;
    int gi = 0, refl = 0, denoise = 3, spec = 0, gi_rate = 1;
    float cl_strength = 0, cl_scale = 160.0f, cl_speed = 6.0f;      /* moss: cloud shadows */
    float gi_far = 0.0f;
    float spec_strength = 1.0f, bump = 0.0f, clearcoat = 0.0f;
    int smooth = 1, shells = 0, ftimer = 0, hud = 0;
    float wstyle = 0;
    float sh_height = 3.5f, sh_density = 0.3f, sh_width = 0.45f;
    int sh_shadow = 0;
    float gr_wind = 0, gr_patches = 0;
    int scatter = 0;
    float sc_density = 1.0f, sc_size = 1.0f;
    float sk_wrap = 0, sk_trans = 0, sk_depth = 4.0f, sk_rim = 0, sk_catch = 0, sk_contact = 0, sk_reach = 20.0f;
    float lf_glow = 0, lf_wrap = 0, lf_depth = 40.0f;
    float rl_depth = 0.0f, rl_shadow = 6.0f;
    int tex_compress = 0, tex_max = 0;
    int on = 1, samples = 8, filter = 2, sky = 0, sandbox = 0;
    if (!f) return;
    fx_setting(NULL, 0);
    atmos_setting(NULL, 0);
    motion_setting(NULL, 0);
    post_settings_begin();
    smoke_settings_begin();
    round_settings_begin();
    edges_strength = 0; edges_radius = 8.0f;   /* edges: code defaults (off) */
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %31[A-Za-z_] = %f", name, &v) != 2) continue;
        if (post_setting(name, v)) continue;
        if (!strcmp(name, "rt")) on = v != 0;
        else if (!strncmp(name, "fx", 2)) fx_setting(name, v);
        else if (!strncmp(name, "atmos", 5)) atmos_setting(name, v);
        else if (!strncmp(name, "motion_blur", 11)) motion_setting(name, v);
        else if (!strcmp(name, "samples")) samples = (int)v;
        else if (!strcmp(name, "radius")) radius = v;
        else if (!strcmp(name, "filter")) filter = (int)v;
        else if (!strcmp(name, "scale")) scale = v;
        else if (!strcmp(name, "max_megapixels")) max_mp = v;
        else if (!strcmp(name, "sandbox")) sandbox = v != 0;
        else if (!strcmp(name, "sky")) sky = (int)v;
        else if (!strcmp(name, "sky_reach")) sky_reach = v;
        else if (!strcmp(name, "sky_strength")) sky_strength = v;
        else if (!strcmp(name, "bounce")) bounce = v;
        else if (!strcmp(name, "accumulate")) accum = v;
        else if (!strcmp(name, "gi")) gi = v != 0;
        else if (!strcmp(name, "gi_sky")) gi_sky = v;
        else if (!strcmp(name, "gi_bounce")) gi_bounce = v;
        else if (!strcmp(name, "gi_tint")) gi_tint = v;
        else if (!strcmp(name, "gi_rate")) gi_rate = (int)v;
        else if (!strcmp(name, "cloud_shadows")) cl_strength = v;
        else if (!strcmp(name, "cloud_scale")) cl_scale = v;
        else if (!strcmp(name, "cloud_speed")) cl_speed = v;
        else if (!strcmp(name, "gi_far")) gi_far = v;
        else if (!strcmp(name, "reflections")) refl = v != 0;
        else if (!strcmp(name, "reflection_strength")) refl_strength = v;
        else if (!strcmp(name, "denoise")) denoise = (int)v;
        else if (!strcmp(name, "specular")) spec = v != 0;
        else if (!strcmp(name, "specular_strength")) spec_strength = v;
        else if (!strcmp(name, "clearcoat")) clearcoat = v;
        else if (!strcmp(name, "bump")) bump = v;
        else if (!strcmp(name, "smooth")) smooth = v != 0;
        else if (!strcmp(name, "shells")) shells = (int)v;
        else if (!strcmp(name, "shell_height")) sh_height = v;
        else if (!strcmp(name, "shell_density")) sh_density = v;
        else if (!strcmp(name, "shell_width")) sh_width = v;
        else if (!strcmp(name, "shell_shadow")) sh_shadow = (int)v;
        else if (!strcmp(name, "grass_wind")) gr_wind = v;
        else if (!strcmp(name, "blade_glow")) blade_glow = v;
        else if (!strcmp(name, "blade_tip")) blade_tip = v;
        else if (!strcmp(name, "grass_patches")) gr_patches = v;
        else if (!strcmp(name, "frame_timer")) ftimer = v != 0;
        else if (!strcmp(name, "hide_hud")) hud = (int)v;
        else if (!strcmp(name, "water_style")) wstyle = v;
        else if (!strcmp(name, "scatter")) scatter = v != 0;
        else if (!strcmp(name, "scatter_density")) sc_density = v;
        else if (!strcmp(name, "scatter_size")) sc_size = v;
        else if (!strcmp(name, "skin_wrap")) sk_wrap = v;
        else if (!strcmp(name, "skin_translucency")) sk_trans = v;
        else if (!strcmp(name, "skin_depth")) sk_depth = v;
        else if (!strcmp(name, "rim_light")) sk_rim = v;
        else if (!strcmp(name, "catchlights")) sk_catch = v;
        else if (!strcmp(name, "contact_shadow")) sk_contact = v;
        else if (!strcmp(name, "foliage_glow")) lf_glow = v;
        else if (!strcmp(name, "foliage_wrap")) lf_wrap = v;
        else if (!strcmp(name, "foliage_depth")) lf_depth = v;
        else if (!strcmp(name, "contact_reach")) sk_reach = v;
        else if (!strcmp(name, "relief")) rl_depth = v;
        else if (!strcmp(name, "relief_shadow")) rl_shadow = v;
        else if (!strncmp(name, "smoke", 5)) smoke_setting(name, v);
        else if (!strcmp(name, "tex_compress")) tex_compress = (int)v;
        else if (!strcmp(name, "tex_max")) tex_max = (int)v;
        else if (!strcmp(name, "edges")) edges_strength = v;
        else if (!strcmp(name, "edges_radius")) edges_radius = v;
        else round_read_settings(name, v);
    }
    fclose(f);
    post_settings_end();
    if (GetFileAttributesExA("wumrt.ini", GetFileExInfoStandard, &info)) settings_time = info.ftLastWriteTime;
    if (!settings_started) sandbox_mode = sandbox;      /* this one only counts at start-up */
    settings_started = 1;
    rt_wanted = on;
    rt_set_soft(samples, radius, filter);
    rt_set_scale(scale, max_mp * 1e6f);
    rt_set_sky(sky, sky_reach, sky_strength, bounce);
    rt_set_accumulate(accum);
    rt_set_gi(gi, gi_sky, gi_bounce, gi_tint);
    rt_set_gi_rate(gi_rate, gi_far);
    rt_set_clouds(cl_strength, cl_scale, cl_speed);
    rt_set_reflections(refl, refl_strength);
    rt_set_denoise(denoise);
    rt_set_specular(spec, spec_strength);
    rt_set_clearcoat(clearcoat);
    bump_strength = bump;
    smooth_on = smooth;
    shell_count = shells < 0 ? 0 : shells;
    shell_height = sh_height;
    shell_density = sh_density;
    shell_width = sh_width;
    shell_shadow = sh_shadow;
    grass_wind = gr_wind;
    grass_patches = gr_patches;
    frame_timer_on = ftimer;
    if (hud != hide_hud) hud_logged = 0;
    hide_hud = hud;
    water_style = wstyle;
    scatter_on = scatter;
    relief_depth = rl_depth;
    relief_shadow = rl_shadow;
    rt_set_scatter(scatter, sc_density, sc_size);
    rt_set_smooth(smooth);
    rt_set_textures(tex_compress, tex_max);
    char_wrap = sk_wrap; char_trans = sk_trans; char_rim = sk_rim; char_catch = sk_catch;
    rt_set_characters(sk_wrap > 0 || sk_trans > 0, sk_depth, sk_contact, sk_reach);
    leaf_glow = lf_glow; leaf_wrap = lf_wrap;
    rt_set_foliage(lf_glow != 0 || lf_wrap > 0, lf_depth);
    rt_set_enabled(on);
    logmsg("settings: rt=%d samples=%d radius=%.2f filter=%d scale=%.2f max_megapixels=%.1f sky=%d reach=%.0f "
           "strength=%.2f bounce=%.2f gi=%d (sky %.2f, bounce %.2f) sandbox=%d", on, samples, radius, filter, scale,
           max_mp, sky, sky_reach, sky_strength, bounce, gi, gi_sky, gi_bounce, sandbox_mode);
}

/* Once a second: pick up edits to wumrt.ini, and in normal play let F9 switch the tracing
   on and off for comparison. */
static void settings_poll(void)
{
    static DWORD last_check;
    static int f9_down;
    DWORD now = GetTickCount();
    if (now - last_check >= 1000) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        last_check = now;
        if (GetFileAttributesExA("wumrt.ini", GetFileExInfoStandard, &info) &&
            CompareFileTime(&info.ftLastWriteTime, &settings_time) != 0)
            read_settings();
    }
    if (!sandbox_mode && game_hwnd && GetForegroundWindow() == game_hwnd) {
        int down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !f9_down) {
            rt_wanted = !rt_wanted;
            rt_set_enabled(rt_wanted);
            logmsg("F9: ray tracing %s", rt_wanted ? "on" : "off");
        }
        f9_down = down;
    }
}

/* ---- free camera commands ---------------------------------------------------------
   path, as floats: eye x y z, yaw, pitch (in); the same, then on, the distance to the
   scene at the centre of the view (0 = unknown) and how many matrices were replaced (out). */
static u32 freecam_command(u32 mode)
{
    float *io = (float *)ctl->path;
    fc_check_left = 2;
    if (mode == 0) {
        FC->on = 0;
        L->reapply_values();
        logmsg("free camera off");
    } else if (mode == 1) {
        FC->eye[0] = io[0]; FC->eye[1] = io[1]; FC->eye[2] = io[2];
        FC->yaw = io[3]; FC->pitch = io[4] < -89.5f ? -89.5f : io[4] > 89.5f ? 89.5f : io[4];
        FC->on = 1;
        fc_build();
        L->reapply_values();
    } else if (mode == 2) {
        float worst = 0;
        int k;
        if (!FC->ngame) return 2;
        fc_pose_of(FC->game[0], FC->eye, &FC->yaw, &FC->pitch);
        FC->on = 1;
        fc_build();
        for (k = 0; k < 16; k++)
            if (fabs(FC->view[k] - FC->game[0][k]) > worst) worst = (float)fabs(FC->view[k] - FC->game[0][k]);
        logmsg("free camera on at the game's view: eye %.2f %.2f %.2f yaw %.2f pitch %.2f (rebuilt view differs by %.5f)",
               FC->eye[0], FC->eye[1], FC->eye[2], FC->yaw, FC->pitch, worst);
        L->reapply_values();
    }
    if (FC->on) {
        io[0] = FC->eye[0]; io[1] = FC->eye[1]; io[2] = FC->eye[2]; io[3] = FC->yaw; io[4] = FC->pitch;
    } else if (FC->ngame) {
        fc_pose_of(FC->game[0], io, &io[3], &io[4]);
    } else memset(io, 0, 5 * sizeof(float));
    io[5] = (float)FC->on;
    io[6] = rt_center_distance();
    io[7] = (float)FC->overrides;
    return 0;
}

/* ---- the core's life: memory, start, end --------------------------------------------
   Everything the core allocates comes from a heap of its own, destroyed with the DLL, so
   unloading a core frees its memory whatever it held. */
static HANDLE core_heap;
void *core_malloc(size_t n) { return HeapAlloc(core_heap, 0, n ? n : 1); }
void *core_calloc(size_t n, size_t m)
{
    if (m && n > 0xffffffffu / m) return NULL;
    return HeapAlloc(core_heap, HEAP_ZERO_MEMORY, n * m ? n * m : 1);
}
void *core_realloc(void *p, size_t n)
{
    if (!p) return core_malloc(n);
    if (!n) { HeapFree(core_heap, 0, p); return NULL; }
    return HeapReAlloc(core_heap, 0, p, n);
}
void core_free(void *p) { if (p) HeapFree(core_heap, 0, p); }

static struct freecam fc_fallback;

/* Before FreeLibrary, at the end of a frame: leave the game's shaders with our extras off,
   stop the worker thread, delete our GL objects. */
static void core_shutdown(void)
{
    int i;
    if (capturing && capf) { fclose(capf); capf = NULL; capturing = 0; }
    for (i = 0; i < nprogs; i++) {
        struct prog *p = &progs[i];
        if (p->p_rt_params) real_cgGLSetParameter4f(p->p_rt_params, 0, 0, 0, 0);
        if (p->p_rt_params3) real_cgGLSetParameter4f(p->p_rt_params3, 0, 0, 0, 0);
        if (p->p_rt_char) real_cgGLSetParameter4f(p->p_rt_char, 0, 0, 0, 0);
        if (p->p_rt_shell) real_cgGLSetParameter4f(p->p_rt_shell, 0, 0, 0, 0);
    }
    post_shutdown();
    rt_shutdown();
    rnd_free_all();
    logmsg("core %u shut down", L->generation);
}

__declspec(dllexport) int __cdecl wumrt_core_init(struct wumrt_loader *loader, struct wumrt_core *out)
{
    HMODULE cg = GetModuleHandleA("cg.dll");
    u32 i;
    if (!loader || loader->version != WUMRT_API_VERSION || !core_heap) return 0;
    L = loader;
    ctl = L->ctl ? L->ctl : &dummy_ctl;
    game_hwnd = L->game_window();
    if (game_hwnd) ctl->hwnd = (u32)game_hwnd;
    FC = L->persist("freecam", sizeof(struct freecam), NULL);
    frame_cap_kept = L->persist("frame_cap", sizeof(u32), NULL);
    if (frame_cap_kept) frame_cap = *frame_cap_kept;
    if (!FC) FC = &fc_fallback;
    real_wglGetProcAddress = (void *)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglGetProcAddress");
    p_cgGetProfileString = *(void **)IAT_cgGetProfileString;
    p_cgGetParameterName = *(void **)IAT_cgGetParameterName;
    if (cg) p_cgGetParameterProgram = (void *)GetProcAddress(cg, "cgGetParameterProgram");
    if (cg) p_cgGetNamedParameter = (void *)GetProcAddress(cg, "cgGetNamedParameter");

    real_SwapBuffers = hook_iat(IAT_SwapBuffers, my_SwapBuffers);
    real_glDrawElements = hook_iat(IAT_glDrawElements, my_glDrawElements);
    real_glDrawArrays = hook_iat(IAT_glDrawArrays, my_glDrawArrays);
    real_glBegin = hook_iat(IAT_glBegin, my_glBegin);
    real_glCallList = hook_iat(IAT_glCallList, my_glCallList);
    real_cgGLSetMatrixParameterfr = hook_iat(IAT_cgGLSetMatrixParameterfr, my_cgGLSetMatrixParameterfr);
    real_cgGLSetParameter3f = hook_iat(IAT_cgGLSetParameter3f, my_cgGLSetParameter3f);
    real_cgGLSetParameter4f = hook_iat(IAT_cgGLSetParameter4f, my_cgGLSetParameter4f);
    real_cgGLBindProgram = hook_iat(IAT_cgGLBindProgram, my_cgGLBindProgram);
    real_cgGLEnableProfile = hook_iat(IAT_cgGLEnableProfile, my_cgGLEnableProfile);
    real_cgGetFirstParameter = hook_iat(IAT_cgGetFirstParameter, my_cgGetFirstParameter);
    real_cgGetNextParameter = hook_iat(IAT_cgGetNextParameter, my_cgGetNextParameter);
    real_cgGLSetTextureParameter = hook_iat(IAT_cgGLSetTextureParameter, my_cgGLSetTextureParameter);
    real_glVertexPointer = hook_iat(IAT_glVertexPointer, my_glVertexPointer);
    real_glTexCoordPointer = hook_iat(IAT_glTexCoordPointer, my_glTexCoordPointer);
    real_glNormalPointer = hook_iat(IAT_glNormalPointer, my_glNormalPointer);
    real_glEnableClientState = hook_iat(IAT_glEnableClientState, my_glEnableClientState);
    real_glDisableClientState = hook_iat(IAT_glDisableClientState, my_glDisableClientState);
    real_glEnable = hook_iat(IAT_glEnable, my_glEnable);
    real_glDisable = hook_iat(IAT_glDisable, my_glDisable);
    real_glAlphaFunc = hook_iat(IAT_glAlphaFunc, my_glAlphaFunc);
    real_glPopAttrib = hook_iat(IAT_glPopAttrib, my_glPopAttrib);
    real_glPopClientAttrib = hook_iat(IAT_glPopClientAttrib, my_glPopClientAttrib);
    real_cgGLDisableProfile = hook_iat(IAT_cgGLDisableProfile, my_cgGLDisableProfile);
    real_glDeleteTextures = hook_iat(IAT_glDeleteTextures, my_glDeleteTextures);
    real_glTexImage2D = hook_iat(IAT_glTexImage2D, my_glTexImage2D);
    real_glBlendFunc = hook_iat(IAT_glBlendFunc, my_glBlendFunc);
    real_glColorPointer = hook_iat(IAT_glColorPointer, my_glColorPointer);

    sandbox_mode = L->sandbox_mode;
    settings_started = 1;               /* the sandbox setting only counts at start-up, in the loader */
    rt_start(L->persist);
    read_settings();

    for (i = 0; i < L->nprog; i++) register_program(&L->prog[i]);
    {
        void *h = L->bound_program(1);
        cur_vp = h ? find_prog(h) : -1;
        h = L->bound_program(0);
        cur_fp = h ? find_prog(h) : -1;
        vp_on = vertex_profile >= 0 && L->profile_enabled(vertex_profile);
        fp_on = fragment_profile >= 0 && L->profile_enabled(fragment_profile);
    }
    if (wglGetCurrentContext()) {       /* the GL state we track, as it is now */
        read_array_state();
        gs.alpha_test = glIsEnabled(GL_ALPHA_TEST);
        gs.blend = glIsEnabled(GL_BLEND);
        glGetFloatv(GL_ALPHA_TEST_REF, &gs.alpha_ref);
        {
            GLint bv = 0;
            glGetIntegerv(GL_BLEND_SRC, &bv); gs.blend_src = (GLenum)bv;
            glGetIntegerv(GL_BLEND_DST, &bv); gs.blend_dst = (GLenum)bv;
        }
    }
    out->version = WUMRT_API_VERSION;
    out->wrap_proc = core_wrap_proc;
    out->program_created = register_program;
    out->programs_replaced = programs_replaced;
    out->shutdown = core_shutdown;
    logmsg("core %u started (%s mode): %d programs, vertex program %d, fragment program %d bound", L->generation,
           sandbox_mode ? "sandbox" : "play", nprogs, cur_vp, cur_fp);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) core_heap = HeapCreate(0, 0, 0);
    else if (reason == DLL_PROCESS_DETACH && core_heap) { HeapDestroy(core_heap); core_heap = NULL; }
    return core_heap != NULL;
}
