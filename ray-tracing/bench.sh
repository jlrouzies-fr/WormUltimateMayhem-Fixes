#!/bin/sh
# Benchmark the tracer in the current (ideally paused) view: GPU time for hard and soft
# shadows at the sandbox's own low resolution. Larger masks (rt.py scale N) load the GPU
# heavily; only use them when nothing else needs it.
cd "$(dirname "$0")"
b() { py -3 rt.py rt on $1 $2 $3 >/dev/null; sleep 0.6; py -3 rt.py rtstat 2 | grep -o "trace (GPU) [0-9.]* ms" | grep -o "[0-9.]* ms"; }
py -3 rt.py scale 1
echo "hard $(b 1 0.5 0) | soft 8 samples $(b 8 1.5 2)"
py -3 rt.py rt on 8 1.5 2 >/dev/null
