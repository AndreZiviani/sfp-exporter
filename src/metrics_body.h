/*
 * The metrics themselves, shared by both transports: metricsd.c (standalone
 * HTTP server — what this stick needs) and metrics.c (CGI, for devices whose
 * web server can exec one).
 *
 * Everything writes to an explicit fd so the same code serves stdout and a
 * socket.
 *
 * The optical values are not in /proc — `ls /proc` turns up only rtk_smux — so
 * they come from /bin/diag, run once per scrape and scraped back out of its
 * output. See README.md for the command list and how it was established.
 */

#ifndef ODI_METRICS_BODY_H
#define ODI_METRICS_BODY_H

#include "syscall.h"

#define DIAG_PATH "/bin/diag"

/*
 * Build identity, reported as gpon_exporter_build_info.
 *
 * This matters more here than it looks. The exporter can be replaced WITHOUT
 * reflashing: rc35 prefers /etc/config/metricsd — on the jffs2 config
 * partition, which fwu.sh does not touch — over the /bin/metricsd baked into
 * the image. So an override survives both reboots and reflashes, and can end up
 * older than the image it is running on with nothing to say so. Exporting the
 * version AND the path it was started from makes that visible from a query
 * instead of an inspection.
 *
 * BUILD_ID comes from `git describe` on the host at build time; see the
 * Makefile.
 */
#ifndef BUILD_ID
#define BUILD_ID "unknown"
#endif

/* argv[0] as invoked, set by main(). Not a copy: argv lives for the life of the
 * process. */
static const char *exporter_path = "unknown";

/*
 * Values are emitted as the literal text diag printed. No parsing to a number
 * and back: Prometheus wants a bare decimal and diag already produces one, so
 * this avoids float formatting, and with it soft-float and a libc. It also
 * cannot introduce a rounding difference between what the device reports and
 * what is scraped.
 */
static int scan_decimal(const char *s, unsigned long *start, unsigned long *len)
{
	unsigned long i;

	for (i = 0; s[i]; i++) {
		unsigned long j = i;
		unsigned long int_start, frac_start;

		if (s[j] == '-')
			j++;

		int_start = j;
		while (s[j] >= '0' && s[j] <= '9')
			j++;
		if (j == int_start || s[j] != '.')
			continue;

		j++;
		frac_start = j;
		while (s[j] >= '0' && s[j] <= '9')
			j++;
		if (j == frac_start)
			continue;

		*start = i;
		*len = j - i;
		return 1;
	}
	return 0;
}

/* `diag gpon get onu-state` prints e.g. "ONU state: operation state(O5)". */
static int scan_onu_state(const char *s, unsigned long *start, unsigned long *len)
{
	unsigned long i;

	for (i = 0; s[i]; i++) {
		unsigned long j;

		if (s[i] != '(' || s[i + 1] != 'O')
			continue;

		j = i + 2;
		*start = j;
		while (s[j] >= '0' && s[j] <= '9')
			j++;
		if (j == *start)
			continue;

		*len = j - *start;
		return 1;
	}
	return 0;
}

static void emit_header(int fd, const char *name, const char *help, const char *type)
{
	put_fd(fd, "# HELP ");
	put_fd(fd, name);
	put_fd(fd, " ");
	put_fd(fd, help);
	put_fd(fd, "\n# TYPE ");
	put_fd(fd, name);
	put_fd(fd, " ");
	put_fd(fd, type);
	put_fd(fd, "\n");
}

/* Normalise a label in place: lowercase, spaces to underscores. Written into a
 * caller buffer so the whole label goes out in one write rather than one
 * syscall per character. */
static unsigned long norm_label(char *dst, unsigned long cap,
				const char *src, unsigned long len)
{
	unsigned long n = 0, k;

	for (k = 0; k < len && n + 1 < cap; k++) {
		char c = src[k];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		else if (c == ' ' || c == '-')
			c = '_';
		dst[n++] = c;
	}
	dst[n] = 0;
	return n;
}

/*
 * A single-valued metric: one extractor applied to one command's section of the
 * diag output. On a parse failure nothing is emitted at all — an absent series
 * is honest, whereas a zero would look like a real reading of zero dBm.
 */
static void emit_scalar(int fd, const char *name, const char *help,
			const char *sec,
			int (*scan)(const char *, unsigned long *, unsigned long *))
{
	unsigned long start = 0, len = 0;

	if (!scan(sec, &start, &len))
		return;

	emit_header(fd, name, help, "gauge");
	put_fd(fd, name);
	put_fd(fd, " ");
	write_all(fd, sec + start, len);
	put_fd(fd, "\n");
}

static void put_first_token(int fd, const char *s)
{
	unsigned long i = 0;

	while (s[i] && s[i] != ' ' && s[i] != '\n' && s[i] != '\t')
		i++;

	if (i == 0) {
		put_fd(fd, "0");
		return;
	}
	write_all(fd, s, i);
}

/* Offset just past `key` if the line starting at i begins with it, else 0. */
static unsigned long line_key(const char *buf, unsigned long i, const char *key)
{
	unsigned long k = 0;

	while (key[k] && buf[i + k] == key[k])
		k++;
	return key[k] ? 0 : i + k;
}

/*
 * /proc/meminfo on a device with ~64 MB is the metric most likely to explain a
 * reboot, and it costs no fork. Values are in kB; multiplying to bytes keeps
 * Prometheus base units and stays in integer arithmetic.
 */
static void metric_meminfo(int fd)
{
	static const char *const keys[] = {
		"MemTotal:", "MemFree:", "Buffers:", "Cached:", 0
	};
	static const char *const names[] = {
		"total", "free", "buffers", "cached", 0
	};
	char buf[2048];
	unsigned long i;
	int k, have_header = 0;

	if (read_file("/proc/meminfo", buf, sizeof(buf)) <= 0)
		return;

	for (k = 0; keys[k]; k++) {
		for (i = 0; buf[i]; i++) {
			unsigned long v, start, val = 0;

			if (i && buf[i - 1] != '\n')
				continue;
			v = line_key(buf, i, keys[k]);
			if (!v)
				continue;

			while (buf[v] == ' ')
				v++;
			start = v;
			while (buf[v] >= '0' && buf[v] <= '9') {
				val = val * 10 + (unsigned long)(buf[v] - '0');
				v++;
			}
			if (v == start)
				break;

			if (!have_header) {
				emit_header(fd, "gpon_memory_bytes",
					    "Kernel memory, from /proc/meminfo.", "gauge");
				have_header = 1;
			}
			put_fd(fd, "gpon_memory_bytes{kind=\"");
			put_fd(fd, names[k]);
			put_fd(fd, "\"} ");
			put_u32_fd(fd, val * 1024);
			put_fd(fd, "\n");
			break;
		}
	}
}

/*
 * /proc/net/dev. Unlike the ONU MAC registers these are ordinary Linux
 * counters: monotonic, and reading does not clear them. Field order is fixed —
 * rx: bytes packets errs drop ...  tx: bytes packets errs drop ...
 */
static void metric_netdev(int fd)
{
	static const unsigned long FIELD[] = { 0, 1, 2, 3, 8, 9, 10, 11 };
	static const char *const METRIC[] = {
		"gpon_network_receive_bytes_total",
		"gpon_network_receive_packets_total",
		"gpon_network_receive_errs_total",
		"gpon_network_receive_drop_total",
		"gpon_network_transmit_bytes_total",
		"gpon_network_transmit_packets_total",
		"gpon_network_transmit_errs_total",
		"gpon_network_transmit_drop_total",
	};
	char buf[4096];
	char dev[32];
	int m;

	if (read_file("/proc/net/dev", buf, sizeof(buf)) <= 0)
		return;

	/*
	 * Metric-major: one pass per metric over the whole buffer. The exposition
	 * format requires every sample of a metric family to be contiguous, and
	 * the natural interface-major loop scatters them. Re-scanning an in-memory
	 * buffer eight times costs nothing.
	 */
	for (m = 0; m < 8; m++) {
		unsigned long i = 0;
		int line = 0, header_done = 0;

		while (buf[i]) {
			unsigned long ls = i, le = i, c, a, b, n, f, pos;

			while (buf[le] && buf[le] != '\n')
				le++;

			if (line++ < 2)		/* two header lines */
				goto next;

			c = ls;
			while (c < le && buf[c] != ':')
				c++;
			if (c == le)
				goto next;

			a = ls;
			while (a < c && buf[a] == ' ')
				a++;
			b = c;
			if (b == a)
				goto next;
			n = norm_label(dev, sizeof(dev), buf + a, b - a);

			pos = c + 1;
			for (f = 0; f < 16 && pos < le; f++) {
				unsigned long start;

				while (pos < le && buf[pos] == ' ')
					pos++;
				start = pos;
				while (pos < le && buf[pos] >= '0' && buf[pos] <= '9')
					pos++;
				if (pos == start)
					break;
				if (f != FIELD[m])
					continue;

				if (!header_done) {
					emit_header(fd, METRIC[m],
						    "Interface counter from /proc/net/dev.",
						    "counter");
					header_done = 1;
				}
				put_fd(fd, METRIC[m]);
				put_fd(fd, "{device=\"");
				write_all(fd, dev, n);
				put_fd(fd, "\"} ");
				write_all(fd, buf + start, pos - start);
				put_fd(fd, "\n");
			}

next:
			i = (buf[le] == '\n') ? le + 1 : le;
		}
	}
}

/* /proc/loadavg: "0.00 0.01 0.05 1/45 1234" — three decimals, emitted verbatim. */
static void metric_loadavg(int fd)
{
	static const char *const NAMES[] = { "gpon_load1", "gpon_load5", "gpon_load15" };
	char buf[128];
	unsigned long i = 0;
	int k;

	if (read_file("/proc/loadavg", buf, sizeof(buf)) <= 0)
		return;

	for (k = 0; k < 3; k++) {
		unsigned long start;

		while (buf[i] == ' ')
			i++;
		start = i;
		while ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == '.')
			i++;
		if (i == start)
			return;

		emit_header(fd, NAMES[k], "Load average, from /proc/loadavg.", "gauge");
		put_fd(fd, NAMES[k]);
		put_fd(fd, " ");
		write_all(fd, buf + start, i - start);
		put_fd(fd, "\n");
	}
}

static void metric_uptime(int fd)
{
	char buf[64];

	if (read_file("/proc/uptime", buf, sizeof(buf)) <= 0)
		return;

	emit_header(fd, "gpon_uptime_seconds", "Time since the stick booted.", "gauge");
	put_fd(fd, "gpon_uptime_seconds ");
	put_first_token(fd, buf);
	put_fd(fd, "\n");
}

/*
 * `diag gpon get alarm-status` prints one line per alarm:
 *
 *     Alarm LOS, status: clear
 *     Alarm TX Too Long, status: clear
 *
 * Seven of them, so this is one labelled metric rather than seven names. Alarm
 * names are normalised to lowercase with underscores: "TX Too Long" ->
 * tx_too_long.
 */
static void metric_alarms(int fd, const char *buf)
{
	char label[64];
	unsigned long i = 0;
	int have_header = 0;

	while (buf[i]) {
		unsigned long name, end, st, n;
		int clear;

		if (buf[i] != 'A' || buf[i+1] != 'l' || buf[i+2] != 'a' ||
		    buf[i+3] != 'r' || buf[i+4] != 'm' || buf[i+5] != ' ') {
			i++;
			continue;
		}

		name = i + 6;
		end = name;
		while (buf[end] && buf[end] != ',' && buf[end] != '\n')
			end++;
		if (buf[end] != ',')
			break;

		st = end;
		while (buf[st] && buf[st] != ':' && buf[st] != '\n')
			st++;
		if (buf[st] != ':')
			break;
		st++;
		while (buf[st] == ' ')
			st++;

		clear = (buf[st] == 'c' && buf[st+1] == 'l' && buf[st+2] == 'e' &&
			 buf[st+3] == 'a' && buf[st+4] == 'r');

		n = norm_label(label, sizeof(label), buf + name, end - name);

		if (!have_header) {
			emit_header(fd, "gpon_alarm",
				    "GPON alarm state; 1 means the alarm is asserted.",
				    "gauge");
			have_header = 1;
		}

		put_fd(fd, "gpon_alarm{alarm=\"");
		write_all(fd, label, n);
		put_fd(fd, "\"} ");
		put_fd(fd, clear ? "0\n" : "1\n");

		i = end;
	}
}

/*
 * `diag gpon show counter global ds-eth` prints a banner and then one
 * "Label : value" line per counter:
 *
 *     GPON ONU MAC Device Counter: DS ETH
 *     Total Unicast   : 989
 *     FCS Error       : 0
 *
 * Parsed generically — any line whose text after the colon is nothing but
 * digits becomes a series. That rejects the banner (value "DS ETH") and the
 * "RTK.0> command:" prompt (no value) without special-casing either, and picks
 * up counters that are not in this list if the firmware grows them.
 *
 * us-eth currently prints the banner and no rows, so it yields no series at
 * all — which is the correct representation of "the device reports nothing".
 */
__attribute__((unused))
static void metric_counters(int fd, const char *name, const char *help,
			    char *const argv[])
{
	char buf[2048];
	char label[64];
	unsigned long i = 0;
	int have_header = 0;

	if (run_to_buf(DIAG_PATH, argv, buf, sizeof(buf)) <= 0)
		return;
	while (buf[i]) {
		unsigned long ls = i, le = i, col, v, vstart, tail, a, b, n;

		while (buf[le] && buf[le] != '\n')
			le++;

		col = ls;
		while (col < le && buf[col] != ':')
			col++;
		if (col == le)
			goto next;

		v = col + 1;
		while (v < le && buf[v] == ' ')
			v++;
		vstart = v;
		while (v < le && buf[v] >= '0' && buf[v] <= '9')
			v++;
		if (v == vstart)
			goto next;		/* no number: banner or prompt */

		tail = v;
		while (tail < le && (buf[tail] == ' ' || buf[tail] == '\r'))
			tail++;
		if (tail != le)
			goto next;		/* trailing junk: not a counter */

		a = ls;
		b = col;
		while (a < b && buf[a] == ' ')
			a++;
		while (b > a && buf[b - 1] == ' ')
			b--;
		if (b == a)
			goto next;

		if (!have_header) {
			emit_header(fd, name, help, "counter");
			have_header = 1;
		}

		n = norm_label(label, sizeof(label), buf + a, b - a);
		put_fd(fd, name);
		put_fd(fd, "{counter=\"");
		write_all(fd, label, n);
		put_fd(fd, "\"} ");
		write_all(fd, buf + vstart, v - vstart);
		put_fd(fd, "\n");

next:
		i = (buf[le] == '\n') ? le + 1 : le;
	}
}

/*
 * `diag mib dump counter port all` — the switch/PON MAC MIB counters.
 *
 * This is the block the vendor web UI reads (boa's ponGetStatus, which prints
 * them with %llu), and it is a DIFFERENT counter block from
 * `gpon show counter global ...` further down this file. The difference is the
 * whole reason these can be exported and those cannot:
 *
 *   - Free-running, not read-and-clear. `diag mib get count-mode` reports
 *     "normal free run", and two reads six seconds apart gave 165417214 then
 *     172149239 on port 0 — it grew, it did not reset. Reading is therefore
 *     non-destructive, so a scrape does not steal counts from the web UI or
 *     from a manual diag. Resetting is a separate explicit command
 *     (`diag mib reset counter port ...`), which nothing here ever runs.
 *   - Wider than 32 bits: port 2 read ifInOctets 4881693552, past 2^32. So
 *     there is no wrap to work around and no constraint on scrape interval.
 *
 * Both properties together mean these are real Prometheus counters and go out
 * as-is, with no in-process accumulation.
 *
 * Ports, established by correlating deltas over one 25 s window: port 2 is the
 * PON side and port 0 the host SerDes side. Their deltas mirror each other —
 * p2-in 13220591 against p0-out 13203165, p0-in 12626164 against p2-out
 * 12653844 — which is the switch forwarding between the two.
 *
 * This is also why /proc/net/dev is no substitute and pon0 there reads zero:
 * forwarding happens in switch hardware and never reaches the CPU, so eth0's
 * counters only ever show the stick's own management traffic.
 *
 * Only the counters with an unambiguous unit are exported. The device prints 46
 * per port, the rest being packet-size histograms and half-duplex collision
 * counters that mean nothing on a SerDes or a PON; run the command by hand to
 * see those. Emitting them here would put octets and packets in one family.
 */
struct mib_key {
	const char *key;	/* the label diag prints, matched exactly */
	const char *labels;	/* extra label text, or "" */
};

struct mib_fam {
	const char *metric;
	const char *help;
	const struct mib_key *keys;	/* terminated by a NULL key */
};

static const struct mib_key mib_rx_octets[] = {
	{ "ifInOctets", "" }, { 0, 0 }
};
static const struct mib_key mib_tx_octets[] = {
	{ "ifOutOctets", "" }, { 0, 0 }
};
static const struct mib_key mib_rx_pkts[] = {
	{ "ifInUcastPkts",     ",kind=\"unicast\""   },
	{ "ifInMulticastPkts", ",kind=\"multicast\"" },
	{ "ifInBroadcastPkts", ",kind=\"broadcast\"" },
	{ 0, 0 }
};
static const struct mib_key mib_tx_pkts[] = {
	{ "ifOutUcastPkts",     ",kind=\"unicast\""   },
	{ "ifOutMulticastPkts", ",kind=\"multicast\"" },
	{ "ifOutBroadcastPkts", ",kind=\"broadcast\"" },
	{ 0, 0 }
};
static const struct mib_key mib_rx_drops[] = {
	{ "dot1dTpPortInDiscards", "" }, { 0, 0 }
};
static const struct mib_key mib_tx_drops[] = {
	{ "ifOutDiscards", "" }, { 0, 0 }
};
static const struct mib_key mib_rx_errors[] = {
	{ "etherStatsCRCAlignErrors", ",kind=\"crc_align\"" },
	{ "etherStatsFragments",      ",kind=\"fragment\""  },
	{ "etherStatsJabbers",        ",kind=\"jabber\""    },
	{ "etherStatsRxUndersizePkts", ",kind=\"undersize\"" },
	{ "etherStatsRxOversizePkts",  ",kind=\"oversize\""  },
	{ 0, 0 }
};
static const struct mib_key mib_pause[] = {
	{ "dot3InPauseFrames",  ",direction=\"receive\""  },
	{ "dot3OutPauseFrames", ",direction=\"transmit\"" },
	{ 0, 0 }
};

static const struct mib_fam mib_fams[] = {
	{ "gpon_port_receive_octets_total",
	  "Octets received on a switch port. port=\"2\" is the PON side, \"0\" the host SerDes side.",
	  mib_rx_octets },
	{ "gpon_port_transmit_octets_total",
	  "Octets transmitted on a switch port.", mib_tx_octets },
	{ "gpon_port_receive_packets_total",
	  "Packets received on a switch port, by destination kind.", mib_rx_pkts },
	{ "gpon_port_transmit_packets_total",
	  "Packets transmitted on a switch port, by destination kind.", mib_tx_pkts },
	{ "gpon_port_receive_drops_total",
	  "Received frames dropped by the bridge on a switch port.", mib_rx_drops },
	{ "gpon_port_transmit_drops_total",
	  "Frames dropped instead of being transmitted on a switch port.", mib_tx_drops },
	{ "gpon_port_receive_errors_total",
	  "Malformed frames received on a switch port, by error kind.", mib_rx_errors },
	{ "gpon_port_pause_frames_total",
	  "802.3x pause frames seen on a switch port.", mib_pause },
	{ 0, 0, 0 }
};

/*
 * Emit every sample for one key, walking the whole buffer and tracking which
 * "Port: N" block each line falls in. Driven per key rather than per line so
 * each family's "# HELP"/"# TYPE" is written exactly once — the device prints
 * one complete block per port, so a line-ordered walk would repeat the header
 * for every port and Prometheus rejects a duplicated header.
 */
static void mib_emit_key(int fd, const char *buf, const char *metric,
			 const struct mib_key *mk)
{
	char port[8];
	unsigned long i = 0, plen = 0;

	while (buf[i]) {
		unsigned long ls = i, le = i, k, vs;

		while (buf[le] && buf[le] != '\n')
			le++;

		k = line_key(buf, ls, "Port:");
		if (k) {
			while (k < le && buf[k] == ' ')
				k++;
			plen = 0;
			while (k < le && buf[k] >= '0' && buf[k] <= '9' &&
			       plen + 1 < sizeof(port))
				port[plen++] = buf[k++];
			goto next;
		}

		if (!plen)
			goto next;	/* a counter before any "Port:" line */

		k = line_key(buf, ls, mk->key);
		if (!k)
			goto next;
		/* The label has to END here: only spaces, then the colon.
		 * Without this a key would also match any longer label it
		 * happens to be a prefix of. */
		while (k < le && buf[k] == ' ')
			k++;
		if (k >= le || buf[k] != ':')
			goto next;

		k++;
		while (k < le && buf[k] == ' ')
			k++;
		vs = k;
		while (k < le && buf[k] >= '0' && buf[k] <= '9')
			k++;
		if (k == vs)
			goto next;	/* no number: not a counter line */

		put_fd(fd, metric);
		put_fd(fd, "{port=\"");
		write_all(fd, port, plen);
		put_fd(fd, "\"");
		put_fd(fd, mk->labels);
		put_fd(fd, "} ");
		write_all(fd, buf + vs, k - vs);
		put_fd(fd, "\n");

next:
		i = (buf[le] == '\n') ? le + 1 : le;
	}
}

static void metric_port_mib(int fd, const char *buf)
{
	unsigned long f, k;

	for (f = 0; mib_fams[f].metric; f++) {
		emit_header(fd, mib_fams[f].metric, mib_fams[f].help, "counter");
		for (k = 0; mib_fams[f].keys[k].key; k++)
			mib_emit_key(fd, buf, mib_fams[f].metric,
				     &mib_fams[f].keys[k]);
	}
}

/*
 * One diag invocation per scrape.
 *
 * /bin/diag costs ~32 ms per run on this CPU and almost all of it is startup:
 * a single transceiver read measured 32.5 ms against 35.5 ms for the 92-counter
 * mib dump, and 4.0 ms for a bare fork+exec of /bin/true. Paying that eight
 * times came to 259.5 ms a scrape; the same eight commands piped into one diag
 * take 47.5 ms. So the cost scales with the number of PROCESSES, not the number
 * of metrics, and the fix is to stop starting more of them.
 *
 * diag reads commands from stdin and echoes each after its "RTK.0> " prompt,
 * which is what makes the output splittable back into per-command sections.
 *
 * The trade: this is now one failure domain. Previously a diag that hung or
 * crashed cost one metric; now it costs all of them. The /proc metrics are
 * unaffected and gpon_exporter_up still reports, so a scrape still tells you
 * the stick is alive.
 */
#define DIAG_PROMPT "RTK.0> "

struct diag_sec {
	const char *cmd;	/* sent to diag, and matched against its echo */
	const char *metric;	/* single-valued case */
	const char *help;
	int (*scan)(const char *, unsigned long *, unsigned long *);
	void (*custom)(int fd, const char *sec);	/* multi-series case */
};

/*
 * Every metric name in this file lives here and nowhere else; rename in one
 * place if you need to match an existing dashboard. The order is the order the
 * commands are sent, and therefore the order the series come out.
 */
static const struct diag_sec diag_secs[] = {
	{ "pon get transceiver bias-current", "gpon_bias_current_ma",
	  "Bias current of the GPON transceiver, in mA.", scan_decimal, 0 },
	{ "pon get transceiver rx-power", "gpon_rx_power_dbm",
	  "Rx power of the GPON transceiver, in dBm.", scan_decimal, 0 },
	{ "pon get transceiver tx-power", "gpon_tx_power_dbm",
	  "Tx power of the GPON transceiver, in dBm.", scan_decimal, 0 },
	{ "pon get transceiver temperature", "gpon_temperature_celsius",
	  "Temperature of the GPON transceiver, in Celsius.", scan_decimal, 0 },
	{ "pon get transceiver voltage", "gpon_voltage_volts",
	  "Supply voltage of the GPON transceiver, in Volts.", scan_decimal, 0 },
	{ "gpon get onu-state", "gpon_onu_state",
	  "ONU state number, the N in O(N). 5 is operational.", scan_onu_state, 0 },
	{ "gpon get alarm-status", 0, 0, 0, metric_alarms },
	{ "mib dump counter port all", 0, 0, 0, metric_port_mib },
	{ 0, 0, 0, 0, 0 }
};

/*
 * Built from the table rather than written out as a literal, so the commands
 * sent and the echoes matched against cannot drift apart. A mismatch would not
 * fail loudly — it would silently drop that metric.
 */
static unsigned long build_diag_script(char *dst, unsigned long cap)
{
	const char *tail = "exit\n";
	unsigned long n = 0, i, k;

	for (i = 0; diag_secs[i].cmd; i++) {
		for (k = 0; diag_secs[i].cmd[k]; k++)
			if (n + 2 < cap)
				dst[n++] = diag_secs[i].cmd[k];
		if (n + 2 < cap)
			dst[n++] = '\n';
	}
	/* Closing stdin would end it too, but `exit` lets diag leave on its own
	 * terms rather than on a read error. */
	for (k = 0; tail[k]; k++)
		if (n + 2 < cap)
			dst[n++] = tail[k];

	dst[n] = 0;
	return n;
}

/* Index just past `needle`, or 0 if absent. 0 is unambiguous as "not found"
 * because a match always lands past the needle's own length. */
static unsigned long find_after(const char *buf, unsigned long from,
				const char *needle)
{
	unsigned long i, k;

	for (i = from; buf[i]; i++) {
		for (k = 0; needle[k] && buf[i + k] == needle[k]; k++)
			;
		if (!needle[k])
			return i + k;
	}
	return 0;
}

/* Whether the `alen` bytes at `a` are exactly the string `b`. */
static int cmd_is(const char *a, unsigned long alen, const char *b)
{
	unsigned long k;

	for (k = 0; k < alen; k++)
		if (!b[k] || a[k] != b[k])
			return 0;
	return b[alen] == 0;
}

/*
 * Whether the diag scrape worked, and how completely.
 *
 * Batching every command into one fork made diag a single failure domain: one
 * hang, crash or missing binary now costs ~90 metric families at once. Their
 * absence is detectable in Prometheus, but absence is a weak signal -- it looks
 * identical to a stick that has not been scraped yet, or to a relabelling
 * mistake, and gpon_exporter_up is hardcoded to 1 so it keeps reporting health
 * that only covers the /proc half.
 *
 * The section count matters as much as the boolean. diag's output is about
 * 6.9 KB into a 16 KB buffer, and a truncated read costs the TAIL sections
 * silently -- the mib counter dump is last, so a partial scrape looks like a
 * working one that simply has no forwarding data. Emitting parsed against
 * expected makes that a comparison rather than something you have to notice.
 */
static void emit_diag_health(int fd, int up, unsigned long parsed,
			     unsigned long expected)
{
	emit_header(fd, "gpon_diag_up",
		    "1 when /bin/diag ran and at least one section parsed.", "gauge");
	put_fd(fd, "gpon_diag_up ");
	put_fd(fd, up ? "1\n" : "0\n");

	emit_header(fd, "gpon_diag_sections_parsed",
		    "diag command sections understood in this scrape.", "gauge");
	put_fd(fd, "gpon_diag_sections_parsed ");
	put_u32_fd(fd, parsed);
	put_fd(fd, "\n");

	emit_header(fd, "gpon_diag_sections_expected",
		    "diag command sections this build asks for; parsed below this is a truncated scrape.",
		    "gauge");
	put_fd(fd, "gpon_diag_sections_expected ");
	put_u32_fd(fd, expected);
	put_fd(fd, "\n");
}

static void emit_diag_metrics(int fd)
{
	static char *const argv[] = { "diag", 0 };
	char script[512];
	/* Two ports of mib counters are 5856 bytes on their own; the whole
	 * scrape's output measured about 6.9 KB, so this is a bit over 2x
	 * headroom. A truncated read costs the tail sections rather than
	 * corrupting anything, but the mib dump is last, so headroom matters
	 * more than it looks. */
	char buf[16384];
	unsigned long cs;
	unsigned long parsed = 0, expected = 0;

	while (diag_secs[expected].cmd)
		expected++;

	build_diag_script(script, sizeof(script));
	if (run_script_to_buf(DIAG_PATH, argv, script, buf, sizeof(buf)) <= 0) {
		emit_diag_health(fd, 0, 0, expected);
		return;
	}

	/* cs always points at a command, just past its prompt. find_after
	 * returns the position AFTER the needle, so the next section's start is
	 * the previous search's result — searching again from it would step over
	 * a prompt and drop every other metric. */
	cs = find_after(buf, 0, DIAG_PROMPT);
	while (cs) {
		unsigned long ce, ss, nxt, cut, t;
		char saved = 0;

		ce = cs;
		while (buf[ce] && buf[ce] != '\n' && buf[ce] != '\r')
			ce++;
		if (!buf[ce])
			break;		/* the trailing prompt, with no command */

		ss = ce;
		while (buf[ss] == '\n' || buf[ss] == '\r')
			ss++;

		nxt = find_after(buf, ss, DIAG_PROMPT);

		/* Terminate this section before handing it over. Every extractor
		 * takes a NUL-terminated string and stops at its first match, so
		 * without this a command that printed nothing would be handed the
		 * NEXT command's output and report it as its own value. The byte
		 * is restored afterwards because it is part of the marker used to
		 * find the following section. */
		cut = nxt ? nxt - (sizeof(DIAG_PROMPT) - 1) : 0;
		if (cut) {
			saved = buf[cut];
			buf[cut] = 0;
		}

		for (t = 0; diag_secs[t].cmd; t++) {
			if (!cmd_is(buf + cs, ce - cs, diag_secs[t].cmd))
				continue;
			if (diag_secs[t].custom)
				diag_secs[t].custom(fd, buf + ss);
			else
				emit_scalar(fd, diag_secs[t].metric,
					    diag_secs[t].help, buf + ss,
					    diag_secs[t].scan);
			parsed++;
			break;
		}

		if (cut)
			buf[cut] = saved;
		cs = nxt;
	}

	emit_diag_health(fd, parsed > 0, parsed, expected);
}

/*
 * A label value must not contain a quote, a backslash or a newline, and this
 * one comes from argv[0] — chosen by whoever started the process, not by us.
 * Escaping it properly would be more code than refusing it: an unexpected path
 * is reported as "unknown" rather than being allowed to produce an exposition
 * Prometheus cannot parse.
 */
static int label_safe(const char *s)
{
	unsigned long i;

	for (i = 0; s[i]; i++)
		if (s[i] == '"' || s[i] == '\\' || s[i] == '\n' || s[i] == '\r')
			return 0;
	return i != 0;
}

static void metric_build_info(int fd)
{
	emit_header(fd, "gpon_exporter_build_info",
		    "Always 1. version is `git describe` at build time; path is argv[0], "
		    "which says whether this is the image's /bin/metricsd or an "
		    "/etc/config override.", "gauge");
	put_fd(fd, "gpon_exporter_build_info{version=\"" BUILD_ID "\",path=\"");
	put_fd(fd, label_safe(exporter_path) ? exporter_path : "unknown");
	put_fd(fd, "\"} 1\n");
}

static void emit_metrics(int fd)
{
	put_fd(fd, "# HELP gpon_exporter_up Always 1. Confirms the exporter ran.\n"
		   "# TYPE gpon_exporter_up gauge\n"
		   "gpon_exporter_up 1\n");

	metric_build_info(fd);
	metric_uptime(fd);
	metric_loadavg(fd);
	metric_meminfo(fd);
	metric_netdev(fd);

	/* Everything from /bin/diag, in a single fork. */
	emit_diag_metrics(fd);

	/*
	 * NOT exporting `gpon show counter global ds-eth`. Four consecutive reads
	 * gave 55827, 13537, 53607, 8873 — the registers are read-and-clear, each
	 * read returning the count since the previous one.
	 *
	 * Two problems, the second worse than the first:
	 *
	 *  - They are not monotonic, so Prometheus `counter` is the wrong type and
	 *    rate() would read every reset as a counter restart.
	 *  - Reading is DESTRUCTIVE. A scrape consumes the delta, so the vendor web
	 *    UI or a manual `diag` silently loses whatever we took, and we lose
	 *    whatever they take. Scraping would mutate device state that something
	 *    else may depend on.
	 *
	 * The fix, if we want them, is to accumulate deltas into a monotonic total
	 * held in this process — which is the standard treatment for read-and-clear
	 * hardware counters, and gives correct counter-reset semantics on restart.
	 * It is only sound if this exporter is the sole reader. Left undone until
	 * that is established.
	 */
}

#endif /* ODI_METRICS_BODY_H */
