/*
 * Prometheus exporter for the ODI DFP-34X-2C2, as a standalone HTTP server.
 *
 * Realtek's boa cannot exec external CGI (it 404s anything typed
 * application/x-httpd-cgi, while serving the same file happily as a static
 * download), so the exporter has to carry its own listener. That turns out to
 * be about forty lines on top of the syscall layer.
 *
 * Still freestanding: no libc, so nothing has to respect the -march=mips2
 * ceiling on our behalf. Single-threaded and serial — Prometheus makes one
 * request per scrape, and a fork per connection would need a wait/reap loop for
 * no benefit here.
 *
 * MIPS socket ABI traps, all silent if wrong and all verified against the
 * mips-linux-gnu headers: SOCK_STREAM is 2 (not 1 — MIPS swaps it with
 * SOCK_DGRAM), and SOL_SOCKET is 65535 (not 1). See syscall.h.
 *
 * This CPU is big-endian, so host order already is network order: there is no
 * htons or htonl anywhere in this file, deliberately.
 */

#include "metrics_body.h"

#define DEFAULT_PORT 9100
#define BACKLOG      8

static unsigned long parse_u16(const char *s, unsigned long fallback)
{
	unsigned long v = 0;
	unsigned long i = 0;

	if (!s || !s[0])
		return fallback;

	for (i = 0; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9')
			return fallback;
		v = v * 10 + (unsigned long)(s[i] - '0');
		if (v > 65535)
			return fallback;
	}
	return v ? v : fallback;
}

static void serve(int conn)
{
	char req[512];

	/* Read the request and discard it: we answer the same on every path.
	 * Draining it matters anyway — replying to an unread request invites the
	 * client's close to arrive as an RST that discards our response. */
	syscall3(__NR_read, conn, (long)req, sizeof(req) - 1);

	put_fd(conn,
	       "HTTP/1.0 200 OK\r\n"
	       "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
	       "Connection: close\r\n"
	       "\r\n");
	emit_metrics(conn);
}

int main(int argc, char **argv)
{
	unsigned long port = parse_u16(argc > 1 ? argv[1] : 0, DEFAULT_PORT);

	/* Reported as the `path` label on gpon_exporter_build_info, so a scrape
	 * says whether this is the image's binary or an /etc/config override. */
	if (argc > 0 && argv[0] && argv[0][0])
		exporter_path = argv[0];

	long one = 1;
	long fd, conn;

	/* struct sockaddr_in, laid out by hand to avoid assuming a header we do
	 * not include. sin_family is host order; sin_port is network order, which
	 * on this big-endian CPU is the same thing. */
	unsigned char addr[16];
	unsigned long i;

	for (i = 0; i < sizeof(addr); i++)
		addr[i] = 0;

	addr[0] = 0;                                    /* sin_family, high byte */
	addr[1] = AF_INET;                              /* sin_family, low byte  */
	addr[2] = (unsigned char)((port >> 8) & 0xff);  /* sin_port              */
	addr[3] = (unsigned char)(port & 0xff);
	/* addr[4..7] stay zero: sin_addr = INADDR_ANY */

	fd = syscall3(__NR_socket, AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		put("socket() failed\n");
		return 1;
	}

	/* Without SO_REUSEADDR a restart inside the TIME_WAIT window fails to
	 * bind, which looks exactly like the port already being taken. */
	__syscall6(__NR_setsockopt, fd, SOL_SOCKET, SO_REUSEADDR,
		   (long)&one, sizeof(one), 0);

	if (syscall3(__NR_bind, fd, (long)addr, sizeof(addr)) < 0) {
		put("bind() failed -- port already in use?\n");
		return 1;
	}

	if (syscall3(__NR_listen, fd, BACKLOG, 0) < 0) {
		put("listen() failed\n");
		return 1;
	}

	for (;;) {
		conn = syscall3(__NR_accept, fd, 0, 0);
		if (conn < 0)
			continue;	/* EINTR and friends: keep serving */

		serve((int)conn);
		syscall3(__NR_close, conn, 0, 0);
	}
}
