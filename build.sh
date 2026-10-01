#!/usr/bin/env bash
# Build the Kray-SoM-RKC Armbian image.
#
#   ./build.sh                 # vendor 6.1 kernel (default, NPU supported)
#   ./build.sh BRANCH=current  # mainline 6.18, no NPU (experimental)
#
# Extra KEY=VALUE arguments are passed through to Armbian's compile.sh.
set -euo pipefail

ARMBIAN_TAG="${ARMBIAN_TAG:-v26.5.1}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
ARMBIAN="${ROOT}/armbian-build"

if [[ ! -d "${ARMBIAN}/.git" ]]; then
	echo "==> Cloning armbian/build ${ARMBIAN_TAG}"
	git -c advice.detachedHead=false clone -q --depth 1 --branch "${ARMBIAN_TAG}" https://github.com/armbian/build "${ARMBIAN}"
fi

"${ROOT}/scripts/fetch-npu.sh"
"${ROOT}/scripts/fetch-hailo.sh"

# Rockchip MPP / RGA / FFmpeg / GStreamer packages for vendor images. Built once
# and reused; delete userpatches/multimedia/*.deb to rebuild them.
if ! compgen -G "${ROOT}/userpatches/multimedia/*.deb" > /dev/null; then
	"${ROOT}/scripts/build-multimedia.sh"
fi

# HailoRT runtime for the Hailo-8 on the M.2 slot. Built once and reused;
# delete userpatches/hailo/*.deb to rebuild it.
if ! compgen -G "${ROOT}/userpatches/hailo/*.deb" > /dev/null; then
	"${ROOT}/scripts/build-hailort.sh"
fi

# Rockchip camera 3A engine (rkaiq) for the vendor image. Built once and
# reused; delete userpatches/camera/*.deb to rebuild it.
if ! compgen -G "${ROOT}/userpatches/camera/camera-engine-rkaiq_*.deb" > /dev/null; then
	"${ROOT}/scripts/build-rkaiq.sh"
fi

echo "==> Syncing userpatches into armbian-build"
mkdir -p "${ARMBIAN}/userpatches"
cp -a "${ROOT}/userpatches/." "${ARMBIAN}/userpatches/"

cd "${ARMBIAN}"
exec ./compile.sh build \
	BOARD=kray-som-rkc \
	BRANCH=vendor \
	RELEASE=trixie \
	BUILD_MINIMAL=no \
	BUILD_DESKTOP=no \
	KERNEL_CONFIGURE=no \
	COMPRESS_OUTPUTIMAGE=sha,img,xz \
	EXPERT=yes \
	"$@"
