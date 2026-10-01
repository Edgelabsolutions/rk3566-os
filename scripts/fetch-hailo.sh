#!/usr/bin/env bash
# Fetch the Hailo-8 PCIe driver sources and firmware into userpatches/hailo/:
#
#   driver/     hailo-ai/hailort-drivers sources (GPL-2.0), built into the kernel
#               tree by the board config (custom_kernel_config hook)
#   firmware/   hailo8_fw.bin and Hailo's firmware EULA, installed into the image
#
# Hailo-8 is supported by the 4.x line only (the 5.x master branch is for
# Hailo-10/15). Driver, firmware and HailoRT (scripts/build-hailort.sh) must
# have the same version.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ROOT}/.cache/hailo"
OUT="${ROOT}/userpatches/hailo"

HAILO_VERSION=4.24.0
DRIVERS_REPO=https://github.com/hailo-ai/hailort-drivers
DRIVERS_COMMIT=5d9ce886bd1c91451e3e0a9c2e2e5ee07e5bcc63
HAILORT_REPO=https://github.com/hailo-ai/hailort
HAILORT_COMMIT=63adffec12da88f1e7cfdc07f2e07681671e83e8
FW_URL="https://hailo-hailort.s3.eu-west-2.amazonaws.com/Hailo8/${HAILO_VERSION}/FW/hailo8_fw.${HAILO_VERSION}.bin"
FW_SHA256=3ebeb209987bb0e484353be23774d3893908de84069016c4e37361c871d8433a

log() { echo "==> $*"; }

fetch() { # dir repo tag commit
	local dir="${WORK}/$1"
	if [[ ! -d "${dir}/.git" ]]; then
		log "Fetching $1 v${HAILO_VERSION}"
		git -c advice.detachedHead=false clone -q --depth 1 --branch "v${HAILO_VERSION}" "$2" "${dir}"
	fi
	[[ "$(git -C "${dir}" rev-parse HEAD)" == "$3" ]] || { echo "$1: unexpected commit" >&2; exit 1; }
}

mkdir -p "${WORK}"
fetch hailort-drivers "${DRIVERS_REPO}" "${DRIVERS_COMMIT}"
fetch hailort "${HAILORT_REPO}" "${HAILORT_COMMIT}"

log "Staging driver sources"
rm -rf "${OUT}/driver"
mkdir -p "${OUT}/driver/linux"
cp -a "${WORK}/hailort-drivers/common" "${OUT}/driver/"
cp -a "${WORK}/hailort-drivers/linux/vdma" "${WORK}/hailort-drivers/linux/utils" "${OUT}/driver/linux/"
mkdir -p "${OUT}/driver/linux/pcie"
cp -a "${WORK}/hailort-drivers/linux/pcie/src" "${OUT}/driver/linux/pcie/"
cp "${WORK}/hailort-drivers/linux/pcie/51-hailo-udev.rules" "${WORK}/hailort-drivers/linux/pcie/hailo_pci.conf" "${OUT}/driver/"
cp "${WORK}/hailort-drivers/LICENSE" "${OUT}/driver/"

mkdir -p "${OUT}/firmware"
if ! echo "${FW_SHA256}  ${OUT}/firmware/hailo8_fw.bin" | sha256sum -c --status 2>/dev/null; then
	log "Downloading hailo8_fw.${HAILO_VERSION}.bin"
	curl -sfL -o "${OUT}/firmware/hailo8_fw.bin" "${FW_URL}"
	echo "${FW_SHA256}  ${OUT}/firmware/hailo8_fw.bin" | sha256sum -c --quiet
fi
cp "${WORK}/hailort/hailort/hailo_firmware_eula" "${OUT}/firmware/EULA"
log "Hailo ${HAILO_VERSION} driver and firmware ready in ${OUT}"
