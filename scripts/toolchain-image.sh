#!/bin/sh
#
# Print the toolchain image to build in, pulling it first if it is not here.
#
#     IMAGE=$(scripts/toolchain-image.sh)
#
# The image is the freestanding toolchain from the odi-toolchain repository
# (https://github.com/AndreZiviani/odi-toolchain): Debian gcc-mips-linux-gnu,
# qemu-mips-static, and the shared ISA audit (isa-audit, isa-allowlist).
# toolchain.env pins it by digest -- the one place that names it.
#
# TOOLCHAIN_IMAGE overrides the pin, for an image built locally:
#
#     make -C odi-toolchain freestanding    # tags odi-toolchain-freestanding:local
#     TOOLCHAIN_IMAGE=odi-toolchain-freestanding:local make
#
# A pinned reference is pulled on first use. An override that is not a
# registry reference must already exist locally; it is never pulled.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=../toolchain.env
. "$ROOT/toolchain.env"
ref=${TOOLCHAIN_IMAGE:-$TOOLCHAIN_IMAGE_PINNED}

if docker image inspect "$ref" >/dev/null 2>&1; then
	echo "$ref"
	exit 0
fi

fallback() {
	cat >&2 <<MSG
To build the image locally instead, from the odi-toolchain repository:

    git clone https://github.com/AndreZiviani/odi-toolchain
    make -C odi-toolchain freestanding     # tags odi-toolchain-freestanding:local
    export TOOLCHAIN_IMAGE=odi-toolchain-freestanding:local
MSG
}

case $ref in
*/*@sha256:*|*/*:*) ;;
*)
	echo "toolchain-image.sh: no local image $ref (TOOLCHAIN_IMAGE)" >&2
	fallback
	exit 1 ;;
esac

echo "pulling $ref" >&2
if ! docker pull -q "$ref" >&2; then
	cat >&2 <<MSG

toolchain-image.sh: could not pull $ref

If the package is still private, log in to ghcr.io first with a GitHub
token that has read:packages (once it is public no login is needed):

    echo "\$TOKEN" | docker login ghcr.io -u <github user> --password-stdin

MSG
	fallback
	exit 1
fi
echo "$ref"
