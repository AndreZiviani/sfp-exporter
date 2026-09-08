#!/usr/bin/env bash
#
# Assert that a built binary has the shape the ODI stick can actually run.
# Cheaper than flashing it and staring at a dead telnet session.

set -euo pipefail

BIN="${1:?usage: verify.sh <binary>}"
CROSS="${CROSS:-mips-linux-gnu-}"
fail=0

check() {
	local label="$1" expected="$2" actual="$3"
	if [[ "$actual" == *"$expected"* ]]; then
		printf '  ok    %-22s %s\n' "$label" "$actual"
	else
		printf '  FAIL  %-22s got %q, want %q\n' "$label" "$actual" "$expected"
		fail=1
	fi
}

header=$("${CROSS}readelf" -h "$BIN")
# Trim with sed, not xargs: readelf prints "2's complement, big endian" and
# the apostrophe makes xargs choke on an unmatched quote.
get() { echo "$header" | grep -m1 "$1" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'; }

echo "verifying $BIN"

check "class"        "ELF32"                        "$(get '^  Class')"
check "endianness"   "big endian"                   "$(get '^  Data')"
check "machine"      "MIPS"                         "$(get '^  Machine')"
check "type"         "EXEC"                         "$(get '^  Type')"

# A dynamic binary would need the stick's 2009 uClibc loader at a path we have
# not verified. Freestanding means neither an interpreter nor a .dynamic.
if "${CROSS}readelf" -l "$BIN" | grep -q INTERP; then
	printf '  FAIL  %-22s has a PT_INTERP segment\n' "interpreter"
	fail=1
else
	printf '  ok    %-22s none (static)\n' "interpreter"
fi

if "${CROSS}readelf" -d "$BIN" 2>/dev/null | grep -q NEEDED; then
	printf '  FAIL  %-22s links against shared libraries\n' "shared deps"
	fail=1
else
	printf '  ok    %-22s none\n' "shared deps"
fi

# RLX5281 has unaligned load/store (CONFIG_CPU_HAS_ULS=y), so these are legal
# here. Reported anyway: on an older LX4180/LX5280 core they would be fatal.
uls=$("${CROSS}objdump" -d "$BIN" | grep -cE '\b(lwl|lwr|swl|swr)\b' || true)
printf '  info  %-22s %s (legal on RLX5281)\n' "unaligned ld/st" "$uls"

printf '  info  %-22s %s bytes\n' "size" "$(wc -c < "$BIN" | tr -d ' ')"

if (( fail )); then
	echo "FAILED"
	exit 1
fi
echo "all checks passed"
