#!/bin/zsh
# run.sh <run name>: boots a disc in an X1 BOX build on the Odin 3, plays the scenario and writes the measured
# segments to runs/<run name>/.
#   PKG         package (default: this branch's build, com.izzy2lost.x1box.odin3)
#   SCENARIO    scenarios/<SCENARIO>.txt (default conker); its `rom` line names the disc in $ROMS
#   ROM         disc in $ROMS to play instead of the scenario's `rom` line (for the generic survey scenario)
#   TARGET      frame rate the game aims for, for the slow-frame count (default 30)
#   KEEP=1      leave the game running at the end
#   PREFS       "key=true key2=false": boolean app preferences for this run only (hidden options, A/B runs)
#   HOLD=1      keep the lock for the next run of a series (the series releases it)
#   LOCK_OWNER  owner in the shared Odin lock (default xbox)
# Scenario lines: rom <file> | wait <s> | btn <name> [ms] | stick|rstick <x> <y> <ms> | trig <lt|rt> <ms> |
#   repeat <n> <line> | shot <name> | measure <label> ... end   (inputs between measure and end run while measuring)
A=~/Library/Android/sdk/platform-tools/adb
D=${0:A:h}
LOCK=~/PhpstormProjects/vita3k/tools/odin3/lock.sh
NAME=$1; shift
OUT=$D/runs/$NAME
PKG=${PKG:-com.izzy2lost.x1box.odin3}
SC=$D/scenarios/${SCENARIO:-conker}.txt
ROMS=/storage/FBDD-1F17/Emulations/ROMs/xbox
T=/data/local/tmp/xbox
[ -f $SC ] || { echo "no scenario $SC"; exit 2; }
ROM=${ROM:-$(sed -n 's/^rom \(.*\)$/\1/p' $SC)}
rm -rf $OUT; mkdir -p $OUT || exit 1

OWNER=${LOCK_OWNER:-xbox}
$LOCK take $OWNER || exit 1
P=shared_prefs/x1box_prefs.xml
trap '$A shell pkill -f $T/poll.sh; [ "$KEEP" = 1 ] || $A shell am force-stop $PKG
  [ -f $OUT/prefs.xml ] && $A shell "run-as $PKG sh -c '\''cat > $P'\''" < $OUT/prefs.xml
  [ "$HOLD" = 1 ] || $LOCK release $OWNER' EXIT

focused() { $A shell dumpsys window | grep mCurrentFocus | grep -q "$PKG/com.izzy2lost.x1box.MainActivity"; }
pad() { focused || { echo "ABORT: X1 BOX lost the focus" | tee -a $OUT/summary.txt; exit 1; }; $A shell sh $T/pad.sh "$@"; }
shot() {
  print "$1 $($A shell date +%T.%N | cut -c1-12)" >> $OUT/shots.txt
  $A exec-out screencap -p > $OUT/$1.png && sips -Z 960 $OUT/$1.png --out $OUT/$1.png >/dev/null
}

$A shell mkdir -p $T
$A push $D/pad.sh $D/poll.sh $T/ >/dev/null
$A shell am force-stop $PKG
if [ -n "$PREFS" ]; then
  $A shell "run-as $PKG cat $P" > $OUT/prefs.xml.0 || { echo "PREFS needs a debuggable build"; exit 2; }
  python3 -I - $OUT/prefs.xml.0 ${=PREFS} <<'PY' | $A shell "run-as $PKG sh -c 'cat > $P'"
import re, sys
xml = open(sys.argv[1]).read()
for kv in sys.argv[2:]:
    k, v = kv.split("=")
    xml = re.sub(r'\s*<boolean name="%s" value="\w+" />' % re.escape(k), "", xml)
    xml = xml.replace("</map>", '    <boolean name="%s" value="%s" />\n</map>' % (k, v))
print(xml, end="")
PY
  mv $OUT/prefs.xml.0 $OUT/prefs.xml
fi
# Start every run from a cool GPU and board so throttling does not depend on the previous run
for i in $(seq 90); do
  [ $($A shell cat /sys/class/kgsl/kgsl-3d0/temp | tr -d '\r') -lt 40000 ] &&
    [ $($A shell cat /sys/class/power_supply/battery/temp | tr -d '\r') -lt 340 ] && break
  sleep 2
done
# The fork reads the disc by path (MANAGE_EXTERNAL_STORAGE); the stock app resolves it in its games folder
$A shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow 2>/dev/null
URI="file://$(python3 -I -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$ROMS/$ROM")"
$A shell input keyevent KEYCODE_WAKEUP; $A shell wm dismiss-keyguard
$A logcat -c
$A shell am start -n $PKG/com.izzy2lost.x1box.LauncherActivity -a android.intent.action.VIEW -d "'$URI'" >/dev/null
for i in $(seq 60); do focused && break; sleep 1; done
focused || { echo "ABORT: MainActivity never got the focus" | tee $OUT/summary.txt; exit 1; }
echo "pkg=$PKG version=$($A shell dumpsys package $PKG | sed -n 's/.*versionName=//p' | head -1 | tr -d '\r') rom=$ROM start=$(date +%T)" > $OUT/context.txt

# The SDL activity may have several SurfaceViews: the game's is the one that presents
game_layer() {
  local n
  for n in ${(u)${(f)"$($A shell dumpsys SurfaceFlinger --list | tr -d '\r' |
      grep -oE "SurfaceView\[$PKG/com\.izzy2lost\.x1box\.MainActivity\]\(BLAST\)#[0-9]+")"}}; do
    print -r -- "$($A shell "dumpsys SurfaceFlinger --latency '$n'" | awk 'NF==3 && $2>0 {c++} END {print c+0}') $n"
  done | sort -rn | head -1 | cut -d' ' -f2-
}
LAYER=""
SEG=""
seg_begin() {
  SEG=$OUT/$1; mkdir -p $SEG
  [ -n "$LAYER" ] || LAYER=$(game_layer)
  PID=$($A shell pidof $PKG:xemu 2>/dev/null | tr -d '\r'); [ -n "$PID" ] || PID=$($A shell pidof $PKG | tr -d '\r')
  $A shell "cat /proc/$PID/task/*/stat" > $SEG/threads_start.txt
  # The display mode the game set before the segment (VBLANK rate)
  $A logcat -d -s xemu-mode | tail -1 > $SEG/mode.log
  $A logcat -c
  $A shell sh $T/poll.sh 3600 > $SEG/poll.csv &
  # SurfaceFlinger keeps the last 128 presents: sample every second
  { while [ ! -f $SEG/.stop ]; do $A shell "dumpsys SurfaceFlinger --latency '$LAYER'"; sleep 1; done } > $SEG/sf_latency.txt &
  T0=$SECONDS
}
seg_end() {
  $A shell "cat /proc/$PID/task/*/stat" > $SEG/threads_end.txt
  echo $((SECONDS - T0)) > $SEG/duration
  $A logcat -d -s xemu-fps > $SEG/fps.log
  # The nv2a phase, FIFO, surface and workload lines the core logs every 60 flips
  $A logcat -d -s hakuX-phase hakuX-cpu xemu-surf xemu-pace xemu-work xemu-gpu > $SEG/nv2a.log
  $A logcat -d -s xemu-mode >> $SEG/mode.log
  touch $SEG/.stop; $A shell pkill -f $T/poll.sh; wait
  python3 -I $D/summarize.py $SEG ${TARGET:-30} | tee -a $OUT/summary.txt
  SEG=""
}
step() {
  case $1 in
    rom|""|\#*) ;;
    wait) sleep $2;;
    btn|stick|rstick|trig) pad "$@";;
    shot) [ -z "$SEG" ] || { echo "a screenshot during a measure stalls SurfaceFlinger" >&2; exit 2; }; shot $2;;
    repeat) local n=$2; shift 2; for i in $(seq $n); do step "$@"; done;;
    measure) seg_begin $2;;
    end) seg_end;;
    *) echo "unknown scenario line: $*" >&2; exit 2;;
  esac
}
# Read the whole file first: adb shell would swallow the rest of the scenario from stdin
for line in "${(@f)$(<$SC)}"; do step ${=line%%\#*}; done
[ -z "$SEG" ] || seg_end
focused || echo "WARNING: X1 BOX no longer has the focus (crash?)" | tee -a $OUT/summary.txt
