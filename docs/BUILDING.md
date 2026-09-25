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

Once the package is public, no login is needed: the pull is anonymous.

**While it is private**, log in once with a GitHub token that has
`read:packages`, and keep the token out of the repository and out of shared
shell histories:

    echo "$TOKEN" | docker login ghcr.io -u <github user> --password-stdin

A failed pull says this and prints the fallback below.

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

`.github/workflows/release.yml` logs in to ghcr.io with the workflow
`GITHUB_TOKEN` and pulls the pinned image. While the package is private,
that works only once the package grants this repository read access (on
GitHub: the package settings, "Manage Actions access", add this repository
with the Read role). Once the package is public the login step is
unnecessary and harmless.
