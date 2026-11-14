"""Reader for wumrt frame captures (see struct draw_rec in wumrt.c)."""
import struct
from types import SimpleNamespace

import numpy as np

PROG = struct.Struct("<3I64s64s16s")
DRAW = struct.Struct("<6I I 4i 9I 2I I 2I I 16f16f16f16f4f 16f16f 6I 4I 2I I")
GL_TYPES = {0x1400: "i1", 0x1401: "u1", 0x1402: "<i2", 0x1403: "<u2", 0x1404: "<i4", 0x1405: "<u4",
            0x1406: "<f4", 0x140A: "<f8"}
KINDS = ["elements", "arrays", "range", "begin", "calllist"]
MODES = {0: "points", 1: "lines", 3: "line_strip", 4: "triangles", 5: "tri_strip", 6: "tri_fan", 7: "quads"}


def _s(b):
    return b.split(b"\0")[0].decode("latin1")


def load(path):
    """Returns (programs {index: namespace}, draws [namespace])."""
    d = open(path, "rb").read()
    off, progs, draws = 0, {}, []
    while off < len(d):
        magic, = struct.unpack_from("<I", d, off)
        if magic == 0x474F5250:
            _, idx, isv, f, e, p = PROG.unpack_from(d, off)
            off += PROG.size
            progs[idx] = SimpleNamespace(index=idx, is_vertex=bool(isv), file=_s(f), entry=_s(e), profile=_s(p))
        elif magic == 0x57415244:
            r = DRAW.unpack_from(d, off)
            off += DRAW.size
            m = lambda i: np.array(r[i:i + 16], dtype=np.float32).reshape(4, 4)
            dr = SimpleNamespace(
                n=len(draws), kind=KINDS[r[1]], mode=MODES.get(r[2], r[2]), first=r[3], count=r[4], index_type=r[5],
                fbo=r[6], viewport=r[7:11], depth_test=r[11], blend=r[12], cull=r[13], alpha_test=r[14],
                depth_mask=r[15], color_mask=r[16], depth_func=r[17], cull_mode=r[18], front_face=r[19],
                blend_src=r[20], blend_dst=r[21], tex0=r[22],
                vp=progs.get(r[23]), fp=progs.get(r[24]), have=r[25],
                model=m(26), view=m(42), proj=m(58), shadow=m(74), light=np.array(r[90:93], dtype=np.float32),
                gl_modelview=m(94), gl_projection=m(110),
                v_size=r[126], v_type=r[127], v_stride=r[128], v_ptr=r[129], v_buffer=r[130], elem_buffer=r[131],
                n_size=r[132], n_ptr=r[134], n_buffer=r[135], nverts=r[136], vbytes=r[137], nidx=r[138],
                pos=None, idx=None)
            if dr.vbytes and dr.v_type in GL_TYPES and dr.v_size >= 2:
                dt = np.dtype(GL_TYPES[dr.v_type])
                stride = dr.v_stride or dr.v_size * dt.itemsize
                raw = np.frombuffer(d, dtype=np.uint8, count=dr.vbytes, offset=off)
                rows = np.lib.stride_tricks.as_strided(
                    raw, shape=(dr.nverts, dr.v_size * dt.itemsize), strides=(stride, 1))
                dr.pos = np.ascontiguousarray(rows).view(dt).reshape(dr.nverts, dr.v_size).astype(np.float32)
            off += dr.vbytes
            if dr.nidx:
                dr.idx = np.frombuffer(d, dtype="<u4", count=dr.nidx, offset=off).copy()
            off += 4 * dr.nidx
            draws.append(dr)
        elif magic == 0x21444E45:
            break
        else:
            raise ValueError(f"bad record at {off}: {magic:#x}")
    return progs, draws


def triangles(dr):
    """Index triples (N,3) into dr.pos for the draw's primitives, or None."""
    if dr.pos is None:
        return None
    if dr.kind == "arrays":
        idx = np.arange(dr.first, dr.first + dr.count, dtype=np.uint32)
    elif dr.idx is not None:
        idx = dr.idx
    else:
        return None
    n = len(idx)
    if dr.mode == "triangles":
        return idx[:n - n % 3].reshape(-1, 3)
    if dr.mode == "tri_strip" and n >= 3:
        i = np.arange(n - 2)
        a, b, c = idx[i], idx[i + 1], idx[i + 2]
        odd = (i % 2 == 1)
        t = np.stack([np.where(odd, b, a), np.where(odd, a, b), c], axis=1)
        return t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    if dr.mode == "tri_fan" and n >= 3:
        i = np.arange(1, n - 1)
        return np.stack([np.full(len(i), idx[0]), idx[i], idx[i + 1]], axis=1)
    if dr.mode == "quads":
        q = idx[:n - n % 4].reshape(-1, 4)
        return np.concatenate([q[:, [0, 1, 2]], q[:, [0, 2, 3]]])
    return None
