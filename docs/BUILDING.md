# Building

Everything compiles in a container; the host needs Docker and `make`.

    make            # build, verify and audit -- the usual case
    make release    # everything CI does, including SHA256SUMS

## The toolchain image

The container is the **freestanding toolchain image** from the
odi-toolchain repository (<https://github.com/AndreZiviani/odi-toolchain>),
shared with the other projects for this stick: Debian `gcc-mips-linux-gnu`,
`qemu-mips-static`, and the shared ISA audit (`isa-audit`,
`isa-allowlist`). `toolchain.env` pins it **by digest** -- the one place that
names it -- and `make image` (a dependency of every target that needs it)
pulls it on first use through `scripts/toolchain-image.sh`. It is published
for `linux/amd64` and `linux/arm64`.

The package is public, so no login or token is needed: `make image` pulls it
anonymously. A GitHub token is only useful as an optional way to raise
anonymous-pull rate limits; it is never required.

A failed pull prints the fallback below.

## Building the image locally instead

    git clone https://github.com/AndreZiviani/odi-toolchain
    make -C odi-toolchain freestanding          # tags odi-toolchain-freestanding:local
    TOOLCHAIN_IMAGE=odi-toolchain-freestanding:local make

A local tag is used as is and never pulled. The binary it builds is the same
as with the pinned image: v1.0.3 rebuilt with it is byte-for-byte the
released `metricsd`.

## Moving the pin

Tag a new image version in odi-toolchain, take the digest from the summary
of its publish run, change `toolchain.env`, and rebuild. The binary should
come out identical to the one the old pin produced; if it does not, every
difference needs a reason before the change merges.

## CI

`.github/workflows/release.yml` pulls the pinned image anonymously — no
login step. Every push and PR builds and gates the binary (`make httpd`,
`make verify`, `make isa`, `make sums`), so a tag can never fail on something
an ordinary commit would have caught.

## Targets

    make            # build, verify and audit — the usual case
    make httpd      # the exporter -> build/metricsd
    make verify     # assert the ELF shape the stick can actually load
    make isa        # the ISA gate: isa-audit and isa-allowlist, from the toolchain image
    make run        # execute locally under qemu-user
    make release    # everything CI does, including SHA256SUMS
    make shell      # a shell in the toolchain container
    make clean

`make run` uses qemu-user. It proves logic and syscalls but **not instruction
legality** — qemu emulates a full MIPS32 CPU and will happily execute the `mul`
and `clz` that trap on real hardware. `make isa` is the gate for that.

`make isa` fails when the binary contains an instruction known to trap, or any
floating point. A mnemonic that has never been executed on a real device is
printed as UNVERIFIED without failing the target; CI turns that into a warning.

## Releases

Tagging is the whole process:

    git tag -a v1.0.0 -m "first release"
    git push origin v1.0.0

`.github/workflows/release.yml` builds in the same container this repo uses
locally, runs `verify` and the ISA audit, and publishes the binaries with
generated release notes. Every push and pull request runs the identical build
and gates, so a tag cannot fail on something an ordinary commit would have
caught. Run `make release` first if you want the same answer without pushing.
