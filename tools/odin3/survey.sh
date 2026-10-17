#!/bin/zsh
# survey.sh <prefix> [disc...]: plays scenarios/survey.txt on each disc (default: every disc in $ROMS) and writes
# runs/<prefix>-<disc slug>/; prints each disc's measured segments. Same environment as run.sh (PKG, TARGET...).
A=~/Library/Android/sdk/platform-tools/adb
D=${0:A:h}
LOCK=~/PhpstormProjects/vita3k/tools/odin3/lock.sh
ROMS=/storage/FBDD-1F17/Emulations/ROMs/xbox
PREFIX=$1; shift
OWNER=${LOCK_OWNER:-xbox}
discs=("$@")
[ $#discs -gt 0 ] || discs=(${(f)"$($A shell ls $ROMS | tr -d '\r' | grep -i '\.iso$')"})
$LOCK take $OWNER || exit 1
trap '$LOCK release $OWNER' EXIT
for disc in $discs; do
  slug=${${${disc%%.*}%% \(*}//[^A-Za-z0-9]/-}
  print "== $disc"
  ROM=$disc SCENARIO=survey TARGET=${TARGET:-60} HOLD=1 $D/run.sh $PREFIX-$slug | grep -v '^  power'
done
