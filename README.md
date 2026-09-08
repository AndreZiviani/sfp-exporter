# sfp-exporter

[![build](https://github.com/AndreZiviani/sfp-exporter/actions/workflows/release.yml/badge.svg)](https://github.com/AndreZiviani/sfp-exporter/actions/workflows/release.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A Prometheus exporter that runs **on** an RTL9601-based GPON SFP ONU stick,
rather than scraping one from outside.

It is a single 8 KB static binary with no dependencies — no libc, no shell, no
web server. It serves the exposition format on its own port and reads the
optical diagnostics straight off the device.

Developed and confirmed on an **ODI DFP-34X-2C2** (RTL9601D). Other RTL960x
sticks are likely to work; see [Porting](#porting-to-another-stick).

```
gpon_rx_power_dbm -24.814861
gpon_tx_power_dbm 2.439562
gpon_temperature_celsius 35.582031
gpon_voltage_volts 3.162500
gpon_bias_current_ma 13.350000
gpon_onu_state 5
gpon_alarm{alarm="los"} 0
```

## Why it is built this way

Three constraints shape the whole design. Each was established by measurement,
not assumption, and each rules out the obvious approach.

**Go does not run on this hardware.** The RLX5281 core implements SPECIAL2 only
partially: `madd` works, but `mul` and `clz` raise SIGILL. Go's MIPS backend
emits both — 479 `mul` and 65 `clz` in a hello world. TinyGo emits them too and
`-llvm-features=+mips2` does not change its codegen. gccgo produces identical
output at every `-march` because the instructions come from prebuilt libgo and
glibc. The general rule: **the runtime library sets the ISA floor, not the
compiler flag.** The practical ceiling for this device is `-march=mips2`.

**The stock web server cannot run CGI**, so the exporter carries its own
listener. boa 0.93.15 contains the entire CGI/1.1 environment —
`GATEWAY_INTERFACE`, `REQUEST_METHOD`, `SCRIPT_NAME` — and still returns 404 for
anything typed `application/x-httpd-cgi`. Remove that `AddType` and the same
file is served as a static download, so it is present, readable and executable;
the CGI path simply is not wired up. Those strings serve boa's internal
`/boaform/` handlers.

**There is no libc worth linking.** The stick has uClibc 0.9.30.3 from 2009 on
Linux 2.6.30.9. Anything linked against a modern libc would also have to respect
the ISA ceiling. Going freestanding — raw syscalls, no libc at all — removes the
question entirely, and costs about forty lines.

The result is 8 KB and takes ~280 ms per scrape, most of which is forking
`diag`.

## Metrics

From `/bin/diag`, one fork each:

| metric | source |
|---|---|
| `gpon_rx_power_dbm` | `diag pon get transceiver rx-power` |
| `gpon_tx_power_dbm` | `diag pon get transceiver tx-power` |
| `gpon_bias_current_ma` | `diag pon get transceiver bias-current` |
| `gpon_temperature_celsius` | `diag pon get transceiver temperature` |
| `gpon_voltage_volts` | `diag pon get transceiver voltage` |
| `gpon_onu_state` | `diag gpon get onu-state` — the N in O(N); 5 is operational |
| `gpon_alarm{alarm="..."}` | `diag gpon get alarm-status` — `los`, `lof`, `lom`, `sf`, `sd`, `tx_too_long`, `tx_mismatch`; 1 means asserted |

From `/proc`, which costs no fork at all:

| metric | source |
|---|---|
| `gpon_uptime_seconds` | `/proc/uptime` |
| `gpon_load{1,5,15}` | `/proc/loadavg` |
| `gpon_memory_bytes{kind="total\|free\|buffers\|cached"}` | `/proc/meminfo` |
| `gpon_network_{receive,transmit}_{bytes,packets,errs,drop}_total{device="..."}` | `/proc/net/dev` |

Plus `gpon_exporter_up`, always 1, to distinguish "scraped and found nothing"
from "did not scrape".

**A metric that cannot be read is omitted entirely, never emitted as zero.** An
absent series is honest; `0` reads as a genuine measurement of zero dBm.

### Known caveats

- **`gpon_load*` is pinned and carries no signal.** Linux counts uninterruptible
  tasks in the load average, and this firmware keeps two kernel threads
  (`watchdog`, `led_swBlink`) permanently in D state. Load therefore sits at
  exactly 2.00 forever. Do not alert on it.
- **`/proc/net/dev` counters are 32-bit** and wrap at 4.29 GB, because
  `struct net_device_stats` uses `unsigned long` on this platform. Prometheus
  cannot distinguish a wrap from a counter reset, so `rate()` undercounts on a
  busy link. At 100 Mbps sustained a wrap happens roughly every six minutes.
- **`gpon_memory_bytes` would overflow above 4 GB.** These devices have tens of
  megabytes, so this is theoretical.
- **`pon0` reports zeros** on every field. Traffic is accounted on `eth0` and
  `br0`.

### Deliberately not exported

`diag gpon show counter global ds-eth` returns Ethernet frame counters, and they
are **read-and-clear**: four consecutive reads gave 55827, 13537, 53607, 8873.
Two problems, the second worse — they are not monotonic, so `counter` is the
wrong type; and reading is destructive, so a scrape silently consumes the delta
from anything else reading those registers, including the vendor web UI.

Making them usable means accumulating deltas into a running total held in the
exporter process, which is only sound if it is the sole reader. The parser is
still in `src/metrics_body.h`, unused.

OMCI (`omcicli`) is not used either. Its ANI-G optical values duplicate `diag`
at 0.002 dB granularity instead of six decimals, and the FEC performance
monitoring entity that would have justified it (ME 312) has no instances unless
the OLT creates it.

## Download

Every tagged release carries prebuilt static binaries, so you do not need a
toolchain to use this:

```sh
curl -fsSLO https://github.com/AndreZiviani/sfp-exporter/releases/latest/download/metricsd
curl -fsSLO https://github.com/AndreZiviani/sfp-exporter/releases/latest/download/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
```

The release also carries `exporter-up.sh`, the start/stop helper. Each build is
gated in CI on the same two checks described below — ELF shape, and an
instruction census against what the RLX5281 actually implements.

## Build

Requires Docker. The cross toolchain lives in a container; nothing is installed
on the host. Works on x86-64 and Apple Silicon.

```sh
make            # build, verify and audit — the usual case
make httpd      # the exporter -> build/metricsd
make verify     # assert the ELF shape the stick can actually load
make isa        # instruction census against what the RLX5281 implements
make run        # execute locally under qemu-user
make release    # everything CI does, including SHA256SUMS
make shell      # a shell in the toolchain container
make clean
```

`make run` uses qemu-user. It proves logic and syscalls but **not instruction
legality** — qemu emulates a full MIPS32 CPU and will happily execute the `mul`
and `clz` that trap on real hardware. `make isa` is the gate for that.

`make isa` exits 0 when every mnemonic is confirmed present on the hardware, 2
when the binary contains one that has never been executed on a real device, and
1 when it contains one known to trap. CI treats 1 as fatal and 2 as a warning.

## Releases

Tagging is the whole process:

```sh
git tag -a v1.0.0 -m "first release"
git push origin v1.0.0
```

`.github/workflows/release.yml` builds in the same container this repo uses
locally, runs `verify` and the ISA audit, and publishes the binaries with
generated release notes. Every push and pull request runs the identical build
and gates, so a tag cannot fail on something an ordinary commit would have
caught. Run `make release` first if you want the same answer without pushing.

## Install

The stick cannot open connections to you, so it listens and your machine
connects.

```sh
make deploy BIN=build/metricsd       IP=192.168.1.1
make deploy BIN=stick/exporter-up.sh IP=192.168.1.1
```

Each prints a `nc -l -p ...` line to run on the stick first, waits for you, then
pushes. Then on the stick:

```sh
sh /tmp/exporter-up.sh 9100
```

and scrape `http://<stick>:9100/metrics`. Any path answers; `/metrics` is
convention. `sh /tmp/exporter-up.sh stop` stops it — the script only ever kills
a pid it recorded itself, since `killall boa`-style cleanup would take down the
stick's management interface.

Everything lands in `/var`, which is ramfs — the rootfs is read-only squashfs.
So nothing survives a reboot, which is what you want while iterating, and a bad
binary cannot brick the stick.

### Permanent install

To survive reboots, `metricsd` has to go into the firmware image.
[Anime4000/RTL960x](https://github.com/Anime4000/RTL960x) provides the tooling —
`Tools/emulator/qemu-test.sh`, which unpacks a stock image, drops you into an
emulated shell inside it, and repacks on exit.

Needs `tar`, `squashfs-tools`, `qemu-user-static`, `binwalk`, and root:

```sh
./qemu-test.sh FIRMWARE.tar           # unpack, chroot in, repack on exit
./qemu-test.sh FIRMWARE.tar -d        # unpack only, no repack
./qemu-test.sh FIRMWARE.tar 0         # stamp the build date as the sw version
```

It moves `rootfs` aside, `unsquashfs`es it to `squashfs-root/`, copies
`qemu-mips-static` into the tree and chroots into it, so you get the firmware's
own busybox shell on your workstation.

Place additions in a `custom/` directory beside the script — it mirrors
`squashfs-root/` and is copied over it on exit:

```
custom/
├── bin/metricsd
└── etc/init.d/rc35
```

`rcS` loops `rc0` through `rc63` and runs whichever exist, and the highest
script in a stock image is `rc34` — so **`rc35` runs last and no stock file
needs patching**. Guard the line on a flag file under `/etc/config`, which is a
symlink to `/var/config`, the partition that survives reflashing:

```sh
[ -f /etc/config/exporter ] && [ -x /bin/metricsd ] && /bin/metricsd 9100 &
```

Then `touch /etc/config/exporter && reboot` turns it on, and `rm` turns it off,
without rebuilding an image.

**Keep the `&`, and do not let anything in that script block.** `/etc/inittab`
runs `rcS` as `::sysinit:`, and busybox init waits for sysinit to finish before
starting any `respawn:` entry — which is where both telnet (`::respawn:/bin/inetd`)
and the serial login live. A script that hangs there costs you every way back
into the device, and by `rc34` the watchdog is armed, so it becomes a reboot
loop rather than a hang you can interrupt. `rcS` invokes each script as
`sh <file>`, a subshell, so a non-zero exit is harmless — but a stall is not.

Rolling back is cheap: these sticks keep **two firmware partitions** and an
update always writes to the inactive one, so the running firmware is never
overwritten. `nv getenv sw_commit`, then `nv setenv sw_commit`/`sw_active` to
the other value and reboot.

**Licensing, if you are redistributing anything.** The RTL960x repository is
[The Unlicense](https://github.com/Anime4000/RTL960x/blob/main/LICENSE), so the
scripts, documentation and web GUI assets are public domain and free to use.
That does **not** extend to the firmware images or modified rootfs trees in the
same repository: those are derived from proprietary ODI/Realtek firmware, and a
repository licence cannot relicense someone else's binaries. The bundled
`mksquashfs` is GPL squashfs-tools. So: use the tooling freely, build your own
image from a stock one you already have, and do not redistribute images.


### Prometheus

```yaml
scrape_configs:
  - job_name: gpon
    scrape_interval: 30s
    static_configs:
      - targets: ['192.168.1.1:9100']
```

## Porting to another stick

Two things differ between devices, and both fail quietly.

**The instruction set.** `scripts/isa-audit.sh` grades a binary against a list
of mnemonics confirmed present on the RLX5281 *by executing them*, and reports
everything else as UNVERIFIED. That list is not transferable: another RTL960x
core may implement more or fewer. Do not assume an ISA level — on this one,
SPECIAL2 and MIPS-II are each implemented in pieces (`madd` and `bltzl` run;
`mul`, `clz`, `teq`, `beql` and `bnel` trap), so neither name means anything as
a unit.

The way to extend the list is to execute one instruction per invocation, so a
SIGILL ends only that test rather than the whole run, and to grow the list only
from what actually ran. A census that lists the instructions you *suspect* are
missing will pass a binary that dies on the one you did not think of.

**The diag command names.** They differ between firmware revisions, and the CLI
grammar uses hyphens where the internal symbol names use underscores
(`alarm-status`, not `alarm_status`):

```sh
strings /bin/diag | grep cparser_cmd_
```

That lists the command tree. Every command this exporter runs is in
`src/metrics_body.h`, in one place.

## Layout

```
src/metricsd.c        the exporter: socket, accept loop, HTTP response
src/metrics_body.h    the metrics themselves; every metric name lives here
src/syscall.h         o32 syscall layer, file reads, fork/exec, decimals
src/start.S           _start and a 6-argument syscall stub for setsockopt
scripts/verify.sh     asserts ELF32 / big endian / MIPS / static / no INTERP
scripts/isa-audit.sh  instruction census against what the RLX5281 implements
scripts/deploy.sh     push a file to the stick over netcat
stick/exporter-up.sh  on-device start/stop
Dockerfile            the cross toolchain: gcc-mips-linux-gnu + qemu-user
Makefile              every target above; re-enters itself with IN_CONTAINER=1
.github/workflows/release.yml   build + gate on every push, publish on v* tags
```

## Implementation notes

A few decisions that are not obvious from the code.

**`diag` output is captured through a pipe, not a temp file.** The file version
needed a writable directory, and on a flashed image nothing created it, so every
optical metric silently vanished while the `/proc` ones kept working. Note
`pipe2` rather than `pipe`: on MIPS the raw `pipe(2)` returns the second
descriptor in `$v1` instead of through the pointer. The parent drains before
`waitpid` — the other order deadlocks once a child outgrows the pipe buffer.

**Values are emitted as the literal text `diag` printed**, never parsed to a
number and back. That avoids float formatting — which would pull in a libc, and
soft-float behind it — and cannot introduce a rounding difference between what
the device reports and what is scraped. `/proc/uptime` gets the same treatment.

**The counter parser is generic**: any line whose text after the colon is
nothing but digits becomes a series. That rejects banners, separators and the
`RTK.0> command:` prompt without special-casing any of them.

**`/proc/net/dev` is emitted metric-major**, one pass over the buffer per
metric. The natural interface-major loop scatters each metric family across the
output, which the exposition format forbids.

**Three flag groups are load-bearing** and fail quietly if wrong:
`-EB` (big-endian; `mips-`, never `mipsel-`), `-G0 -fno-pic -mno-abicalls`
(a hand-written `_start` never sets up `$gp`, so any gp-relative or GOT
reference faults), and `-msoft-float` (RLX cores have no FPU).

**MIPS diverges from the generic ABI in three families of constants**, all
silent when wrong: `SOCK_STREAM` is 2 and `SOCK_DGRAM` is 1 (swapped),
`SOL_SOCKET` is 65535, and `O_CREAT` is 0x100. The CPU is big-endian, so host
order already is network order — there is no `htons` anywhere.

## Licence

[MIT](LICENSE).

One piece is not original: the inline-asm formulation of `syscall3` in
`src/syscall.h` — loading `$v0` inside the asm block, and the clobber list —
follows [musl](https://musl.libc.org/)'s MIPS `syscall_arch.h`. musl is MIT
licensed, so the terms match; it is called out here and in the source rather
than absorbed silently. Nothing else derives from another project, and nothing
derives from vendor firmware or from any GPL source.

This covers the exporter only. The **firmware images** it can be installed into
are proprietary ODI/Realtek binaries that no licence here reaches — build your
own from a stock image you already have, rather than redistributing one.

## Acknowledgements

[Anime4000/RTL960x](https://github.com/Anime4000/RTL960x) for the netcat
transfer method and firmware modding documentation, and
[tripleoxygen/realtek-libohwtc](https://github.com/tripleoxygen/realtek-libohwtc)
for demonstrating that a stock toolchain can build working code for these
devices.
