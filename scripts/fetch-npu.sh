#!/usr/bin/env bash
# Fetch the Rockchip RKNN runtime, headers and the MobileNet test model from
# airockchip/rknn-toolkit2 (pinned tag) into userpatches/npu, then cross-compile
# the NPU smoke test. These files are under the Rockchip RKNN SDK license, so
# they are downloaded at build time instead of being stored in this repository.
set -euo pipefail

RKNN_TAG="${RKNN_TAG:-v2.3.2}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NPU="${ROOT}/userpatches/npu"
CACHE="${ROOT}/.cache/rknn-toolkit2-${RKNN_TAG}"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"

if [[ ! -d "${CACHE}/rknpu2" ]]; then
	echo "==> Fetching rknn-toolkit2 ${RKNN_TAG}"
	rm -rf "${CACHE}"
	git -c advice.detachedHead=false clone -q --depth 1 --branch "${RKNN_TAG}" --filter=blob:none --sparse \
		https://github.com/airockchip/rknn-toolkit2 "${CACHE}"
	git -C "${CACHE}" sparse-checkout set \
		rknpu2/runtime/Linux/librknn_api \
		rknpu2/examples/rknn_mobilenet_demo/model \
		rknpu2/examples/3rdparty/stb
fi

API="${CACHE}/rknpu2/runtime/Linux/librknn_api"
DEMO="${CACHE}/rknpu2/examples/rknn_mobilenet_demo/model"

echo "==> Installing RKNN runtime and headers into userpatches/npu"
mkdir -p "${NPU}/include" "${NPU}/npu-test/model"
install -m 0644 "${API}/aarch64/librknnrt.so" "${NPU}/librknnrt.so"
install -m 0644 "${API}"/include/*.h "${NPU}/include/"
install -m 0644 "${DEMO}/RK3566_RK3568/mobilenet_v1.rknn" "${DEMO}"/*.jpg "${NPU}/npu-test/model/"

echo "==> Cross-compiling npu_test with ${CC}"
"${CC}" -O2 -Wall -o "${NPU}/npu-test/npu_test" "${NPU}/npu-test/npu_test.c" \
	-I"${API}/include" -I"${CACHE}/rknpu2/examples/3rdparty" \
	-L"${API}/aarch64" -lrknnrt -lm -Wl,-rpath,'$ORIGIN/lib'

echo "==> NPU files ready: $(strings "${NPU}/librknnrt.so" | grep -m1 'librknnrt version')"
