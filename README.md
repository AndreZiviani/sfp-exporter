# odi-sfp-exporter

[![build](https://github.com/AndreZiviani/odi-sfp-exporter/actions/workflows/release.yml/badge.svg)](https://github.com/AndreZiviani/odi-sfp-exporter/actions/workflows/release.yml)
[![license: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)

A Prometheus exporter that runs **on** an RTL9601-based GPON SFP ONU stick,
rather than scraping one from outside. It is a single 8 KB static binary with
no dependencies — no libc, no shell, no web server — that serves the
exposition format on its own port and reads the optical diagnostics and
forwarding counters straight off the device.

Developed and confirmed on an **ODI DFP-34X-2C2** (RTL9601D). Other RTL960x
sticks are likely to work; see [Porting](docs/DESIGN.md#porting-to-another-stick).

```
gpon_rx_power_dbm -24.814861
gpon_tx_power_dbm 2.439562
gpon_temperature_celsius 35.582031
gpon_voltage_volts 3.162500
gpon_bias_current_ma 13.350000
gpon_onu_state 5
gpon_alarm{alarm="los"} 0
```

See [docs/METRICS.md](docs/METRICS.md) for the full metric reference and
[docs/DESIGN.md](docs/DESIGN.md) for why it is built this way.

Part of [odi-oss](https://github.com/AndreZiviani/odi-oss), the open firmware
project for the ODI DFP-34X-2C2 GPON stick.

## Get it

Every tagged release carries prebuilt static binaries and `exporter-up.sh`,
the start/stop helper — no toolchain needed:

```sh
curl -fsSLO https://github.com/AndreZiviani/odi-sfp-exporter/releases/latest/download/metricsd
curl -fsSLO https://github.com/AndreZiviani/odi-sfp-exporter/releases/latest/download/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
```

It also ships inside the odi-oss firmware image.

## Build and test

Requires Docker. The cross toolchain runs in a public, pinned container —
nothing installed on the host, no login or token needed. Works on x86-64 and
Apple Silicon.

```sh
make            # build, verify and audit — the usual case
make run        # run the built binary locally under qemu-user
make release    # everything CI does, including SHA256SUMS
```

See [docs/BUILDING.md](docs/BUILDING.md) for the toolchain image, all Make
targets, and the release/tagging process.

## Install on a stick

```sh
make deploy BIN=build/metricsd       IP=192.168.1.1
make deploy BIN=stick/exporter-up.sh IP=192.168.1.1
sh /tmp/exporter-up.sh 9100   # on the stick
```

Scrape `http://<stick>:9100/metrics`. This lands in ramfs and does not
survive a reboot — see [docs/DESIGN.md](docs/DESIGN.md#install) for a
permanent, reboot-surviving install and a Prometheus scrape config.

## Licence

GPL-2.0-or-later — see [LICENSE](LICENSE). One piece of bundled code (the
`syscall3` formulation in `src/syscall.h`, from musl) keeps its own MIT
attribution; see [docs/DESIGN.md](docs/DESIGN.md#licence) for that and for
the licensing terms around firmware images built with third-party tooling.

## Acknowledgements

[Anime4000/RTL960x](https://github.com/Anime4000/RTL960x) for the netcat
transfer method and firmware modding documentation, and
[tripleoxygen/realtek-libohwtc](https://github.com/tripleoxygen/realtek-libohwtc)
for demonstrating that a stock toolchain can build working code for these
devices.
