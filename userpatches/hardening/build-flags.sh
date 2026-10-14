# SPDX-License-Identifier: MIT
# shellcheck shell=bash disable=SC2034
# Compiler and linker hardening flags for everything rk3566-os builds for the
# board (the usual Debian/Ubuntu set). Sourced by scripts/*.sh and by the
# board config; see docs/security.md.
#
# -mbranch-protection=standard: PAC/BTI instructions are NOPs on the RK3566's
# Cortex-A55 and take effect on newer cores.
KRAY_HARDEN_CFLAGS="-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE -mbranch-protection=standard"
KRAY_HARDEN_LDFLAGS="-pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack"
# Shared libraries: same compile flags with -fPIC, no -pie.
KRAY_HARDEN_LIB_CFLAGS="${KRAY_HARDEN_CFLAGS/-fPIE/-fPIC}"
KRAY_HARDEN_LIB_LDFLAGS="-Wl,-z,relro,-z,now -Wl,-z,noexecstack"
