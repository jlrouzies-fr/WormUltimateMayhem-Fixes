/*
 * Light from effects. The game draws its explosions, fire, sparks and other glowing effects as
 * camera-facing textured quads with additive blending (FFVertexMainTexCol, vertices in plain
 * memory, a vertex colour per corner that fades them out). Each such draw is a source of light:
 * its glowing area (quad area x vertex colour x the texture's average colour) at its centre.
 * Sources close together are merged into one light; lights are followed from frame to frame,
 * so an explosion can flash when it appears, fade after its sprites have gone and flicker
 * while it burns. The next frame's tracer traces one shadow ray per pixel towards each of the
 * four brightest (from a random point of the light's ball, so shadows are soft), accumulates
 * those over time and adds the light of all of them to the indirect light the game's shaders
 * use (wumrt_fx.comp). The indirect light texture is also what the sky rays read where they
 * hit something on screen, so the light bounces.
 *
 * Settings (wumrt.ini): fx (0/1), fx_strength, fx_flash, fx_decay (seconds), fx_flicker,
 * fx_shadows (0..4), fx_max (lights, up to 16), fx_alpha (share of alpha-blended fire-coloured
 * sprites that counts as glowing; 0 = additive only), fx_log (1 = log what is found).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fx.h"

#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_RGBA16F 0x881A
#define GL_READ_ONLY 0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_READ_WRITE 0x88BA
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x20
#define GL_CLAMP_TO_EDGE 0x812F
#define FX_GL_TIMESTAMP 0x8E28
#define FX_GL_QUERY_RESULT 0x8866
#define FX_GL_QUERY_RESULT_AVAILABLE 0x8867

/* Cost (fx_log >= 1): CPU time of the draw scan and the light tracking, and GPU time of the three
   passes (timestamp queries, so they can sit inside the tracer's own elapsed-time query). */
static struct { LARGE_INTEGER f; double draws, lights; u32 frames; double gpu; u32 gpu_n; } C;
static double now_ms(void)
{
    LARGE_INTEGER t;
    if (!C.f.QuadPart) QueryPerformanceFrequency(&C.f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)C.f.QuadPart;
}

#define MAX_SRC 256
#define MAX_TRK 32
#define MAX_GPU 16
#define MAX_CAMS_FX 4

static struct {
    int on, log, shadows, max;
    float strength, flash, decay, flicker, alpha, ember, ember_time;
    float test, test_height;            /* a test light where the view's centre looks (power; 0 = none) */
    float knee;                         /* powers above this grow only logarithmically (0 = no knee) */
} S;

/* Big explosions are drawn with sprites of 100+ units, so their summed power reaches 20000 while
   a single fireball is ~1500: above the knee the power grows with the log, so a big blast is
   brighter but does not light the whole level flat. */
static float knee(float p)
{
    if (S.knee <= 0 || p <= S.knee) return p;
    return S.knee * (1.0f + (float)log(p / S.knee));
}

static void defaults(void)
{
    S.on = 0; S.log = 0; S.shadows = 4; S.max = 16;
    S.strength = 1.0f; S.flash = 2.0f; S.decay = 0.35f; S.flicker = 0.2f; S.alpha = 1.0f;
    S.ember = 0.15f; S.ember_time = 1.5f;
    S.test = 0; S.test_height = 30.0f;
    S.knee = 1000.0f;
}

void fx_setting(const char *name, float v)
{
    static int started;
    if (!started) { defaults(); started = 1; }
    if (!name) { defaults(); return; }
    if (!strcmp(name, "fx")) S.on = v != 0;
    else if (!strcmp(name, "fx_log")) S.log = (int)v;
    else if (!strcmp(name, "fx_shadows")) S.shadows = v < 0 ? 0 : v > 4 ? 4 : (int)v;
    else if (!strcmp(name, "fx_max")) S.max = v < 1 ? 1 : v > MAX_GPU ? MAX_GPU : (int)v;
    else if (!strcmp(name, "fx_strength")) S.strength = v;
    else if (!strcmp(name, "fx_flash")) S.flash = v;
    else if (!strcmp(name, "fx_decay")) S.decay = v;
    else if (!strcmp(name, "fx_flicker")) S.flicker = v;
    else if (!strcmp(name, "fx_alpha")) S.alpha = v;
    else if (!strcmp(name, "fx_ember")) S.ember = v;
    else if (!strcmp(name, "fx_ember_time")) S.ember_time = v;
    else if (!strcmp(name, "fx_test")) S.test = v;
    else if (!strcmp(name, "fx_test_height")) S.test_height = v;
    else if (!strcmp(name, "fx_knee")) S.knee = v;
}

/* ---- texture colours: average of colour x alpha, and of alpha ----------------------- */
static struct { GLuint tex; float rgb[3], prem[3], a; } tcol[256];
static u32 ntcol;

static const float *tex_colour(GLuint tex)
{
    static const float white[7] = { 1, 1, 1, 1, 1, 1, 1 };
    GLint prev = 0, w = 0, h = 0, level;
    u32 i;
    u8 *buf;
    for (i = 0; i < ntcol; i++)
        if (tcol[i].tex == tex) return tcol[i].rgb;
    if (!tex || ntcol == 256) return white;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (level = 0; level < 14; level++) {
        GLint nw = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level + 1, GL_TEXTURE_WIDTH, &nw);
        if (nw < 8) break;
    }
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
    buf = (w > 0 && h > 0 && w <= 4096 && h <= 4096) ? malloc((size_t)w * h * 4) : NULL;
    memset(&tcol[ntcol], 0, sizeof tcol[0]);
    tcol[ntcol].tex = tex;
    tcol[ntcol].rgb[0] = tcol[ntcol].rgb[1] = tcol[ntcol].rgb[2] = 1;
    tcol[ntcol].prem[0] = tcol[ntcol].prem[1] = tcol[ntcol].prem[2] = 1;
    tcol[ntcol].a = 1;
    if (buf) {
        double s[3] = { 0, 0, 0 }, p[3] = { 0, 0, 0 }, a = 0;
        u32 n = (u32)w * (u32)h, k;
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glPopClientAttrib();
        for (i = 0; i < n; i++) {
            double al = buf[i * 4 + 3] / 255.0;
            for (k = 0; k < 3; k++) { s[k] += buf[i * 4 + k] / 255.0; p[k] += buf[i * 4 + k] / 255.0 * al; }
            a += al;
        }
        for (k = 0; k < 3; k++) { tcol[ntcol].rgb[k] = (float)(s[k] / n); tcol[ntcol].prem[k] = (float)(p[k] / n); }
        tcol[ntcol].a = (float)(a / n);
        free(buf);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    return tcol[ntcol++].rgb;
}

void fx_texture_gone(GLuint id)
{
    u32 i;
    for (i = 0; i < ntcol; i++)
        if (tcol[i].tex == id) { tcol[i] = tcol[--ntcol]; return; }
}

/* ---- sources: one per glowing draw ------------------------------------------------- */
struct src { float pos[3], radius, power[3]; };
static struct src srcs[MAX_SRC];
static u32 nsrc;
static struct { u32 tex, src, dst, prims; float power, pos[3], radius, tc[3]; int glow; } seen[32];
static u32 nseen, seen_glow;
static u32 rej[8], last_blend;

static float lum3(const float *c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

static int read_index(const struct rt_draw *d, const u8 *ibase, u32 ilimit, u32 i, u32 *out)
{
    if (d->arrays) { *out = d->first + i; return 1; }
    if (!ibase) return 0;
    if (d->itype == GL_UNSIGNED_SHORT) { if ((i + 1) * 2 > ilimit) return 0; *out = ((const unsigned short *)ibase)[i]; }
    else if (d->itype == GL_UNSIGNED_INT) { if ((i + 1) * 4 > ilimit) return 0; *out = ((const u32 *)ibase)[i]; }
    else if (d->itype == GL_UNSIGNED_BYTE) { if (i + 1 > ilimit) return 0; *out = ibase[i]; }
    else return 0;
    return 1;
}

static void vertex_colour(const struct fx_draw_info *f, const u8 *cbase, u32 climit, u32 v, float *c)
{
    u32 off;
    c[0] = c[1] = c[2] = c[3] = 1;
    if (!f->col_on || !cbase || f->col_size < 3) return;
    if (f->col_type == GL_UNSIGNED_BYTE) {
        off = v * (f->col_stride ? f->col_stride : f->col_size);
        if (off + f->col_size > climit) return;
        c[0] = cbase[off] / 255.0f; c[1] = cbase[off + 1] / 255.0f; c[2] = cbase[off + 2] / 255.0f;
        c[3] = f->col_size > 3 ? cbase[off + 3] / 255.0f : 1;
    } else if (f->col_type == GL_FLOAT) {
        const float *p;
        off = v * (f->col_stride ? f->col_stride : f->col_size * 4);
        if (off + f->col_size * 4 > climit) return;
        p = (const float *)(cbase + off);
        c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; c[3] = f->col_size > 3 ? p[3] : 1;
    }
}

/* Is a colour fire-like (for alpha-blended sprites with fx_alpha > 0): bright, red over blue. */
static float fireness(const float *c)
{
    float l = lum3(c);
    if (c[0] < 0.35f || c[0] < c[2] + 0.25f || c[0] + 0.05f < c[1]) return 0;
    return l > 1 ? 1 : l;
}

static void on_draw(const struct fx_draw_info *f)
{
    const struct rt_draw *d = f->d;
    const float *m = f->model;
    const float *tc;
    u32 per, nprim, p, k, n, stride;
    float power[3] = { 0, 0, 0 }, wsum = 0, cen[3] = { 0, 0, 0 }, lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    float tcp[3], texrgb[3] = { 1, 1, 1 }, weight;
    int additive, alpha_blend;
    const u8 *vbase, *ibase, *cbase;
    u32 vlimit, ilimit, climit;
    if (!S.on && !S.log) return;
    rej[0]++;
    additive = f->blend_dst == GL_ONE && (f->blend_src == GL_SRC_ALPHA || f->blend_src == GL_ONE);
    alpha_blend = f->blend_src == GL_SRC_ALPHA && f->blend_dst == GL_ONE_MINUS_SRC_ALPHA;
    if (!additive && !(alpha_blend && (S.alpha > 0 || S.log))) { rej[1]++; last_blend = f->blend_src << 16 | f->blend_dst; return; }
    if (!d->v_on || d->v_type != GL_FLOAT || d->v_size < 3) { rej[2]++; return; }
    vbase = (const u8 *)d->v_ptr;
    vlimit = 0xffffffffu;
    if (d->v_buf) {
        u32 size = 0;
        const u8 *b = rt_buffer_data(d->v_buf, &size, NULL);
        if (!b || (u32)d->v_ptr >= size) { rej[2]++; return; }
        vbase = b + (u32)d->v_ptr;
        vlimit = size - (u32)d->v_ptr;
    } else if (!vbase) { rej[2]++; return; }
    ibase = (const u8 *)d->indices;
    ilimit = 0xffffffffu;
    if (!d->arrays && d->elem_buf) {
        u32 size = 0;
        const u8 *b = rt_buffer_data(d->elem_buf, &size, NULL);
        if (!b || (u32)d->indices >= size) { rej[2]++; return; }
        ibase = b + (u32)d->indices;
        ilimit = size - (u32)d->indices;
    }
    cbase = (const u8 *)f->col_ptr;
    climit = 0xffffffffu;
    if (f->col_on && f->col_buf) {
        u32 size = 0;
        const u8 *b = rt_buffer_data(f->col_buf, &size, NULL);
        if (b && (u32)f->col_ptr < size) { cbase = b + (u32)f->col_ptr; climit = size - (u32)f->col_ptr; }
        else cbase = NULL;
    }
    if (!f->view || !rt_is_main_view(f->view)) { rej[3]++; return; }
    /* Primitives: quads and triangles as they come; strips and fans one triangle per vertex
       after the first two (the corner indices are worked out below). */
    n = d->count > 4096 ? 4096 : d->count;
    if (d->mode == GL_QUADS) { per = 4; nprim = n / 4; }
    else if (d->mode == GL_TRIANGLES) { per = 3; nprim = n / 3; }
    else if (d->mode == GL_TRIANGLE_STRIP || d->mode == GL_TRIANGLE_FAN) { per = 3; nprim = n >= 3 ? n - 2 : 0; }
    else { rej[4]++; return; }
    if (!nprim) return;
    stride = d->v_stride ? d->v_stride : d->v_size * 4;
    {
        u32 i;
        tex_colour(d->texture);
        for (i = 0; i < ntcol && tcol[i].tex != d->texture; i++) ;
        if (i < ntcol) memcpy(texrgb, tcol[i].rgb, sizeof texrgb);
        if (i < ntcol) tc = f->blend_src == GL_ONE ? tcol[i].rgb : tcol[i].prem;
        else { static const float one[3] = { 1, 1, 1 }; tc = one; }
        memcpy(tcp, tc, sizeof tcp);
    }
    for (p = 0; p < nprim; p++) {
        float w[4][3], col[4] = { 0, 0, 0, 0 }, area, e[3], a[3], b[3], cr[3], l;
        u32 j;
        for (j = 0; j < per; j++) {
            u32 vi;
            const float *v;
            float vc[4];
            u32 at = d->mode == GL_TRIANGLE_STRIP ? p + j : d->mode == GL_TRIANGLE_FAN ? (j == 0 ? 0 : p + j) : p * per + j;
            if (!read_index(d, ibase, ilimit, at, &vi)) return;
            if ((u64)vi * stride + 12 > vlimit) return;
            v = (const float *)(vbase + (size_t)vi * stride);
            for (k = 0; k < 3; k++) w[j][k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + m[12 + k];
            vertex_colour(f, cbase, climit, vi, vc);
            for (k = 0; k < 4; k++) col[k] += vc[k] / (float)per;
        }
        for (k = 0; k < 3; k++) { a[k] = w[1][k] - w[0][k]; b[k] = w[per - 1][k] - w[0][k]; }
        if (per == 4) for (k = 0; k < 3; k++) b[k] = w[2][k] - w[0][k];
        cr[0] = a[1] * b[2] - a[2] * b[1]; cr[1] = a[2] * b[0] - a[0] * b[2]; cr[2] = a[0] * b[1] - a[1] * b[0];
        area = (float)sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]) * (per == 4 ? 1.0f : 0.5f);
        weight = f->blend_src == GL_SRC_ALPHA ? col[3] : 1.0f;
        if (!additive) {
            float lit[3];
            for (k = 0; k < 3; k++) lit[k] = col[k] * texrgb[k];
            weight *= (S.alpha > 0 ? S.alpha : S.log ? 1.0f : 0.0f) * fireness(lit);
        }
        for (k = 0; k < 3; k++) e[k] = area * col[k] * weight * tcp[k];
        l = lum3(e);
        if (l <= 0) continue;
        for (k = 0; k < 3; k++) {
            float c = 0;
            for (j = 0; j < per; j++) {
                c += w[j][k] / (float)per;
                if (w[j][k] < lo[k]) lo[k] = w[j][k];
                if (w[j][k] > hi[k]) hi[k] = w[j][k];
            }
            cen[k] += c * l;
            power[k] += e[k];
        }
        wsum += l;
    }
    if (wsum <= 0) return;
    for (k = 0; k < 3; k++) cen[k] /= wsum;
    {
        float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
        float r = 0.5f * (float)sqrt(dx * dx + dy * dy + dz * dz);
        if (r > 150) return;                    /* not an effect: something sky- or screen-sized (a shock ring is ~180) */
        if (S.log && nseen < 32) {
            seen[nseen].tex = d->texture; seen[nseen].src = f->blend_src; seen[nseen].dst = f->blend_dst;
            seen[nseen].prims = nprim; seen[nseen].power = lum3(power); seen[nseen].radius = r;
            memcpy(seen[nseen].pos, cen, 12); memcpy(seen[nseen].tc, tcp, 12);
            seen[nseen].glow = additive ? 1 : 2;
            seen_glow += seen[nseen].glow == 1;
            nseen++;
        }
        if (!S.on || !(additive || S.alpha > 0) || nsrc == MAX_SRC) return;
        memcpy(srcs[nsrc].pos, cen, 12);
        srcs[nsrc].radius = r;
        memcpy(srcs[nsrc].power, power, 12);
        nsrc++;
    }
}

/* ---- census of unlit draws (fx_log = 2) ------------------------------------------- */
static struct { char vp[40], fp[40]; u32 src, dst, tex; int blend; u32 n; } kinds[256];
static u32 nkinds;

void fx_census(const char *vp, const char *fp, const float *view, int blend, u32 src, u32 dst, u32 tex,
               GLenum mode, u32 count, int vbo)
{
    u32 i;
    const float *c;
    if (S.log < 2 || !view || !rt_is_main_view(view)) return;
    for (i = 0; i < nkinds; i++)
        if (kinds[i].src == src && kinds[i].dst == dst && kinds[i].tex == tex && kinds[i].blend == blend &&
            !strcmp(kinds[i].vp, vp) && !strcmp(kinds[i].fp, fp)) { kinds[i].n++; return; }
    if (nkinds == 256) return;
    strncpy(kinds[nkinds].vp, vp, 39); strncpy(kinds[nkinds].fp, fp, 39);
    kinds[nkinds].src = src; kinds[nkinds].dst = dst; kinds[nkinds].tex = tex; kinds[nkinds].blend = blend; kinds[nkinds].n = 1;
    nkinds++;
    c = tex_colour(tex);
    {
        u32 k;
        for (k = 0; k < ntcol && tcol[k].tex != tex; k++) ;
        rt_log("fx: NEW unlit draw kind %u: %s / %s blend %d %#x/%#x tex %u (rgb %.2f %.2f %.2f, alpha %.2f) mode %u count %u %s",
               nkinds, vp, fp, blend, src, dst, tex, c[0], c[1], c[2], k < ntcol ? tcol[k].a : 1.0f, mode, count, vbo ? "vbo" : "memory");
    }
}

/* ---- tracked lights --------------------------------------------------------------- */
struct trk {
    u32 id;
    float pos[3], radius, power[3];     /* power: smoothed measurement (falls slowly, rises at once) */
    float age, lost;                    /* seconds since it appeared / since its sources were last seen */
    float phase[2];
    float peak, ember;                  /* brightest it got; the glow left behind after its sprites have gone */
    int boomed;
    int slot;                           /* shadowed slot 0..3, or -1 */
};
static struct trk trk[MAX_TRK];
static u32 ntrk, next_id = 1;

struct gpu_light { float pr[4], col[4]; };
static struct gpu_light glights[MAX_GPU];
static u32 n_glights, n_gshadow;
static float slot_reset[4];
static u32 slot_id[4];
static double last_time;
static u32 frames_logged, frames_dbg;

static double now_s(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static float rnd(u32 *s) { *s = *s * 1664525u + 1013904223u; return (float)(*s >> 8) / 16777216.0f; }

static void frame_end(void)
{
    struct src cl[MAX_SRC];
    u32 ncl = 0, i, j, k;
    double t = now_s();
    float dt = last_time > 0 ? (float)(t - last_time) : 0.033f;
    static u32 seed = 12345;
    last_time = t;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0) dt = 0;

    if (S.log && nseen && (frames_logged++ % 15) == 0) {
        rt_log("fx: %u effect draws (%u glowing), %u sources, %u lights", nseen, seen_glow, nsrc, n_glights);
        for (i = 0; i < nseen; i++)
            rt_log("fx:   tex %u blend %#x/%#x %u prims power %.2f at %.1f %.1f %.1f r %.1f tex colour %.2f %.2f %.2f%s",
                   seen[i].tex, seen[i].src, seen[i].dst, seen[i].prims, seen[i].power, seen[i].pos[0], seen[i].pos[1],
                   seen[i].pos[2], seen[i].radius, seen[i].tc[0], seen[i].tc[1], seen[i].tc[2], seen[i].glow == 1 ? " GLOW" : " FIRE?");
    }
    nseen = seen_glow = 0;
    if (S.log && (++frames_dbg % 150) == 0) { rt_log("fx: draws seen %u, not glowing %u (last blend %#x), not plain memory %u, other camera %u, other mode %u", rej[0], rej[1], last_blend, rej[2], rej[3], rej[4]); memset(rej, 0, sizeof rej); }

    if (!S.on) { nsrc = 0; ntrk = 0; n_glights = 0; return; }

    /* A test light (fx_test): placed once where the centre of the view looks, raised a little,
       then kept there as a steady orange source, to try the lighting without an explosion. */
    {
        static float test_pos[3];
        static int test_placed;
        if (S.test <= 0) test_placed = 0;
        else {
            if (!test_placed) {
                float d = rt_center_distance();
                if (d > 0) {
                    for (k = 0; k < 3; k++) test_pos[k] = rt_stats.cam_eye[k] + rt_stats.cam_fwd[k] * d;
                    test_pos[1] += S.test_height;
                    test_placed = 1;
                    rt_log("fx: test light at %.1f %.1f %.1f (%.1f ahead), power %.1f", test_pos[0], test_pos[1], test_pos[2], d, S.test);
                }
            }
            if (test_placed && nsrc < MAX_SRC) {
                memcpy(srcs[nsrc].pos, test_pos, 12);
                srcs[nsrc].radius = 6.0f;
                srcs[nsrc].power[0] = S.test * 1.6f; srcs[nsrc].power[1] = S.test * 0.75f; srcs[nsrc].power[2] = S.test * 0.25f;
                nsrc++;
            }
        }
    }

    /* 1. Merge sources that overlap (an explosion is several draws: core, sparks, flames). */
    for (i = 0; i < nsrc; i++) {
        struct src *s = &srcs[i];
        float ls = lum3(s->power);
        for (j = 0; j < ncl; j++) {
            float d2 = 0, reach = cl[j].radius + s->radius + 15.0f;     /* world units: a worm is ~25 tall */
            for (k = 0; k < 3; k++) d2 += (cl[j].pos[k] - s->pos[k]) * (cl[j].pos[k] - s->pos[k]);
            if (d2 < reach * reach) break;
        }
        if (j < ncl) {
            float lc = lum3(cl[j].power), wsum = lc + ls, d = 0;
            for (k = 0; k < 3; k++) d += (cl[j].pos[k] - s->pos[k]) * (cl[j].pos[k] - s->pos[k]);
            d = (float)sqrt(d);
            if (wsum > 0)
                for (k = 0; k < 3; k++) cl[j].pos[k] = (cl[j].pos[k] * lc + s->pos[k] * ls) / wsum;
            if (s->radius + d * 0.5f > cl[j].radius) cl[j].radius = s->radius + d * 0.5f;
            for (k = 0; k < 3; k++) cl[j].power[k] += s->power[k];
        } else if (ncl < MAX_SRC) {
            cl[ncl++] = *s;
        }
    }
    nsrc = 0;

    /* 2. Follow lights from frame to frame. */
    for (i = 0; i < ntrk; i++) trk[i].lost += dt;
    for (j = 0; j < ncl; j++) {
        int best = -1;
        float bd = 1e30f;
        for (i = 0; i < ntrk; i++) {
            float d2 = 0, reach = trk[i].radius + cl[j].radius + 25.0f;
            for (k = 0; k < 3; k++) d2 += (trk[i].pos[k] - cl[j].pos[k]) * (trk[i].pos[k] - cl[j].pos[k]);
            if (d2 < reach * reach && d2 < bd && trk[i].lost > 0) { bd = d2; best = (int)i; }
        }
        if (best < 0) {
            if (ntrk == MAX_TRK) {          /* replace the dimmest */
                float lmin = 1e30f;
                for (i = 0; i < ntrk; i++)
                    if (lum3(trk[i].power) < lmin) { lmin = lum3(trk[i].power); best = (int)i; }
                if (lmin > lum3(cl[j].power)) continue;
            } else best = (int)ntrk++;
            memset(&trk[best], 0, sizeof trk[best]);
            trk[best].id = next_id++;
            trk[best].slot = -1;
            trk[best].phase[0] = rnd(&seed) * 6.2831853f;
            trk[best].phase[1] = rnd(&seed) * 6.2831853f;
            memcpy(trk[best].pos, cl[j].pos, 12);
            trk[best].radius = cl[j].radius;
        }
        {
            struct trk *L = &trk[best];
            float fall = (float)exp(-dt / 0.08f);
            for (k = 0; k < 3; k++) {
                L->pos[k] += (cl[j].pos[k] - L->pos[k]) * 0.5f;
                L->power[k] = cl[j].power[k] > L->power[k] * fall ? cl[j].power[k] : L->power[k] * fall;
            }
            L->radius += (cl[j].radius - L->radius) * 0.5f;
            L->lost = 0;
            if (lum3(L->power) > L->peak) L->peak = lum3(L->power);
            if (S.log && !L->boomed && L->age < 1.0f && L->peak > 400.0f) {
                L->boomed = 1;
                rt_log("fx: BOOM new bright light %u at %.1f %.1f %.1f r %.1f power %.0f", L->id, L->pos[0], L->pos[1], L->pos[2], L->radius, L->peak);
            }
        }
    }
    /* Lights whose sprites are gone fade out (afterglow), then are dropped. */
    for (i = 0; i < ntrk; i++) {
        struct trk *L = &trk[i];
        L->age += dt;
        if (L->lost > 0) {
            float f = S.decay > 0.01f ? (float)exp(-dt / S.decay) : 0.0f;
            for (k = 0; k < 3; k++) L->power[k] *= f;
            /* An explosion (not a long fire) leaves its ground glowing for a moment. */
            if (L->lost <= dt + 1e-4f && L->age < 4.0f) L->ember = L->peak * S.ember;
            L->ember *= S.ember_time > 0.01f ? (float)exp(-dt / S.ember_time) : 0.0f;
        }
        if ((lum3(L->power) < 0.02f && L->ember < 0.02f) || L->lost > 3.0f + S.ember_time * 2) { trk[i--] = trk[--ntrk]; }
    }

    /* 3. This frame's light list: intensity with the flash and the flicker, shadowed slots kept stable. */
    {
        float inten[MAX_TRK];
        int order[MAX_TRK];
        u32 nsh = (u32)S.shadows, used = 0, s;
        for (i = 0; i < ntrk; i++) {
            struct trk *L = &trk[i];
            float m = S.strength * (1.0f + S.flash * (float)exp(-L->age / 0.12f));
            if (S.flicker > 0 && L->age > 0.6f) {
                float tt = L->age;
                float nz = 0.5f * (float)sin(tt * 45.9f + L->phase[0]) + 0.3f * (float)sin(tt * 82.3f + L->phase[1]) +
                           0.2f * (rnd(&seed) * 2 - 1);
                m *= 1.0f + S.flicker * nz;
            }
            inten[i] = m;
            order[i] = (int)i;
        }
        for (i = 1; i < ntrk; i++)          /* brightest first */
            for (j = i; j > 0 && lum3(trk[order[j]].power) * inten[order[j]] > lum3(trk[order[j - 1]].power) * inten[order[j - 1]]; j--) {
                int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x;
            }
        /* shadowed slots: the brightest nsh lights; one that keeps its place keeps its slot */
        for (s = 0; s < 4; s++) slot_reset[s] = 0;
        {
            int want[4] = { -1, -1, -1, -1 }, taken[4] = { 0, 0, 0, 0 };
            u32 nwant = ntrk < nsh ? ntrk : nsh;
            for (i = 0; i < ntrk; i++) trk[i].slot = -1;
            for (i = 0; i < nwant; i++) {
                for (s = 0; s < 4; s++)
                    if (slot_id[s] == trk[order[i]].id) break;
                if (s < nsh) { trk[order[i]].slot = (int)s; taken[s] = 1; want[i] = (int)s; }
            }
            for (i = 0; i < nwant; i++) {
                if (want[i] >= 0) continue;
                for (s = 0; s < nsh && taken[s]; s++) ;
                if (s == nsh) break;
                taken[s] = 1;
                trk[order[i]].slot = (int)s;
                slot_id[s] = trk[order[i]].id;
                slot_reset[s] = 1;
            }
            for (s = 0; s < 4; s++) if (!taken[s]) slot_id[s] = 0;
        }
        memset(glights, 0, sizeof glights);
        n_gshadow = nsh;
        used = nsh;
        for (i = 0; i < ntrk; i++) {
            struct trk *L = &trk[order[i]];
            struct gpu_light *g;
            float lum, reach;
            if (L->slot >= 0) g = &glights[L->slot];
            else if (used < (u32)S.max && used < MAX_GPU) g = &glights[used++];
            else continue;
            {
                static const float ember_rgb[3] = { 2.0f, 0.55f, 0.15f };      /* luminance 0.66 per unit */
                float base = lum3(L->power), c = base > 0 ? knee(base) / base : 0, em = knee(L->ember);
                for (k = 0; k < 3; k++) { g->pr[k] = L->pos[k]; g->col[k] = L->power[k] * c * inten[order[i]] + em * S.strength * ember_rgb[k] / 0.66f; }
            }
            g->pr[3] = L->radius > 0.5f ? L->radius * 0.6f : 0.3f;
            if (g->pr[3] > 30.0f) g->pr[3] = 30.0f;     /* a big cluster is several fireballs, not one huge ball */
            lum = lum3(g->col);
            /* where its light falls below about 1% of the ambient light */
            reach = (float)sqrt(lum / 0.005f);
            if (reach < g->pr[3] * 2) reach = g->pr[3] * 2;
            if (reach > 800) reach = 800;
            g->col[3] = lum > 0 ? reach : 0;
        }
        n_glights = ntrk ? used : 0;
        if (S.log && ntrk && (frames_logged % 15) == 1)
            for (i = 0; i < ntrk && i < 6; i++) {
                struct trk *L = &trk[order[i]];
                rt_log("fx: light %u slot %d age %.2f lost %.2f at %.1f %.1f %.1f r %.1f power %.2f x %.2f",
                       L->id, L->slot, L->age, L->lost, L->pos[0], L->pos[1], L->pos[2], L->radius, lum3(L->power), inten[order[i]]);
            }
    }
}

void fx_on_draw(const struct fx_draw_info *f)
{
    double t0 = S.log ? now_ms() : 0;
    on_draw(f);
    if (S.log) C.draws += now_ms() - t0;
}

void fx_frame_end(void)
{
    double t0 = S.log ? now_ms() : 0;
    frame_end();
    if (!S.log) return;
    C.lights += now_ms() - t0;
    if (++C.frames >= 90) {
        rt_log("fx: cost per frame over %u frames: CPU %.3f ms (draw scan %.3f, lights %.3f); GPU %.3f ms over %u traced frames",
               C.frames, (C.draws + C.lights) / C.frames, C.draws / C.frames, C.lights / C.frames,
               C.gpu_n ? C.gpu / C.gpu_n : 0.0, C.gpu_n);
        C.draws = C.lights = C.gpu = 0; C.frames = C.gpu_n = 0;
    }
}

/* ---- GPU --------------------------------------------------------------------------- */
static struct {
    void (APIENTRY *UseProgram)(GLuint);
    GLint (APIENTRY *GetUniformLocation)(GLuint, const char *);
    void (APIENTRY *Uniform1i)(GLint, GLint);
    void (APIENTRY *Uniform3f)(GLint, float, float, float);
    void (APIENTRY *Uniform4f)(GLint, float, float, float, float);
    void (APIENTRY *UniformMatrix4fv)(GLint, GLsizei, GLboolean, const float *);
    void (APIENTRY *GenBuffers)(GLsizei, GLuint *);
    void (APIENTRY *DeleteBuffers)(GLsizei, const GLuint *);
    void (APIENTRY *BindBuffer)(GLenum, GLuint);
    void (APIENTRY *BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
    void (APIENTRY *BindBufferBase)(GLenum, GLuint, GLuint);
    void (APIENTRY *DispatchCompute)(GLuint, GLuint, GLuint);
    void (APIENTRY *MemoryBarrier)(GLbitfield);
    void (APIENTRY *BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    void (APIENTRY *TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (APIENTRY *DeleteProgram)(GLuint);
} G;
static int g_state;
static struct {
    void (APIENTRY *GenQueries)(GLsizei, GLuint *);
    void (APIENTRY *QueryCounter)(GLuint, GLenum);
    void (APIENTRY *GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
    void (APIENTRY *GetQueryObjectui64v)(GLuint, GLenum, unsigned long long *);
} Q;
static GLuint q_ids[4][2];
static int q_state, q_pending[4], q_next;

/* Reads the oldest finished pair, then starts a new one: 1 = before the passes, 0 = after. */
static void q_mark(int before)
{
    if (!q_state) {
        PROC *slot = (PROC *)&Q;
        static const char *names[] = { "glGenQueries", "glQueryCounter", "glGetQueryObjectuiv", "glGetQueryObjectui64v" };
        int i;
        q_state = 1;
        for (i = 0; i < 4; i++) if (!(slot[i] = rt_real_proc(names[i]))) q_state = -1;
        if (q_state > 0) Q.GenQueries(8, &q_ids[0][0]);
    }
    if (q_state < 0 || !S.log) return;
    if (before) {
        GLuint avail = 0;
        if (q_pending[q_next]) {
            Q.GetQueryObjectuiv(q_ids[q_next][1], FX_GL_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) {
                unsigned long long a = 0, b = 0;
                Q.GetQueryObjectui64v(q_ids[q_next][0], FX_GL_QUERY_RESULT, &a);
                Q.GetQueryObjectui64v(q_ids[q_next][1], FX_GL_QUERY_RESULT, &b);
                if (b > a) { C.gpu += (double)(b - a) / 1e6; C.gpu_n++; }
            }
            q_pending[q_next] = 0;
        }
        Q.QueryCounter(q_ids[q_next][0], FX_GL_TIMESTAMP);
    } else {
        Q.QueryCounter(q_ids[q_next][1], FX_GL_TIMESTAMP);
        q_pending[q_next] = 1;
        q_next = (q_next + 1) & 3;
    }
}

static int g_init(void)
{
    static const char *names[] = { "glUseProgram", "glGetUniformLocation", "glUniform1i", "glUniform3f", "glUniform4f",
        "glUniformMatrix4fv", "glGenBuffers", "glDeleteBuffers", "glBindBuffer", "glBufferData", "glBindBufferBase",
        "glDispatchCompute", "glMemoryBarrier", "glBindImageTexture", "glTexStorage2D", "glDeleteProgram" };
    PROC *slot = (PROC *)&G;
    int i;
    if (g_state) return g_state > 0;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!(slot[i] = rt_real_proc(names[i]))) { g_state = -1; rt_log("fx: %s missing; effect lights off", names[i]); return 0; }
    g_state = 1;
    return 1;
}

struct fxcam { GLuint vis, acc[2]; u32 w, h, cur, frame; };
static struct {
    GLuint tprog, cprog, buf;           /* shadow rays (wumrt_trace.comp + FX_PASS), accumulate/compose (wumrt_fx.comp) */
    GLuint for_main;                    /* the tracer program they were made with */
    int failed;
    GLint t_eye, t_base, t_dx, t_dy, t_counts, t_fx;
    GLint c_mode, c_eye, c_base, c_dx, c_dy, c_fx, c_reset, c_hist, c_prev_vp, c_prev_eye;
    struct fxcam cam[MAX_CAMS_FX];
    u32 uploaded_frame;
    double gpu_lights_logged;
} X;

GLuint fx_program(int i) { return i == 0 ? X.tprog : i == 1 ? X.cprog : 0; }

static int programs(const struct fx_view *v)
{
    char defs[256];
    if (X.tprog && X.cprog && X.for_main == v->main_prog) return 1;
    if (X.failed && X.for_main == v->main_prog) return 0;
    if (X.tprog) G.DeleteProgram(X.tprog);
    if (X.cprog) G.DeleteProgram(X.cprog);
    X.tprog = X.cprog = 0;
    X.for_main = v->main_prog;
    _snprintf(defs, sizeof defs, "#define FX_PASS 1\n%s", v->defines ? v->defines : "");
    defs[sizeof defs - 1] = 0;
    X.tprog = rt_compile_compute("wumrt_trace.comp", defs);
    X.cprog = rt_compile_compute("wumrt_fx.comp", NULL);
    if (!X.tprog || !X.cprog) {
        if (X.tprog) G.DeleteProgram(X.tprog);
        if (X.cprog) G.DeleteProgram(X.cprog);
        X.tprog = X.cprog = 0;
        X.failed = 1;
        rt_log("fx: effect light programs failed to compile; effect lights off until the next reload");
        return 0;
    }
    X.failed = 0;
    X.t_eye = G.GetUniformLocation(X.tprog, "uEye"); X.t_base = G.GetUniformLocation(X.tprog, "uBase");
    X.t_dx = G.GetUniformLocation(X.tprog, "uDx"); X.t_dy = G.GetUniformLocation(X.tprog, "uDy");
    X.t_counts = G.GetUniformLocation(X.tprog, "uCounts"); X.t_fx = G.GetUniformLocation(X.tprog, "uFx");
    X.c_mode = G.GetUniformLocation(X.cprog, "uMode");
    X.c_eye = G.GetUniformLocation(X.cprog, "uEye"); X.c_base = G.GetUniformLocation(X.cprog, "uBase");
    X.c_dx = G.GetUniformLocation(X.cprog, "uDx"); X.c_dy = G.GetUniformLocation(X.cprog, "uDy");
    X.c_fx = G.GetUniformLocation(X.cprog, "uFx"); X.c_reset = G.GetUniformLocation(X.cprog, "uReset");
    X.c_hist = G.GetUniformLocation(X.cprog, "uHist"); X.c_prev_vp = G.GetUniformLocation(X.cprog, "uPrevVP");
    X.c_prev_eye = G.GetUniformLocation(X.cprog, "uPrevEye");
    rt_log("fx: effect light programs ready");
    return 1;
}

static GLuint make_tex(u32 w, u32 h)
{
    GLuint t = 0;
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    G.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    return t;
}

void fx_trace(const struct fx_view *v)
{
    u32 gx, gy, nxt;
    int hist;
    struct fxcam *c;
    if (!S.on || !n_glights || v->cam < 0 || v->cam >= MAX_CAMS_FX) return;
    if (!g_init() || !programs(v)) return;
    c = &X.cam[v->cam];
    if (!c->vis || c->w != v->w || c->h != v->h) {
        if (c->vis) { glDeleteTextures(1, &c->vis); glDeleteTextures(2, c->acc); }
        c->vis = make_tex(v->w, v->h); c->acc[0] = make_tex(v->w, v->h); c->acc[1] = make_tex(v->w, v->h);
        c->w = v->w; c->h = v->h; c->frame = 0xfffffff0u;
    }
    hist = v->hist && c->frame + 1 == v->frame;
    nxt = c->cur ^ 1;
    if (!X.buf) G.GenBuffers(1, &X.buf);
    G.BindBuffer(GL_SHADER_STORAGE_BUFFER, X.buf);
    if (X.uploaded_frame != v->frame + 1) {
        G.BufferData(GL_SHADER_STORAGE_BUFFER, sizeof glights, glights, GL_DYNAMIC_DRAW);
        X.uploaded_frame = v->frame + 1;
    }
    G.BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    G.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, X.buf);
    gx = (v->w + 7) / 8; gy = (v->h + 7) / 8;
    q_mark(1);

    /* 1. Shadow rays (the scene's buffers are still bound from the main trace). */
    G.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    G.UseProgram(X.tprog);
    G.Uniform3f(X.t_eye, v->eye[0], v->eye[1], v->eye[2]);
    G.Uniform3f(X.t_base, v->base[0], v->base[1], v->base[2]);
    G.Uniform3f(X.t_dx, v->dx[0], v->dx[1], v->dx[2]);
    G.Uniform3f(X.t_dy, v->dy[0], v->dy[1], v->dy[2]);
    G.Uniform1i(X.t_counts, (GLint)v->tlas_count);
    G.Uniform4f(X.t_fx, (float)n_glights, (float)n_gshadow, (float)(v->frame & 63), 0.0f);
    G.BindImageTexture(1, v->geom, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    G.BindImageTexture(3, c->vis, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    G.DispatchCompute(gx, gy, 1);

    /* 2. Accumulate the visibilities. */
    G.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    G.UseProgram(X.cprog);
    G.Uniform3f(X.c_eye, v->eye[0], v->eye[1], v->eye[2]);
    G.Uniform3f(X.c_base, v->base[0], v->base[1], v->base[2]);
    G.Uniform3f(X.c_dx, v->dx[0], v->dx[1], v->dx[2]);
    G.Uniform3f(X.c_dy, v->dy[0], v->dy[1], v->dy[2]);
    G.Uniform4f(X.c_fx, (float)n_glights, (float)n_gshadow, 1.0f, 0.0f);
    G.Uniform4f(X.c_reset, slot_reset[0], slot_reset[1], slot_reset[2], slot_reset[3]);
    G.Uniform4f(X.c_hist, 0.3f, hist ? 1.0f : 0.0f, 0.0f, 0.0f);
    G.UniformMatrix4fv(X.c_prev_vp, 1, GL_FALSE, v->prev_vp);
    G.Uniform3f(X.c_prev_eye, v->prev_eye[0], v->prev_eye[1], v->prev_eye[2]);
    G.Uniform1i(X.c_mode, 0);
    G.BindImageTexture(1, v->geom, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    G.BindImageTexture(2, c->vis, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    G.BindImageTexture(4, c->acc[c->cur], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    G.BindImageTexture(5, v->prev_mask, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    G.BindImageTexture(0, c->acc[nxt], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    G.BindImageTexture(6, v->light, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
    G.DispatchCompute(gx, gy, 1);

    /* 3. Blur them along surfaces, add the light. */
    G.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    G.Uniform1i(X.c_mode, 1);
    G.BindImageTexture(2, c->acc[nxt], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    G.BindImageTexture(0, c->acc[c->cur], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    G.DispatchCompute(gx, gy, 1);
    G.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    q_mark(0);

    c->cur = nxt;
    c->frame = v->frame;
}

void fx_forget(void)
{
    memset(&X, 0, sizeof X);
    q_state = 0; memset(q_pending, 0, sizeof q_pending);
    ntcol = 0;
    g_state = 0;
}

void fx_shutdown(void)
{
    int i;
    if (g_state <= 0) return;
    for (i = 0; i < MAX_CAMS_FX; i++)
        if (X.cam[i].vis) { glDeleteTextures(1, &X.cam[i].vis); glDeleteTextures(2, X.cam[i].acc); }
    if (X.buf) G.DeleteBuffers(1, &X.buf);
    memset(X.cam, 0, sizeof X.cam);
    X.buf = 0;
}
