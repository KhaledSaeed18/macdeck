#!/bin/sh
# Simulate touches on the tablet (for testing without a finger), via sendevent on the
# touchscreen (the shell user is in the input group). Screen coordinates are the deck's
# landscape 1024x600; the SM-T210 panel reports portrait, so touch x = 598 - y, touch y = x.
#   scripts/deck-touch.sh tap <x> <y>
#   scripts/deck-touch.sh swipe left|right       (one page)
set -eu
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
D=/dev/input/event0
case "${1:-}" in
tap)
  tx=$((598 - $3)); ty=$2
  $ADB shell "sendevent $D 3 57 30; sendevent $D 3 53 $tx; sendevent $D 3 54 $ty; sendevent $D 1 330 1; sendevent $D 0 0 0;
              sleep 0.12; sendevent $D 3 57 4294967295; sendevent $D 1 330 0; sendevent $D 0 0 0" ;;
swipe)
  if [ "$2" = left ]; then xs="900 800 700 600 500 400 300 200 100"; else xs="100 200 300 400 500 600 700 800 900"; fi
  first=${xs%% *}; rest=${xs#* }
  moves=""; for x in $rest; do moves="$moves sendevent $D 3 54 $x; sendevent $D 0 0 0;"; done
  $ADB shell "sendevent $D 3 57 31; sendevent $D 3 53 298; sendevent $D 3 54 $first; sendevent $D 1 330 1; sendevent $D 0 0 0;
              $moves sendevent $D 3 57 4294967295; sendevent $D 1 330 0; sendevent $D 0 0 0" ;;
*) echo "usage: deck-touch.sh tap <x> <y> | swipe left|right" >&2; exit 2 ;;
esac
