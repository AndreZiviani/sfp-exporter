# AGENTS.md

Guidance for any coding agent (or human) working in this repository. Read
the top-level `README.md` first for what this project is and why it is built
the way it is; this file is about how to work in it safely and correctly.

## What this is, and how it fits with the firmware image

`sfp-exporter` builds `metricsd`, a Prometheus exporter that runs **on** an
RTL9601-based GPON SFP ONU stick and serves optics and forwarding metrics on
its own HTTP port. It is a standalone, freestanding binary with no
dependencies of its own.

It does not build a flashable firmware image. A separate, private firmware
project consumes this repo's **releases** (the `metricsd` binary plus
`SHA256SUMS`, verified before use) rather than its source, and bakes them
into a custom image alongside the stock vendor firmware and a config UI. If
you are looking for image-building, flashing, or device-provisioning logic,
it lives in that other project, not here.

## Layout

    src/metricsd.c        the exporter: socket, accept loop, HTTP response
    src/metrics_body.h    the metrics themselves; every metric name lives here
    src/syscall.h         o32 syscall layer, file reads, fork/exec, decimals
    src/start.S           _start and a 6-argument syscall stub for setsockopt
    scripts/verify.sh     asserts ELF32 / big endian / MIPS / static / no INTERP
    scripts/toolchain-image.sh  prints (and pulls) the pinned toolchain image
    scripts/deploy.sh     push a file to the stick over netcat
    stick/exporter-up.sh  on-device start/stop
    toolchain.env         the toolchain image (odi-toolchain freestanding), pinned by digest
    docs/BUILDING.md      the toolchain image: pulling, logging in, building it locally
    Makefile               every target below; re-enters itself with IN_CONTAINER=1
    .github/workflows/release.yml   build + gate on every push, publish on v* tags

## Build and test commands

    make image      # pull the pinned toolchain image (once; docs/BUILDING.md)
    make httpd      # build build/metricsd (what CI runs)
    make verify      # ELF shape: ELF32, big-endian MIPS, static, no INTERP
    make isa         # instruction census against the RLX5281's confirmed ISA
    make run ARGS=... # run a built binary under qemu-user (proves logic/syscalls,
                       # NOT instruction legality -- qemu emulates full MIPS32)
    make all          # httpd + verify + isa -- what you almost always want
    make release      # httpd + verify + isa + sums -- exactly what CI runs on a tag
    make sums         # SHA256SUMS over build/metricsd
    make clean

There is no separate `make test`; `make all` (or `make release`) is the
regression gate, and it is run on every push and PR via
`.github/workflows/release.yml`, not only on tags.

## Release process

Every push and PR builds and gates the binary (`make httpd`, `make verify`,
`make isa`, `make sums`) so a tag can never fail on something an ordinary
commit would have caught. Only a `v*` tag publishes a GitHub release, with
`metricsd`, `SHA256SUMS`, and `stick/exporter-up.sh` as assets. `BUILD_ID` is
`git describe --tags --always --dirty`, computed on the host (the toolchain
container has no git history) and compiled in as `gpon_exporter_build_info` --
this is how a stick reports drift between what an image manifest claims and
what is actually running. A shallow checkout breaks this silently by making
every tag describe as `unknown`; CI always fetches full history.

## Coding rules

- **Freestanding C, no libc, no dependencies.** `-nostdlib -nostartfiles
  -static`, one hand-written `_start` in `src/start.S`, raw syscalls via
  `src/syscall.h`. There is no libc worth linking against on this device (a
  2009 uClibc build); going freestanding removes the ISA-compatibility
  question entirely rather than trying to satisfy it.
- **Big-endian MIPS-I, targeting a specific trapping core.** The RLX5281
  core implements the MIPS instruction set in pieces: `movz`/`movn`/`ll`/
  `sc`/`sync`/`bltzl`/`madd` are confirmed to work; `mul`, `clz` (and the
  rest of the SPECIAL2 class), `teq`/`tne`/`tge`/`tlt`, and `beql`/`bnel`
  are confirmed illegal and raise SIGILL. Every build uses `-march=mips1
  -mabi=32 -EB -msoft-float -G0 -fno-pic -mno-abicalls -ffreestanding
  -fno-builtin -fno-stack-protector`. Do not raise `-march`, add FPU code,
  or assume a generic MIPS32/MIPS-II toolchain default is safe here --
  `mul` and `clz` are the two most common ways a normal C compile silently
  becomes unrunnable on this hardware.
  - **Go and TinyGo cannot target this core**, and no compiler flag fixes
    it: the runtime library sets the ISA floor, not the compiler's code
    generation flag. Do not propose porting this to Go.
  - `isa-audit` and `isa-allowlist`, shared with the other RLX5281 projects
    and installed in the toolchain image (odi-toolchain), are the actual
    gate, run by `make isa` and by CI: the binary fails if any
    confirmed-illegal mnemonic or floating point appears, and anything
    unverified is printed as UNVERIFIED (a CI warning, not a build failure).
    Run it, do not just trust `-march`. The allowlist grows in odi-toolchain,
    and only by executing an instruction on a device.
  - `scripts/verify.sh` checks the ELF shape a dynamic or non-static binary
    would fail on the device: ELF32, big-endian, MIPS, no `PT_INTERP`, no
    `NEEDED` shared libraries.
- **MIPS syscall/ABI constants differ from the generic Linux ones you may
  remember from x86 or ARM**: `SOCK_STREAM`/`SOCK_DGRAM` are swapped,
  `SOL_SOCKET` is `65535`, `O_CREAT` is `0x100`, and o32 syscall numbers are
  offset from a base of 4000. A wrong constant here is a syscall that
  "succeeds" at doing the wrong thing, not one that fails loudly. Look up
  the actual target header rather than porting a constant from memory.
- **No apostrophes in shell-script comments.** A single quote inside a
  single-quoted inline block (e.g. `bash -c '...'`) silently terminates the
  shell word and runs the rest of the line in the outer shell. Rephrase;
  do not escape.
- Values read from `diag` are emitted as the literal text it printed, never
  parsed to a number and back -- see "Implementation notes" in the README
  before changing anything in the metric-formatting path.

## Testing on a stick safely

- **Copy to `/tmp`, never overwrite a running binary in place.**
  `scripts/deploy.sh` pushes over a netcat pipe (the device cannot open
  connections outward, so it listens and this host connects) and lands the
  file in `/tmp`, which is tmpfs: a bad binary cannot brick the device and
  does not survive a reboot. `stick/exporter-up.sh` starts/stops it from
  there.
- The rootfs is read-only squashfs and the daemon has no installer of its
  own; there is no in-place replacement to accidentally perform on this
  binary, but keep that same discipline (temp location, verify, then run)
  for anything else you push alongside it.
- This repo does not flash firmware and has no `sw_tryactive`/`sw_commit`
  logic; if your task involves writing to a flash partition or a boot slot,
  that is out of scope here.

## Dangerous commands on the stick

These are properties of the device's other userland tools, not of this
exporter, but anyone testing on real hardware needs to know them because
they are easy to trigger by accident while poking around:

- **`omcicli get tables` wedges the OMCI daemon.** Do not run it against a
  device you need to stay provisioned.
- **`diag`, read from a stdin that never closes, spins at 100% CPU.** Any
  script or shell that pipes to `diag` and leaves stdin open (no EOF, no
  command) will peg the CPU. Always give it a way to see EOF, or wrap the
  invocation in a timeout.
- **Reading an undecoded SoC register address with `devmem` can stall the
  bus until the hardware watchdog resets the device.** Do not probe
  addresses you cannot already account for.
