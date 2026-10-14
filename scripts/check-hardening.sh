#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Check that ELF files were built with the hardening flags from
# userpatches/hardening/build-flags.sh: stack protector, full RELRO, PIE,
# FORTIFY_SOURCE and a non-executable stack.
#
#   scripts/check-hardening.sh FILE|DIR|PACKAGE.deb ...
#   scripts/check-hardening.sh userpatches/multimedia/*.deb userpatches/hailo/*.deb
#
# Prints one line per ELF file and exits 1 if any check fails. FORTIFY is
# only reported (n/a) when the file calls none of the functions it covers.
set -euo pipefail

READELF="${READELF:-readelf}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
fails=0

check() { # elf file, label
	local f="$1" label="$2" hdr dyn syms canary relro pie nx fortify
	hdr="$(LC_ALL=C "${READELF}" -lhW "$f" 2>/dev/null)" || return 0
	grep -q 'Class:.*ELF' <<< "${hdr}" || return 0
	dyn="$(LC_ALL=C "${READELF}" -dW "$f" 2>/dev/null || true)"
	syms="$(LC_ALL=C "${READELF}" -sW --dyn-syms "$f" 2>/dev/null || true)"

	canary=no; grep -q '__stack_chk_fail\|__stack_chk_guard' <<< "${syms}" && canary=yes
	relro=no
	if grep -q GNU_RELRO <<< "${hdr}"; then
		relro=partial
		grep -q 'BIND_NOW\|FLAGS_1.*NOW' <<< "${dyn}" && relro=full
	fi
	pie=no
	if grep -q 'Type:.*DYN' <<< "${hdr}"; then
		# Shared libraries are position independent by definition.
		if grep -q 'FLAGS_1.*PIE' <<< "${dyn}" || [[ "$f" == *.so* ]]; then pie=yes; fi
	fi
	nx=yes; grep -A1 GNU_STACK <<< "${hdr}" | grep -q 'RWE' && nx=no
	fortify=n/a; grep -q '__[a-z_]*_chk' <<< "${syms}" && fortify=yes

	local bad=""
	[[ "${canary}" == yes ]] || bad+=" canary"
	[[ "${relro}" == full ]] || bad+=" relro"
	[[ "${pie}" == yes ]] || bad+=" pie"
	[[ "${nx}" == yes ]] || bad+=" nx"
	if [[ -n "${bad}" ]]; then
		fails=$((fails + 1))
		printf 'FAIL %-60s missing:%s\n' "${label}" "${bad}"
	else
		printf 'PASS %-60s fortify=%s\n' "${label}" "${fortify}"
	fi
}

scan() { # dir, label prefix
	local f
	while IFS= read -r -d '' f; do
		check "$f" "$2${f#"$1"}"
	done < <(find "$1" -type f -print0 | sort -z)
}

[[ $# -gt 0 ]] || { sed -n '3,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
for arg in "$@"; do
	if [[ "${arg}" == *.deb ]]; then
		d="${TMP}/$(basename "${arg}" .deb)"
		mkdir -p "$d"
		dpkg-deb -x "${arg}" "$d"
		scan "$d" "$(basename "${arg}"):"
	elif [[ -d "${arg}" ]]; then
		scan "${arg}" "${arg}"
	else
		check "${arg}" "${arg}"
	fi
done
[[ "${fails}" -eq 0 ]]
