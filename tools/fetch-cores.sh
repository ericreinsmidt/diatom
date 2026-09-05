#!/bin/sh
# Verifies the cores Diatom's measurements were made against, fetching any that
# are missing, from libretro's own buildbot.
#
#   tools/fetch-cores.sh                 the whole test matrix
#   tools/fetch-cores.sh gambatte mgba   just these
#
# Why this exists, and why it records rather than vendors:
#
# Cores are THIRD-PARTY binaries that Diatom does not ship, build or depend on
# (see docs/working-agreement.md). Diatom has no core list. But the test matrix
# is evidence - the geometry, envelope and pacing claims in docs/ rest on
# specific binaries - and evidence whose provenance is unstated cannot be
# re-checked. So the bytes stay out of the repository and the manifest goes in,
# the same trade ADR-0012 made for the sysroot.
#
# The buildbot publishes to latest/, which is UNPINNED. That is the exact drift
# ADR-0012 rejected `:latest` for, and it cannot be fixed from this end: the
# buildbot keeps no archive to pin to. What can be fixed is knowing what was
# actually used, which is what CORES.md is for. A core's own reported version
# string, logged whenever Diatom runs it, usually carries an upstream git hash
# and is the stronger record of the two.
#
# THIS SCRIPT VERIFIES. IT DOES NOT DISCOVER.
#
# Until 2026-09-05 it did the opposite: it truncated CORES.md, fetched whatever
# the buildbot was serving, and wrote the hash it had just computed. Nothing
# compared that against the hash already in the file, so a republished core
# silently replaced the pin and the run printed `ok` - in a file whose own text,
# which this script generates, says "these hashes are the pin". Three ways to
# lose a pin without being told, all from the same design:
#
#   - a republished core overwrote its hash;
#   - a partial run (`tools/fetch-cores.sh mgba`) rewrote the manifest with one
#     row, dropping the other four;
#   - a failed fetch `continue`d past the manifest write, dropping that row, and
#     the run still exited 0.
#
# TortOS's mk/fetch-vendor.sh met the same event correctly, because its hashes
# are literals in the script and a mismatch stops it. That is the shape adopted
# here. The pin is the table below; CORES.md is generated FROM that table rather
# than from whatever came down the wire, so the two cannot drift and no run can
# narrow the file.
set -eu

ARCH=${ARCH:-linux/aarch64}
BASE="https://buildbot.libretro.com/nightly/$ARCH/latest"
# Licenses are pinned below with the hashes, and re-checked on every run against
# libretro's own core-info repo, which is where the `license` field a frontend
# would display is maintained. Pinned rather than simply recorded: the question
# "which of these are GPL" sat open in the register for days, and an answer that
# a network blip can turn into "UNKNOWN" is not an answer.
INFO="https://raw.githubusercontent.com/libretro/libretro-core-info/master"
OUT=${OUT:-cores}
MANIFEST=CORES.md

# THE PIN, and the date it was established. Nothing here changes on its own.
#
# The five cores cover all nine in-scope systems and actually run on the device.
# snes9x2010 rather than mainline snes9x: mainline is C++ and wants
# GLIBCXX_3.4.29 (see below), while 2010 is pure C, holds frame rate, and is the
# only light fork that reports the correct 50.0070 PAL rate - 2002 and 2005 say
# 50.3197, which runs PAL content 0.62% fast. genesis_plus_gx rather than
# picodrive: see docs/spikes/2026-08-25-sega-core-comparison.md. No gambatte:
# mGBA covers GB and GBC identically, from a smaller binary, and gambatte is C++
# so it cannot run here anyway.
#
# To adopt a republished core: decide to, rather than letting a fetch decide for
# you. Then change the line here AND in TortOS's mk/fetch-vendor.sh together,
# and re-measure every row in docs/reference/core-facts.md that used it, because
# each of those numbers was made against the old bytes.
#
# Fields: name, sha256, bytes, license. License is last so it may contain
# spaces.
PINNED_ON=2026-08-26
pins() {
	cat <<-'EOF'
	fceumm            1b13b00d4680394dad8000d5175f97be727107e0945bc9b412da91d70c07b267 4451672 GPLv2
	snes9x2010        3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c 2859704 Non-commercial
	mgba              abde7a0764f08fa0cc2c7d3d9a29b9d1245a9f3b7df0e7a594b74df642ee53c6 3280408 MPLv2.0
	genesis_plus_gx   3673a22b906509461e23a5a118b1d1bec15cbda105f260cbcbc08a16b2124e48 12589216 Non-commercial
	mednafen_pce_fast aca90a14b18108c86398da2267ef40d5145eaddbc1c1b310614d745b258552b1 4465432 GPLv2
	EOF
}

CORES=${*:-$(pins | awk '{print $1}')}

command -v curl   >/dev/null || { echo "fetch-cores: need curl" >&2; exit 1; }
command -v unzip  >/dev/null || { echo "fetch-cores: need unzip" >&2; exit 1; }
command -v shasum >/dev/null || { echo "fetch-cores: need shasum" >&2; exit 1; }

mkdir -p "$OUT"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pin_field() { pins | awk -v c="$1" -v n="$2" '$1 == c { if (n == 4) { $1=""; $2=""; $3=""; sub(/^ +/, ""); print } else print $n }'; }

warned=0

for c in $CORES; do
	want=$(pin_field "$c" 2)
	if [ -z "$want" ]; then
		echo "fetch-cores: \`$c\` is not pinned in this script." >&2
		echo "  The pinned set is: $(pins | awk '{print $1}' | tr '\n' ' ')" >&2
		echo "  Adding a core is a decision, not a fetch. Pin it above first." >&2
		exit 1
	fi
	want_sz=$(pin_field "$c" 3)
	want_lic=$(pin_field "$c" 4)
	so="$OUT/${c}_libretro.so"

	printf 'checking %-22s ' "$c"

	# An already-present, already-matching core needs no network. This is what
	# makes a re-run cheap and lets the check work offline.
	if [ -f "$so" ] && [ "$(shasum -a 256 "$so" | cut -d' ' -f1)" = "$want" ]; then
		echo "ok, matches the pin"
		continue
	fi

	z="$tmp/$c.zip"
	# A failed fetch is fatal. It used to `continue`, which dropped that core's
	# row from the manifest and still exited 0.
	if ! curl -sSfL --max-time 120 -o "$z" "$BASE/${c}_libretro.so.zip"; then
		echo "FAILED"
		echo "fetch-cores: could not fetch $c from $BASE" >&2
		exit 1
	fi
	unzip -oq "$z" -d "$OUT"

	got=$(shasum -a 256 "$so" | cut -d' ' -f1)
	if [ "$got" != "$want" ]; then
		echo "MISMATCH"
		echo "fetch-cores: $c does not match the pin" >&2
		echo "    want $want" >&2
		echo "    got  $got" >&2
		echo "  The buildbot republished this core. Verify it, then update the" >&2
		echo "  hash here and in TortOS's mk/fetch-vendor.sh together, and" >&2
		echo "  re-measure the core-facts.md rows that used it." >&2
		exit 1
	fi

	got_sz=$(wc -c < "$so" | tr -d ' ')
	if [ "$got_sz" != "$want_sz" ]; then
		# Unreachable from a republished core, since the hash already matched.
		# Reachable from a typo in the table above, which is the point.
		echo "MISMATCH"
		echo "fetch-cores: $c matched its hash but not its byte count" >&2
		echo "    want $want_sz, got $got_sz - the pin above is mistyped" >&2
		exit 1
	fi

	echo "fetched, matches the pin"

	# The bytes are already verified, so the license cannot have changed with
	# them. A difference here means libretro corrected their metadata about the
	# same binary, which is worth saying and is not worth stopping for.
	lic=$(curl -sSfL --max-time 60 "$INFO/${c}_libretro.info" 2>/dev/null \
	      | sed -n 's/^license *= *"\(.*\)"/\1/p' | head -1)
	if [ -z "$lic" ]; then
		echo "  note: could not reach core-info; license stays as pinned ($want_lic)" >&2
		warned=1
	elif [ "$lic" != "$want_lic" ]; then
		echo "  NOTE: core-info now says \"$lic\" for $c, pinned as \"$want_lic\"." >&2
		echo "        Same bytes, so this is a metadata correction upstream." >&2
		echo "        Check it against ADR-0023 and update the pin if it stands." >&2
		warned=1
	fi
done

# Written from the pin, not from the loop above, so a partial run cannot narrow
# it and a skipped core cannot vanish from it.
{
	echo "# Cores the test matrix was verified against"
	echo
	echo "Pinned $PINNED_ON. Generated from the table in"
	echo "\`tools/fetch-cores.sh\`, which is where the pin lives; this file is the"
	echo "readable copy of it, and the script fails rather than rewriting it."
	echo
	echo "**Not part of Diatom.** These are third-party binaries; Diatom neither"
	echo "ships nor depends on them, and loads whatever core it is handed. This file"
	echo "exists so the measurements in \`docs/\` name the exact bytes that produced"
	echo "them. The binaries themselves are gitignored."
	echo
	echo "Source: \`$BASE\` - libretro's own buildbot. The path is **unpinned**;"
	echo "these hashes are the pin."
	echo
	echo "Licenses are pinned with the hashes and re-checked on every run against"
	echo "libretro's core-info. The set is **not uniformly GPL** and the"
	echo "differences matter to anyone shipping an image - see"
	echo "[ADR-0023](docs/decisions/0023-core-licensing.md)."
	echo
	echo "| Core | License | Bytes | sha256 |"
	echo "|---|---|---|---|"
	pins | while read -r name sha sz lic; do
		printf '| `%s` | %s | %s | `%s` |\n' "$name" "$lic" "$sz" "$sha"
	done
	echo
	echo "Each core also reports its own name and version, which Diatom logs at"
	echo "load and which usually carries an upstream git hash. Those strings are"
	echo "recorded alongside the measurements that used them."
	echo
	echo "## C++ cores do not run on the Brick as fetched"
	echo
	echo "Measured 2026-08-24/25. \`snes9x\` (mainline), \`gambatte\`, \`mesen-s\` and"
	echo "\`mednafen_supafaust\` all fail to \`dlopen\` with"
	echo "\`GLIBCXX_3.4.29 not found\`. The buildbot builds C++ cores against GCC 11;"
	echo "the Brick ships \`libstdc++.so.6.0.28\`, which tops out at \`GLIBCXX_3.4.28\`."
	echo "One version short."
	echo
	echo "Checking \`GLIBC_\` alone does not catch this - the C++ runtime versions"
	echo "its symbols separately as \`GLIBCXX_\`, and a core can be clean on one and"
	echo "fail on the other."
	echo
	echo "The fix is already to hand: the toolchain container from ADR-0012 has"
	echo "GCC 10.2.1, whose cross \`libstdc++.so.6.0.28\` provides exactly"
	echo "\`GLIBCXX_3.4.28\` - the same version the device ships. A C++ core built"
	echo "there runs unmodified, with provenance pinned to an upstream commit"
	echo "rather than to a nightly. Bundling a newer \`libstdc++\` beside the cores"
	echo "would also work and is what other firmwares do."
	echo
	echo "C cores - fceumm, picodrive, genesis_plus_gx, mgba, mednafen_pce_fast -"
	echo "are unaffected and load as fetched."
} > "$MANIFEST"

echo
if [ "$warned" -eq 1 ]; then
	echo "every core matched its pinned hash; see the notes above"
else
	echo "every core matched its pinned hash"
fi
