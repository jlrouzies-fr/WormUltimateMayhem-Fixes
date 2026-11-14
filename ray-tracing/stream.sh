#!/bin/sh
# Parallel work streams: each stream has its own copy of ray-tracing, its own sandbox game and a
# merge base, so several sessions can work at once and merge into the main copy file by file.
#
#   sh stream.sh new NAME [--private-textures]   make WormsXHD/streams/NAME (copy, base, sandbox) and build it
#   sh stream.sh merge NAME    merge the stream's changes into main ray-tracing, build main; KEEPS merge.lock
#                              (run main's selftest while holding it, then: sh stream.sh unlock)
#   sh stream.sh sync NAME     bring main's changes into the stream, rebuild its sandbox, re-patch its CG
#   sh stream.sh unlock        release WormUltimateMayhem-Fixes/merge.lock
#   sh stream.sh slot [take NAME [MIN] | give [NEXT] | free | queue NAME]   the one game slot (sandbox.py
#                              start refuses unless your stream holds it; see sandbox.py)
#   sh stream.sh list          streams, their games, the lock
#   sh stream.sh delete NAME   remove a stream (its game must be stopped; junction targets are kept)
#
# Layout (the relative paths ../../rt-sandbox in build.sh, rt.py, sandbox.py ... then point at the
# stream's own sandbox):
#   WormsXHD/streams/NAME/WormUltimateMayhem-Fixes/ray-tracing/   the stream's working copy (top-level files;
#                                                                  out/, captures/, build/ start empty, models/ is a junction)
#   WormsXHD/streams/NAME/base/                                    main's files when last forked/synced/merged = merge base
#   WormsXHD/streams/NAME/rt-sandbox/                              own game copy; Data, EFMV, speech and wumrt_textures
#                                                                  are junctions (wumrt_textures copied with --private-textures)
# Only top-level files of ray-tracing are tracked (sources, scripts, shaders, ini files, README.md).
# Can be run from main ray-tracing or from any stream's copy: it always finds WormsXHD.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
UP3=$(cd "$HERE/../../.." && pwd)
if [ "$(basename "$UP3")" = streams ] && [ -d "$UP3/../WormUltimateMayhem-Fixes/ray-tracing" ]; then
  ROOT=$(cd "$UP3/.." && pwd)               # run from a stream's copy
else
  ROOT=$(cd "$HERE/../.." && pwd)           # run from main
fi
MAIN="$ROOT/WormUltimateMayhem-Fixes/ray-tracing"
MSB="$ROOT/rt-sandbox"
STREAMS="$ROOT/streams"
LOCK="$ROOT/WormUltimateMayhem-Fixes/merge.lock"

die() { echo "stream.sh: $*" >&2; exit 1; }
winpath() { cygpath -w "$1"; }
junction() {   # junction LINK TARGET
  MSYS_NO_PATHCONV=1 cmd /c mklink /J "$(winpath "$1")" "$(winpath "$2")" >/dev/null || die "could not make junction $1"
}
check_name() {
  echo "$1" | grep -Eq '^[a-z0-9][a-z0-9_-]{0,39}$' || die "stream names are lower case letters, digits, - and _ (got '$1')"
}
files_of() {   # top-level regular files of a folder, one name per line
  [ -d "$1" ] && find "$1" -maxdepth 1 -type f ! -name '*.pyc' ! -name '*~' -printf '%f\n' | sort
}
# Text is compared and merged with CRs stripped and written with LF only: an editor or a Windows
# Python that writes CRLF must not turn whole files into conflicts.
TMPD=$(mktemp -d); trap 'rm -rf "$TMPD"' EXIT
CR=$(printf '\r')
is_text() { [ ! -s "$1" ] || grep -Iq . "$1"; }
lf() {   # lf FILE -> FILE itself if it has no CR (or is binary), else an LF copy in TMPD
  if is_text "$1" && grep -qU "$CR" "$1"; then o=$(mktemp "$TMPD/lf.XXXXXX"); tr -d '\r' < "$1" > "$o"; echo "$o"
  else echo "$1"; fi
}
put() {  # put SRC DST: copy, LF only for text
  if is_text "$1"; then tr -d '\r' < "$1" > "$2"; else cp -p "$1" "$2"; fi
}
same() { cmp -s "$1" "$2" || cmp -s "$(lf "$1")" "$(lf "$2")"; }

take_lock() {  # take_lock WHO: atomic mkdir; re-entrant for the same WHO
  if mkdir "$LOCK" 2>/dev/null; then
    echo "$1 $(date '+%H:%M:%S') pid $$" > "$LOCK/owner"
    return 0
  fi
  owner=$(cat "$LOCK/owner" 2>/dev/null)
  case "$owner" in "$1 "*) return 0 ;; esac
  die "merge.lock is held by: ${owner:-unknown}. Wait for it (that session runs main's selftest, then 'sh stream.sh unlock'); try again later."
}
release_lock() { rm -f "$LOCK/owner"; rmdir "$LOCK" 2>/dev/null; }

game_running() {  # game_running SANDBOX_DIR
  powershell -NoProfile -Command "Get-Process WormsMayhem -ErrorAction SilentlyContinue | ForEach-Object { \$_.Path }" \
    | tr -d '\r' | while read -r p; do
        [ -n "$p" ] && [ "$(cygpath -u "$p" | tr A-Z a-z)" = "$(echo "$1/WormsMayhem.exe" | tr A-Z a-z)" ] && echo yes
      done | head -1
}

patch_cg() {   # fresh copies of the game's shaders, patched with this copy's patch_shaders.py
  sb=$1; src=$2
  rm -rf "$sb/CG.new" && mkdir "$sb/CG.new" && cp "$ROOT/CG/"* "$sb/CG.new/" || die "copying $ROOT/CG failed"
  (cd "$src" && py -3 patch_shaders.py "$sb/CG.new" | tail -1) || die "patch_shaders.py failed; $sb/CG left as it was"
  rm -rf "$sb/CG" && mv "$sb/CG.new" "$sb/CG"
}

cmd_new() {
  name=$1; shift; check_name "$name"
  private=0; [ "${1:-}" = --private-textures ] && private=1
  S="$STREAMS/$name"; W="$S/WormUltimateMayhem-Fixes/ray-tracing"; B="$S/base"; SB="$S/rt-sandbox"
  [ -e "$S" ] && die "$S already exists"
  [ -f "$MSB/WormsMayhem.exe" ] || die "main sandbox $MSB not found"
  mkdir -p "$W/out" "$W/captures" "$W/build" "$B" "$SB" || die "mkdir failed"
  for f in $(files_of "$MAIN"); do cp -p "$MAIN/$f" "$W/$f"; cp -p "$MAIN/$f" "$B/$f"; done
  [ -d "$MAIN/models" ] && junction "$W/models" "$MAIN/models"
  [ -f "$MAIN/build/rtcpu.exe" ] && cp -p "$MAIN/build/rtcpu.exe" "$W/build/"   # CPU reference tracer (built by hand)
  # the sandbox: same exe and DLLs as the main one; the harness DLLs are built below
  for f in WormsMayhem.exe glut32_real.dll cg.dll cgGL.dll fmod_event.dll fmodex.dll MemoryBufferedFilter.dll \
           steam_api.dll steam_appid.txt lang_eng local.cfg Default.cfg wumrt.ini; do
    [ -f "$MSB/$f" ] && cp -p "$MSB/$f" "$SB/$f"
  done
  sed -i 's|/W:[0-9]*|/W:480|; s|/H:[0-9]*|/H:270|' "$SB/Default.cfg"
  grep -q '^sandbox *= *1' "$SB/wumrt.ini" || echo "sandbox = 1" >> "$SB/wumrt.ini"
  sed -i 's/^scale *=.*/scale = 1/' "$SB/wumrt.ini"
  for d in Data EFMV speech; do junction "$SB/$d" "$ROOT/$d"; done
  if [ $private = 1 ]; then cp -r "$MSB/wumrt_textures" "$SB/wumrt_textures"; else junction "$SB/wumrt_textures" "$MSB/wumrt_textures"; fi
  patch_cg "$SB" "$W"
  (cd "$W" && sh build.sh 2>&1 | grep -E "error|built|cannot|loader") || true
  echo "stream $name ready:"
  echo "  work in   $W"
  echo "  its game  $SB (control block wumrt_ctl_$name; sh go.sh / python rt.py there talk to it)"
  echo "  merge     sh stream.sh merge $name   (then main's selftest, then sh stream.sh unlock)"
  [ $private = 1 ] || echo "  wumrt_textures is shared with the main sandbox (a junction): do not write to it from here"
}

merge_one() {  # merge_one CURRENT BASE OTHER FILE -> merged text on stdout, status 0 when clean
  a=$(lf "$1"); b=$(lf "$2"); c=$(lf "$3"); o=$(mktemp "$TMPD/merge.XXXXXX")
  if [ "$4" = README.md ]; then
    git merge-file --union -p "$a" "$b" "$c" > "$o" || return 1
    la=$(wc -l < "$a"); lc=$(wc -l < "$c"); lo=$(wc -l < "$o"); big=$la; [ "$lc" -gt "$big" ] && big=$lc
    if [ "$lo" -gt $((big * 13 / 10 + 40)) ]; then
      echo "stream.sh: the union merge of README.md came out $lo lines from $la and $lc: refused (whole sections doubled?)" >&2
      return 1
    fi
    cat "$o"
  else
    git merge-file -p "$a" "$b" "$c"
  fi
}

cmd_merge() {
  name=$1; check_name "$name"
  S="$STREAMS/$name"; W="$S/WormUltimateMayhem-Fixes/ray-tracing"; B="$S/base"
  [ -d "$W" ] || die "no stream $name"
  take_lock "$name"
  tmp=$(mktemp); conflicts=""; wrote=""; notes=""
  for f in $(files_of "$W"); do
    s="$W/$f"; b="$B/$f"; m="$MAIN/$f"
    if [ -f "$b" ] && same "$s" "$b"; then continue; fi           # not changed in the stream
    if grep -q '^<<<<<<< \|^>>>>>>> ' "$s"; then conflicts="$conflicts $f(unresolved conflict markers)"; continue; fi
    if [ ! -f "$b" ]; then                                          # new in the stream
      if [ ! -f "$m" ]; then put "$s" "$m"; wrote="$wrote $f(new)"
      elif same "$m" "$s"; then :
      else conflicts="$conflicts $f(added in both, different)"; continue; fi
    elif [ ! -f "$m" ]; then conflicts="$conflicts $f(deleted in main)"; continue
    elif same "$m" "$b"; then put "$s" "$m"; wrote="$wrote $f"
    elif same "$m" "$s"; then :
    elif merge_one "$m" "$b" "$s" "$f" > "$tmp"; then cp "$tmp" "$m"; wrote="$wrote $f(merged)"
    else conflicts="$conflicts $f"; continue; fi
    put "$s" "$b"                                                   # merged: the stream's version is the new base
  done
  for f in $(files_of "$B"); do [ -f "$W/$f" ] || notes="$notes $f"; done
  rm -f "$tmp"
  echo "written to main:${wrote:- nothing}"
  [ -n "$notes" ] && echo "deleted in the stream, NOT deleted in main (do it by hand if meant):$notes"
  if [ -n "$wrote" ]; then (cd "$MAIN" && sh build.sh 2>&1 | grep -E "error|built|cannot|loader"); fi
  if [ -n "$conflicts" ]; then
    echo "CONFLICTS, nothing written for:$conflicts"
    echo "  fix: sh stream.sh sync $name (merges main into your copy, shows the conflicts), resolve them in your copy, merge again."
    echo "  merge.lock is still yours; release it with sh stream.sh unlock if you stop here."
    exit 2
  fi
  if [ -z "$wrote" ]; then release_lock; echo "nothing to merge; merge.lock released"; return 0; fi
  echo "merge.lock is held by $name: run main's selftest (cd $MAIN && sh selftest.sh) now, then sh stream.sh unlock"
}

cmd_sync() {
  name=$1; check_name "$name"
  S="$STREAMS/$name"; W="$S/WormUltimateMayhem-Fixes/ray-tracing"; B="$S/base"; SB="$S/rt-sandbox"
  [ -d "$W" ] || die "no stream $name"
  # main must not change while we read it: take merge.lock for the sync (keep it if this stream
  # already held it, e.g. after a merge with conflicts); another stream's merge makes us stop
  keep=0; [ "$(cut -d' ' -f1 "$LOCK/owner" 2>/dev/null)" = "$name" ] && keep=1
  take_lock "$name"
  tmp=$(mktemp); conflicts=""; got=""
  for f in $(files_of "$MAIN"); do
    m="$MAIN/$f"; b="$B/$f"; s="$W/$f"
    if [ -f "$b" ] && same "$m" "$b"; then continue; fi           # main unchanged since base
    if [ ! -f "$s" ]; then
      if [ -f "$b" ]; then echo "  $f: changed in main but deleted in the stream; left out"; continue; fi
      put "$m" "$s"; got="$got $f(new)"
    elif [ ! -f "$b" ]; then
      if same "$s" "$m"; then :; else conflicts="$conflicts $f(added in both, different)"; continue; fi
    elif same "$s" "$b"; then put "$m" "$s"; got="$got $f"
    elif same "$s" "$m"; then :
    elif merge_one "$s" "$b" "$m" "$f" > "$tmp"; then cp "$tmp" "$s"; got="$got $f(merged)"
    else                                    # like git: markers into your copy, base moves on to main
      git merge-file -p "$(lf "$s")" "$(lf "$b")" "$(lf "$m")" > "$tmp" 2>/dev/null; cp "$tmp" "$s"
      conflicts="$conflicts $f"
    fi
    put "$m" "$b"
  done
  rm -f "$tmp"
  [ $keep = 1 ] || release_lock
  echo "from main:${got:- nothing new}"
  [ -f "$MAIN/build/rtcpu.exe" ] && cp -pu "$MAIN/build/rtcpu.exe" "$W/build/"
  (cd "$W" && sh build.sh 2>&1 | grep -E "error|built|cannot|loader")
  patch_cg "$SB" "$W"
  [ -n "$(game_running "$SB")" ] && echo "its game is running: python rt.py reload-core and python rt.py cg-reload pick the changes up"
  if [ -n "$conflicts" ]; then
    echo "CONFLICTS, marked with <<<<<<< ======= >>>>>>> in your copy of:$conflicts"
    echo "  edit them out (keep what both sides need); merge refuses a file that still has markers"
    exit 2
  fi
}

cmd_unlock() {
  [ -d "$LOCK" ] || { echo "merge.lock is not held"; return 0; }
  echo "releasing merge.lock (was: $(cat "$LOCK/owner" 2>/dev/null))"
  release_lock
}

cmd_list() {
  [ -d "$LOCK" ] && echo "merge.lock: $(cat "$LOCK/owner" 2>/dev/null)" || echo "merge.lock: free"
  echo "main sandbox game: $([ -n "$(game_running "$MSB")" ] && echo running || echo stopped)"
  [ -d "$STREAMS" ] || return 0
  for d in "$STREAMS"/*/; do
    [ -d "$d/rt-sandbox" ] || continue
    n=$(basename "$d")
    echo "stream $n: game $([ -n "$(game_running "${d%/}/rt-sandbox")" ] && echo running || echo stopped)"
  done
}

cmd_delete() {
  name=$1; check_name "$name"
  S="$STREAMS/$name"
  [ -d "$S" ] || die "no stream $name"
  [ -n "$(game_running "$S/rt-sandbox")" ] && die "its game is running: (cd $S/WormUltimateMayhem-Fixes/ray-tracing && py -3 sandbox.py stop) first"
  # junctions first, with rmdir, so nothing behind them is touched
  for j in "$S/rt-sandbox/Data" "$S/rt-sandbox/EFMV" "$S/rt-sandbox/speech" "$S/rt-sandbox/wumrt_textures" \
           "$S/WormUltimateMayhem-Fixes/ray-tracing/models"; do
    [ -L "$j" ] && MSYS_NO_PATHCONV=1 cmd /c rmdir "$(winpath "$j")"
  done
  left=$(powershell -NoProfile -Command "(Get-ChildItem -LiteralPath '$(winpath "$S")' -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue | Measure-Object).Count" | tr -d '\r')
  [ "$left" = 0 ] || die "$S still contains $left links; not deleting"
  rm -rf "$S" && echo "deleted stream $name"
}

case "${1:-}" in
  new) [ $# -ge 2 ] || die "usage: sh stream.sh new NAME [--private-textures]"; shift; cmd_new "$@" ;;
  merge) [ $# = 2 ] || die "usage: sh stream.sh merge NAME"; cmd_merge "$2" ;;
  sync) [ $# = 2 ] || die "usage: sh stream.sh sync NAME"; cmd_sync "$2" ;;
  unlock) cmd_unlock ;;
  slot) shift; py -3 "$HERE/sandbox.py" slot "$@" ;;
  list) cmd_list ;;
  delete) [ $# = 2 ] || die "usage: sh stream.sh delete NAME"; cmd_delete "$2" ;;
  *) sed -n '2,24p' "$0"; exit 1 ;;
esac
