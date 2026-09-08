#!/bin/sh
#
# Start the Prometheus exporter on the stick.
#
# It carries its own HTTP listener, so nothing here depends on boa. That is not
# a preference: Realtek's boa 0.93.15 returns 404 for anything typed
# application/x-httpd-cgi while serving the same file happily as a static
# download, so it has no working external-CGI path.
#
# /var is ramfs, so nothing survives a reboot — the point while iterating. For a
# permanent install put metricsd in the firmware image and start it from
# /etc/inittab; see Anime4000/RTL960x Firmware_Mod.
#
# Written for this stick's busybox, which has no head, tail, pkill, seq, nohup
# or setsid.
#
#     sh /tmp/exporter-up.sh [port]
#     sh /tmp/exporter-up.sh stop

ROOT=/var/exp
SRC=/tmp/metricsd
BIN="$ROOT/metricsd"
PIDFILE="$ROOT/metricsd.pid"

if [ "$1" = "stop" ]; then
	if [ -f "$PIDFILE" ]; then
		kill "$(cat "$PIDFILE")" 2>/dev/null && echo "stopped $(cat "$PIDFILE")"
		rm -f "$PIDFILE"
	else
		echo "no $PIDFILE; nothing to stop"
	fi
	exit 0
fi

PORT="${1:-9100}"

if [ ! -f "$SRC" ]; then
	echo "missing $SRC -- push it first" >&2
	exit 1
fi

# Only ever kill a pid we recorded ourselves.
if [ -f "$PIDFILE" ]; then
	kill "$(cat "$PIDFILE")" 2>/dev/null
	rm -f "$PIDFILE"
	sleep 1
fi

mkdir -p "$ROOT"
cp "$SRC" "$BIN"
chmod +x "$BIN"

"$BIN" "$PORT" >/dev/null 2>&1 &
echo $! > "$PIDFILE"
sleep 1

# The exporter exits immediately if it cannot bind, so a dead pid here means the
# port was taken rather than that the binary is broken.
if kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
	IP=$(ifconfig 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p' | grep -v '^127' | sed -n '1p')
	[ -z "$IP" ] && IP="<stick-ip>"
	echo "metricsd running, pid $(cat "$PIDFILE")"
	echo
	echo "scrape URL:   http://$IP:$PORT/metrics"
	echo "any path answers; /metrics is convention"
	echo
	echo "stop with:    sh /tmp/exporter-up.sh stop"
else
	echo "metricsd exited -- port $PORT already in use? try another:" >&2
	echo "  sh /tmp/exporter-up.sh 9101" >&2
	rm -f "$PIDFILE"
	exit 1
fi
