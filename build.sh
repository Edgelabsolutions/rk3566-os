#!/usr/bin/env bash
# Build the Kray-SoM-RKC Armbian image.
#
#   ./build.sh                 # vendor 6.1 kernel (default, NPU supported)
#   ./build.sh BRANCH=current  # mainline 6.18, no NPU (experimental)
#   ./build.sh KRAY_DEMO=yes   # exhibition demo image (vendor), see docs/demo.md
#
# Extra KEY=VALUE arguments are passed through to Armbian's compile.sh.
set -euo pipefail

ARMBIAN_TAG="v26.5.1"
ARMBIAN_COMMIT="8de11a017f7f05a82c77850f8322928cb6a3b70c"
ROOT="$(cd "$(dirname "$0")" && pwd)"
ARMBIAN="${ROOT}/armbian-build"

if [[ ! -d "${ARMBIAN}/.git" ]]; then
	echo "==> Cloning armbian/build ${ARMBIAN_TAG}"
	git -c advice.detachedHead=false clone -q --depth 1 --branch "${ARMBIAN_TAG}" https://github.com/armbian/build "${ARMBIAN}"
fi
[[ "$(git -C "${ARMBIAN}" rev-parse HEAD)" == "${ARMBIAN_COMMIT}" ]] ||
	{ echo "armbian-build is not at ${ARMBIAN_TAG} (${ARMBIAN_COMMIT})" >&2; exit 1; }

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

# Exhibition demo image (./build.sh KRAY_DEMO=yes): program and NPU model.
if [[ " $* " == *" KRAY_DEMO=yes "* ]]; then
	"${ROOT}/scripts/build-demo.sh"
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
