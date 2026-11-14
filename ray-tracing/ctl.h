/* The shared-memory control block between the harness inside the game and rt.py. Used by
   both the permanent loader (loader.c, the proxy glut32.dll) and the swappable core
   (wumrt.c and friends, wumrt_core.dll). Its layout is mirrored in rt.py. */
#ifndef CTL_H
#define CTL_H
#include "rt.h"

#define CTL_MAGIC 0x31545257u   /* 'WRT1' */
#define CTL_NAME "wumrt_ctl"     /* WormsXHD/rt-sandbox; a parallel stream's sandbox,
                                    streams/NAME/rt-sandbox, uses "wumrt_ctl_NAME" (the loader
                                    derives it from the exe's path, rt.py from its own folder) */

/* Commands the core answers (at the end of a frame). */
#define CMD_SCREENSHOT 1
#define CMD_CAPTURE 2
#define CMD_RT 5           /* arg0: 0 off / 1 on, arg1: sun samples, arg2: sun radius in millidegrees, arg3: filter radius + 1 */
#define CMD_RT_RELOAD 6    /* recompile wumrt_trace.comp */
#define CMD_DUMP_MASK 7    /* path: 'WRTF', w, h, then w*h*2 floats */
#define CMD_VERIFY 8       /* arg0: check the tracked GL state against the real one for this many draws */
#define CMD_PACING 9       /* arg0: swap interval (0 = no V-Sync), arg1: frame cap in fps (0 = none) */
#define CMD_RT_SCALE 10     /* arg0: mask resolution in thousandths of the viewport's */
#define CMD_RT_SKY 11       /* arg0: sky rays, arg1: reach in world units, arg2: strength, arg3: bounce (thousandths) */
#define CMD_DUMP_LAYERS 12 /* path: every material texture layer, for labelling (see rt_dump_layers) */
#define CMD_DUMP_TEXTURES 13 /* path: prefix; every material texture, full size, as PREFIX + HASH.wtx */
#define CMD_FREECAM 14     /* arg0: 0 off, 1 set the pose in path (floats: eye xyz, yaw, pitch in degrees),
                              2 take the game's current view as the pose (and switch on); the pose in use
                              is written back to path as the same five floats */

/* Commands the loader answers (after the core's end of frame). The core leaves them alone. */
#define CMD_LOADER_FIRST 100
#define CMD_RELOAD_CORE 100    /* unload the core, load wumrt_core.dll again; result: 0 ok */
#define CMD_CG_RELOAD 101      /* compile the game's Cg shaders again; path: file name to limit it to ("" = all);
                                  result: number of files that failed (they keep their old programs) */
#define CMD_CG_STATUS 102      /* nothing; arg0..3 come back as generation, programs, cached values, translated params */

#pragma pack(push, 1)
struct ctl {
    u32 magic, size;
    u32 frame;                  /* SwapBuffers count */
    u32 cmd_seq, ack_seq;       /* rt.py bumps cmd_seq; we copy it to ack_seq when done */
    u32 cmd, result;
    u32 arg[4];
    u32 stat[8];                /* last frame: elements, arrays, range, begin, calllist, verts, masked draws, - */
    u32 kbd_state_calls, kbd_data_calls, kbd_devices, pad_enum_blocked;
    u32 shot_w, shot_h;
    u32 hwnd;
    u32 swap_tick;              /* GetTickCount at the last swap */
    i32 mouse_x, mouse_y;       /* client coordinates GetCursorPos reports */
    u32 verify_checked, verify_bad;     /* CMD_VERIFY results */
    u32 mouse_seq; i32 mouse_dx, mouse_dy;  /* bump mouse_seq to move the game's mouse by dx, dy for one frame */
    u32 core_loads;             /* how many times a core was loaded in this process */
    u32 core_ok;                /* 1 while a core is loaded */
    u32 reserved[9];
    u8 dik[256];                /* DirectInput scan codes held down */
    u8 vk[256];                 /* virtual keys held down */
    char path[260];             /* output file for the command */
    struct rt_stats rt;         /* real-time tracer, last frame */
};
#pragma pack(pop)

#endif
