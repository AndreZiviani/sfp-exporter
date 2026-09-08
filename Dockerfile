# Cross-toolchain for the ODI DFP-34X-2C2 (RTL9601D / RLX5281, big-endian MIPS).
#
# Debian's mips-linux-gnu targets big-endian MIPS o32 — the right endianness and
# ABI for this stick. We only ever use it to build freestanding binaries, so the
# fact that its libc is glibc (far too new for the stick's Linux 2.6.30.9) never
# comes into play. See README.md.
#
# qemu-user-static runs the results here. It emulates a full MIPS32 CPU, so it
# proves logic and syscalls but NOT instruction legality — `mul` and `clz` run
# fine under qemu and trap on the real RLX5281. `make isa` remains the ISA gate.
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc-mips-linux-gnu \
      binutils-mips-linux-gnu \
      qemu-user-static \
      make file xxd \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
