#!/bin/sh
# Commit the current state of ray-tracing/ to the branch `cgi` and push it to origin.
# Safe beside the user's checkout and other sessions: it uses its own index file and only
# moves refs/heads/cgi, so the checked-out branch, the normal index and the working tree are
# never touched. Skips a round while a stream merge holds merge.lock (main may be half-merged).
#   sh cgi_push.sh            one round
#   sh cgi_push.sh loop MIN   a round every MIN minutes (default 15) until killed
# Not committed (too big or generated): out/, captures/, models/ (*.pth: see README), build/.
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)

skips=0
round() {
  note=""
  if [ -d "$REPO/merge.lock" ]; then
    holder=$(cat "$REPO/merge.lock/owner" 2>/dev/null || ls "$REPO/merge.lock" 2>/dev/null | head -1)
    skips=$((skips + 1))
    if [ "$skips" -lt "${FORCE_AFTER:-5}" ]; then
      echo "$(date +%H:%M) skip: merge.lock held ($holder)"
      return 2
    fi
    # the lock has been held for every try in ~10 min: snapshot anyway, and say so
    note="Taken while merge.lock was held by: $holder (main may be mid-merge; the next snapshot supersedes it)."
    echo "$(date +%H:%M) lock held $skips times in a row: snapshot anyway"
  fi
  skips=0
  GIT_INDEX_FILE="$(git rev-parse --git-dir)/cgi-index"
  export GIT_INDEX_FILE
  parent=$(git rev-parse -q --verify refs/heads/cgi || git rev-parse HEAD) || return 1
  git read-tree "$parent" || return 1
  # build/ and __pycache__ are already ignored by the repo's .gitignore
  git -c core.autocrlf=false add -A -- ray-tracing \
    ':(exclude)ray-tracing/out' ':(exclude)ray-tracing/captures' \
    ':(exclude)ray-tracing/models' || return 1
  tree=$(git write-tree) || return 1
  if [ "$tree" = "$(git rev-parse "$parent^{tree}")" ]; then
    echo "$(date +%H:%M) no changes since $(git rev-parse --short "$parent")"
  else
    msg="WUM ray-tracing film look: snapshot $(date '+%Y-%m-%d %H:%M')

Automatic snapshot of WormUltimateMayhem-Fixes/ray-tracing (merged streams only).
$note

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
    commit=$(printf '%s\n' "$msg" | git commit-tree "$tree" -p "$parent") || return 1
    old=$(git rev-parse -q --verify refs/heads/cgi)
    git update-ref refs/heads/cgi "$commit" $old || return 1
    echo "$(date +%H:%M) committed $(git rev-parse --short "$commit")"
  fi
  unset GIT_INDEX_FILE
  if git push -q origin refs/heads/cgi:refs/heads/cgi 2>&1; then
    echo "$(date +%H:%M) pushed cgi -> origin ($(git rev-parse --short refs/heads/cgi))"
  else
    echo "$(date +%H:%M) PUSH FAILED"
    return 1
  fi
}

if [ "${1:-}" = loop ]; then
  every=$(( ${2:-15} * 60 ))
  while :; do
    round; rc=$?
    if [ "$rc" = 2 ]; then sleep 120; else sleep "$every"; fi   # lock held: retry soon
  done
else
  round
fi
