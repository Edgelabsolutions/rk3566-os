# Rockchip RK3566 quad core 1/2/4GB RAM GBE HDMI USB2 microSD (optional eMMC)
BOARD_NAME="Kray-SoM-RKC"
BOARD_VENDOR="kray"
BOARDFAMILY="rk35xx"
BOARD_MAINTAINER=""
INTRODUCED="2023"
BOOTCONFIG="generic-rk3568_defconfig"
# vendor (Rockchip BSP 6.1) is the default branch and the only one with the NPU.
# current (mainline 6.18) is for setups that do not need the NPU.
KERNEL_TARGET="vendor,current"
KERNEL_TEST_TARGET="vendor"
FULL_DESKTOP="yes"
BOOT_LOGO="desktop"
BOOT_FDT_FILE="rockchip/rk3566-kray-som-rkc.dtb"
IMAGE_PARTITION_TABLE="gpt"
BOOT_SCENARIO="spl-blobs"
# BOOT_SOC is derived as rk3568 from BOOTCONFIG; use RK3566 DDR init and clocks
DDR_BLOB="rk35/rk3566_ddr_1056MHz_v1.21.bin"
CPUMAX="1800000"
# 8250 console on UART2 for every branch (vendor DTS disables the FIQ debugger)
SERIALCON="ttyS2:1500000"

# Mainline U-Boot for every kernel branch, with a board defconfig added by
# userpatches/u-boot/v2025.10: it probes the RK809 so vdd_npu/vdd_gpu are on
# before the kernel starts (vendor kernel hangs in NPU power-domain setup otherwise).
function post_family_config__kray_som_rkc_use_mainline_uboot() {
	display_alert "$BOARD" "Using mainline U-Boot (kray-som-rkc-rk3566) for $BOARD / $BRANCH" "info"

	declare -g BOOTCONFIG="kray-som-rkc-rk3566_defconfig"
	declare -g BOOTSOURCE="https://github.com/u-boot/u-boot"
	declare -g BOOTBRANCH="tag:v2025.10"
	declare -g BOOTPATCHDIR="v2025.10"

	declare -g UBOOT_TARGET_MAP="BL31=$RKBIN_DIR/$BL31_BLOB ROCKCHIP_TPL=$RKBIN_DIR/$DDR_BLOB;;u-boot-rockchip.bin"

	unset uboot_custom_postprocess write_uboot_platform write_uboot_platform_mtd

	function write_uboot_platform() {
		dd if=$1/u-boot-rockchip.bin of=$2 seek=64 conv=notrunc status=none
	}
}

# Serial-only console: kernel log, initramfs rescue shell and login on UART2
# (1500000 8N1). HDMI still gets a login prompt on tty1.
function post_customize_image__kray_som_rkc_serial_console() {
	display_alert "$BOARD" "Setting console=serial in armbianEnv.txt" "info"
	sed -i -e 's/^console=.*/console=serial/' -e 's/^verbosity=.*/verbosity=1/' "${SDCARD}"/boot/armbianEnv.txt
}

# RKNN runtime (librknnrt 2.3.2, matches the vendor kernel's rknpu 0.9.8) and a
# MobileNet smoke test in /usr/share/kray-som-rkc/npu-test. Vendor only: mainline
# has no driver for the RK3566 NPU.
function post_customize_image__kray_som_rkc_npu_runtime() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	display_alert "$BOARD" "Installing RKNN runtime 2.3.2 and NPU test" "info"
	local src="${USERPATCHES_PATH}/npu"
	install -D -m 0644 "${src}/librknnrt.so" "${SDCARD}/usr/lib/librknnrt.so"
	install -d "${SDCARD}/usr/include"
	install -m 0644 "${src}"/include/*.h "${SDCARD}/usr/include/"
	install -d "${SDCARD}/usr/share/kray-som-rkc"
	cp -r "${src}/npu-test" "${SDCARD}/usr/share/kray-som-rkc/"
	chroot_sdcard ldconfig
}

# Mainline: in-kernel driver for RTL8821AU USB WiFi adapters (e.g. TP-Link
# Archer T2U Plus). The vendor kernel already ships the out-of-tree 88XXau.
function custom_kernel_config__kray_som_rkc_rtw88_8821au() {
	[[ "${BRANCH}" == "vendor" ]] && return 0
	opts_m+=("RTW88_8821AU")
}
