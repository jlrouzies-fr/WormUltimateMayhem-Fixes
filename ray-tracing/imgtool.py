"""Small image helpers for reviewing screenshots.

    python imgtool.py crop IN.png OUT.png X Y W H [SCALE]     cut a region and enlarge it
    python imgtool.py pair A.png B.png OUT.png [X Y W H [SCALE]]   A and B side by side (same region)
    python imgtool.py diff A.png B.png OUT.png [GAIN]          absolute difference, amplified
"""
import sys

import numpy as np

from trace_capture import read_png, write_png


def region(img, a):
    if len(a) >= 4:
        x, y, w, h = map(int, a[:4])
        img = img[y:y + h, x:x + w]
        s = int(a[4]) if len(a) > 4 else 1
        if s > 1:
            img = np.repeat(np.repeat(img, s, axis=0), s, axis=1)
    return img


def main():
    a = sys.argv[1:]
    if a[0] == "crop":
        write_png(a[2], np.ascontiguousarray(region(read_png(a[1]), a[3:])))
    elif a[0] == "pair":
        A, B = region(read_png(a[1]), a[4:]), region(read_png(a[2]), a[4:])
        gap = np.full((A.shape[0], 6, 3), 255, np.uint8)
        write_png(a[3], np.ascontiguousarray(np.concatenate([A, gap, B], axis=1)))
    elif a[0] == "diff":
        A, B = read_png(a[1]).astype(np.int16), read_png(a[2]).astype(np.int16)
        g = float(a[4]) if len(a) > 4 else 4
        write_png(a[3], np.clip(np.abs(A - B) * g, 0, 255).astype(np.uint8))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
