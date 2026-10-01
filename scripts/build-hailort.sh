#!/usr/bin/env bash
# Cross-compile HailoRT (libhailort + hailortcli) for arm64 Debian trixie and
# package it as userpatches/hailo/hailort_<version>_arm64.deb.
#
# The version must match the hailo_pci driver and the firmware staged by
# scripts/fetch-hailo.sh (run that first: it also fetches the HailoRT sources).
# HailoRT downloads its own build dependencies (protobuf, spdlog, ...) with
# CMake FetchContent and builds a host protoc for code generation.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ROOT}/.cache/hailo"
OUT="${ROOT}/userpatches/hailo"
SRC="${WORK}/hailort"
SYSROOT="${WORK}/sysroot"
BUILD="${WORK}/build"
STAGE="${WORK}/stage"
TRIPLET=aarch64-linux-gnu

HAILO_VERSION=4.24.0
PKG_VERSION="${PKG_VERSION:-${HAILO_VERSION}~kray1}"

log() { echo "==> $*"; }

[[ -d "${SRC}/.git" ]] || { echo "HailoRT sources missing: run scripts/fetch-hailo.sh" >&2; exit 1; }

if [[ ! -f "${SYSROOT}/usr/include/stdio.h" ]]; then
	log "Creating arm64 trixie sysroot"
	mmdebstrap --quiet --architectures=arm64 --variant=minbase --include=libc6-dev \
		trixie "${SYSROOT}" http://deb.debian.org/debian
fi

cat > "${WORK}/toolchain.cmake" <<-EOF
	set(CMAKE_SYSTEM_NAME Linux)
	set(CMAKE_SYSTEM_PROCESSOR aarch64)
	set(CMAKE_C_COMPILER ${TRIPLET}-gcc)
	set(CMAKE_CXX_COMPILER ${TRIPLET}-g++)
	set(CMAKE_SYSROOT ${SYSROOT})
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

log "Building HailoRT ${HAILO_VERSION}"
cmake -S "${SRC}" -B "${BUILD}" -G Ninja -Wno-dev \
	-DCMAKE_TOOLCHAIN_FILE="${WORK}/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR="lib/${TRIPLET}" \
	-DHAILO_BUILD_EXAMPLES=OFF -DHAILO_BUILD_SERVICE=OFF -DHAILO_BUILD_GSTREAMER=OFF >/dev/null
ninja -C "${BUILD}" -j "$(nproc)" libhailort hailortcli >/dev/null

log "Packaging hailort ${PKG_VERSION}"
rm -rf "${STAGE}"
DESTDIR="${STAGE}" cmake --install "${BUILD}/hailort/libhailort" >/dev/null
DESTDIR="${STAGE}" cmake --install "${BUILD}/hailort/hailortcli" >/dev/null
# hailortcli installs its bash completion to ${CMAKE_INSTALL_PREFIX}/usr/share
if [[ -d "${STAGE}/usr/usr/share" ]]; then
	cp -a "${STAGE}/usr/usr/share/." "${STAGE}/usr/share/"
	rm -rf "${STAGE}/usr/usr"
fi
"${TRIPLET}-strip" --strip-unneeded "${STAGE}/usr/lib/${TRIPLET}"/libhailort.so.* "${STAGE}/usr/bin/hailortcli"
install -D -m 0644 "${SRC}/hailort/LICENSE" "${STAGE}/usr/share/doc/hailort/copyright"

mkdir -p "${STAGE}/DEBIAN"
cat > "${STAGE}/DEBIAN/control" <<-EOF
	Package: hailort
	Version: ${PKG_VERSION}
	Architecture: arm64
	Maintainer: Kray-SoM-RKC OS <tishenko.alexandr@gmail.com>
	Depends: libc6, libgcc-s1, libstdc++6
	Section: libs
	Priority: optional
	Description: Hailo AI accelerator runtime (libhailort, hailortcli)
	 HailoRT ${HAILO_VERSION} for Hailo-8 devices on PCIe, built by kray-som-rkc-os
	 scripts/build-hailort.sh. Needs the hailo_pci driver and firmware of the same version.
EOF
mkdir -p "${OUT}"
rm -f "${OUT}"/hailort_*.deb
dpkg-deb --root-owner-group -Zxz --build "${STAGE}" "${OUT}/hailort_${PKG_VERSION}_arm64.deb" >/dev/null
dpkg-deb -c "${OUT}/hailort_${PKG_VERSION}_arm64.deb" | awk '{print $6}' | grep -E 'bin/|\.so'
log "Packaged ${OUT}/hailort_${PKG_VERSION}_arm64.deb"
