#!/usr/bin/env bash
#
# Push a binary to the stick over netcat.
#
# The transfer method is the one documented in Anime4000/RTL960x
# (Firmware_Mod/README.md): the stick listens, this host connects and writes.
# So the stick has to be armed first — there is no service listening on it by
# default that we could push to unprompted.
#
# Everything lands in /tmp: the rootfs is read-only squashfs and /tmp is tmpfs,
# so a bad binary cannot brick the stick and does not survive a reboot.

set -euo pipefail

BIN="${1:?usage: deploy.sh <binary> [stick-ip] [port]}"
IP="${2:-}"
PORT="${3:-12345}"

NAME="$(basename "$BIN")"
DEST="/tmp/$NAME"
SIZE="$(wc -c < "$BIN" | tr -d ' ')"

if command -v md5sum >/dev/null 2>&1; then
	SUM="$(md5sum < "$BIN" | cut -d' ' -f1)"
else
	SUM="$(md5 -q "$BIN")"
fi

step_arm() {
	cat <<-MSG

	1. On the stick (telnet), arm the listener:

	     nc -l -p $PORT > $DEST

	MSG
}

step_run() {
	cat <<-MSG

	2. On the stick: Ctrl+C to end nc — it does not exit by itself when the
	   transfer completes. Then check what arrived and run it:

	     ls -l $DEST          # expect $SIZE bytes
	     md5sum $DEST         # expect $SUM
	     chmod +x $DEST
	     $DEST

	MSG
}

if [[ -z "$IP" ]]; then
	echo "# $NAME — $SIZE bytes, md5 $SUM"
	echo "# No IP given, so here is the manual recipe."
	step_arm
	cat <<-MSG
	   Then from this machine:

	     ./scripts/deploy.sh $BIN <stick-ip> $PORT
	MSG
	step_run
	exit 0
fi

echo "# $NAME — $SIZE bytes, md5 $SUM"
step_arm
read -r -p "   Press Enter once that is running (Ctrl-C to abort)... " _

# -w bounds the wait after EOF: the stick's nc holds the connection open, so
# without it this would hang here rather than at the stick end.
printf '   pushing %s bytes -> %s:%s ... ' "$SIZE" "$IP" "$PORT"
if nc -w 5 "$IP" "$PORT" < "$BIN"; then
	echo "done"
else
	echo "FAILED"
	echo "   Is the listener armed, and is $IP reachable?" >&2
	exit 1
fi

step_run
