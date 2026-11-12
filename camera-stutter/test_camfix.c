/* Mock harness for camfix.c. Build with -DCAMFIX_TEST. Simulates the engine's frame
   loop (integer-ms game clock, 20 ms camera task) and checks what the render camera
   would show each frame. */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "camfix.c"

static u32 fake_vt[32];
static char fake_xcam[0x200];
static char fake_app[0x40];
char *mock_appdata;
void *mock_cammgr;
u32 mock_freq = 10000000u;

static double real_ms;          /* simulated wall clock */
static double qpc_latency_ms;   /* delay between the app's clock sample and our QPC sample */
static float truth_x;           /* what the camera simulation believes position.x is */
static int corrupt;             /* set if the sim ever saw a non-true transform */

static double spin_step_ms;     /* time that passes per QPC call (for wait loops) */
static int coarse_sleep;        /* 1 = Sleep wakes only on a 15.625 ms grid, like Windows' default timer */
u32 mock_limiter_last;

int mock_qpc(void *p)
{
    unsigned long long t;
    real_ms += spin_step_ms;
    t = (unsigned long long)((real_ms + qpc_latency_ms) * 10000.0);
    memcpy(p, &t, 8);
    return 1;
}

void mock_sleep(u32 ms)
{
    if (coarse_sleep && !camfix_state.timer_fine)
        real_ms = ceil((real_ms + ms) / 15.625) * 15.625;
    else
        real_ms += ms + 0.3;
}

int mock_swap(void *hdc) { (void)hdc; real_ms += 0.5; return 1; }
u32 mock_time_ms(void) { return (u32)floor(real_ms); }

static float *xv(void) { return (float *)(fake_xcam + XCAM_VEC_OFF); }

i32 mock_cam_update(void *mgr, i32 t)
{
    /* Like the real update: read XCamera, move it, write it back. */
    (void)mgr; (void)t;
    if (xv()[0] != truth_x) corrupt = 1;
    truth_x += 1.0f;
    xv()[0] = truth_x;
    xv()[4] = truth_x * 0.5f;
    fake_xcam[XCAM_DIRTY_OFF] = 1;
    return 20;
}

static void reset(void)
{
    memset(&camfix_state, 0, sizeof camfix_state);
    memset(&camfix_log, 0, sizeof camfix_log);
    camfix_log.enabled = 1;
    memset(fake_xcam, 0, sizeof fake_xcam);
    memset(fake_app, 0, sizeof fake_app);
    fake_vt[0x2c / 4] = XCAM_VT_SETPOS;
    fake_vt[0x38 / 4] = XCAM_VT_GETPOS;
    *(u32 **)fake_xcam = fake_vt;
    *(char **)(fake_app + 8) = fake_xcam;
    mock_appdata = fake_app;
    mock_cammgr = fake_app;
    real_ms = 1000.3;
    truth_x = 0;
    corrupt = 0;
    spin_step_ms = 0;
    coarse_sleep = 0;
    qpc_latency_ms = 0;
}

/* Run the limiter for `frames` frames with `work_ms` of work per frame. Returns the
   share of frames longer than 1.5x the shortest; *lo and *hi get the extremes. */
static double run_limiter(int original, u32 limit_us, double work_ms, int frames, double *lo, double *hi)
{
    double last, mn = 1e9, mx = 0, d[4000];
    int i, n = 0, longf = 0;
    reset();
    camfix_log.limiter_original = (u32)original;
    camfix_log.limit_us = limit_us;
    spin_step_ms = 0.002;
    coarse_sleep = 1;
    mock_limiter_last = mock_time_ms();
    last = real_ms;
    for (i = 0; i < frames; i++) {
        real_ms += work_ms;
        camfix_limiter();
        mock_limiter_last = mock_time_ms();     /* the engine stores this after the limiter */
        if (i >= 5) d[n++] = real_ms - last;
        last = real_ms;
    }
    for (i = 0; i < n; i++) { if (d[i] < mn) mn = d[i]; if (d[i] > mx) mx = d[i]; }
    for (i = 0; i < n; i++) if (d[i] > 1.5 * mn) longf++;
    *lo = mn; *hi = mx;
    return (double)longf / n;
}

/* Run `frames` frames at `fps`. Returns the worst deviation of the per-frame camera
   step from its mean, as a fraction of the mean, over the steady-state part. */
static double run(double fps, int frames, int interp, double jitter_ms, int verbose)
{
    u32 next_tick = 1000;
    double prev_shown = 0, sum = 0, worst = 0, mean;
    static double steps[20000];
    int i, n = 0;

    reset();
    for (i = 0; i < frames; i++) {
        u32 now;
        double j = jitter_ms * (((i * 7919) % 13) / 6.0 - 1.0);
        real_ms += 1000.0 / fps + j;
        now = (u32)floor(real_ms);
        qpc_latency_ms = 0.05;

        if (interp) camfix_frame_begin(now);
        while (next_tick <= now) {
            if (interp) camfix_camera_tick((i32)next_tick);
            else { mock_cam_update(0, 0); mock_cam_update(0, 0); }
            next_tick += 20;
        }
        if (interp) camfix_frame_present(now);

        {
            double shown = xv()[0];
            if (i >= 200) steps[n++] = shown - prev_shown;
            prev_shown = shown;
        }
        real_ms -= j;
    }
    for (i = 0; i < n; i++) sum += steps[i];
    mean = sum / n;
    for (i = 0; i < n; i++) {
        double d = fabs(steps[i] - mean) / mean;
        if (d > worst) worst = d;
    }
    if (verbose) {
        printf("    first steady steps:");
        for (i = 0; i < 12; i++) printf(" %.3f", steps[i]);
        printf("\n");
    }
    return worst;
}

int main(void)
{
    static const double rates[] = { 50, 60, 62.5, 90, 120, 144, 30 };
    int i, fail = 0;

    for (i = 0; i < (int)(sizeof rates / sizeof rates[0]); i++) {
        double before = run(rates[i], 3000, 0, 0.0, 0);
        double after = run(rates[i], 3000, 1, 0.0, 1);
        double jit = run(rates[i], 3000, 1, 0.4, 0);
        printf("%6.1f fps: worst step error  original %5.1f%%  patched %5.2f%%  patched+0.4ms jitter %5.2f%%  sim corrupt=%d ticks=%u interp=%u restore=%u\n",
               rates[i], before * 100, after * 100, jit * 100, corrupt,
               camfix_state.n_ticks, camfix_state.n_interp, camfix_state.n_restore);
        if (corrupt) fail = 1;
        if (rates[i] >= 50 && after > 0.02) fail = 1;
    }

    /* Simulation must end exactly where the unpatched one would. */
    run(60, 3000, 1, 0.0, 0);
    {
        float t1 = truth_x;
        run(60, 3000, 0, 0.0, 0);
        printf("truth after 3000 frames: patched %.1f, original %.1f\n", t1, truth_x);
        if (t1 != truth_x) fail = 1;
    }

    /* Recorder: one entry per frame, last entry matches what the camera shows. */
    run(60, 3000, 1, 0.0, 0);
    {
        struct camfix_frame *f = &camfix_log.f[(camfix_log.widx - 1) & (LOG_N - 1)];
        int ok = camfix_log.widx == 3000 && *(float *)&f->v[0] == xv()[0] && (f->flags & FLAG_ENABLED);
        printf("recorder: %u frames logged, last entry matches camera: %s\n", camfix_log.widx, ok ? "yes" : "NO");
        if (!ok) fail = 1;
    }

    /* Switch off (as probe.py does mid-run): behaviour must equal the original. */
    reset();
    {
        u32 next_tick = 1000, now;
        int i, frozen = 0;
        float last = -1;
        camfix_log.enabled = 0;
        for (i = 0; i < 600; i++) {
            real_ms += 1000.0 / 60;
            now = (u32)floor(real_ms);
            camfix_frame_begin(now);
            while (next_tick <= now) { camfix_camera_tick((i32)next_tick); next_tick += 20; }
            camfix_frame_present(now);
            if (xv()[0] == last) frozen++;
            last = xv()[0];
        }
        /* 500 ticks over 600 frames: one frame in six has no tick (99 or 100 by phase). */
        printf("switched off: %d of 600 frames frozen (expect 99-100), interp=%u corrupt=%d\n",
               frozen, camfix_state.n_interp, corrupt);
        if (frozen < 99 || frozen > 100 || camfix_state.n_interp || corrupt) fail = 1;
    }

    /* Limiter. Sleep is modelled on Windows' default 15.625 ms timer grid. */
    {
        double lo, hi, lf;
        lf = run_limiter(1, 0, 2.0, 600, &lo, &hi);
        printf("limiter original, 2 ms work : frame %.2f..%.2f ms, long frames %.0f%%\n", lo, hi, lf * 100);
        lf = run_limiter(0, 16667, 2.0, 600, &lo, &hi);
        printf("limiter precise 16.667 ms    : frame %.3f..%.3f ms, long frames %.0f%%\n", lo, hi, lf * 100);
        if (lo < 16.6 || hi > 16.75 || lf > 0) fail = 1;
        lf = run_limiter(0, 4000, 8.33, 600, &lo, &hi);
        printf("limiter 4 ms cap, 8.33 ms work: frame %.3f..%.3f ms (must not add delay)\n", lo, hi);
        if (hi > 8.36) fail = 1;
        lf = run_limiter(0, 0, 1.0, 600, &lo, &hi);
        printf("limiter off, 1 ms work       : frame %.3f..%.3f ms\n", lo, hi);
        if (hi > 1.02) fail = 1;
        /* Swap wrapper records both timestamps and passes the result through. */
        reset();
        if (camfix_swap(0) != 1 || camfix_state.swap_q1 - camfix_state.swap_q0 != 5000) {
            printf("swap wrapper: NO\n");
            fail = 1;
        } else printf("swap wrapper: timestamps ok\n");
    }

    /* Another writer (e.g. a path camera) moves XCamera after us: we must not undo it. */
    reset();
    {
        u32 now = 1000;
        real_ms = 1000.3;
        camfix_frame_begin(now);
        camfix_camera_tick(1000);
        camfix_frame_present(now);
        real_ms += 16.7; now = (u32)floor(real_ms);
        camfix_frame_begin(now);
        camfix_frame_present(now);          /* writes an interpolated value */
        xv()[0] = 777.0f;                   /* external write */
        real_ms += 16.7; now = (u32)floor(real_ms);
        camfix_frame_begin(now);
        printf("external write kept: %s (x=%.1f, external=%u)\n",
               xv()[0] == 777.0f ? "yes" : "NO", xv()[0], camfix_state.n_external);
        if (xv()[0] != 777.0f) fail = 1;
    }

    /* Unknown camera class: hooks must do nothing. */
    reset();
    fake_vt[0x2c / 4] = 0x12345678;
    camfix_frame_begin(1000);
    camfix_camera_tick(1000);
    camfix_frame_present(1005);
    printf("unknown XCamera class ignored: %s\n", camfix_state.valid == 0 && camfix_state.n_interp == 0 ? "yes" : "NO");
    if (camfix_state.valid || camfix_state.n_interp) fail = 1;

    printf(fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fail;
}
