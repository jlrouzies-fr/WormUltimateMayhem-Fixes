/* Interface between the permanent loader (loader.c -> glut32.dll) and the swappable core
   (wumrt.c, rt_core.c, bvh.c -> wumrt_core.dll).

   The loader owns everything that must outlive a core: the import-table slots (each points
   at a loader thunk or loader function for the life of the process), the functions the
   engine fetched with wglGetProcAddress (same), the input isolation, the control block,
   the log, the record of every Cg program the engine created, and the translation of the
   engine's Cg program and parameter handles to recompiled programs. A core can be unloaded
   at the end of any frame and a new build loaded in its place (rt.py reload-core). */
#ifndef WUMRT_API_H
#define WUMRT_API_H

#include <stdarg.h>
#include "ctl.h"

#define WUMRT_API_VERSION 1

/* One program the engine created with cgCreateProgramFromFile. `orig` is the handle the
   engine holds; `cur` is what it stands for now (different after a Cg reload). */
struct cg_record {
    void *ctx;
    int type, profile, is_vertex;
    char file[260];         /* as the engine gave it */
    char entry[64];
    char profile_name[16];
    const char *args[17];   /* NULL-terminated, pointing into argbuf (all NULL: none given) */
    int has_args;
    char argbuf[512];
    void *orig, *cur;
};

struct wumrt_loader {
    u32 version;                        /* WUMRT_API_VERSION */
    u32 generation;                     /* 1 for the first core of this process, 2 after one reload, ... */
    struct ctl *ctl;
    int sandbox_mode;                   /* input isolation is on (read once, at start-up) */
    void (*vlog)(const char *fmt, va_list ap);
    /* Point an import-table slot of WormsMayhem.exe at fn; returns the function to call on
       (the real one, or for slots the loader handles itself, the loader's next step). */
    void *(*hook_iat)(u32 slot, void *fn);
    PROC (*real_proc)(const char *name);            /* the real wglGetProcAddress */
    /* Zeroed memory owned by the loader that survives core reloads, found by name. A second
       call with the same name and size returns the same block; with another size, a new
       zeroed one. *created (if not NULL) says whether it is new. */
    void *(*persist)(const char *name, u32 size, int *created);
    u32 nprog;
    struct cg_record *prog;             /* nprog records, in creation order */
    void *(*cg_current)(void *h);       /* any program handle the engine has seen -> the one in use */
    void *(*bound_program)(int is_vertex);  /* last program the engine bound, current handle */
    int (*profile_enabled)(int profile);
    HWND (*game_window)(void);
    /* Give every Cg parameter the last value the engine gave it again, through the core's
       hooks (used when the free camera changes). Returns how many. */
    u32 (*reapply_values)(void);
};

struct wumrt_core {
    u32 version;
    /* Called for every function the engine fetches with wglGetProcAddress (again for all of
       them after a reload); returns what the engine's calls should go to (real, or a wrapper). */
    PROC (*wrap_proc)(const char *name, PROC real);
    /* The engine created a program (after the core is loaded; earlier ones are in L->prog). */
    void (*program_created)(const struct cg_record *r);
    /* A Cg reload replaced programs: L->prog[i].cur changed for some i. */
    void (*programs_replaced)(void);
    /* Stop threads, delete GL objects, leave the game's shaders in their stock state. Called
       at the end of a frame, with the game's GL context current, before FreeLibrary. */
    void (*shutdown)(void);
};

typedef int (__cdecl *wumrt_core_init_fn)(struct wumrt_loader *L, struct wumrt_core *out);

#endif
