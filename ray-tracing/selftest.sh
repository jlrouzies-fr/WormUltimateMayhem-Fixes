#!/bin/sh
# Smoke test: build, start the sandbox, check the tracer runs, matches the CPU reference with
# hard shadows, and logs no errors. Leaves the sandbox stopped.
cd "$(dirname "$0")"
py -3 sandbox.py stop >/dev/null 2>&1
sh build.sh 2>&1 | grep -E "error|built|cannot"
# A start refused because all sandbox game places are in use prints BUSY: show it and exit 3,
# so a caller can wait and retry (exit 0 would hide it behind "sandbox game is not running").
START=$(sh go.sh 2>&1)
echo "$START" | grep -E "start watch|BUSY"
if echo "$START" | grep -q "BUSY"; then
    echo "selftest not run: the sandbox could not start (BUSY)"
    exit 3
fi
sleep 8
py -3 rt.py verify 2000
py -3 rt.py rtstat 2 | sed -n 1,5p
py -3 rt.py key esc; sleep 2
py -3 rt.py rt on 1 0.5 0 >/dev/null; py -3 rt.py sky 0; sleep 1
# The CPU reference uses flat triangle normals: compare with smooth normals off, then put the setting back.
INI=../../rt-sandbox/wumrt.ini
SMOOTH=$(sed -n 's/^smooth = //p' $INI)
sed -i 's/^smooth = .*/smooth = 0/' $INI; sleep 3
py -3 compare_mask.py out/selftest | head -3
[ -n "$SMOOTH" ] && sed -i "s/^smooth = .*/smooth = $SMOOTH/" $INI
echo "errors in the harness log: $(grep -c -i -E 'rt:.*(fail|not found|not available)|differs from the real' ../../rt-sandbox/wumrt.log)"
echo "stack overflow fallbacks: $(grep -c 'overflowed the traversal' ../../rt-sandbox/wumrt.log)"
py -3 sandbox.py stop
rm -f out/selftest.bin out/selftest_gpu.wrtf
