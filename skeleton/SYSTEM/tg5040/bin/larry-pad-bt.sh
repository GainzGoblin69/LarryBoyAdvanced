#!/bin/sh
# Larry Boy Advanced: Bluetooth for controller mode, and back.
#
#   larry-pad-bt.sh start   BlueZ as a gamepad: restarted without its input
#                           plugin (which takes the HID channels as a host),
#                           with a gamepad's device class and name
#   larry-pad-bt.sh stop    BlueZ as it was: on as usual if it was on, else off
#
# What it did and saw goes to the pad log beside the catalog.

SDCARD_PATH=${SDCARD_PATH:-/mnt/SDCARD}
SYSTEM_PATH=${SYSTEM_PATH:-$SDCARD_PATH/.system/tg5040}
BT_INIT="$SYSTEM_PATH/etc/bluetooth/bt_init.sh"
LOG="$SDCARD_PATH/.userdata/shared/larry/logs/pad.log"
WAS_ON=/tmp/larry-pad.bt-was-on
CONF=/tmp/larry-pad-main.conf
BLUETOOTHD=/usr/bin/bluetoothd

mkdir -p "$(dirname "$LOG")"
log() { echo "$(date '+%H:%M:%S') $*" >> "$LOG"; }

running() { ps | grep -v grep | grep -q "$1"; }

STOPPING=/tmp/larry-pad.stopping

case "$1" in
start)
	# a stop still putting Bluetooth back finishes first
	for i in $(seq 1 50); do [ -f "$STOPPING" ] || break; usleep 200000; done
	: > "$LOG"
	if running bluetoothd; then touch "$WAS_ON"; else rm -f "$WAS_ON"; fi
	log "starting; Bluetooth was $( [ -f "$WAS_ON" ] && echo on || echo off )"

	# the radio up, as Bluetooth on does
	if [ ! -d /sys/class/bluetooth/hci0 ]; then
		sh "$BT_INIT" start >> "$LOG" 2>&1
	fi
	killall bluealsa 2>/dev/null
	killall bluetoothd 2>/dev/null
	for i in 1 2 3 4 5 6 7 8 9 10; do running bluetoothd || break; usleep 200000; done

	cat > "$CONF" <<CONF
[General]
Name = Larry Boy Advanced
Class = 0x002508
DiscoverableTimeout = 0
PairableTimeout = 0
JustWorksRepairing = always
CONF
	hciconfig hci0 up >> "$LOG" 2>&1
	"$BLUETOOTHD" -n -P input -f "$CONF" >> "$LOG" 2>&1 &
	sleep 1
	if ! running bluetoothd; then
		log "bluetoothd would not take the settings file; starting it without"
		"$BLUETOOTHD" -n -P input >> "$LOG" 2>&1 &
		sleep 1
	fi
	# a gamepad, so the TV lists it as one; and findable
	hciconfig hci0 class 0x002508 >> "$LOG" 2>&1
	hciconfig hci0 piscan >> "$LOG" 2>&1
	# lowest latency: no sniff mode, in which the radio only wakes every so
	# often to save power. The TV asks for it; the Hammer now says no.
	hciconfig hci0 lp RSWITCH >> "$LOG" 2>&1
	log "link policy: $(hciconfig hci0 lp 2>&1 | tail -1)"
	running bluetoothd && log "bluetoothd running" || { log "bluetoothd did not start"; exit 1; }
	;;
stop)
	touch "$STOPPING"
	log "stopping"
	killall bluetoothd 2>/dev/null
	for i in 1 2 3 4 5 6 7 8 9 10; do running bluetoothd || break; usleep 200000; done
	if [ -f "$WAS_ON" ]; then
		sh "$BT_INIT" start >> "$LOG" 2>&1
	else
		sh "$BT_INIT" stop >> "$LOG" 2>&1
	fi
	# power saving back for headphones
	hciconfig hci0 lp RSWITCH,SNIFF >> "$LOG" 2>&1
	rm -f "$WAS_ON" "$CONF" "$STOPPING"
	log "Bluetooth back as it was"
	;;
*)
	echo "usage: $0 start|stop" >&2
	exit 2
	;;
esac
