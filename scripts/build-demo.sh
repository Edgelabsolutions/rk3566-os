#!/usr/bin/env bash
# Build the exhibition demo (kray-demo) for the vendor image: cross-compile the
# program and convert the YOLOv8n person detector for the RK3566 NPU.
#
# Output in userpatches/demo/ (not stored in git):
#   kray-demo                 the program (arm64)
#   yolov8n-rk3566.rknn       the model, int8
#
# Needs: aarch64-linux-gnu-g++, curl, git, and the multimedia packages from
# scripts/build-multimedia.sh (for librga). The RKNN runtime headers come from
# scripts/fetch-npu.sh, which is run if needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="${ROOT}/userpatches/demo"
WORK="${ROOT}/.cache/demo"
CXX="${CROSS_COMPILE:-aarch64-linux-gnu-}g++"

RKNN_TAG="v2.3.2"
ZOO_TAG="v2.3.2"
STB_COMMIT="2c980bb59875b0d32144a71867fbdebb2f77cd20"
LIBDRM_VERSION="2.4.124-2"   # Debian 13 (trixie)
ONNX_URL="https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8/yolov8n.onnx"
ONNX_SHA256="0c8716701f471067932b797eeb67c8e5db47c693c2557c881d7679ec12e21bc5"

mkdir -p "${WORK}"

# --- RKNN runtime API
API="${ROOT}/.cache/rknn-toolkit2-${RKNN_TAG}/rknpu2/runtime/Linux/librknn_api"
[[ -f "${API}/include/rknn_api.h" ]] || "${ROOT}/scripts/fetch-npu.sh"

# --- arm64 sysroot: libdrm from Debian, librga from our multimedia packages
SYSROOT="${WORK}/sysroot"
RGA_DEB="$(ls "${ROOT}"/userpatches/multimedia/librga-rockchip_*_arm64.deb 2>/dev/null | head -1 || true)"
[[ -n "${RGA_DEB}" ]] || { echo "librga package missing: run scripts/build-multimedia.sh first" >&2; exit 1; }
if [[ ! -f "${SYSROOT}/usr/include/xf86drm.h" || ! -f "${SYSROOT}/usr/include/rga/im2d.hpp" ]]; then
	echo "==> Preparing arm64 sysroot"
	rm -rf "${SYSROOT}" "${WORK}/debs"
	mkdir -p "${SYSROOT}" "${WORK}/debs"
	for p in libdrm2 libdrm-dev; do
		curl -fsSL -o "${WORK}/debs/${p}.deb" \
			"https://deb.debian.org/debian/pool/main/libd/libdrm/${p}_${LIBDRM_VERSION}_arm64.deb"
		dpkg-deb -x "${WORK}/debs/${p}.deb" "${SYSROOT}"
	done
	dpkg-deb -x "${RGA_DEB}" "${SYSROOT}"
fi

# --- stb_truetype (public domain / MIT)
STB="${WORK}/stb"
if [[ ! -f "${STB}/stb_truetype.h" ]]; then
	mkdir -p "${STB}"
	curl -fsSL -o "${STB}/stb_truetype.h" "https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/stb_truetype.h"
fi

echo "==> Cross-compiling kray-demo with ${CXX}"
"${CXX}" -O2 -Wall -std=c++17 -o "${DEMO}/kray-demo" "${DEMO}"/src/*.cpp \
	-I"${DEMO}/src" -I"${DEMO}/src/uapi" -I"${STB}" -I"${API}/include" \
	-I"${SYSROOT}/usr/include" -I"${SYSROOT}/usr/include/libdrm" \
	-L"${API}/aarch64" -L"${SYSROOT}/usr/lib/aarch64-linux-gnu" \
	-lrknnrt -lrga -ldrm -lpthread -lm
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
	ONNX="${ZOO}/examples/yolov8/model/yolov8n.onnx"
	if [[ ! -f "${ONNX}" ]] || ! echo "${ONNX_SHA256}  ${ONNX}" | sha256sum -c --quiet; then
		curl -fsSL -o "${ONNX}" "${ONNX_URL}"
		echo "${ONNX_SHA256}  ${ONNX}" | sha256sum -c --quiet
	fi

	# rknn-toolkit2 2.3.2 runs on Python 3.8-3.12; use a private Python 3.12 via uv.
	UV="${WORK}/uv"
	if [[ ! -x "${UV}" ]]; then
		curl -fsSL https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-unknown-linux-gnu.tar.gz |
			tar -xz -C "${WORK}" --strip-components=1 uv-x86_64-unknown-linux-gnu/uv
	fi
	VENV="${WORK}/rknn"
	if [[ ! -x "${VENV}/bin/python" ]]; then
		"${UV}" venv -q -p 3.12 "${VENV}"
		"${UV}" pip install -q -p "${VENV}/bin/python" rknn-toolkit2==2.3.2 \
			onnx==1.16.1 onnxruntime==1.18.1 'setuptools<70' opencv-python-headless
		"${UV}" pip uninstall -q -p "${VENV}/bin/python" opencv-python || true
		"${UV}" pip install -q -p "${VENV}/bin/python" --reinstall opencv-python-headless
	fi
	echo "==> Converting YOLOv8n for the RK3566 NPU (int8)"
	(cd "${ZOO}/examples/yolov8/python" && "${VENV}/bin/python" convert.py ../model/yolov8n.onnx rk3566 i8 "${MODEL}")
fi

echo "==> Demo ready in ${DEMO}: $(ls -1 "${DEMO}" | grep -v '^src$' | tr '\n' ' ')"
