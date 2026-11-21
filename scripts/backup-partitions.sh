#!/bin/zsh
# Dump SM-T210 eMMC partitions to the Mac over root ADB (run while booted in TWRP).
# Usage: scripts/backup-partitions.sh <out-dir> [PARTITION ...]
# Default list = everything except USER (/data). Pass names to override (e.g. USER, mmcblk0boot0).
#
# Every partition is copied in CHUNK_MB (default 8) MiB chunks. Each chunk's SHA-256 is computed on the device
# and on the Mac and must match; a failed chunk is retried after waiting for the device
# (on a weak cable the USB link drops when VBUS sags). Finished chunks are kept, so
# re-running the script resumes. Final image size is checked against /proc/partitions.
set -eu
# adb: $ADB, else adb on PATH, else the default Android SDK location on macOS.
ADB=${ADB:-$(command -v adb || echo "${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb")}
# Device serial: set SERIAL when more than one device is attached.
SERIAL=${SERIAL:-}
CHUNK_MB=${CHUNK_MB:-8}   # small chunks fit between VBUS drops (every ~6–30 s on this cable)
OUT=${1:?usage: $0 <out-dir> [PARTITION ...]}; shift
PARTS=(MRD MRD_BK BOTA0 SBL2 SBL EFS PARAM MODEM KERNEL RECOVERY MISC NVM MEP SYSTEM CACHE HIDDEN mmcblk0boot0 mmcblk0boot1)
(( $# )) && PARTS=("$@")

a(){ if [[ -n $SERIAL ]]; then "$ADB" -s "$SERIAL" "$@"; else "$ADB" "$@"; fi; }
# `adb shell` on old devices returns CRLF line endings; strip CR from text output
as(){ a shell "$@" | tr -d '\r'; }
wait_recovery(){
  for i in {1..60}; do [[ "$(a get-state 2>/dev/null)" == recovery ]] && return 0; sleep 3; done
  echo "Device did not return to recovery"; exit 1
}

wait_recovery
mkdir -p "$OUT"
SUMS="$OUT/SHA256SUMS"; touch "$SUMS"

for p in $PARTS; do
  if [[ $p == mmcblk0boot* ]]; then dev=/dev/block/$p
  else dev=$(as "readlink -f /dev/block/platform/*/by-name/$p"); fi
  blk=${dev:t}
  kib=$(as "grep -w $blk /proc/partitions" | awk '{print $3}')
  expect=$(( kib * 1024 ))
  file="$OUT/$p.img"
  if [[ -f $file && $(stat -f %z "$file") == $expect ]] && grep -qE "  $p\.img\$" "$SUMS"; then
    echo "$p: already done, skipping"; continue
  fi
  parts_dir="$OUT/.$p.chunks-${CHUNK_MB}M"; mkdir -p "$parts_dir"
  nchunks=$(( (kib + CHUNK_MB*1024 - 1) / (CHUNK_MB*1024) ))
  for (( c=0; c<nchunks; c++ )); do
    cf=$(printf '%s/%04d' "$parts_dir" $c)
    [[ -f $cf.ok ]] && continue
    for attempt in {1..10}; do
      wait_recovery
      dd_cmd="dd if=$dev bs=1048576 skip=$(( c*CHUNK_MB )) count=$CHUNK_MB 2>/dev/null"
      # The USB link drops when VBUS sags (kernel: "mv-udc: vbus is 0"), so chunks can arrive
      # truncated; the hash check + retry handles that. gzip in transit just moves fewer bytes.
      a exec-out "$dd_cmd | gzip -1" | gunzip > "$cf" 2>/dev/null || true
      msum=$(shasum -a 256 "$cf" | awk '{print $1}')
      dsum=$(as "$dd_cmd | sha256sum" 2>/dev/null | awk '{print $1}') || dsum=""
      if [[ -n $dsum && $msum == $dsum ]]; then touch "$cf.ok"; break; fi
      echo "  $p chunk $c attempt $attempt failed (mac=${msum:0:12} dev=${dsum:0:12}); retrying"
      sleep 5
    done
    [[ -f $cf.ok ]] || { echo "Stopping: $p chunk $c failed 10 times"; exit 2; }
    (( nchunks > 1 )) && printf '  %s chunk %d/%d ok\n' "$p" $((c+1)) $nchunks
  done
  cat "$parts_dir"/[0-9][0-9][0-9][0-9] > "$file"
  size=$(stat -f %z "$file")
  [[ $size == $expect ]] || { echo "Stopping: $p size $size != $expect"; exit 3; }
  sum=$(shasum -a 256 "$file" | awk '{print $1}')
  rm -rf "$parts_dir"
  printf '%-13s %-12s %12s bytes  %s  OK\n' "$p" "$blk" "$size" "$sum"
  print "$sum  $p.img" >> "$SUMS"
done
