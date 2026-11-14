/*
 * wumrt loader - the permanent part of the harness. Loaded by the SANDBOX copy of Worms
 * Ultimate Mayhem as a stand-in for glut32.dll (the three glut calls are passed on to
 * glut32_real.dll). It owns what has to live as long as the process:
 *
 *   - the import-table slots of WormsMayhem.exe: every slot the core hooks points at a
 *     loader thunk that jumps to the core's function (or the real one when no core is
 *     loaded), and every function the engine fetches with wglGetProcAddress is handed out
 *     as such a thunk too, so a core can be unloaded at any frame boundary;
 *   - the control block shared with rt.py, the log, and the input isolation (sandbox mode);
 *   - the record of the Cg programs the engine created, and the translation of its program
 *     and parameter handles after the .cg files are compiled again (rt.py cg-reload), with
 *     the values the engine set re-applied to the new programs.
 *
 * Everything else (GL state tracking, the tracer, screenshots, commands) is in the core,
 * wumrt_core.dll, which is loaded from a uniquely named copy so that it can be rebuilt
 * while the game runs and swapped in with rt.py reload-core.
 *
 * 32-bit, WormsMayhem.exe build 1077 only (import table addresses are fixed).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "rt.h"
#include "ctl.h"
#include "wumrt_api.h"

#define EXE_TIMESTAMP        0x51828619u
/* import-table slots the loader handles itself */
#define IAT_DirectInput8Create 0x815008
#define IAT_SwapBuffers      0x815024
#define IAT_wglGetProcAddress 0x8154ac
#define IAT_SetCapture       0x81557c
#define IAT_ShowCursor       0x815580
#define IAT_ReleaseCapture   0x815584
#define IAT_SetKeyboardState 0x815598
#define IAT_SetCursorPos     0x81559c
#define IAT_GetFocus         0x8155a0
#define IAT_GetCursorPos     0x8155a4
#define IAT_GetKeyboardState 0x8155ac
#define IAT_GetForegroundWindow 0x815570
#define IAT_SetForegroundWindow 0x8155b8
#define IAT_SetFocus         0x8155bc
#define IAT_ShowWindow       0x8155b4
#define IAT_SetWindowPos     0x815594
#define IAT_ChangeDisplaySettingsA 0x8155d8
#define IAT_XInputSetState   0x815664
#define IAT_cgDestroyProgram 0x81567c
#define IAT_cgGetFirstStructParameter 0x815680
#define IAT_cgGetNextParameter 0x815684
#define IAT_cgGetArrayParameter 0x815688
#define IAT_cgGetFirstParameter 0x81569c
#define IAT_cgCreateProgramFromFile 0x8156a4
#define IAT_cgUpdateProgramParameters 0x8156a8
#define IAT_cgGLSetParameter2f 0x8156bc
#define IAT_cgGLDisableProfile 0x8156c0
#define IAT_cgGLSetParameter1f 0x8156c8
#define IAT_cgGLSetParameter4f 0x8156cc
#define IAT_cgGLSetParameter3f 0x8156d0
#define IAT_cgGLSetMatrixParameterfr 0x8156d4
#define IAT_cgGLEnableProfile 0x8156d8
#define IAT_cgGLEnableTextureParameter 0x8156dc
#define IAT_cgGLSetTextureParameter 0x8156e0
#define IAT_cgGLDisableTextureParameter 0x8156e4
#define IAT_cgGLLoadProgram  0x8156e8
#define IAT_cgGLBindProgram  0x8156ec

static struct ctl *ctl;
static struct ctl dummy_ctl;
static FILE *logf;
static CRITICAL_SECTION log_lock;
static HWND game_hwnd;
static int sandbox_mode;

static void vlogmsg(const char *fmt, va_list ap)
{
    if (!logf) return;
    EnterCriticalSection(&log_lock);
    fprintf(logf, "%8lu  ", (unsigned long)GetTickCount());
    vfprintf(logf, fmt, ap);
    fputc('\n', logf);
    fflush(logf);
    LeaveCriticalSection(&log_lock);
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlogmsg(fmt, ap);
    va_end(ap);
}

static void *patch_slot(u32 slot, void *fn)
{
    void **p = (void **)slot, *old;
    DWORD prot;
    VirtualProtect(p, sizeof *p, PAGE_READWRITE, &prot);
    old = *p;
    *p = fn;
    VirtualProtect(p, sizeof *p, prot, &prot);
    return old;
}

/* ---- glut pass-through --------------------------------------------------------- */
void *real_glut[3];
__asm__(".text\n"
        ".globl _stub_glut0\n_stub_glut0:\n jmp *_real_glut\n"
        ".globl _stub_glut1\n_stub_glut1:\n jmp *_real_glut+4\n"
        ".globl _stub_glut2\n_stub_glut2:\n jmp *_real_glut+8\n");

/* ---- thunks: stable addresses whose target can change ------------------------------
   Each thunk is `jmp dword ptr [dispatch[i]]`. They point at the core's function while a
   core is loaded and at the real one otherwise. Used for the import-table slots the core
   hooks and for every function the engine got from wglGetProcAddress. */
#define MAX_THUNKS 2048
static u8 *thunk_code;
static void *dispatch[MAX_THUNKS], *thunk_orig[MAX_THUNKS];
static u32 thunk_slot[MAX_THUNKS];          /* import-table slot, or 0 for a wglGetProcAddress function */
static char thunk_name[MAX_THUNKS][48];
static u32 nthunks;

static void *new_thunk(void *orig)
{
    u8 *c;
    if (!thunk_code) {
        thunk_code = VirtualAlloc(NULL, MAX_THUNKS * 8, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!thunk_code) return NULL;
    }
    if (nthunks == MAX_THUNKS) return NULL;
    c = thunk_code + nthunks * 8;
    dispatch[nthunks] = thunk_orig[nthunks] = orig;
    c[0] = 0xFF; c[1] = 0x25;                       /* jmp dword ptr [abs32] */
    *(u32 *)(c + 2) = (u32)&dispatch[nthunks];
    c[6] = 0xCC; c[7] = 0xCC;
    nthunks++;
    return c;
}

/* ---- slots the loader handles itself; the core may hook them too (chain) ------------- */
enum { O_SWAP, O_CREATE, O_BIND, O_LOAD, O_SET1, O_SET2, O_SET3, O_SET4, O_SETMAT, O_SETTEX, O_ENTEX, O_DISTEX,
       O_UPDATE, O_FIRST, O_NEXT, O_FIRSTSTRUCT, O_ARRAY, O_DESTROY, O_ENPROF, O_DISPROF, O_COUNT };
static struct owned { u32 slot; void *fn; void *orig; void *chain; } owned[O_COUNT];

static void *loader_hook_iat(u32 slot, void *fn)
{
    u32 i;
    if (slot == IAT_wglGetProcAddress) {
        logmsg("loader: the core asked to hook wglGetProcAddress; it gets wrap_proc calls instead");
        return (void *)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglGetProcAddress");
    }
    for (i = 0; i < O_COUNT; i++)
        if (owned[i].slot == slot) {
            owned[i].chain = fn ? fn : owned[i].orig;
            return owned[i].orig;
        }
    for (i = 0; i < nthunks; i++)
        if (thunk_slot[i] == slot) {
            dispatch[i] = fn ? fn : thunk_orig[i];
            return thunk_orig[i];
        }
    {
        void *orig = *(void **)slot, *t = new_thunk(orig);
        if (!t) { logmsg("loader: out of thunks"); return orig; }
        thunk_slot[nthunks - 1] = slot;
        patch_slot(slot, t);
        dispatch[nthunks - 1] = fn ? fn : orig;
        return orig;
    }
}

/* ---- memory that survives core reloads -------------------------------------------- */
#define MAX_PERSIST 64
static struct { char name[32]; u32 size; void *p; } persist[MAX_PERSIST];

static void *loader_persist(const char *name, u32 size, int *created)
{
    int i, freeslot = -1;
    if (created) *created = 0;
    for (i = 0; i < MAX_PERSIST; i++) {
        if (!persist[i].p) { if (freeslot < 0) freeslot = i; continue; }
        if (!strcmp(persist[i].name, name)) {
            if (persist[i].size == size) return persist[i].p;
            logmsg("loader: persistent block %s changed size (%u -> %u); starting it afresh", name, persist[i].size, size);
            HeapFree(GetProcessHeap(), 0, persist[i].p);
            persist[i].p = NULL;
            if (freeslot < 0) freeslot = i;
        }
    }
    if (freeslot < 0) return NULL;
    persist[freeslot].p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!persist[freeslot].p) return NULL;
    strncpy(persist[freeslot].name, name, sizeof persist[freeslot].name - 1);
    persist[freeslot].size = size;
    if (created) *created = 1;
    return persist[freeslot].p;
}

/* ---- the core ------------------------------------------------------------------------ */
static struct wumrt_loader L;
static struct wumrt_core core;
static HMODULE core_mod;
static int core_loaded, core_tried;
static char core_copy[MAX_PATH];

/* ---- Cg programs: record, translation, cached values, recompiling -------------------- */
typedef void *(__cdecl *create_fn)(void *, int, const char *, int, const char *, const char **);
typedef void (__cdecl *prog_fn)(void *);
typedef void *(__cdecl *first_fn)(void *, int);
typedef void *(__cdecl *param_fn)(void *);
typedef void *(__cdecl *array_fn)(void *, int);
typedef void (__cdecl *set1_fn)(void *, float);
typedef void (__cdecl *set2_fn)(void *, float, float);
typedef void (__cdecl *set3_fn)(void *, float, float, float);
typedef void (__cdecl *set4_fn)(void *, float, float, float, float);
typedef void (__cdecl *setm_fn)(void *, const float *);
typedef void (__cdecl *sett_fn)(void *, GLuint);
typedef void (__cdecl *prof_fn)(int);
#define CHAIN(o, t) ((t)owned[o].chain)
#define ORIG(o, t) ((t)owned[o].orig)

static const char *(__cdecl *p_cgGetProfileString)(int);
static const char *(__cdecl *p_cgGetParameterName)(void *);
static void *(__cdecl *p_cgGetParameterProgram)(void *);
static void *(__cdecl *p_cgGetNamedParameter)(void *, const char *);
static const char *(__cdecl *p_cgGetLastListing)(void *);
static int (__cdecl *p_cgGetError)(void);
static const char *(__cdecl *p_cgGetErrorString)(int);

#define MAX_RECORDS 1024
static struct cg_record records[MAX_RECORDS];
static u32 nrecords;
static u32 cg_gen;                  /* how many Cg reloads changed something */

/* any program handle we have seen (original, current, in between) -> record index + 1 */
#define RH 4096
static struct { void *h; int rec; } rh[RH];

static u32 hash_ptr(void *p, u32 n) { return ((u32)p >> 3) * 2654435761u & (n - 1); }

static void rh_put(void *h, int rec)
{
    u32 i = hash_ptr(h, RH), n;
    for (n = 0; n < RH; n++, i = (i + 1) & (RH - 1))
        if (!rh[i].h || rh[i].h == h) { rh[i].h = h; rh[i].rec = rec + 1; return; }
    logmsg("loader: program handle table full");
}

static int rh_get(void *h)
{
    u32 i = hash_ptr(h, RH), n;
    if (!h) return -1;
    for (n = 0; n < RH; n++, i = (i + 1) & (RH - 1)) {
        if (rh[i].h == h) return rh[i].rec - 1;
        if (!rh[i].h) return -1;
    }
    return -1;
}

static void *map_prog(void *h)
{
    int r;
    if (!cg_gen || !h) return h;
    r = rh_get(h);
    return r >= 0 && records[r].cur ? records[r].cur : h;
}

/* parameter handle (any generation) -> the parameter of the program in use; rebuilt lazily */
#define PP 16384
static struct { void *p, *cur; } pp[PP];
static u32 pp_used, pp_missing;

static void *map_param(void *p)
{
    u32 i, n;
    void *owner, *cur_owner, *cur;
    if (!cg_gen || !p) return p;
    i = hash_ptr(p, PP);
    for (n = 0; n < 64; n++, i = (i + 1) & (PP - 1)) {
        if (pp[i].p == p) return pp[i].cur;
        if (!pp[i].p) break;
    }
    owner = p_cgGetParameterProgram ? p_cgGetParameterProgram(p) : NULL;
    cur_owner = map_prog(owner);
    if (cur_owner == owner) cur = p;
    else {
        const char *name = p_cgGetParameterName ? p_cgGetParameterName(p) : NULL;
        cur = name && p_cgGetNamedParameter ? p_cgGetNamedParameter(cur_owner, name) : NULL;
        if (!cur && pp_missing++ < 20)
            logmsg("cg: parameter '%s' is not in the recompiled program; the engine's calls with it are dropped",
                   name ? name : "?");
    }
    if (n < 64 && !pp[i].p && pp_used < PP * 3 / 4) { pp[i].p = p; pp[i].cur = cur; pp_used++; }
    return cur;
}

/* Values the engine gave each parameter, newest call last, re-applied after a recompile
   (through the core's hooks, so that what it tracks about each program is right again). */
#define VC 8192
enum { VAL_F1 = 1, VAL_F2, VAL_F3, VAL_F4, VAL_MAT, VAL_TEX };
static struct vc { void *p; u32 seq; u8 kind; float v[16]; } vc[VC];
static u32 vc_used, vc_seq, vc_full_logged;

static void vc_record(void *p, int kind, const float *v, int n)
{
    u32 i = hash_ptr(p, VC), k;
    if (!p) return;
    for (k = 0; k < VC; k++, i = (i + 1) & (VC - 1)) {
        if (vc[i].p == p) break;
        if (!vc[i].p) {
            if (vc_used >= VC * 7 / 8) {
                if (!vc_full_logged++) logmsg("cg: value cache full; later parameters will not be re-applied after a reload");
                return;
            }
            vc[i].p = p; vc_used++;
            break;
        }
    }
    if (k == VC) return;
    vc[i].seq = ++vc_seq;
    vc[i].kind = (u8)kind;
    memcpy(vc[i].v, v, (size_t)n * sizeof(float));
}

static int vc_cmp(const void *a, const void *b)
{
    u32 x = vc[*(const u32 *)a].seq, y = vc[*(const u32 *)b].seq;
    return x < y ? -1 : x > y;
}

static u32 reapply_values(void)
{
    u32 *order = malloc(VC * sizeof(u32)), n = 0, i, done = 0;
    if (!order) return 0;
    for (i = 0; i < VC; i++) if (vc[i].p) order[n++] = i;
    qsort(order, n, sizeof(u32), vc_cmp);
    for (i = 0; i < n; i++) {
        struct vc *e = &vc[order[i]];
        void *q = e->kind ? map_param(e->p) : NULL;     /* kind 0: its program was destroyed */
        if (!q) continue;
        switch (e->kind) {
        case VAL_F1: CHAIN(O_SET1, set1_fn)(q, e->v[0]); break;
        case VAL_F2: CHAIN(O_SET2, set2_fn)(q, e->v[0], e->v[1]); break;
        case VAL_F3: CHAIN(O_SET3, set3_fn)(q, e->v[0], e->v[1], e->v[2]); break;
        case VAL_F4: CHAIN(O_SET4, set4_fn)(q, e->v[0], e->v[1], e->v[2], e->v[3]); break;
        case VAL_MAT: CHAIN(O_SETMAT, setm_fn)(q, e->v); break;
        case VAL_TEX: { GLuint t; memcpy(&t, &e->v[0], 4); CHAIN(O_SETTEX, sett_fn)(q, t); break; }
        }
        done++;
    }
    free(order);
    if (p_cgGetError) while (p_cgGetError()) ;
    return done;
}

static const char *base_name(const char *s)
{
    const char *b = s;
    for (; *s; s++)
        if (*s == '\\' || *s == '/') b = s + 1;
    return b;
}

static void *__cdecl L_cgCreateProgramFromFile(void *ctx, int type, const char *file, int profile,
                                               const char *entry, const char **args)
{
    void *h = ORIG(O_CREATE, create_fn)(ctx, type, file, profile, entry, args);
    if (h && nrecords < MAX_RECORDS) {
        struct cg_record *r = &records[nrecords];
        const char *ps = p_cgGetProfileString ? p_cgGetProfileString(profile) : "";
        memset(r, 0, sizeof *r);
        r->ctx = ctx; r->type = type; r->profile = profile;
        strncpy(r->file, file ? file : "", sizeof r->file - 1);
        strncpy(r->entry, entry ? entry : "", sizeof r->entry - 1);
        strncpy(r->profile_name, ps ? ps : "", sizeof r->profile_name - 1);
        r->is_vertex = strstr(r->profile_name, "vp") != NULL || strstr(r->profile_name, "glslv") != NULL;
        if (args) {
            int i, used = 0;
            r->has_args = 1;
            for (i = 0; args[i] && i < 16; i++) {
                int len = (int)strlen(args[i]);
                if (used + len + 1 > (int)sizeof r->argbuf) { logmsg("cg: arguments of %s too long to keep", r->entry); break; }
                memcpy(r->argbuf + used, args[i], (size_t)len + 1);
                r->args[i] = r->argbuf + used;
                used += len + 1;
            }
        }
        r->orig = r->cur = h;
        rh_put(h, (int)nrecords);
        nrecords++;
        L.nprog = nrecords;
        if (core_loaded && core.program_created) core.program_created(r);
    } else if (h) logmsg("cg: more than %d programs; the rest cannot be reloaded", MAX_RECORDS);
    return h;
}

static int bound_rec[2] = { -1, -1 };      /* fragment, vertex */
static struct { int profile, on; } prof_state[8];

static void __cdecl L_cgGLBindProgram(void *h)
{
    int r = rh_get(h);
    if (r >= 0) bound_rec[records[r].is_vertex] = r;
    CHAIN(O_BIND, prog_fn)(map_prog(h));
}

static void __cdecl L_cgGLLoadProgram(void *h) { CHAIN(O_LOAD, prog_fn)(map_prog(h)); }
static void __cdecl L_cgUpdateProgramParameters(void *h) { CHAIN(O_UPDATE, prog_fn)(map_prog(h)); }

static void __cdecl L_cgDestroyProgram(void *h)
{
    int r = rh_get(h);
    void *cur = map_prog(h);
    u32 i;
    for (i = 0; i < VC && p_cgGetParameterProgram; i++)    /* forget the values of its parameters */
        if (vc[i].p && vc[i].kind) {
            void *owner = p_cgGetParameterProgram(vc[i].p);
            if (owner == h || owner == cur) vc[i].kind = 0;
        }
    CHAIN(O_DESTROY, prog_fn)(h);
    if (cur != h) CHAIN(O_DESTROY, prog_fn)(cur);
    if (r >= 0 && (records[r].orig == h || records[r].cur == h)) {
        logmsg("cg: the engine destroyed program %s (%s)", records[r].entry, base_name(records[r].file));
        records[r].orig = records[r].cur = NULL;
    }
    memset(pp, 0, sizeof pp); pp_used = 0;
}

/* Our extra uniforms (rtMask, rtParams, ...) are hidden from the engine's parameter walks:
   it collects each program's parameters when the program is created and writes a log line
   on every draw for any it has no handler for, which made the game crawl. */
static int is_rt_uniform(void *p)
{
    const char *n = p_cgGetParameterName ? p_cgGetParameterName(p) : NULL;
    return n && n[0] == 'r' && n[1] == 't' && n[2] >= 'A' && n[2] <= 'Z';
}

static void *__cdecl L_cgGetFirstParameter(void *h, int ns)
{
    void *p = CHAIN(O_FIRST, first_fn)(map_prog(h), ns);
    while (p && is_rt_uniform(p)) p = CHAIN(O_NEXT, param_fn)(p);
    return p;
}

static void *__cdecl L_cgGetNextParameter(void *p)
{
    p = map_param(p);
    if (!p) return NULL;
    do p = CHAIN(O_NEXT, param_fn)(p); while (p && is_rt_uniform(p));
    return p;
}
static void *__cdecl L_cgGetFirstStructParameter(void *p) { p = map_param(p); return p ? CHAIN(O_FIRSTSTRUCT, param_fn)(p) : NULL; }
static void *__cdecl L_cgGetArrayParameter(void *p, int i) { p = map_param(p); return p ? CHAIN(O_ARRAY, array_fn)(p, i) : NULL; }

static void __cdecl L_cgGLSetParameter1f(void *p, float x)
{
    void *q = map_param(p);
    vc_record(p, VAL_F1, &x, 1);
    if (q) CHAIN(O_SET1, set1_fn)(q, x);
}

static void __cdecl L_cgGLSetParameter2f(void *p, float x, float y)
{
    void *q = map_param(p);
    float v[2]; v[0] = x; v[1] = y;
    vc_record(p, VAL_F2, v, 2);
    if (q) CHAIN(O_SET2, set2_fn)(q, x, y);
}

static void __cdecl L_cgGLSetParameter3f(void *p, float x, float y, float z)
{
    void *q = map_param(p);
    float v[3]; v[0] = x; v[1] = y; v[2] = z;
    vc_record(p, VAL_F3, v, 3);
    if (q) CHAIN(O_SET3, set3_fn)(q, x, y, z);
}

static void __cdecl L_cgGLSetParameter4f(void *p, float x, float y, float z, float w)
{
    void *q = map_param(p);
    float v[4]; v[0] = x; v[1] = y; v[2] = z; v[3] = w;
    vc_record(p, VAL_F4, v, 4);
    if (q) CHAIN(O_SET4, set4_fn)(q, x, y, z, w);
}

static void __cdecl L_cgGLSetMatrixParameterfr(void *p, const float *m)
{
    void *q = map_param(p);
    if (m) vc_record(p, VAL_MAT, m, 16);
    if (q) CHAIN(O_SETMAT, setm_fn)(q, m);
}

static void __cdecl L_cgGLSetTextureParameter(void *p, GLuint tex)
{
    void *q = map_param(p);
    float v; memcpy(&v, &tex, 4);
    vc_record(p, VAL_TEX, &v, 1);
    if (q) CHAIN(O_SETTEX, sett_fn)(q, tex);
}

static void __cdecl L_cgGLEnableTextureParameter(void *p) { p = map_param(p); if (p) CHAIN(O_ENTEX, prog_fn)(p); }
static void __cdecl L_cgGLDisableTextureParameter(void *p) { p = map_param(p); if (p) CHAIN(O_DISTEX, prog_fn)(p); }

static void set_prof(int profile, int on)
{
    int i;
    for (i = 0; i < 8; i++)
        if (prof_state[i].profile == profile || !prof_state[i].profile) {
            prof_state[i].profile = profile; prof_state[i].on = on;
            return;
        }
}
static void __cdecl L_cgGLEnableProfile(int profile) { set_prof(profile, 1); CHAIN(O_ENPROF, prof_fn)(profile); }
static void __cdecl L_cgGLDisableProfile(int profile) { set_prof(profile, 0); CHAIN(O_DISPROF, prof_fn)(profile); }

static void *loader_cg_current(void *h) { return map_prog(h); }
static void *loader_bound_program(int is_vertex)
{
    int r = bound_rec[is_vertex ? 1 : 0];
    return r >= 0 ? records[r].cur : NULL;
}
static int loader_profile_enabled(int profile)
{
    int i;
    for (i = 0; i < 8; i++) if (prof_state[i].profile == profile) return prof_state[i].on;
    return 0;
}

/* Compile every program from the .cg files again (only: a file name, "" = all). A file is
   replaced only when all of its programs compile and load; otherwise the old ones stay.
   Returns the number of files that failed. */
static int cg_reload(const char *only, u32 *n_ok_files, u32 *n_progs)
{
    static void *fresh[MAX_RECORDS];
    char done[64][260];
    u32 ndone = 0, i, j, k;
    int failed = 0;
    double t0 = (double)GetTickCount();
    *n_ok_files = 0; *n_progs = 0;
    if (p_cgGetError) while (p_cgGetError()) ;
    for (i = 0; i < nrecords; i++) {
        const char *file = records[i].file;
        u32 n = 0;
        int ok = 1;
        if (!records[i].orig) continue;
        for (k = 0; k < ndone; k++) if (!strcmp(done[k], file)) break;
        if (k < ndone || ndone == 64) continue;
        strcpy(done[ndone++], file);
        if (only && only[0] && _stricmp(base_name(file), only) && _stricmp(file, only)) continue;
        for (j = i; j < nrecords && ok; j++) {
            struct cg_record *r = &records[j];
            void *h;
            int err;
            if (!r->orig || strcmp(r->file, file)) continue;
            h = ORIG(O_CREATE, create_fn)(r->ctx, r->type, r->file, r->profile, r->entry, r->has_args ? r->args : NULL);
            if (!h) {
                const char *listing = p_cgGetLastListing ? p_cgGetLastListing(r->ctx) : NULL;
                err = p_cgGetError ? p_cgGetError() : 0;
                logmsg("cg: %s (%s, %s) failed to compile: %s\n%s", r->entry, base_name(file), r->profile_name,
                       err && p_cgGetErrorString ? p_cgGetErrorString(err) : "?", listing ? listing : "(no listing)");
                ok = 0;
                break;
            }
            fresh[j] = h;
            n++;
            ORIG(O_LOAD, prog_fn)(h);
            err = p_cgGetError ? p_cgGetError() : 0;
            if (err) {
                const char *listing = p_cgGetLastListing ? p_cgGetLastListing(r->ctx) : NULL;
                logmsg("cg: %s (%s) failed to load: %s\n%s", r->entry, base_name(file),
                       p_cgGetErrorString ? p_cgGetErrorString(err) : "?", listing ? listing : "");
                ok = 0;
            }
        }
        for (j = i; j < nrecords; j++) {
            struct cg_record *r = &records[j];
            if (!r->orig || strcmp(r->file, file) || !fresh[j]) continue;
            if (ok) {
                r->cur = fresh[j];
                rh_put(fresh[j], (int)j);
                (*n_progs)++;
            } else ORIG(O_DESTROY, prog_fn)(fresh[j]);
            fresh[j] = NULL;
        }
        if (ok) {
            (*n_ok_files)++;
            logmsg("cg: %s compiled again: %u programs", base_name(file), n);
        } else {
            failed++;
            logmsg("cg: %s keeps its old programs", base_name(file));
        }
    }
    if (*n_progs) {
        u32 applied;
        cg_gen++;
        memset(pp, 0, sizeof pp); pp_used = 0; pp_missing = 0;
        if (core_loaded && core.programs_replaced) core.programs_replaced();
        applied = reapply_values();
        logmsg("cg: reload %u done in %.0f ms: %u files, %u programs replaced, %u of %u cached values re-applied, %d files failed",
               cg_gen, (double)GetTickCount() - t0, *n_ok_files, *n_progs, applied, vc_used, failed);
    } else logmsg("cg: reload: nothing replaced (%d files failed)", failed);
    if (p_cgGetError) while (p_cgGetError()) ;
    return failed;
}

/* ---- wglGetProcAddress: everything the engine fetches goes out as a thunk -------------- */
static PROC (WINAPI *real_wglGetProcAddress)(LPCSTR);

static PROC WINAPI L_wglGetProcAddress(LPCSTR name)
{
    PROC p = real_wglGetProcAddress(name);
    u32 i;
    void *t;
    if (!name || !p || (u32)name < 0x10000) return p;
    for (i = 0; i < nthunks; i++)
        if (!thunk_slot[i] && thunk_orig[i] == (void *)p && !strcmp(thunk_name[i], name))
            return (PROC)(thunk_code + i * 8);
    t = new_thunk((void *)p);
    if (!t) return p;
    strncpy(thunk_name[nthunks - 1], name, sizeof thunk_name[0] - 1);
    if (core_loaded && core.wrap_proc) dispatch[nthunks - 1] = (void *)core.wrap_proc(name, p);
    return (PROC)t;
}

static PROC loader_real_proc(const char *name)
{
    return real_wglGetProcAddress ? real_wglGetProcAddress(name) : NULL;
}

static HWND loader_game_window(void) { return game_hwnd; }

/* ---- loading and unloading the core ------------------------------------------------- */
static void unhook_all(void)
{
    u32 i;
    for (i = 0; i < nthunks; i++) dispatch[i] = thunk_orig[i];
    for (i = 0; i < O_COUNT; i++) owned[i].chain = owned[i].orig;
}

static int load_core(void)
{
    wumrt_core_init_fn init;
    u32 i;
    CreateDirectoryA("wumrt_core_copies", NULL);
    sprintf(core_copy, "wumrt_core_copies\\core_%lu_%u.dll", (unsigned long)GetCurrentProcessId(), L.generation + 1);
    if (!CopyFileA("wumrt_core.dll", core_copy, FALSE)) {
        logmsg("loader: cannot copy wumrt_core.dll (error %lu); running without a core", (unsigned long)GetLastError());
        return 0;
    }
    core_mod = LoadLibraryA(core_copy);
    if (!core_mod) {
        logmsg("loader: cannot load %s (error %lu)", core_copy, (unsigned long)GetLastError());
        DeleteFileA(core_copy);
        return 0;
    }
    init = (wumrt_core_init_fn)GetProcAddress(core_mod, "wumrt_core_init");
    L.generation++;
    L.nprog = nrecords;
    memset(&core, 0, sizeof core);
    if (!init || !init(&L, &core) || core.version != WUMRT_API_VERSION) {
        logmsg("loader: %s did not start (init %p, version %u)", core_copy, (void *)init, core.version);
        unhook_all();
        FreeLibrary(core_mod);
        DeleteFileA(core_copy);
        core_mod = NULL;
        return 0;
    }
    core_loaded = 1;
    for (i = 0; i < nthunks; i++)
        if (!thunk_slot[i]) dispatch[i] = core.wrap_proc ? (void *)core.wrap_proc(thunk_name[i], (PROC)thunk_orig[i]) : thunk_orig[i];
    ctl->core_loads++;
    ctl->core_ok = 1;
    logmsg("loader: core %u loaded from %s", L.generation, core_copy);
    return 1;
}

static void unload_core(void)
{
    double t0 = (double)GetTickCount();
    if (!core_loaded) return;
    if (core.shutdown) core.shutdown();
    unhook_all();
    core_loaded = 0;
    ctl->core_ok = 0;
    FreeLibrary(core_mod);
    core_mod = NULL;
    if (!DeleteFileA(core_copy)) logmsg("loader: could not delete %s", core_copy);
    logmsg("loader: core %u unloaded in %.0f ms", L.generation, (double)GetTickCount() - t0);
}

/* ---- input and focus isolation (sandbox mode) ------------------------------------------ */
static u32 mouse_seq_seen;
static int mouse_frames;
static i32 mouse_now_dx, mouse_now_dy;

static BOOL WINAPI my_SetCursorPos(int x, int y) { (void)x; (void)y; return TRUE; }
/* The game reads the cursor every frame as an offset from the window centre. */
static BOOL WINAPI my_GetCursorPos(POINT *p)
{
    POINT c;
    RECT rc = { 0, 0, 0, 0 };
    if (game_hwnd) GetClientRect(game_hwnd, &rc);
    c.x = rc.right / 2 + (mouse_frames ? mouse_now_dx : 0); c.y = rc.bottom / 2 + (mouse_frames ? mouse_now_dy : 0);
    ctl->mouse_x = c.x; ctl->mouse_y = c.y;
    if (game_hwnd) ClientToScreen(game_hwnd, &c);
    if (p) *p = c;
    return TRUE;
}
static HWND WINAPI my_SetCapture(HWND h) { (void)h; return NULL; }
static BOOL WINAPI my_ReleaseCapture(void) { return TRUE; }
static int WINAPI my_ShowCursor(BOOL show) { return show ? 0 : -1; }
static BOOL WINAPI my_SetForegroundWindow(HWND h) { (void)h; return TRUE; }
static HWND WINAPI my_SetFocus(HWND h) { (void)h; return game_hwnd; }
static HWND WINAPI my_GetForegroundWindow(void) { return game_hwnd ? game_hwnd : GetForegroundWindow(); }
static HWND WINAPI my_GetFocus(void) { return game_hwnd ? game_hwnd : GetFocus(); }
static BOOL WINAPI my_GetKeyboardState(BYTE *keys)
{
    int i;
    if (!keys) return FALSE;
    for (i = 0; i < 256; i++) keys[i] = ctl->vk[i] ? 0x80 : 0;
    return TRUE;
}
static BOOL WINAPI my_SetKeyboardState(BYTE *keys) { (void)keys; return TRUE; }
static LONG WINAPI my_ChangeDisplaySettingsA(void *mode, DWORD flags)
{
    logmsg("ChangeDisplaySettingsA(%p, %#lx) swallowed", mode, (unsigned long)flags);
    return 0;   /* DISP_CHANGE_SUCCESSFUL */
}
static DWORD WINAPI my_XInputSetState(DWORD user, void *vib) { (void)user; (void)vib; return 1167; }

/* DirectInput: the keyboard reports only keys from the control block; pads are hidden. */
static const GUID guid_syskeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static void *kbd_dev;
static u8 kbd_reported[256];
static u32 kbd_seq;

static HRESULT (WINAPI *real_DirectInput8Create)(HINSTANCE, DWORD, const GUID *, void **, void *);
static HRESULT (WINAPI *real_CreateDevice)(void *, const GUID *, void **, void *);
static HRESULT (WINAPI *real_EnumDevices)(void *, DWORD, void *, void *, DWORD);
static HRESULT (WINAPI *real_Acquire)(void *);
static HRESULT (WINAPI *real_GetDeviceState)(void *, DWORD, void *);
static HRESULT (WINAPI *real_GetDeviceData)(void *, DWORD, void *, DWORD *, DWORD);
static HRESULT (WINAPI *real_Poll)(void *);

static void *hook_vtbl(void *obj, int index, void *fn)
{
    void **vt = *(void ***)obj, *old;
    DWORD prot;
    if (vt[index] == fn) return NULL;
    VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &prot);
    old = vt[index];
    vt[index] = fn;
    VirtualProtect(&vt[index], sizeof(void *), prot, &prot);
    return old;
}

static HRESULT WINAPI my_Acquire(void *self)
{
    HRESULT hr = real_Acquire(self);
    return self == kbd_dev ? 0 : hr;
}

static HRESULT WINAPI my_Poll(void *self)
{
    if (self == kbd_dev) return 0;
    return real_Poll(self);
}

static HRESULT WINAPI my_GetDeviceState(void *self, DWORD cb, void *data)
{
    if (self == kbd_dev && data) {
        DWORD i;
        ctl->kbd_state_calls++;
        memset(data, 0, cb);
        for (i = 0; i < cb && i < 256; i++)
            if (ctl->dik[i]) ((u8 *)data)[i] = 0x80;
        return 0;
    }
    return real_GetDeviceState(self, cb, data);
}

static HRESULT WINAPI my_GetDeviceData(void *self, DWORD cbobj, void *out, DWORD *inout, DWORD flags)
{
    if (self == kbd_dev && inout) {
        DWORD cap = *inout, n = 0;
        int i;
        ctl->kbd_data_calls++;
        for (i = 0; i < 256; i++) {
            u8 now = ctl->dik[i] ? 0x80 : 0;
            if (now == kbd_reported[i]) continue;
            if (out) {
                if (n >= cap) break;
                {
                    u32 *e = (u32 *)((u8 *)out + n * cbobj);
                    memset(e, 0, cbobj);
                    e[0] = (u32)i; e[1] = now; e[2] = GetTickCount(); e[3] = kbd_seq++;
                }
            }
            n++;
            if (!(flags & 1)) kbd_reported[i] = now;    /* DIGDD_PEEK = 1 */
        }
        *inout = n;
        return 0;
    }
    return real_GetDeviceData(self, cbobj, out, inout, flags);
}

static HRESULT WINAPI my_CreateDevice(void *self, const GUID *g, void **dev, void *outer)
{
    HRESULT hr = real_CreateDevice(self, g, dev, outer);
    if (hr >= 0 && dev && *dev && g && !memcmp(g, &guid_syskeyboard, sizeof *g)) {
        void *p;
        kbd_dev = *dev;
        ctl->kbd_devices++;
        if ((p = hook_vtbl(kbd_dev, 7, my_Acquire))) real_Acquire = p;
        if ((p = hook_vtbl(kbd_dev, 9, my_GetDeviceState))) real_GetDeviceState = p;
        if ((p = hook_vtbl(kbd_dev, 10, my_GetDeviceData))) real_GetDeviceData = p;
        if ((p = hook_vtbl(kbd_dev, 25, my_Poll))) real_Poll = p;
        logmsg("DirectInput keyboard device %p hooked", kbd_dev);
    } else {
        logmsg("DirectInput CreateDevice hr=%#lx (not the keyboard)", (unsigned long)hr);
    }
    return hr;
}

static HRESULT WINAPI my_EnumDevices(void *self, DWORD type, void *cb, void *ref, DWORD flags)
{
    if (type == 4 || type == 0) {       /* DI8DEVCLASS_GAMECTRL or all: report nothing */
        ctl->pad_enum_blocked++;
        logmsg("DirectInput EnumDevices(type %lu) blocked", (unsigned long)type);
        return 0;
    }
    return real_EnumDevices(self, type, cb, ref, flags);
}

static HRESULT WINAPI my_DirectInput8Create(HINSTANCE inst, DWORD ver, const GUID *iid, void **out, void *outer)
{
    HRESULT hr = real_DirectInput8Create(inst, ver, iid, out, outer);
    if (hr >= 0 && out && *out) {
        void *p;
        if ((p = hook_vtbl(*out, 3, my_CreateDevice))) real_CreateDevice = p;
        if ((p = hook_vtbl(*out, 4, my_EnumDevices))) real_EnumDevices = p;
        logmsg("DirectInput8 %p hooked", *out);
    }
    return hr;
}

/* ---- end of frame: the core's work, then the loader's own commands ------------------------ */
static void loader_commands(void)
{
    u32 cmd;
    if (ctl->cmd_seq == ctl->ack_seq) return;
    cmd = ctl->cmd;
    if (cmd < CMD_LOADER_FIRST) {
        if (!core_loaded) { ctl->result = 1; ctl->ack_seq = ctl->cmd_seq; }     /* nobody else will answer */
        return;
    }
    if (cmd == CMD_RELOAD_CORE) {
        unload_core();
        ctl->result = load_core() ? 0 : 1;
    } else if (cmd == CMD_CG_RELOAD) {
        char only[260];
        u32 files = 0, progs = 0;
        memcpy(only, ctl->path, sizeof only);
        only[sizeof only - 1] = 0;
        ctl->result = (u32)cg_reload(only, &files, &progs);
        ctl->arg[0] = files; ctl->arg[1] = progs; ctl->arg[2] = cg_gen;
    } else if (cmd == CMD_CG_STATUS) {
        ctl->arg[0] = cg_gen; ctl->arg[1] = nrecords; ctl->arg[2] = vc_used; ctl->arg[3] = pp_used;
        ctl->result = 0;
    } else ctl->result = 1;
    ctl->ack_seq = ctl->cmd_seq;
}

static BOOL WINAPI L_SwapBuffers(HDC hdc)
{
    BOOL r;
    if (!game_hwnd) {
        game_hwnd = WindowFromDC(hdc);
        ctl->hwnd = (u32)game_hwnd;
        logmsg("first swap, window %p", (void *)game_hwnd);
    }
    if (!core_tried) {          /* the first core comes in at the end of the first frame */
        core_tried = 1;
        load_core();
    }
    if (mouse_frames) mouse_frames--;
    if (ctl->mouse_seq != mouse_seq_seen) {
        mouse_seq_seen = ctl->mouse_seq;
        mouse_now_dx = ctl->mouse_dx; mouse_now_dy = ctl->mouse_dy;
        mouse_frames = 1;
    }
    r = CHAIN(O_SWAP, BOOL (WINAPI *)(HDC))(hdc);
    if (!core_loaded) { ctl->swap_tick = GetTickCount(); ctl->frame++; }
    loader_commands();
    return r;
}

/* ---- start-up --------------------------------------------------------------------------- */
static int host_is_game(void)
{
    u8 *base = (u8 *)GetModuleHandleA(NULL);
    u32 pe = *(u32 *)(base + 0x3c);
    return (u32)base == 0x400000 && *(u32 *)(base + pe + 8) == EXE_TIMESTAMP;
}

static int read_sandbox_setting(void)
{
    FILE *f = fopen("wumrt.ini", "r");
    char line[128], name[32];
    float v;
    int sandbox = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %31[A-Za-z_] = %f", name, &v) == 2 && !strcmp(name, "sandbox")) sandbox = v != 0;
    fclose(f);
    return sandbox;
}

static void own(int o, u32 slot, void *fn)
{
    owned[o].slot = slot;
    owned[o].fn = fn;
    owned[o].orig = owned[o].chain = patch_slot(slot, fn);
}

static void remove_old_copies(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("wumrt_core_copies\\core_*.dll", &fd);
    char path[MAX_PATH];
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        sprintf(path, "wumrt_core_copies\\%s", fd.cFileName);
        DeleteFileA(path);          /* fails for copies another running instance has loaded: fine */
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---- the window never comes up over what the user is doing (sandbox mode) ----
 * glut32_real.dll creates and shows the game's window, the exe moves and shows it again. In
 * sandbox mode the window is created invisible, can never be activated (WS_EX_NOACTIVATE), has
 * no taskbar button (WS_EX_TOOLWINDOW), and every show or move puts it at the bottom of the Z
 * order without activating it. Screenshots read the framebuffer back, so nobody needs to see it. */
#define OFF_SCREEN (-10000)
static HWND top_windows[16];        /* the top-level windows glut created (a dummy one first) */
static int ntop;

static BOOL is_top(HWND h)
{
    int i;
    if (!h) return FALSE;
    if (h == game_hwnd) return TRUE;
    for (i = 0; i < ntop; i++)
        if (top_windows[i] == h) return TRUE;
    return FALSE;
}

static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU,
                                           HINSTANCE, LPVOID);
static BOOL (WINAPI *real_ShowWindow)(HWND, int);
static BOOL (WINAPI *real_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);

static HWND WINAPI my_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND r;
    int top = !(style & WS_CHILD) && !parent;
    if (top) {
        style &= ~(DWORD)WS_VISIBLE;
        ex &= ~(DWORD)(WS_EX_TOPMOST | WS_EX_APPWINDOW);
        ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        x = y = OFF_SCREEN;         /* sandbox.py moves it to 0,0 at the bottom of the Z order */
    }
    r = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (top && r && ntop < 16) top_windows[ntop++] = r;
    logmsg("loader: CreateWindowExA %s %p parent %p style %08lx ex %08lx%s", HIWORD(cls) ? cls : "#atom",
           (void *)r, (void *)parent, style, ex, top ? ": top level, hidden, off-screen, never activated" : "");
    return r;
}

static BOOL WINAPI my_ShowWindow(HWND h, int cmd)
{
    BOOL was;
    if (!game_hwnd) logmsg("loader: ShowWindow %p %d (%s)", (void *)h, cmd, is_top(h) ? "ours: bottom, no activation" : "passed on");
    if (!is_top(h) || cmd == SW_HIDE || cmd == SW_MINIMIZE || cmd == SW_SHOWMINNOACTIVE)
        return real_ShowWindow(h, cmd == SW_MINIMIZE && is_top(h) ? SW_SHOWMINNOACTIVE : cmd);
    was = IsWindowVisible(h);
    real_SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return was;
}

static BOOL WINAPI my_SetWindowPos(HWND h, HWND after, int x, int y, int w, int cy, UINT flags)
{
    if (!game_hwnd) logmsg("loader: SetWindowPos %p after %p %d,%d %dx%d flags %x (%s)", (void *)h, (void *)after,
                           x, y, w, cy, flags, is_top(h) ? "ours" : "passed on");
    if (is_top(h)) {
        flags |= SWP_NOACTIVATE | SWP_NOMOVE;   /* stays off-screen until sandbox.py places it */
        if (!(flags & SWP_NOZORDER) || (flags & SWP_SHOWWINDOW)) {
            after = HWND_BOTTOM;
            flags &= ~(UINT)SWP_NOZORDER;
        }
    }
    return real_SetWindowPos(h, after, x, y, w, cy, flags);
}

/* The exe creates its window (class XOglWindow) through a CreateWindowEx it looks up at run time,
 * so no import hook sees it. A CBT hook on the main thread sees every window being created and
 * activated: top-level windows start off-screen at the bottom of the Z order, can't be
 * activated, and an activation is refused. */
static HHOOK cbt_hook;

static LRESULT CALLBACK cbt_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_CREATEWND) {
        CBT_CREATEWNDA *c = (CBT_CREATEWNDA *)lp;
        CREATESTRUCTA *cs = c->lpcs;
        HWND h = (HWND)wp;
        if (!(cs->style & WS_CHILD) && !cs->hwndParent) {
            LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
            cs->x = cs->y = OFF_SCREEN;
            cs->style &= ~(LONG)WS_VISIBLE;
            c->hwndInsertAfter = HWND_BOTTOM;
            SetWindowLongA(h, GWL_EXSTYLE, (ex & ~(WS_EX_TOPMOST | WS_EX_APPWINDOW)) | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
            if (!is_top(h) && ntop < 16) top_windows[ntop++] = h;
            logmsg("loader: window %p (%s, %dx%d) being created: off-screen, bottom, never activated", (void *)h,
                   HIWORD(cs->lpszClass) ? cs->lpszClass : "#atom", cs->cx, cs->cy);
        }
    } else if (code == HCBT_ACTIVATE && is_top((HWND)wp)) {
        return 1;                   /* refused */
    }
    return CallNextHookEx(cbt_hook, code, wp, lp);
}

/* Point one import of a loaded module at fn; returns the old target. */
static void *hook_module_import(HMODULE mod, const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!dd->VirtualAddress) return NULL;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *slots;
        if (_stricmp((char *)(base + d->Name), dll)) continue;
        names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            void *prev;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, name)) continue;
            prev = (void *)slots->u1.Function;
            VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            slots->u1.Function = (DWORD)(size_t)fn;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            return prev;
        }
    }
    return NULL;
}

static void hide_window_hooks(HMODULE glut)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    real_CreateWindowExA = (void *)GetProcAddress(u, "CreateWindowExA");
    real_ShowWindow = (void *)GetProcAddress(u, "ShowWindow");
    real_SetWindowPos = (void *)GetProcAddress(u, "SetWindowPos");
    if (glut) {
        int a = !!hook_module_import(glut, "USER32.dll", "CreateWindowExA", my_CreateWindowExA);
        int b = !!hook_module_import(glut, "USER32.dll", "ShowWindow", my_ShowWindow);
        int c = !!hook_module_import(glut, "USER32.dll", "SetWindowPos", my_SetWindowPos);
        logmsg("loader: window hooks in glut32_real: CreateWindowExA %d ShowWindow %d SetWindowPos %d", a, b, c);
    }
    patch_slot(IAT_ShowWindow, my_ShowWindow);
    patch_slot(IAT_SetWindowPos, my_SetWindowPos);
    cbt_hook = SetWindowsHookExA(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
    logmsg("loader: window creation hook %s (thread %lu)", cbt_hook ? "installed" : "FAILED", GetCurrentThreadId());
}

/* The control block's name: "wumrt_ctl" for WormsXHD/rt-sandbox, "wumrt_ctl_NAME" when the exe
 * runs from WormsXHD/streams/NAME/rt-sandbox (a parallel work stream; rt.py derives the same),
 * "wumrt_ctl_play" from any other folder (rt-play, the user's playable copy, or an installed game),
 * so a sandbox's scripted keys and camera moves can never reach the game the user is playing. */
static char ctl_name[96] = CTL_NAME;

static void derive_ctl_name(void)
{
    char path[MAX_PATH], *part[4] = {0};
    int n = 0, i;
    char *p;
    if (!GetModuleFileNameA(NULL, path, sizeof path)) return;
    for (p = path + strlen(path); p > path && n < 4; p--)    /* exe, sandbox folder, NAME, "streams" */
        if (*p == '\\' || *p == '/') { *p = 0; part[n++] = p + 1; }
    if (n >= 2 && _stricmp(part[1], "rt-sandbox")) {
        snprintf(ctl_name, sizeof ctl_name, "%s_play", CTL_NAME);
        return;
    }
    if (n < 4 || _stricmp(part[1], "rt-sandbox") || _stricmp(part[3], "streams") || !*part[2]) return;
    snprintf(ctl_name, sizeof ctl_name, "%s_%.60s", CTL_NAME, part[2]);
    for (i = (int)strlen(CTL_NAME) + 1; ctl_name[i]; i++) {
        char c = ctl_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            ctl_name[i] = '_';
    }
}

static void install(void)
{
    HMODULE glut = LoadLibraryA("glut32_real.dll"), cg = GetModuleHandleA("cg.dll");
    HANDLE map;

    if (glut) {
        real_glut[0] = (void *)GetProcAddress(glut, "glutHideWindow");
        real_glut[1] = (void *)GetProcAddress(glut, "glutDestroyWindow");
        real_glut[2] = (void *)GetProcAddress(glut, "__glutCreateWindowWithExit");
    }
    ctl = &dummy_ctl;
    if (!host_is_game()) return;

    InitializeCriticalSection(&log_lock);
    logf = fopen("wumrt.log", "w");
    derive_ctl_name();
    map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct ctl), ctl_name);
    if (map) {
        struct ctl *c = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct ctl));
        if (c) ctl = c;
    }
    memset(ctl, 0, sizeof *ctl);
    ctl->magic = CTL_MAGIC;
    ctl->size = sizeof *ctl;
    ctl->mouse_x = 950; ctl->mouse_y = 530;
    logmsg("wumrt loader attached, glut=%p cg=%p ctl=%s (%s)", (void *)glut, (void *)cg,
           ctl == &dummy_ctl ? "private" : "shared", ctl_name);
    remove_old_copies();

    p_cgGetProfileString = *(void **)0x81566c;
    p_cgGetParameterName = *(void **)0x815678;
    p_cgGetLastListing = *(void **)0x8156a0;
    p_cgGetError = *(void **)0x815674;
    p_cgGetErrorString = *(void **)0x815670;
    if (cg) p_cgGetParameterProgram = (void *)GetProcAddress(cg, "cgGetParameterProgram");
    if (cg) p_cgGetNamedParameter = (void *)GetProcAddress(cg, "cgGetNamedParameter");

    L.version = WUMRT_API_VERSION;
    L.ctl = ctl;
    L.vlog = vlogmsg;
    L.hook_iat = loader_hook_iat;
    L.real_proc = loader_real_proc;
    L.persist = loader_persist;
    L.prog = records;
    L.cg_current = loader_cg_current;
    L.bound_program = loader_bound_program;
    L.profile_enabled = loader_profile_enabled;
    L.game_window = loader_game_window;
    L.reapply_values = reapply_values;

    real_wglGetProcAddress = patch_slot(IAT_wglGetProcAddress, L_wglGetProcAddress);
    own(O_SWAP, IAT_SwapBuffers, L_SwapBuffers);
    own(O_CREATE, IAT_cgCreateProgramFromFile, L_cgCreateProgramFromFile);
    own(O_BIND, IAT_cgGLBindProgram, L_cgGLBindProgram);
    own(O_LOAD, IAT_cgGLLoadProgram, L_cgGLLoadProgram);
    own(O_SET1, IAT_cgGLSetParameter1f, L_cgGLSetParameter1f);
    own(O_SET2, IAT_cgGLSetParameter2f, L_cgGLSetParameter2f);
    own(O_SET3, IAT_cgGLSetParameter3f, L_cgGLSetParameter3f);
    own(O_SET4, IAT_cgGLSetParameter4f, L_cgGLSetParameter4f);
    own(O_SETMAT, IAT_cgGLSetMatrixParameterfr, L_cgGLSetMatrixParameterfr);
    own(O_SETTEX, IAT_cgGLSetTextureParameter, L_cgGLSetTextureParameter);
    own(O_ENTEX, IAT_cgGLEnableTextureParameter, L_cgGLEnableTextureParameter);
    own(O_DISTEX, IAT_cgGLDisableTextureParameter, L_cgGLDisableTextureParameter);
    own(O_UPDATE, IAT_cgUpdateProgramParameters, L_cgUpdateProgramParameters);
    own(O_FIRST, IAT_cgGetFirstParameter, L_cgGetFirstParameter);
    own(O_NEXT, IAT_cgGetNextParameter, L_cgGetNextParameter);
    own(O_FIRSTSTRUCT, IAT_cgGetFirstStructParameter, L_cgGetFirstStructParameter);
    own(O_ARRAY, IAT_cgGetArrayParameter, L_cgGetArrayParameter);
    own(O_DESTROY, IAT_cgDestroyProgram, L_cgDestroyProgram);
    own(O_ENPROF, IAT_cgGLEnableProfile, L_cgGLEnableProfile);
    own(O_DISPROF, IAT_cgGLDisableProfile, L_cgGLDisableProfile);

    sandbox_mode = L.sandbox_mode = read_sandbox_setting();
    if (!sandbox_mode) {            /* normal play: the game keeps its own input and window handling */
        /* Like the compatibility flag the real game's exe has (HIGHDPIAWARE): without it Windows
           scales the game's window by the display's scaling (200% on a 4K screen), so a
           3840x2160 game no longer fits the screen. Runs before the game makes its window. */
        SetProcessDPIAware();
        logmsg("loader: hooks installed (play mode, DPI aware)");
        return;
    }
    hide_window_hooks(glut);
    real_DirectInput8Create = patch_slot(IAT_DirectInput8Create, my_DirectInput8Create);
    patch_slot(IAT_SetCursorPos, my_SetCursorPos);
    patch_slot(IAT_GetCursorPos, my_GetCursorPos);
    patch_slot(IAT_SetCapture, my_SetCapture);
    patch_slot(IAT_ReleaseCapture, my_ReleaseCapture);
    patch_slot(IAT_ShowCursor, my_ShowCursor);
    patch_slot(IAT_SetForegroundWindow, my_SetForegroundWindow);
    patch_slot(IAT_SetFocus, my_SetFocus);
    patch_slot(IAT_GetForegroundWindow, my_GetForegroundWindow);
    patch_slot(IAT_GetFocus, my_GetFocus);
    patch_slot(IAT_GetKeyboardState, my_GetKeyboardState);
    patch_slot(IAT_SetKeyboardState, my_SetKeyboardState);
    patch_slot(IAT_ChangeDisplaySettingsA, my_ChangeDisplaySettingsA);
    patch_slot(IAT_XInputSetState, my_XInputSetState);
    logmsg("loader: hooks installed (sandbox mode)");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) install();
    return TRUE;
}
