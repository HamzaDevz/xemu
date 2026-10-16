#!/bin/zsh
# install.sh [apk]: installs the Odin 3 build next to the stock X1 BOX and gives it the stock app's BIOS, flash,
# EEPROM and HDD image, so both builds boot the same system. Under the shared Odin lock.
#   LOCK_OWNER  owner in the shared Odin lock (default xbox); HOLD=1 keeps the lock for the runs that follow
A=~/Library/Android/sdk/platform-tools/adb
D=${0:A:h}
LOCK=~/PhpstormProjects/vita3k/tools/odin3/lock.sh
APK=${1:-${D:h:h}/android/app/build/outputs/apk/odin3/app-odin3.apk}
PKG=com.izzy2lost.x1box.odin3
SRC=/storage/emulated/0/Android/data/com.izzy2lost.x1box/files/x1box
DST=/storage/emulated/0/Android/data/$PKG/files/x1box
OWNER=${LOCK_OWNER:-xbox}
[ -f $APK ] || { echo "no apk $APK"; exit 2; }
$LOCK take $OWNER || exit 1
trap '[ "$HOLD" = 1 ] || $LOCK release $OWNER' EXIT

$A install -r $APK || exit 1
$A shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow
$A shell mkdir -p $DST
for f in mcpx.bin flash.bin eeprom.bin hdd.img; do
  $A shell "[ -f $SRC/$f ] && { [ -f $DST/$f ] || cp $SRC/$f $DST/$f; }" || echo "no $f in the stock app"
done
# The setup wizard stores the copied files' paths: write them directly and skip it
P=shared_prefs/x1box_prefs.xml
XML="<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <boolean name=\"setup_complete\" value=\"true\" />
    <string name=\"mcpxPath\">$DST/mcpx.bin</string>
    <string name=\"flashPath\">$DST/flash.bin</string>
    <string name=\"hddPath\">$DST/hdd.img</string>
</map>"
$A shell "run-as $PKG sh -c 'mkdir -p shared_prefs; [ -f $P ] || cat > $P'" <<< $XML
$A shell "run-as $PKG cat $P"
