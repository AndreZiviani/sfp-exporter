# sfp-exporter — Prometheus exporter for RTL9601-based GPON SFP ONU sticks.
#
# Host targets shell out to containers holding each toolchain; the C build
# re-enters this Makefile there with IN_CONTAINER=1.

IMAGE   := sfp-exporter-toolchain
BUILD   := build

# Stick connection, for `make deploy` / `make pull`. IP is deliberately unset so
# a bare `make deploy` only prints the recipe instead of talking to the network.
# The stick cannot open connections to us, so it listens and this host
# connects.
IP        ?=
PORT      ?= 12345

# Which binary the one-off targets act on.
BIN  ?= $(BUILD)/metricsd

# Baked into the binary and exported as gpon_exporter_build_info. It exists
# because the exporter can be updated WITHOUT reflashing — rc35 prefers
# /etc/config/metricsd, on the jffs2 config partition, over the /bin/metricsd
# in the image — so an override can outlive the image it was built against with
# nothing in the metrics to say so. Computed on the host: the toolchain
# container has no git and no repo history.
BUILD_ID ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

ifeq ($(IN_CONTAINER),1)

# ---------------------------------------------------------------- in container

CROSS   := mips-linux-gnu-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

# -march=mips1     RLX5281 is MIPS-I class
# -EB              big-endian, per CONFIG_CPU_BIG_ENDIAN in Realtek's 9601b config
# -msoft-float     RLX cores have no FPU
# -G0              no gp-relative small data — _start never sets up $gp
# -fno-pic
# -mno-abicalls    plain static ELF, no GOT
# -ffreestanding   no libc, and no turning our loops back into libc calls
CFLAGS  := -std=c99 -Os -Wall -Wextra \
           -march=mips1 -mabi=32 -EB -msoft-float -G0 \
           -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector

# Passed in from the host target; the header defaults it if absent so a bare
# in-container build still compiles.
BUILD_ID ?=
ifneq ($(BUILD_ID),)
CFLAGS  += -DBUILD_ID='"$(BUILD_ID)"'
endif

LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none

HDRS := src/syscall.h src/metrics_body.h

# BUILD_ID is compiled in, but it is a make VARIABLE -- make cannot see it
# change, so with the sources untouched it will not rebuild and the binary keeps
# whatever stamp it was last compiled with.
#
# That shipped: an image was built whose manifest said exporter
# v1.0.1-7-gb4d4e9f while /bin/metricsd inside it reported
# v1.0.1-6-g60083e9-dirty. The code was current; only the stamp was stale, and
# the manifest -- whose entire job is saying what is on the stick -- was wrong.
# gpon_exporter_build_info caught it on the first boot after flashing, which is
# exactly why that metric exists.
#
# Park the value in a file and depend on the file. FORCE makes the recipe run
# every time; `cmp` means the file is only rewritten, and the mtime only moves,
# when the value actually differs.
.PHONY: FORCE
$(BUILD)/.build-id: FORCE | $(BUILD)
	@printf '%s' '$(BUILD_ID)' | cmp -s - $@ 2>/dev/null || printf '%s' '$(BUILD_ID)' > $@

$(BUILD)/metricsd: src/start.S src/metricsd.c $(HDRS) $(BUILD)/.build-id | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.S %.c,$^)
	$(STRIP) $@

$(BUILD):
	mkdir -p $@

else

# --------------------------------------------------------------------- on host

RUN   := docker run --rm -v "$(CURDIR)":/src -w /src $(IMAGE)

.PHONY: all httpd image verify isa run deploy shell clean release sums

# What you almost always want: the exporter, checked.
all: httpd verify isa

image:
	docker build -q -t $(IMAGE) .

## --- targets -----------------------------------------------------------------

# The Prometheus exporter, standalone HTTP server. This is the one that works
# on the ODI stick — Realtek's boa has no external-CGI path.
httpd: image
	$(RUN) make IN_CONTAINER=1 BUILD_ID='$(BUILD_ID)' $(BUILD)/metricsd

## --- inspection --------------------------------------------------------------

verify: image
	$(RUN) scripts/verify.sh $(BIN)

# Instruction census against what the RLX5281 is known to implement.
isa: image
	$(RUN) scripts/isa-audit.sh $(BIN)

# Run a built binary under qemu-user. Proves logic and syscalls; does NOT prove
# instruction legality — qemu emulates full MIPS32 and will happily execute the
# `mul` and `clz` that trap on real hardware. Use `make isa` for that.
run: image
	@$(RUN) qemu-mips-static $(BIN) $(ARGS)

## --- moving files ------------------------------------------------------------

# make deploy BIN=build/metricsd IP=192.168.1.1
deploy:
	@scripts/deploy.sh $(BIN) $(if $(IP),$(IP) $(PORT))

# Exactly what the release workflow runs, so a tag cannot fail on something you
# could have caught locally.
release: image
	$(RUN) make IN_CONTAINER=1 $(BUILD)/metricsd
	$(RUN) scripts/verify.sh $(BUILD)/metricsd
	$(RUN) scripts/isa-audit.sh $(BUILD)/metricsd
	$(MAKE) sums

# Written inside the container, not on the host. build/ is created by the
# container as root, so on Linux — every CI runner — the host user cannot write
# into it. On macOS Docker maps ownership and hides the problem, which is
# exactly how this reached CI.
sums: image
	$(RUN) sh -c 'cd $(BUILD) && sha256sum metricsd > SHA256SUMS && cat SHA256SUMS'

shell: image
	docker run --rm -it -v "$(CURDIR)":/src -w /src $(IMAGE) bash

clean:
	rm -rf $(BUILD)

endif
