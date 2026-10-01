#!/usr/bin/env bash
# Cross-compile Rockchip's camera 3A engine (rkaiq) for the RK3566 ISP (v2.1)
# and package it as userpatches/camera/camera-engine-rkaiq_<version>_arm64.deb:
#
#   /usr/lib/aarch64-linux-gnu/librkaiq.so   3A library (auto exposure, white
#                                            balance, colour, noise reduction)
#   /usr/bin/rkaiq_3A_server                 runs 3A while an app streams from rkisp
#   /usr/bin/rkisp_demo                      capture test tool
#   /etc/iqfiles/imx477_rpi-hq_default.json  tuning file for the Raspberry Pi HQ
#                                            Camera (userpatches/camera/rkaiq)
#   rkaiq_3A.service                         starts the server with the IMX477 overlay
#
# Source: Khadas' camera_engine_rkaiq for Linux 6.1 (AIQ v6.0x8.0; Apache-2.0,
# some algorithms are prebuilt Rockchip libraries). Its ISP v2.1 parameter
# structures match the vendor kernel's rk-isp21-config.h.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ROOT}/.cache/rkaiq"
OUT="${ROOT}/userpatches/camera"
SRC="${WORK}/src"
SYSROOT="${WORK}/sysroot"
BUILD="${WORK}/build"
STAGE="${WORK}/stage"
TRIPLET=aarch64-linux-gnu

RKAIQ_REPO=https://github.com/khadas/external_camera_engine_rkaiq
RKAIQ_BRANCH=khadas-edges-6.1.y-v1.1.1_20240920
RKAIQ_COMMIT=88679424c1d225bcad699e84759a57108de6c8be
PKG_VERSION="${PKG_VERSION:-6.0x8.0~kray1}"

log() { echo "==> $*"; }

if [[ ! -d "${SRC}/.git" ]]; then
	log "Fetching rkaiq"
	git -c advice.detachedHead=false clone -q --depth 1 --branch "${RKAIQ_BRANCH}" "${RKAIQ_REPO}" "${SRC}"
fi
[[ "$(git -C "${SRC}" rev-parse HEAD)" == "${RKAIQ_COMMIT}" ]] || { echo "rkaiq: unexpected commit" >&2; exit 1; }

# GCC 14 turns several old warnings into errors under the project's -Werror
sed -i 's/ -Werror\b//g' "${SRC}/rkaiq/cmake/CompileOptions.cmake"

if [[ ! -f "${SYSROOT}/usr/include/xf86drm.h" ]]; then
	log "Creating arm64 trixie sysroot"
	mmdebstrap --quiet --architectures=arm64 --variant=minbase --include=libc6-dev,libdrm-dev \
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

log "Building rkaiq for rk356x (ISP v2.1)"
# Newer libstdc++ no longer pulls these headers in implicitly
cmake -S "${SRC}" -B "${BUILD}" -G Ninja -Wno-dev \
	-DCMAKE_TOOLCHAIN_FILE="${WORK}/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_SKIP_RPATH=TRUE \
	-DCMAKE_CXX_FLAGS="-include cstdio -include cstdint -include cstring" \
	-DRKAIQ_TARGET_SOC=rk356x -DARCH=aarch64 -DISP_HW_VERSION=-DISP_HW_V21 \
	-DRKAIQ_BUILD_BINARY_IQ=OFF -DRKAIQ_USE_RAWSTREAM_LIB=OFF \
	-DRKAIQ_HAVE_FAKECAM=OFF -DRKAIQ_ENABLE_AF=ON >/dev/null
ninja -C "${BUILD}" -j "$(nproc)" rkaiq rkaiq_3A_server rkisp_demo >/dev/null

log "Packaging camera-engine-rkaiq ${PKG_VERSION}"
rm -rf "${STAGE}"
install -D -m 0644 "${BUILD}/rkaiq/all_lib/MinSizeRel/librkaiq.so" "${STAGE}/usr/lib/${TRIPLET}/librkaiq.so"
install -D -m 0755 "${BUILD}/rkaiq_3A_server/rkaiq_3A_server" "${STAGE}/usr/bin/rkaiq_3A_server"
install -D -m 0755 "${BUILD}/rkisp_demo/exe/MinSizeRel/rkisp_demo" "${STAGE}/usr/bin/rkisp_demo"
"${TRIPLET}-strip" --strip-unneeded "${STAGE}/usr/lib/${TRIPLET}/librkaiq.so" "${STAGE}/usr/bin/"*
mkdir -p "${STAGE}/etc/iqfiles"
python3 "${OUT}/rkaiq/make-imx477-iq.py" \
	"${SRC}/rkaiq/iqfiles/isp21/ov13850_ZC-OV13850R2A-V1_Largan-50064B31.json" \
	"${STAGE}/etc/iqfiles/imx477_rpi-hq_default.json"
install -D -m 0644 "${OUT}/rkaiq/rkaiq_3A.service" "${STAGE}/usr/lib/systemd/system/rkaiq_3A.service"
install -D -m 0644 "${SRC}/NOTICE" "${STAGE}/usr/share/doc/camera-engine-rkaiq/copyright"
cat > "${STAGE}/usr/share/doc/camera-engine-rkaiq/README" <<-EOF
	Rockchip rkaiq ${PKG_VERSION%~*} for the RK3566 ISP (v2.1), built by kray-som-rkc-os
	scripts/build-rkaiq.sh from ${RKAIQ_REPO} (${RKAIQ_COMMIT}).
	rkaiq_3A.service runs rkaiq_3A_server when the kray-som-rkc-imx477 overlay is enabled.
	See docs/camera.md in kray-som-rkc-os.
EOF

mkdir -p "${STAGE}/DEBIAN"
cat > "${STAGE}/DEBIAN/control" <<-EOF
	Package: camera-engine-rkaiq
	Version: ${PKG_VERSION}
	Architecture: arm64
	Maintainer: Kray-SoM-RKC OS <tishenko.alexandr@gmail.com>
	Depends: libc6, libgcc-s1, libstdc++6, libdrm2
	Section: video
	Priority: optional
	Description: Rockchip camera 3A engine (rkaiq) for the RK3566 ISP
	 Auto exposure, white balance and image processing for cameras on rkisp,
	 with a tuning file for the Raspberry Pi HQ Camera (IMX477).
EOF
cat > "${STAGE}/DEBIAN/postinst" <<-'EOF'
	#!/bin/sh
	set -e
	if [ "$1" = configure ] && command -v systemctl >/dev/null; then
		systemctl enable rkaiq_3A.service >/dev/null 2>&1 || true
	fi
EOF
chmod 0755 "${STAGE}/DEBIAN/postinst"
mkdir -p "${OUT}"
rm -f "${OUT}"/camera-engine-rkaiq_*.deb
dpkg-deb --root-owner-group -Zxz --build "${STAGE}" "${OUT}/camera-engine-rkaiq_${PKG_VERSION}_arm64.deb" >/dev/null
log "Packaged ${OUT}/camera-engine-rkaiq_${PKG_VERSION}_arm64.deb"
