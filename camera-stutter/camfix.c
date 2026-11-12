/*
 * camfix - per-frame camera interpolation for Worms Ultimate Mayhem (PC).
 *
 * Target: WormsMayhem.exe, build 2013-05-02, SHA1 a2e3a569a798cf2f90185757578d013ec7a13dd1,
 * fixed image base 0x400000.
 *
 * The engine advances the camera only on a 20 ms (50 Hz) task and never interpolates,
 * so on a 60 Hz display one frame in six repeats the previous camera transform.
 * These hooks leave the 50 Hz camera simulation untouched and, once per rendered
 * frame, write a transform interpolated between the last two camera ticks into the
 * render camera (XCamera). Before the next simulation step the true transform is
 * put back, because the camera code reads XCamera as part of its own state.
 *
 * The engine's frame limiter (Sleep on the coarse Windows timer) is replaced by a
 * wait on the high-resolution clock, so frame pacing is left to V-Sync.
 *
 * Entry points, reached from the patched instructions (glue.s):
 *   camfix_frame_begin   - start of TaskManager step, before any logic task
 *   camfix_camera_tick   - replaces ZCamUpdateFudgeService::Run
 *   camfix_frame_present - after timed tasks, before per-frame (graphics) tasks
 *   camfix_limiter       - replaces the engine's frame limiter
 *   camfix_swap          - wraps SwapBuffers to timestamp each present (for measure.py)
 */

typedef unsigned int u32;
typedef int i32;

#define TICK_MS 20

/* XCamera layout: three contiguous vec3 (position, look, up) then two dirty flags. */
#define XCAM_VEC_OFF   0x120
#define XCAM_DIRTY_OFF 0x144
#define NVEC 9

#ifndef CAMFIX_TEST
#define P_APPDATA     (*(char **)0x0095a100)   /* AppDataService instance, +8 = XCamera* */
#define P_CAMMGR      (*(void **)0x0095c370)   /* CameraManagerService instance */
#define DEFAULT_DELAY (*(i32 *)0x00854360)     /* 20 */
#define QPC_FREQ_LO   (*(u32 *)0x00962758)     /* cached QueryPerformanceFrequency */
#define CALL_QPC(p)   ((*(int (__stdcall **)(void *))0x00815080)(p))
#define CALL_CAM_UPDATE(mgr, t) (((i32 (__thiscall *)(void *, i32))0x0051da00)((mgr), (t)))
#define XCAM_VT_SETPOS 0x006e1ab5u             /* XCamera vtbl[0x2c]: copy vec3 to +0x120 */
#define XCAM_VT_GETPOS 0x006e1b2du             /* XCamera vtbl[0x38]: return this+0x120 */
#define CALL_SLEEP(ms) ((*(void (__stdcall **)(u32))0x00815044)(ms))
#define CALL_SWAP(hdc) ((*(int (__stdcall **)(void *))0x00815024)(hdc))
#define CALL_GETMODULE(n) ((*(char *(__stdcall **)(const char *))0x008150a8)(n))
#define GAME_TIME_MS() (((u32 (__cdecl *)(void))0x0063a903)())   /* engine clock, whole ms */
#define LIMITER_LAST  (*(u32 *)0x0095a180)     /* engine limiter's last-frame timestamp */
#define CPU_PAUSE()   __asm__ volatile("pause")
int _fltused = 0x9875;
#else
extern char *mock_appdata;
extern void *mock_cammgr;
extern u32 mock_freq;
int mock_qpc(void *p);
i32 mock_cam_update(void *mgr, i32 t);
#define P_APPDATA     mock_appdata
#define P_CAMMGR      mock_cammgr
#define DEFAULT_DELAY 20
#define QPC_FREQ_LO   mock_freq
#define CALL_QPC(p)   mock_qpc(p)
#define CALL_CAM_UPDATE(mgr, t) mock_cam_update((mgr), (t))
#define XCAM_VT_SETPOS 0x006e1ab5u
#define XCAM_VT_GETPOS 0x006e1b2du
void mock_sleep(u32 ms);
int mock_swap(void *hdc);
u32 mock_time_ms(void);
extern u32 mock_limiter_last;
#define CALL_SLEEP(ms) mock_sleep(ms)
#define CALL_SWAP(hdc) mock_swap(hdc)
#define GAME_TIME_MS() mock_time_ms()
#define LIMITER_LAST  mock_limiter_last
#define CPU_PAUSE()   ((void)0)
#endif

struct camfix_state {
    u32 magic;          /* 'CFIX' */
    u32 valid;          /* prev/cur hold a real tick pair */
    u32 active;         /* XCamera currently holds an interpolated transform */
    u32 tick_time;      /* game time (ms) of the last camera tick */
    u32 q_last;         /* last QPC sample, low dword */
    u32 now_last;       /* last game time seen */
    float frac;         /* sub-millisecond part of game time, 0..1 */
    u32 n_ticks;        /* camera ticks seen */
    u32 n_interp;       /* frames where an interpolated transform was written */
    u32 n_restore;      /* frames where the true transform was put back */
    u32 n_external;     /* frames skipped because something else moved the camera */
    u32 n_noxcam;       /* frames skipped because XCamera was missing or unrecognised */
    u32 prev[NVEC];     /* transform before the last tick */
    u32 cur[NVEC];      /* transform after the last tick (the true state) */
    u32 written[NVEC];  /* what we last wrote into XCamera */
    /* Fields below were added later; keep the ones above in place for probe.py. */
    u32 swap_q0;        /* QPC low dword just before the last SwapBuffers */
    u32 swap_q1;        /* ... and just after it returned */
    u32 wait_ticks;     /* QPC ticks the limiter waited last frame */
    u32 lim_last;       /* limiter: QPC low dword of the last frame boundary */
    u32 timer_fine;     /* 1 while we hold a 1 ms system timer request */
    u32 timer_tried;    /* NtSetTimerResolution lookup done */
    void *nt_set_timer; /* ntdll!NtSetTimerResolution or 0 */
};

struct camfix_state camfix_state = { 0x58494643u };
#define S camfix_state

/* Per-frame recorder, read from outside by probe.py. One entry per rendered frame:
   what the render camera held when the frame's graphics tasks started. */
#define LOG_N 2048                      /* power of two */
#define FLAG_INTERP   1u                /* an interpolated transform was written */
#define FLAG_VALID    2u                /* a tick pair was available */
#define FLAG_ENABLED  4u                /* interpolation switched on */
#define FLAG_XCAM     8u                /* render camera found */
#define FLAG_EXTERNAL 16u               /* something else moved the camera */

#define FLAG_ORIG_LIMITER 32u           /* engine's own Sleep limiter was in use */

struct camfix_frame {
    u32 q;              /* QPC low dword sampled at the start of the frame's step */
    u32 now;            /* game clock, ms */
    u32 ticks;          /* camera ticks so far */
    u32 flags;
    u32 v[6];           /* render camera position and look vectors (float bits) */
    u32 swap_q0;        /* previous frame's SwapBuffers: QPC before */
    u32 swap_q1;        /* ... and after */
    u32 wait_ticks;     /* previous frame's limiter wait, QPC ticks */
    u32 reserved;
};

/* Settings in the header may be changed from outside (measure.py) while running. */
struct camfix_log {
    u32 magic;          /* 'CLG2' */
    u32 enabled;        /* 0 switches camera interpolation off */
    u32 widx;           /* frames recorded so far; entry i lives at f[i % LOG_N] */
    u32 limiter_original; /* 1 = use the engine's own Sleep(16 - elapsed) limiter */
    u32 limit_us;       /* precise limiter: minimum frame time in microseconds, 0 = none */
    u32 reserved[3];
    struct camfix_frame f[LOG_N];
};

/* Default: interpolation on, precise limiter capping at 250 fps (V-Sync paces below that). */
struct camfix_log camfix_log = { 0x32474C43u, 1, 0, 0, 4000 };
#define L camfix_log

static char *get_xcam(void)
{
    char *app = P_APPDATA;
    char *x;
    u32 *vt;
    if (!app) return 0;
    x = *(char **)(app + 8);
    if (!x) return 0;
    vt = *(u32 **)x;
    if (!vt) return 0;
    /* Only touch the object if its position accessors are the plain XCamera ones. */
    if (vt[0x2c / 4] != XCAM_VT_SETPOS || vt[0x38 / 4] != XCAM_VT_GETPOS) return 0;
    return x;
}

static void vcopy(volatile u32 *dst, const u32 *src)
{
    int i;
    for (i = 0; i < NVEC; i++) dst[i] = src[i];
}

static int vsame(const u32 *a, const u32 *b)
{
    int i;
    u32 diff = 0;
    for (i = 0; i < NVEC; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static void xcam_write(char *x, const u32 *v)
{
    vcopy((volatile u32 *)(x + XCAM_VEC_OFF), v);
    x[XCAM_DIRTY_OFF] = 1;
    x[XCAM_DIRTY_OFF + 1] = 1;
}

/* Start of the TaskManager step. `now` is the game clock in whole milliseconds. */
void __cdecl camfix_frame_begin(u32 now)
{
    u32 q[2];
    u32 freq = QPC_FREQ_LO;
    i32 dq, dn;
    float f;

    /* Track the sub-millisecond part of the game clock from QPC. The game clock is
       floor(real ms), so real - game stays within 0..1 while time runs at 1x. */
    q[0] = 0; q[1] = 0;
    CALL_QPC(q);
    dq = (i32)(q[0] - S.q_last);
    S.q_last = q[0];
    dn = (i32)(now - S.now_last);
    S.now_last = now;
    f = S.frac;
    if (freq) f += (float)dq * 1000.0f / (float)freq - (float)dn;
    if (!(f >= 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    S.frac = f;

    /* Put the true transform back before any logic runs. */
    if (S.active) {
        char *x = get_xcam();
        S.active = 0;
        if (x) {
            if (vsame((const u32 *)(x + XCAM_VEC_OFF), S.written)) {
                xcam_write(x, S.cur);
                S.n_restore++;
            } else {
                /* Something else wrote the camera after us; its value stands. */
                S.n_external++;
            }
        }
    }
}

/* Replacement for ZCamUpdateFudgeService::Run(time). */
i32 __cdecl camfix_camera_tick(i32 time)
{
    void *mgr = P_CAMMGR;
    char *x;
    i32 r;

    if (!mgr) return DEFAULT_DELAY;

    x = get_xcam();
    if (x) vcopy(S.prev, (const u32 *)(x + XCAM_VEC_OFF));

    /* Original behaviour: two fixed 0.02 s camera updates per tick. */
    CALL_CAM_UPDATE(mgr, time);
    r = CALL_CAM_UPDATE(mgr, time);

    if (x && x == get_xcam()) {
        vcopy(S.cur, (const u32 *)(x + XCAM_VEC_OFF));
        S.tick_time = (u32)time;
        S.valid = 1;
        S.n_ticks++;
    } else {
        S.valid = 0;
    }
    return r;
}

/* Write the interpolated transform for this frame. Returns log flags. */
static u32 interpolate(char *x, u32 now)
{
    float a;
    u32 w[NVEC];
    int i;

    /* Only interpolate if XCamera still holds exactly what the last tick left. */
    if (!vsame((const u32 *)(x + XCAM_VEC_OFF), S.cur)) { S.n_external++; return FLAG_EXTERNAL; }
    if (vsame(S.prev, S.cur)) return 0;

    a = ((float)(i32)(now - S.tick_time) + S.frac) * (1.0f / TICK_MS);
    if (!(a < 1.0f)) return 0;          /* a full tick has elapsed: show cur as is */
    if (!(a > 0.0f)) a = 0.0f;

    for (i = 0; i < NVEC; i++) {
        float p = *(const float *)&S.prev[i];
        float c = *(const float *)&S.cur[i];
        *(float *)&w[i] = p + (c - p) * a;
    }
    vcopy(S.written, w);
    xcam_write(x, w);
    S.active = 1;
    S.n_interp++;
    return FLAG_INTERP;
}

/* After the timed tasks, before the per-frame tasks and the render. */
void __cdecl camfix_frame_present(u32 now)
{
    char *x = get_xcam();
    u32 flags = 0;
    struct camfix_frame *f;
    int i;

    if (L.enabled) flags |= FLAG_ENABLED;
    if (L.limiter_original) flags |= FLAG_ORIG_LIMITER;
    if (S.valid) flags |= FLAG_VALID;
    if (x) {
        flags |= FLAG_XCAM;
        if (S.valid && L.enabled) flags |= interpolate(x, now);
    } else if (S.valid) {
        S.n_noxcam++;
    }

    f = &L.f[L.widx & (LOG_N - 1)];
    f->q = S.q_last;
    f->now = now;
    f->ticks = S.n_ticks;
    f->flags = flags;
    for (i = 0; i < 6; i++) f->v[i] = x ? ((const u32 *)(x + XCAM_VEC_OFF))[i] : 0;
    f->swap_q0 = S.swap_q0;
    f->swap_q1 = S.swap_q1;
    f->wait_ticks = S.wait_ticks;
    f->reserved = 0;
    L.widx++;
}

/* ---- frame limiter ------------------------------------------------------------
 * The engine's limiter is `if (elapsed < 16) Sleep(16 - elapsed)`. The exe never
 * asks Windows for a fine timer, so Sleep wakes on a 15.6 ms grid and regularly
 * overshoots by a whole tick. This replacement waits on QueryPerformanceCounter:
 * coarse 1 ms sleeps while far from the deadline, then a short spin.
 */

static u32 qpc_lo(void)
{
    u32 q[2];
    q[0] = 0; q[1] = 0;
    CALL_QPC(q);
    return q[0];
}

#ifndef CAMFIX_TEST
static int name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Minimal export lookup (the exe does not import GetProcAddress). */
static void *find_export(char *base, const char *name)
{
    char *nt, *ed;
    u32 exp_rva, exp_size, n, i;
    u32 *funcs, *names;
    unsigned short *ords;

    if (!base || *(unsigned short *)base != 0x5A4D) return 0;
    nt = base + *(i32 *)(base + 0x3c);
    if (*(u32 *)nt != 0x00004550) return 0;
    exp_rva = *(u32 *)(nt + 0x78);
    exp_size = *(u32 *)(nt + 0x7c);
    if (!exp_rva) return 0;
    ed = base + exp_rva;
    n = *(u32 *)(ed + 0x18);
    funcs = (u32 *)(base + *(u32 *)(ed + 0x1c));
    names = (u32 *)(base + *(u32 *)(ed + 0x20));
    ords = (unsigned short *)(base + *(u32 *)(ed + 0x24));
    for (i = 0; i < n; i++) {
        if (name_eq(base + names[i], name)) {
            u32 rva = funcs[ords[i]];
            if (rva >= exp_rva && rva < exp_rva + exp_size) return 0;   /* forwarder */
            return base + rva;
        }
    }
    return 0;
}

/* Ask for (or release) a 1 ms system timer so Sleep(1) really is about 1 ms. */
static void set_timer_fine(u32 on)
{
    u32 actual = 0;
    if (S.timer_fine == on) return;
    if (!S.timer_tried) {
        S.timer_tried = 1;
        S.nt_set_timer = find_export(CALL_GETMODULE("ntdll.dll"), "NtSetTimerResolution");
    }
    if (!S.nt_set_timer) return;        /* stays coarse: the limiter then only spins */
    ((i32 (__stdcall *)(u32, u32, u32 *))S.nt_set_timer)(10000, on, &actual);
    S.timer_fine = on;
}
#else
static void set_timer_fine(u32 on) { S.timer_fine = on; }
#endif

/* Called once per frame in place of the engine's limiter block at 0x4d918e. */
void __cdecl camfix_limiter(void)
{
    u32 t0 = qpc_lo();
    u32 freq = QPC_FREQ_LO;
    u32 us = L.limit_us;

    if (L.limiter_original) {
        /* The engine's own behaviour, kept for A/B measurement. */
        u32 e;
        set_timer_fine(0);
        e = GAME_TIME_MS() - LIMITER_LAST;
        if (e < 16) CALL_SLEEP(16 - e);
        S.lim_last = qpc_lo();
        S.wait_ticks = S.lim_last - t0;
        return;
    }

    if (freq && us) {
        u32 interval = (u32)(i32)((float)freq * (float)us * 0.000001f);
        u32 two_ms = freq / 500;
        u32 now, target;
        set_timer_fine(1);
        for (;;) {
            u32 el = qpc_lo() - S.lim_last;
            if (el >= interval) break;
            if (S.timer_fine && interval - el > two_ms) CALL_SLEEP(1);
            else CPU_PAUSE();
        }
        now = qpc_lo();
        target = S.lim_last + interval;
        /* Keep the ideal boundary unless we are more than one frame late. */
        S.lim_last = (now - target > interval) ? now : target;
        S.wait_ticks = now - t0;
    } else {
        S.lim_last = t0;
        S.wait_ticks = 0;
    }
}

/* Wraps the engine's SwapBuffers call to timestamp the present. */
int __cdecl camfix_swap(void *hdc)
{
    int r;
    u32 q0 = qpc_lo();
    r = CALL_SWAP(hdc);
    S.swap_q1 = qpc_lo();
    S.swap_q0 = q0;
    return r;
}
