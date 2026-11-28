#!/bin/sh
# Larry Boy Advanced: runs larry-sync, the RomM catalog, download and save
# service, from its home folder, one at a time.
#
# Its settings, catalog database and cover-art cache live in
# .userdata/shared/larry, which survives firmware updates. The launcher reads
# the catalog files it writes to the catalog folder there.

PLATFORM="${PLATFORM:-tg5040}"
SDCARD_PATH="${SDCARD_PATH:-/mnt/SDCARD}"
SYSTEM_PATH="${SYSTEM_PATH:-$SDCARD_PATH/.system/$PLATFORM}"
SHARED_USERDATA_PATH="${SHARED_USERDATA_PATH:-$SDCARD_PATH/.userdata/shared}"
LARRY_HOME="$SHARED_USERDATA_PATH/larry"

export CFW=NEXTUI
export SDCARD_PATH
export NEXTUI_DEVICE="$PLATFORM"
export LARRY_DIR="$LARRY_HOME/catalog"
export LD_LIBRARY_PATH="$SYSTEM_PATH/lib/larry:$SYSTEM_PATH/lib:/usr/trimui/lib:$LD_LIBRARY_PATH"

mkdir -p "$LARRY_HOME" "$LARRY_DIR" || exit 1
cd "$LARRY_HOME" || exit 1

# Menu music and File Access touch neither the catalog nor the saves, and
# must not wait behind a library sync (nor hold one up while File Access is
# open), so they run alongside whatever else is running.
case "$1" in
music|files|pad)
	exec "$SYSTEM_PATH/bin/larry-sync" "$@"
	;;
esac

# One at a time. The catalog database is shared, and a full catalog refresh
# turns its journal off while it runs, so a save upload left running in the
# background after a game must finish before the next command starts.
#
# The lock holds the PID of its owner. larry-sync is started with exec, which
# keeps that PID, so a lock left behind by a process that died is spotted and
# taken over instead of waited on.
LOCK=/tmp/larry-sync.lock
waited=0
while ! mkdir "$LOCK" 2>/dev/null; do
	owner=$(cat "$LOCK/pid" 2>/dev/null)
	if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
		rm -rf "$LOCK"
		continue
	fi
	# A lock with no PID after a few seconds belongs to a process that died
	# between taking it and writing its PID.
	if [ -z "$owner" ] && [ "$waited" -ge 5 ]; then
		rm -rf "$LOCK"
		continue
	fi
	if [ "$waited" -ge 300 ]; then
		printf 'ERROR\tAnother RomM task is still running. Try again in a moment.\n'
		exit 1
	fi
	sleep 1
	waited=$((waited + 1))
done
echo $$ > "$LOCK/pid"

exec "$SYSTEM_PATH/bin/larry-sync" "$@"
