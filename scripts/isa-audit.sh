#!/usr/bin/env bash
#
# Instruction census for a MIPS binary, graded against what the RLX5281 in the
# ODI stick is *known* to implement.
#
# The logic is deliberately inverted. An earlier version listed the instructions
# suspected of being absent and reported those; it passed a dropbear build that
# then died with SIGILL on `teq`, because `teq` was not on the list. A check that
# only looks for problems you already thought of cannot find a new one.
#
# So: everything not on the confirmed-present list is reported as UNVERIFIED.
# The list only grows by executing an instruction on real hardware — one per
# invocation, so a SIGILL ends only that test — never by reasoning about which
# ISA level the core claims.

set -euo pipefail

BIN="${1:?usage: isa-audit.sh <binary>}"
CROSS="${CROSS:-mips-linux-gnu-}"

# Confirmed present by execution on the device.
PRESENT="
abs add addi addiu addu and andi b bal beq beqz bgez bgezal bgtz blez bltz
bltzl bne bnez break div divu j jal jalr jr lb lbu lh lhu li lui lw lwl lwr
ll sc sync mfhi mflo mthi mtlo move movz movn madd mult multu neg negu nop nor
not or ori sb sh sll sllv slt slti sltiu sltu sra srav srl srlv sub subu sw
swl swr syscall xor xori seq sne sgt sge sle .word
"

# Confirmed ABSENT — these raise SIGILL. beqzl/bnezl are assembler aliases for
# beql/bnel against $zero, so they are the same instruction and equally fatal;
# counting only "beql" and "bnel" under-reported this binary by 62.
ABSENT="mul clz teq beql bnel beqzl bnezl"

# The lists above are written across lines for readability; collapse the
# whitespace so the space-delimited membership test below actually matches.
PRESENT=" $(printf '%s' "$PRESENT" | tr -s '[:space:]' ' ') "
ABSENT=" $(printf '%s' "$ABSENT" | tr -s '[:space:]' ' ') "

freq=$(mktemp); trap 'rm -f "$freq"' EXIT

# -m mips:isa32 is not optional: these binaries are flagged mips1, under which
# objdump decodes SPECIAL2 and movz/movn as ".word" and the census reports zero
# for exactly what you are looking for.
"${CROSS}objdump" -d -m mips:isa32 "$BIN" 2>/dev/null \
	| awk -F'\t' 'NF>=3 { gsub(/ /, "", $3); print $3 }' \
	| sort | uniq -c > "$freq"

total=$(awk '{s+=$1} END {print s+0}' "$freq")
echo "$BIN — $total instructions, $(wc -l < "$freq" | tr -d ' ') distinct mnemonics"

fail=0
echo
echo "CONFIRMED ABSENT on RLX5281 — any hit is fatal:"
for i in $ABSENT; do
	n=$(awk -v m="$i" '$2==m {print $1}' "$freq" | head -1)
	if [ -n "${n:-}" ]; then printf '  %-8s %8s   TRAPS\n' "$i" "$n"; fail=1
	else printf '  %-8s %8s\n' "$i" 0; fi
done

echo
echo "UNVERIFIED — present in this binary, never executed on the hardware:"
unver=0
while read -r n m; do
	case "$PRESENT$ABSENT" in
	*" $m "*) continue ;;
	esac
	printf '  %-10s %8s\n' "$m" "$n"
	unver=$((unver + 1))
done < "$freq"
[ "$unver" = 0 ] && echo "  (none)"

fp=$(awk '$2 ~ /\.[sd]$/ {s+=$1} END {print s+0}' "$freq")
echo
printf 'Floating point (RLX has no FPU): %s\n' "$fp"
[ "$fp" != 0 ] && fail=1

echo
if [ "$fail" != 0 ]; then
	echo "FAIL: contains instructions this CPU does not implement"
	exit 1
fi
if [ "$unver" != 0 ]; then
	echo "WARNING: $unver unverified mnemonic(s) — execute each on the device"
	echo "and add it to PRESENT above before trusting this binary."
	exit 2
fi
echo "OK: every instruction is confirmed present on the hardware"
