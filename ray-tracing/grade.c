/* Per-level colour script (stream "grade"), included from post.c.
   With grade_levels = 1 every level listed in wumrt_grades.ini gets its own grade on top of the
   global one (tone_contrast, saturation, grade_warm, grade_auto ...): lift (added in the
   shadows), gamma (midtones), gain (multiplies) per channel, and a saturation factor.
   The level is recognised by the terrain textures it draws: wumrt.c reports every landscape
   draw's texture hash (post_note_terrain); the level whose listed textures were seen most in the
   last 2 s wins. Unknown terrain blends back to no level grade; menus (no terrain) keep the last
   one (the file is read again when it changes). A level grade fades out at night (grade_auto) unless it says `night 1`. The grade blends over ~2 s (grade_speed per frame). New terrain hashes go to the log as
   "grade: terrain HASH" so a level's textures are easy to list. */

#define GR_MAX 32
#define GR_HASHES 64
static struct {
    char name[48];
    u32 hash[GR_HASHES], seen[GR_HASHES], nhash;
    float v[10];                    /* lift rgb, gamma rgb, gain rgb, saturation */
    int night;                      /* 1: also at night; 0 (default): fades out as grade_auto sees night */
} GR[GR_MAX];
static u32 n_gr;
static int gr_on, gr_loaded;
static float gr_speed = 0.04f;
static int gr_cur = -1;             /* the level recognised now, -1 none */
static float gr_v[10] = { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1 };     /* blended values in use */
static u32 gr_frame, gr_terrain_seen;
static struct { u32 hash, seen; } gr_recent[64];   /* for the log of new terrain textures */

static void set4(GLuint prog, const char *name, float a, float b, float c, float d);
#include <sys/stat.h>
static long gr_mtime;               /* wumrt_grades.ini is read again when it changes */

static void grade_load(void)
{
    FILE *f = fopen("wumrt_grades.ini", "r");
    char line[512];
    struct stat st;
    gr_loaded = 1;
    gr_mtime = stat("wumrt_grades.ini", &st) ? 0 : (long)st.st_mtime;
    n_gr = 0;
    gr_cur = -1;
    if (!f) { rt_log("grade: no wumrt_grades.ini"); return; }
    while (fgets(line, sizeof line, f)) {
        char *s = line, *h = strchr(line, '#'), key[16];
        float a, b, c;
        int n, k, i;
        if (h) *h = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (n_gr >= GR_MAX || !e) continue;
            *e = 0;
            memset(&GR[n_gr], 0, sizeof GR[0]);
            strncpy(GR[n_gr].name, s + 1, sizeof GR[0].name - 1);
            for (i = 3; i < 10; i++) GR[n_gr].v[i] = 1;
            n_gr++;
            continue;
        }
        if (!n_gr || sscanf(s, "%15s%n", key, &n) != 1) continue;
        s += n;
        if (!strcmp(key, "night")) { GR[n_gr - 1].night = atoi(s) != 0; continue; }
        if (!strcmp(key, "textures")) {
            u32 x;
            while (GR[n_gr - 1].nhash < GR_HASHES && sscanf(s, " %8x%n", &x, &n) == 1) {
                GR[n_gr - 1].hash[GR[n_gr - 1].nhash++] = x;
                s += n;
            }
            continue;
        }
        k = !strcmp(key, "lift") ? 0 : !strcmp(key, "gamma") ? 3 : !strcmp(key, "gain") ? 6 : !strcmp(key, "saturation") ? 9 : -1;
        if (k < 0) continue;
        n = sscanf(s, " %f %f %f", &a, &b, &c);
        if (k == 9) { if (n >= 1) GR[n_gr - 1].v[9] = a; continue; }
        if (n == 1) b = c = a;                              /* one value: all three channels */
        if (n == 1 || n == 3) { GR[n_gr - 1].v[k] = a; GR[n_gr - 1].v[k + 1] = b; GR[n_gr - 1].v[k + 2] = c; }
    }
    fclose(f);
    rt_log("grade: %u level grades read from wumrt_grades.ini", n_gr);
}

int post_wants_terrain(void) { return S.on && gr_on; }

void post_note_terrain(u32 hash)
{
    u32 i, j, old = 0;
    if (!hash) return;
    gr_terrain_seen = gr_frame;
    for (i = 0; i < n_gr; i++)
        for (j = 0; j < GR[i].nhash; j++)
            if (GR[i].hash[j] == hash) GR[i].seen[j] = gr_frame + 1;
    for (i = 0; i < 64; i++) {
        if (gr_recent[i].hash == hash) {
            if (gr_frame - gr_recent[i].seen > 300) rt_log("grade: terrain %08x", hash);
            gr_recent[i].seen = gr_frame;
            return;
        }
        if (gr_recent[i].seen < gr_recent[old].seen) old = i;
    }
    gr_recent[old].hash = hash; gr_recent[old].seen = gr_frame;
    rt_log("grade: terrain %08x", hash);
}

/* Once a frame, at the tone pass: which level, blend towards its grade, set the uniforms. */
static void grade_uniforms(GLuint prog, float night)
{
    static const float none[10] = { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1 };
    float t[10], k;
    int best = -1, i;
    u32 j, best_n = 0;
    gr_frame++;
    if (!gr_on) { set4(prog, "uLift", 0, 0, 0, 0); return; }
    if (gr_loaded && (gr_frame % 30) == 0) {
        struct stat st;
        if (!stat("wumrt_grades.ini", &st) && (long)st.st_mtime != gr_mtime) gr_loaded = 0;
    }
    if (!gr_loaded) grade_load();
    if (gr_frame - gr_terrain_seen < 60) {
        for (i = 0; i < (int)n_gr; i++) {
            u32 n = 0;
            for (j = 0; j < GR[i].nhash; j++) n += GR[i].seen[j] && gr_frame + 1 - GR[i].seen[j] < 60;
            if (n > best_n) { best_n = n; best = i; }
        }
        if (best != gr_cur) {
            rt_log("grade: level %s", best >= 0 ? GR[best].name : "(not listed: no level grade)");
            gr_cur = best;
        }
    }
    /* a day grade fades out on a night match of the same level (grade_auto's night 0..1) */
    k = gr_cur < 0 ? 0.0f : GR[gr_cur].night ? 1.0f : 1.0f - night;
    for (i = 0; i < 10; i++) t[i] = none[i] + ((gr_cur >= 0 ? GR[gr_cur].v[i] : none[i]) - none[i]) * k;
    for (i = 0; i < 10; i++) gr_v[i] += (t[i] - gr_v[i]) * gr_speed;
    set4(prog, "uLift", gr_v[0], gr_v[1], gr_v[2], 1);
    set4(prog, "uGamma", gr_v[3], gr_v[4], gr_v[5], 0);
    set4(prog, "uGain", gr_v[6], gr_v[7], gr_v[8], gr_v[9]);
}
