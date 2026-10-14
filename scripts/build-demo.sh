#!/usr/bin/env bash
# Build the exhibition demo (kray-demo) for the vendor image: cross-compile the
# program and convert the YOLOv8n person detector for the RK3566 NPU.
#
# Output in userpatches/demo/ (not stored in git):
#   kray-demo                 the program (arm64)
#   yolov8n-rk3566.rknn       the model, int8
#
# Needs: aarch64-linux-gnu-g++, curl, git, and the multimedia packages from
# scripts/build-multimedia.sh (librga, Rockchip MPP). The RKNN runtime headers come from
# scripts/fetch-npu.sh, which is run if needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="${ROOT}/userpatches/demo"
WORK="${ROOT}/.cache/demo"
CXX="${CROSS_COMPILE:-aarch64-linux-gnu-}g++"
# shellcheck source=../userpatches/hardening/build-flags.sh
. "${ROOT}/userpatches/hardening/build-flags.sh"

RKNN_TAG="v2.3.2"
ZOO_TAG="v2.3.2"
STB_COMMIT="2c980bb59875b0d32144a71867fbdebb2f77cd20"
LIBDRM_VERSION="2.4.124-2"   # Debian 13 (trixie)
LIBDRM2_SHA256="b535506630bb6a9a616bc07caf3b29915f980d8c3437852fb96ea64fb1a8a3f8"
LIBDRM_DEV_SHA256="7871c74757d8d000c900dbbdb0e81a2665893fcab1704b6019953529bb4c0f4a"
ZOO_COMMIT="bad6c7334531becaf90a561988519b7bec34d0ab"
UV_VERSION="0.12.23"
UV_SHA256="9167d72b3319674b6303c4cbe071854bba13ebdf3d76b1a7cbdc175471fb66d6"
ONNX_URL="https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8/yolov8n.onnx"
ONNX_SHA256="0c8716701f471067932b797eeb67c8e5db47c693c2557c881d7679ec12e21bc5"

mkdir -p "${WORK}"

# --- RKNN runtime API
API="${ROOT}/.cache/rknn-toolkit2-${RKNN_TAG}/rknpu2/runtime/Linux/librknn_api"
[[ -f "${API}/include/rknn_api.h" ]] || "${ROOT}/scripts/fetch-npu.sh"

# --- arm64 sysroot: libdrm from Debian, librga from our multimedia packages
SYSROOT="${WORK}/sysroot"
RGA_DEB="$(ls "${ROOT}"/userpatches/multimedia/librga-rockchip_*_arm64.deb 2>/dev/null | head -1 || true)"
MPP_DEB="$(ls "${ROOT}"/userpatches/multimedia/rockchip-mpp_*_arm64.deb 2>/dev/null | head -1 || true)"
[[ -n "${RGA_DEB}" && -n "${MPP_DEB}" ]] ||
	{ echo "librga/MPP packages missing: run scripts/build-multimedia.sh first" >&2; exit 1; }
if [[ ! -f "${SYSROOT}/usr/include/xf86drm.h" || ! -f "${SYSROOT}/usr/include/rga/im2d.hpp" ||
      ! -f "${SYSROOT}/usr/include/rockchip/rk_mpi.h" ]]; then
	echo "==> Preparing arm64 sysroot"
	rm -rf "${SYSROOT}" "${WORK}/debs"
	mkdir -p "${SYSROOT}" "${WORK}/debs"
	for p in "libdrm2 ${LIBDRM2_SHA256}" "libdrm-dev ${LIBDRM_DEV_SHA256}"; do
		set -- ${p}
		curl -fsSL -o "${WORK}/debs/$1.deb" \
			"https://deb.debian.org/debian/pool/main/libd/libdrm/$1_${LIBDRM_VERSION}_arm64.deb"
		echo "$2  ${WORK}/debs/$1.deb" | sha256sum -c --quiet
		dpkg-deb -x "${WORK}/debs/$1.deb" "${SYSROOT}"
	done
	dpkg-deb -x "${RGA_DEB}" "${SYSROOT}"
	dpkg-deb -x "${MPP_DEB}" "${SYSROOT}"
fi

# --- stb_truetype (public domain / MIT)
STB="${WORK}/stb"
if [[ ! -f "${STB}/stb_truetype.h" ]]; then
	mkdir -p "${STB}"
	curl -fsSL -o "${STB}/stb_truetype.h" "https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/stb_truetype.h"
fi

echo "==> Cross-compiling kray-demo with ${CXX}"
# shellcheck disable=SC2086
"${CXX}" ${KRAY_HARDEN_CFLAGS} -Wall -std=c++17 -o "${DEMO}/kray-demo" "${DEMO}"/src/*.cpp ${KRAY_HARDEN_LDFLAGS} \
	-I"${DEMO}/src" -I"${DEMO}/src/uapi" -I"${STB}" -I"${API}/include" \
	-I"${SYSROOT}/usr/include" -I"${SYSROOT}/usr/include/libdrm" \
	-L"${API}/aarch64" -L"${SYSROOT}/usr/lib/aarch64-linux-gnu" \
	-lrknnrt -lrga -ldrm -lrockchip_mpp -lpthread -lm
aarch64-linux-gnu-strip "${DEMO}/kray-demo"

# --- Model: YOLOv8n (rknn_model_zoo export) converted to int8 for the RK3566
MODEL="${DEMO}/yolov8n-rk3566.rknn"
if [[ ! -f "${MODEL}" ]]; then
	ZOO="${WORK}/zoo"
	if [[ ! -d "${ZOO}/examples/yolov8" ]]; then
		echo "==> Fetching rknn_model_zoo ${ZOO_TAG}"
		rm -rf "${ZOO}"
		git -c advice.detachedHead=false clone -q --depth 1 --branch "${ZOO_TAG}" --filter=blob:none --sparse \
			https://github.com/airockchip/rknn_model_zoo "${ZOO}"
		git -C "${ZOO}" sparse-checkout set examples/yolov8 datasets/COCO
	fi
	[[ "$(git -C "${ZOO}" rev-parse HEAD)" == "${ZOO_COMMIT}" ]] ||
		{ echo "rknn_model_zoo ${ZOO_TAG}: unexpected commit" >&2; exit 1; }
	ONNX="${ZOO}/examples/yolov8/model/yolov8n.onnx"
	if [[ ! -f "${ONNX}" ]] || ! echo "${ONNX_SHA256}  ${ONNX}" | sha256sum -c --quiet; then
		curl -fsSL -o "${ONNX}" "${ONNX_URL}"
		echo "${ONNX_SHA256}  ${ONNX}" | sha256sum -c --quiet
	fi

	# rknn-toolkit2 2.3.2 runs on Python 3.8-3.12; use a private Python 3.12 via uv.
	UV="${WORK}/uv"
	if [[ ! -x "${UV}" ]] || [[ "$("${UV}" --version | awk '{print $2}')" != "${UV_VERSION}" ]]; then
		curl -fsSL -o "${WORK}/uv.tar.gz" \
			"https://github.com/astral-sh/uv/releases/download/${UV_VERSION}/uv-x86_64-unknown-linux-gnu.tar.gz"
		echo "${UV_SHA256}  ${WORK}/uv.tar.gz" | sha256sum -c --quiet
		tar -xzf "${WORK}/uv.tar.gz" -C "${WORK}" --strip-components=1 uv-x86_64-unknown-linux-gnu/uv
		rm -f "${WORK}/uv.tar.gz"
	fi
	# Every package and version is pinned with its hash in demo-requirements.txt.
	VENV="${WORK}/rknn"
	REQ="${ROOT}/scripts/demo-requirements.txt"
	if [[ ! -x "${VENV}/bin/python" || "${REQ}" -nt "${VENV}/.kray-requirements" ]]; then
		rm -rf "${VENV}"
		"${UV}" venv -q -p 3.12 "${VENV}"
		"${UV}" pip install -q -p "${VENV}/bin/python" --require-hashes -r "${REQ}"
		# opencv-python and its headless build share the cv2 module: drop the
		# GUI build and reinstall the headless one over it.
		"${UV}" pip uninstall -q -p "${VENV}/bin/python" opencv-python
		awk '/^opencv-python-headless==/{p=1; print; next} p && /^ /{print; next} {p=0}' "${REQ}" \
			> "${WORK}/opencv-headless.txt"
		"${UV}" pip install -q -p "${VENV}/bin/python" --require-hashes --no-deps --reinstall \
			-r "${WORK}/opencv-headless.txt"
		touch "${VENV}/.kray-requirements"
	fi
	echo "==> Converting YOLOv8n for the RK3566 NPU (int8)"
	(cd "${ZOO}/examples/yolov8/python" && "${VENV}/bin/python" convert.py ../model/yolov8n.onnx rk3566 i8 "${MODEL}")
fi

echo "==> Demo ready in ${DEMO}: $(ls -1 "${DEMO}" | grep -v '^src$' | tr '\n' ' ')"
