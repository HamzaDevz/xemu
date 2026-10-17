#!/bin/zsh
# install.sh [apk]: installs the Odin 3 build next to the stock X1 BOX and gives it the stock app's BIOS, flash,
# EEPROM and HDD image, so both builds boot the same system. Under the shared Odin lock.
#   PKG         package the apk installs as (default com.izzy2lost.x1box.odin3; an upstream build made with
#               applicationIdSuffix ".master" is com.izzy2lost.x1box.master)
#   SRC         folder to copy the BIOS, flash, EEPROM and HDD image from (default: the stock app's)
#   LOCK_OWNER  owner in the shared Odin lock (default xbox); HOLD=1 keeps the lock for the runs that follow
A=~/Library/Android/sdk/platform-tools/adb
D=${0:A:h}
LOCK=~/PhpstormProjects/vita3k/tools/odin3/lock.sh
APK=${1:-${D:h:h}/android/app/build/outputs/apk/odin3/app-odin3.apk}
PKG=${PKG:-com.izzy2lost.x1box.odin3}
SRC=${SRC:-/storage/emulated/0/Android/data/com.izzy2lost.x1box/files/x1box}
DST=/storage/emulated/0/Android/data/$PKG/files/x1box
OWNER=${LOCK_OWNER:-xbox}
[ -f $APK ] || { echo "no apk $APK"; exit 2; }
$LOCK take $OWNER || exit 1
trap '[ "$HOLD" = 1 ] || $LOCK release $OWNER' EXIT

$A install -r $APK || exit 1
$A shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow
$A shell mkdir -p $DST
for f in mcpx.bin flash.bin eeprom.bin hdd.img; do
  $A shell "[ -f $SRC/$f ] && { [ -f $DST/$f ] || cp $SRC/$f $DST/$f; }" || echo "no $f in $SRC"
done
# PAL discs keep their 50 Hz modes unless the dashboard's 60 Hz option is on: turn it on when no refresh rate was
# chosen, as Settings > Refresh rate > 60 Hz does (video flags at 0x94, user section checksum at 0x60)
E=$(mktemp)
$A exec-out cat $DST/eeprom.bin > $E && python3 -I - $E <<'PY' && $A push $E $DST/eeprom.bin >/dev/null && echo "eeprom: 60 Hz on"
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
def checksum():
    hi = lo = 0
    for i in range(0x64, 0xC0, 4):
        v = struct.unpack_from("<I", d, i)[0]
        hi = ((hi << 32 | lo) + v) >> 32 & 0xFFFFFFFF
        lo = (lo + v) & 0xFFFFFFFF
    return ~(hi + lo) & 0xFFFFFFFF
flags = struct.unpack_from("<I", d, 0x94)[0]
if len(d) != 256 or checksum() != struct.unpack_from("<I", d, 0x60)[0] or flags & 0x00C00000:
    sys.exit(1)
struct.pack_into("<I", d, 0x94, flags | 0x00400000)
struct.pack_into("<I", d, 0x60, checksum())
open(sys.argv[1], "wb").write(d)
PY
rm -f $E
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
