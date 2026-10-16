# Runs on the device. Injects Xbox pad inputs through the Odin Controller (SDL sees it as a standard gamepad).
#   sh pad.sh btn <a|b|x|y|lb|rb|back|start|guide|up|down|left|right> [hold_ms]
#   sh pad.sh stick|rstick <x -32767..32767> <y, <0 = up> <hold_ms>
#   sh pad.sh trig <lt|rt> <hold_ms>
E=$(getevent -pl 2>/dev/null | grep -B1 '"Odin Controller"' | sed -n 's/^add device [0-9]*: //p')
E=${E:-/dev/input/event8}
syn() { sendevent $E 0 0 0; }
ms() { usleep $(( $1 * 1000 )); }
case $1 in
  btn)
    case $2 in
      a) k=304;; b) k=305;; y) k=307;; x) k=308;; lb) k=310;; rb) k=311;;
      back) k=314;; start) k=315;; guide) k=316;;
      up) h=17; v=-1;; down) h=17; v=1;; left) h=16; v=-1;; right) h=16; v=1;;
      *) echo "unknown button $2" >&2; exit 2;;
    esac
    if [ -n "$h" ]; then
      sendevent $E 3 $h $v; syn; ms ${3:-120}; sendevent $E 3 $h 0; syn
    else
      sendevent $E 1 $k 1; syn; ms ${3:-120}; sendevent $E 1 $k 0; syn
    fi;;
  stick|rstick)
    if [ $1 = stick ]; then ax=0; ay=1; else ax=2; ay=5; fi
    sendevent $E 3 $ax $2; sendevent $E 3 $ay $3; syn; ms $4
    sendevent $E 3 $ax 0; sendevent $E 3 $ay 0; syn;;
  trig)
    if [ $2 = lt ]; then ax=10; else ax=9; fi
    sendevent $E 3 $ax 32767; syn; ms $3; sendevent $E 3 $ax 0; syn;;
  *) echo "usage: pad.sh btn <name> [ms] | stick|rstick <x> <y> <ms> | trig <lt|rt> <ms>" >&2; exit 2;;
esac
