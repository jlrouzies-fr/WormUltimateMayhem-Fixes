#!/bin/sh
# Final-window install of the style-C set into MAIN's sandbox: main's current folder is kept as
# rt-sandbox/wumrt_textures_redo (renamed, nothing deleted), this copy's finished folder
# (colour + normal maps with model heights + packed .wtc) is copied in, and a stale-pack check is run.
# Main's game must be stopped. Undo: rm -r wumrt_textures; mv wumrt_textures_redo wumrt_textures
set -e
cd "$(dirname "$0")"
MAIN=../../../../rt-sandbox
SRC=../../rt-sandbox/wumrt_textures
[ -d "$MAIN/wumrt_textures_redo" ] && { echo "wumrt_textures_redo already exists - stop"; exit 1; }
[ "$(ls $SRC | wc -l)" -eq 1804 ] || { echo "source folder incomplete"; exit 1; }
rm -rf "$MAIN/wumrt_textures_new"
cp -r "$SRC" "$MAIN/wumrt_textures_new"          # copy first, then two renames: the swap is instant
[ "$(ls $MAIN/wumrt_textures_new | wc -l)" -eq 1804 ] || { echo "copy incomplete"; exit 1; }
mv "$MAIN/wumrt_textures" "$MAIN/wumrt_textures_redo"
mv "$MAIN/wumrt_textures_new" "$MAIN/wumrt_textures"
echo "main: $(ls $MAIN/wumrt_textures | wc -l) files, redo kept: $(ls $MAIN/wumrt_textures_redo | wc -l) files"
py -3 texpack.py "$MAIN/wumrt_textures" --normals | tail -1
