/*
 * Minimal o32 MIPS syscall layer, shared by the freestanding binaries here.
 *
 * o32 convention: number in $v0, args in $a0-$a3, `syscall`, result in $v0 with
 * $a3 non-zero on error. Numbers are offset by 4000.
 */

#ifndef ODI_SYSCALL_H
#define ODI_SYSCALL_H

#define __NR_exit  4001
#define __NR_read  4003
#define __NR_write 4004
#define __NR_open  4005
#define __NR_close 4006

#define __NR_fork       4002
#define __NR_waitpid    4007
#define __NR_execve     4011
#define __NR_dup2       4063
/* pipe2, not pipe. On MIPS the raw pipe(2) returns the second descriptor in
 * $v1 rather than writing both through the pointer, and this syscall layer
 * only ever returns $v0. pipe2 uses the ordinary pointer form on every
 * architecture. It needs Linux 2.6.27; the stick runs 2.6.30.9, and the number
 * below is from Realtek's own asm/unistd.h for this kernel (4000 + 328). */
#define __NR_pipe2      4328
#define __NR_socket     4183
#define __NR_bind       4169
#define __NR_listen     4174
#define __NR_accept     4168
#define __NR_setsockopt 4181

/* MIPS O_* are not the generic values either: O_CREAT is 0x100, not 0x40.
 * Verified against <fcntl.h> for mips-linux-gnu. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x100
#define O_TRUNC  0x200

/*
 * MIPS does not use the generic socket ABI values. All three of these differ
 * from x86/ARM and are silent if wrong: SOCK_STREAM and SOCK_DGRAM are swapped,
 * and SOL_SOCKET is the BSD 0xffff rather than 1. Verified against
 * <bits/socket_type.h> and <asm/socket.h> for mips-linux-gnu.
 */
#define AF_INET       2
#define SOCK_STREAM   2       /* generic ABI says 1 */
#define SOL_SOCKET    65535   /* generic ABI says 1 */
#define SO_REUSEADDR  4

/* Defined in start.S — only setsockopt needs more than three arguments. */
extern long __syscall6(long n, long a, long b, long c, long d, long e, long f);

/*
 * Lifted from musl's MIPS syscall_arch.h (MIT licensed, as is this file):
 * loading $v0 inside the asm — rather than trusting a register-asm variable to
 * survive until the syscall — and this exact clobber list are both
 * load-bearing.
 */
#define SYSCALL_CLOBBERS \
	"$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", \
	"$14", "$15", "$24", "$25", "hi", "lo", "memory"

static long syscall3(long n, long a, long b, long c)
{
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7");
	register long r2 __asm__("$2");

	__asm__ __volatile__ (
		"addu $2, $0, %2 ; syscall"
		: "=&r"(r2), "=r"(r7)
		: "ir"(n), "r"(r4), "r"(r5), "r"(r6)
		: SYSCALL_CLOBBERS);

	return (r7 && r2 > 0) ? -r2 : r2;
}

__attribute__((unused)) static unsigned long str_len(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

static long write_all(int fd, const char *buf, unsigned long len)
{
	unsigned long done = 0;

	while (done < len) {
		long n = syscall3(__NR_write, fd, (long)(buf + done), len - done);

		if (n <= 0)
			return n;
		done += (unsigned long)n;
	}
	return (long)done;
}

__attribute__((unused)) static void put_fd(int fd, const char *s)
{
	write_all(fd, s, str_len(s));
}

/* Unbuffered by construction — every put() is its own write syscall, which is
 * output already on the wire survives a SIGILL, which is what made mapping
 * this CPU possible in the first place. */
static void put(const char *s)
{
	write_all(1, s, str_len(s));
}

/*
 * Read a whole small file. Returns bytes read, or negative on error. Used for
 * /proc entries, which are generated on read and always small.
 */
__attribute__((unused)) static long read_file(const char *path, char *buf, unsigned long cap)
{
	long fd = syscall3(__NR_open, (long)path, O_RDONLY, 0);
	unsigned long got = 0;

	if (fd < 0)
		return fd;

	while (got + 1 < cap) {
		long n = syscall3(__NR_read, fd, (long)(buf + got), cap - got - 1);

		if (n < 0) {
			syscall3(__NR_close, fd, 0, 0);
			return n;
		}
		if (n == 0)
			break;
		got += (unsigned long)n;
	}

	syscall3(__NR_close, fd, 0, 0);
	buf[got] = 0;
	return (long)got;
}

/* Decimal formatting by hand: printf would drag in a libc we do not link, and
 * the float formats would drag in soft-float support with it. */
__attribute__((unused)) static void put_u32_fd(int fd, unsigned long v)
{
	char tmp[11];
	int i = 0;

	if (v == 0) {
		put_fd(fd, "0");
		return;
	}
	while (v && i < (int)sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i--)
		write_all(fd, &tmp[i], 1);
}

/*
 * Run a program with stdout redirected to a file, and wait for it. Used to
 * drive /bin/diag, whose values are not exposed through /proc.
 *
 * A file rather than a pipe on purpose: MIPS `pipe` returns its two descriptors
 * in $v0 and $v1, which the three-argument inline syscall cannot express. pipe2
 * would avoid that, but a temp file in /var costs nothing here and keeps the
 * child setup to open/dup2/execve.
 */
__attribute__((unused))
/*
 * Run a command and capture its output.
 *
 * A pipe, not a temporary file. The file version needed a writable directory,
 * and the one it used (/var/exp) was created by the dev-time helper script but
 * by nothing in the firmware image — so on a flashed stick every diag-derived
 * metric silently vanished while the /proc ones kept working. A pipe removes
 * the dependency: nothing to create, nothing left behind, and no fixed path
 * for two instances to collide on.
 */
static long run_to_buf(const char *path, char *const argv[],
		       char *buf, unsigned long cap)
{
	int fds[2];
	long pid;
	long status = 0;
	unsigned long got = 0;

	if (syscall3(__NR_pipe2, (long)fds, 0, 0) < 0)
		return -1;

	pid = syscall3(__NR_fork, 0, 0, 0);
	if (pid < 0) {
		syscall3(__NR_close, fds[0], 0, 0);
		syscall3(__NR_close, fds[1], 0, 0);
		return -1;
	}

	if (pid == 0) {
		syscall3(__NR_close, fds[0], 0, 0);
		syscall3(__NR_dup2, fds[1], 1, 0);	/* stdout */
		syscall3(__NR_dup2, fds[1], 2, 0);	/* stderr, same place */
		if (fds[1] > 2)
			syscall3(__NR_close, fds[1], 0, 0);
		syscall3(__NR_execve, (long)path, (long)argv, 0);
		syscall3(__NR_exit, 127, 0, 0);	/* exec failed */
	}

	/* The parent's copy of the write end must go, or the read below never
	 * sees EOF: the pipe stays open as long as any descriptor to it does. */
	syscall3(__NR_close, fds[1], 0, 0);

	/* Drain BEFORE waiting. The other order deadlocks the moment a child
	 * outgrows the pipe buffer: it blocks in write() while we block in
	 * waitpid(). diag's output is a few hundred bytes against 64 KB, so it
	 * would not bite today — which is exactly how it would survive to bite
	 * someone later. */
	while (got + 1 < cap) {
		long n = syscall3(__NR_read, fds[0], (long)(buf + got),
				  cap - got - 1);

		if (n <= 0)
			break;
		got += (unsigned long)n;
	}
	buf[got] = 0;

	syscall3(__NR_close, fds[0], 0, 0);
	syscall3(__NR_waitpid, pid, (long)&status, 0);
	return (long)got;
}

#endif /* ODI_SYSCALL_H */
