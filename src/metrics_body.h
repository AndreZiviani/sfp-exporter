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
 * A metric is one diag invocation plus one extractor. On failure nothing is
 * emitted at all — an absent series is honest, whereas a zero would look like a
 * real reading of zero dBm.
 */
static void diag_metric(int fd, const char *name, const char *help,
			char *const argv[],
			int (*scan)(const char *, unsigned long *, unsigned long *))
{
	char buf[512];
	unsigned long start = 0, len = 0;

	/* Bail on a failed run rather than emitting whatever is in buf: on this
	 * path it would be the PREVIOUS command's text, published as this
	 * metric's value. run_to_buf NUL-terminates whatever it did read, so a
	 * partial read degrades to a parse failure rather than a wrong number. */
	if (run_to_buf(DIAG_PATH, argv, buf, sizeof(buf)) <= 0)
		return;
	if (!scan(buf, &start, &len))
		return;

	emit_header(fd, name, help, "gauge");
	put_fd(fd, name);
	put_fd(fd, " ");
	write_all(fd, buf + start, len);
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
static void metric_alarms(int fd)
{
	static char *const argv[] = { "diag", "gpon", "get", "alarm-status", 0 };
	char buf[1024];
	char label[64];
	unsigned long i = 0;
	int have_header = 0;

	if (run_to_buf(DIAG_PATH, argv, buf, sizeof(buf)) <= 0)
		return;
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

static void metric_port_mib(int fd)
{
	static char *const argv[] = {
		"diag", "mib", "dump", "counter", "port", "all", 0
	};
	/* The device prints 5856 bytes for two ports, so this is a little over
	 * 2x headroom. run_to_buf truncates rather than failing, and a truncated
	 * read costs the tail series — port 2, the PON side, is printed last —
	 * so the headroom matters more than it looks. */
	char buf[12288];
	unsigned long f, k;

	if (run_to_buf(DIAG_PATH, argv, buf, sizeof(buf)) <= 0)
		return;

	for (f = 0; mib_fams[f].metric; f++) {
		emit_header(fd, mib_fams[f].metric, mib_fams[f].help, "counter");
		for (k = 0; mib_fams[f].keys[k].key; k++)
			mib_emit_key(fd, buf, mib_fams[f].metric,
				     &mib_fams[f].keys[k]);
	}
}

static void emit_metrics(int fd)
{
	/* Every metric name in this file lives here and nowhere else; rename in
	 * one place if you need to match an existing dashboard. */
	static char *const bias[] = { "diag", "pon", "get", "transceiver", "bias-current", 0 };
	static char *const rx[]   = { "diag", "pon", "get", "transceiver", "rx-power", 0 };
	static char *const tx[]   = { "diag", "pon", "get", "transceiver", "tx-power", 0 };
	static char *const temp[] = { "diag", "pon", "get", "transceiver", "temperature", 0 };
	static char *const volt[] = { "diag", "pon", "get", "transceiver", "voltage", 0 };
	static char *const onu[]  = { "diag", "gpon", "get", "onu-state", 0 };

	put_fd(fd, "# HELP gpon_exporter_up Always 1. Confirms the exporter ran.\n"
		   "# TYPE gpon_exporter_up gauge\n"
		   "gpon_exporter_up 1\n");

	metric_uptime(fd);
	metric_loadavg(fd);
	metric_meminfo(fd);
	metric_netdev(fd);

	diag_metric(fd, "gpon_bias_current_ma",
		    "Bias current of the GPON transceiver, in mA.", bias, scan_decimal);
	diag_metric(fd, "gpon_rx_power_dbm",
		    "Rx power of the GPON transceiver, in dBm.", rx, scan_decimal);
	diag_metric(fd, "gpon_tx_power_dbm",
		    "Tx power of the GPON transceiver, in dBm.", tx, scan_decimal);
	diag_metric(fd, "gpon_temperature_celsius",
		    "Temperature of the GPON transceiver, in Celsius.", temp, scan_decimal);
	diag_metric(fd, "gpon_voltage_volts",
		    "Supply voltage of the GPON transceiver, in Volts.", volt, scan_decimal);
	diag_metric(fd, "gpon_onu_state",
		    "ONU state number, the N in O(N). 5 is operational.", onu, scan_onu_state);

	metric_alarms(fd);
	metric_port_mib(fd);

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
