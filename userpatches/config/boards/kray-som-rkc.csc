# Rockchip RK3566 quad core 1/4GB RAM GBE HDMI USB2 microSD M.2 PCIe
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

# Vendor kernel from the Rockchip SDK rkr7.2 release (6.1.172) instead of rkr5.1,
# as in Armbian main (armbian/build 81e8624). The matching kernel config is in
# userpatches/config/kernel/linux-rk35xx-vendor.config. The Unisoc uwe5622 WiFi
# driver that Armbian v26.5.1 injects does not link against rkr7.2 (the board
# has no such chip), so it is skipped.
function post_family_config__kray_som_rkc_vendor_rkr72() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	display_alert "$BOARD" "Using vendor kernel branch rk-6.1-rkr7.2" "info"
	declare -g KERNELBRANCH="branch:rk-6.1-rkr7.2"
	declare -g -a KERNEL_DRIVERS_SKIP+=(driver_uwe5622)
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

# The Ethernet LEDs are driven by the PHY itself, not by LED triggers. With
# LED_TRIGGER_PHY on, every link-up logs "No phy led trigger registered for
# speed(-1)" at alert level on the console.
function custom_kernel_config__kray_som_rkc_no_phy_led_trigger() {
	opts_n+=("LED_TRIGGER_PHY")
}

# The kernel and DTB packages carry this board's DTS and kernel options, but the
# official Armbian apt repository ships packages with the same names without
# them: an upgrade would drop rk3566-kray-som-rkc.dtb and leave the board
# unbootable. Hold them so `apt upgrade` / `armbian-upgrade` skip them.
function post_customize_image__kray_som_rkc_hold_kernel() {
	local pkgs=("linux-image-${BRANCH}-${LINUXFAMILY}" "linux-dtb-${BRANCH}-${LINUXFAMILY}")
	display_alert "$BOARD" "Holding ${pkgs[*]}" "info"
	chroot_sdcard apt-mark hold "${pkgs[@]}"
}

# Hardware video codecs.
#  vendor:  Rockchip MPP stack built by scripts/build-multimedia.sh
#           (userpatches/multimedia/*.deb) plus the GStreamer runtime it needs
#           and v4l-utils for the camera.
#  current: mainline V4L2 stateless decoders (hantro, rkvdec) through
#           GStreamer v4l2codecs from Debian, plus v4l-utils.
function post_family_config__kray_som_rkc_multimedia_packages() {
	# plugins-bad carries the h264parse/h265parse parsers (and v4l2codecs).
	add_packages_to_image gstreamer1.0-tools gstreamer1.0-plugins-base \
		gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
	case "${BRANCH}" in
		vendor) add_packages_to_image libdrm2 v4l-utils ;;
		current) add_packages_to_image v4l-utils ;;
	esac
}

function post_customize_image__kray_som_rkc_multimedia() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	local src="${USERPATCHES_PATH}/multimedia"
	if ! compgen -G "${src}/*.deb" > /dev/null; then
		display_alert "$BOARD" "No multimedia packages in ${src}; run scripts/build-multimedia.sh" "wrn"
		return 0
	fi
	display_alert "$BOARD" "Installing Rockchip MPP multimedia packages" "info"
	mkdir -p "${SDCARD}/tmp/kray-multimedia"
	cp "${src}"/*.deb "${SDCARD}/tmp/kray-multimedia/"
	chroot_sdcard dpkg -i /tmp/kray-multimedia/*.deb
	rm -rf "${SDCARD}/tmp/kray-multimedia"
	chroot_sdcard ldconfig
}

# Hailo-8 AI accelerator on the M.2 PCIe slot (all branches).
# scripts/fetch-hailo.sh stages hailort-drivers 4.24.0 and the firmware in
# userpatches/hailo; scripts/build-hailort.sh builds the HailoRT .deb.
#
# The driver is built in-tree as a module: the sources go to drivers/misc/hailo
# here, after patching (patching resets the tree) and before olddefconfig.
function custom_kernel_config__kray_som_rkc_hailo_pci() {
	kernel_config_modifying_hashes+=("hailo_pci=4.24.0-5d9ce88")
	[[ -f .config ]] || return 0

	local src="${USERPATCHES_PATH}/hailo"
	local dst="${kernel_work_dir}/drivers/misc/hailo"
	[[ -d "${src}/driver/common" ]] || exit_with_error "Hailo driver sources missing" "run scripts/fetch-hailo.sh"

	rm -rf "${dst}"
	mkdir -p "${dst}"
	cp -a "${src}/driver/common" "${src}/driver/linux" "${dst}/"
	cp "${src}/kernel/Kconfig" "${src}/kernel/Makefile" "${dst}/"
	if ! grep -q 'drivers/misc/hailo/Kconfig' "${kernel_work_dir}/drivers/misc/Kconfig"; then
		sed -i '$ s|^endmenu$|source "drivers/misc/hailo/Kconfig"\nendmenu|' "${kernel_work_dir}/drivers/misc/Kconfig"
		grep -q 'drivers/misc/hailo/Kconfig' "${kernel_work_dir}/drivers/misc/Kconfig" ||
			exit_with_error "Could not hook the Hailo driver into drivers/misc/Kconfig"
	fi
	if ! grep -q 'CONFIG_HAILO_PCI' "${kernel_work_dir}/drivers/misc/Makefile"; then
		echo 'obj-$(CONFIG_HAILO_PCI)	+= hailo/' >> "${kernel_work_dir}/drivers/misc/Makefile"
	fi
	opts_m+=("HAILO_PCI")
}

# Optional hardware as device tree overlays for the vendor kernel, built with
# the kernel into /boot/dtb/rockchip/overlay and enabled with
# overlays=<name> in /boot/armbianEnv.txt (see userpatches/overlay/).
# The overlay Makefile expands dtbo-y with ':=' at "always-y", so the
# entries go in before the "targets" line.
function custom_kernel_config__kray_som_rkc_overlays() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	local src="${USERPATCHES_PATH}/overlay/rk35xx-vendor-6.1"
	kernel_config_modifying_hashes+=("kray_overlays=$(cat "${src}"/*.dts | sha256sum | cut -c1-16)")
	[[ -f .config ]] || return 0

	local dst="${kernel_work_dir}/arch/arm64/boot/dts/rockchip/overlay" f name
	for f in "${src}"/*.dts; do
		name="$(basename "${f}" .dts)"
		cp "${f}" "${dst}/"
		grep -q "${name}.dtbo" "${dst}/Makefile" ||
			sed -i "/^targets += \$(dtbo-y)/i dtbo-\$(CONFIG_ARCH_ROCKCHIP) += ${name}.dtbo\n" "${dst}/Makefile"
		grep -q "${name}.dtbo" "${dst}/Makefile" || exit_with_error "Could not add overlay ${name} to ${dst}/Makefile"
	done
}

# Sony IMX477 (Raspberry Pi HQ Camera) for the vendor kernel: Raspberry Pi's
# driver with the calls Rockchip's CSI-2 D-PHY and ISP need
# (userpatches/camera/imx477). Built in like imx219: as a module it probes
# after rkisp has given up waiting for its sensor ("clear unready subdev"),
# and the sensor never gets linked to csi2-dphy0. Used by the
# kray-som-rkc-imx477 overlay.
function custom_kernel_config__kray_som_rkc_imx477() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	local src="${USERPATCHES_PATH}/camera/imx477"
	kernel_config_modifying_hashes+=("imx477=y-$(cat "${src}"/* | sha256sum | cut -c1-16)")
	[[ -f .config ]] || return 0

	local dir="${kernel_work_dir}/drivers/media/i2c"
	cp "${src}/imx477.c" "${dir}/"
	if ! grep -q 'config VIDEO_IMX477' "${dir}/Kconfig"; then
		sed -i "/^config VIDEO_IMX378$/e cat ${src}/Kconfig" "${dir}/Kconfig"
		grep -q 'config VIDEO_IMX477' "${dir}/Kconfig" ||
			exit_with_error "Could not add VIDEO_IMX477 to ${dir}/Kconfig"
	fi
	grep -q 'CONFIG_VIDEO_IMX477' "${dir}/Makefile" ||
		echo 'obj-$(CONFIG_VIDEO_IMX477) += imx477.o' >> "${dir}/Makefile"
	opts_y+=("VIDEO_IMX477")
}

# Firmware (redistributed in binary form with Hailo's EULA), device node
# permissions, module options and the HailoRT runtime (libhailort, hailortcli).
function post_customize_image__kray_som_rkc_hailo() {
	local src="${USERPATCHES_PATH}/hailo"
	display_alert "$BOARD" "Installing Hailo-8 firmware and HailoRT" "info"
	install -D -m 0644 "${src}/firmware/hailo8_fw.bin" "${SDCARD}/lib/firmware/hailo/hailo8_fw.bin"
	install -D -m 0644 "${src}/firmware/EULA" "${SDCARD}/lib/firmware/hailo/EULA"
	install -D -m 0644 "${src}/driver/51-hailo-udev.rules" "${SDCARD}/etc/udev/rules.d/51-hailo-udev.rules"
	install -D -m 0644 "${src}/driver/hailo_pci.conf" "${SDCARD}/etc/modprobe.d/hailo_pci.conf"
	if ! compgen -G "${src}/*.deb" > /dev/null; then
		display_alert "$BOARD" "No HailoRT package in ${src}; run scripts/build-hailort.sh" "wrn"
		return 0
	fi
	mkdir -p "${SDCARD}/tmp/kray-hailo"
	cp "${src}"/*.deb "${SDCARD}/tmp/kray-hailo/"
	chroot_sdcard dpkg -i /tmp/kray-hailo/*.deb
	rm -rf "${SDCARD}/tmp/kray-hailo"
	chroot_sdcard ldconfig
}

# Hardware smoke test (docs/testing.md): sudo kray-board-check
function post_family_config__kray_som_rkc_board_check_packages() {
	add_packages_to_image pciutils ethtool iw i2c-tools
}

function post_customize_image__kray_som_rkc_board_check() {
	install -D -m 0755 "${USERPATCHES_PATH}/board-check/kray-board-check" "${SDCARD}/usr/bin/kray-board-check"
}

# First login over the network instead of Armbian's interactive first-login
# wizard on the console: no wizard, no console autologin, root keeps the
# build's root password (ROOTPWD, Armbian default 1234) but it is expired, so
# the first login (ssh root@kray-som-rkc.local or the console) has to set a
# new one. avahi publishes <hostname>.local and the SSH service.
function post_family_config__kray_som_rkc_network_first_login_packages() {
	add_packages_to_image avahi-daemon libnss-mdns
}

function post_customize_image__kray_som_rkc_network_first_login() {
	display_alert "$BOARD" "First login over SSH: no wizard, root password expired" "info"
	rm -f "${SDCARD}/root/.not_logged_in_yet"
	rm -f "${SDCARD}/etc/systemd/system/getty@.service.d/override.conf" \
		"${SDCARD}/etc/systemd/system/serial-getty@.service.d/override.conf"
	chroot_sdcard chage -d 0 root
	# Show how to reach the board on the console login prompt (agetty: \4 = IPv4, \n = hostname)
	cat >> "${SDCARD}/etc/issue" <<- 'ISSUE'
		IP: \4   ssh root@\n.local   (first login: password 1234, then set a new one)

	ISSUE
}

# Exhibition demo image (KRAY_DEMO=yes, vendor only; see docs/demo.md): the
# board boots straight into kray-demo (IMX219 camera on HDMI, people detection
# on the NPU, CPU/NPU load). After the first boot has resized the file system,
# the root file system becomes read-only with an overlay in RAM (overlayroot),
# so pulling the power plug cannot damage the SD card.
function user_config__kray_som_rkc_demo() {
	[[ "${KRAY_DEMO:-no}" == "yes" ]] || return 0
	[[ "${BRANCH}" == "vendor" ]] || exit_with_error "KRAY_DEMO=yes needs BRANCH=vendor (camera ISP and NPU)"
	EXTRA_IMAGE_SUFFIXES+=("_demo")
}

function post_family_config__kray_som_rkc_demo_packages() {
	[[ "${KRAY_DEMO:-no}" == "yes" ]] || return 0
	# overlayroot's initramfs script needs grep, which the initramfs only has
	# with busybox.
	add_packages_to_image fonts-dejavu-core overlayroot busybox
}

function post_customize_image__kray_som_rkc_demo() {
	[[ "${KRAY_DEMO:-no}" == "yes" ]] || return 0
	local src="${USERPATCHES_PATH}/demo"
	[[ -x "${src}/kray-demo" && -f "${src}/yolov8n-rk3566.rknn" ]] ||
		exit_with_error "Demo files missing in ${src}: run scripts/build-demo.sh"
	display_alert "$BOARD" "Installing the exhibition demo (kray-demo)" "info"
	install -D -m 0755 "${src}/kray-demo" "${SDCARD}/usr/bin/kray-demo"
	install -D -m 0644 "${src}/yolov8n-rk3566.rknn" "${SDCARD}/usr/share/kray-demo/yolov8n-rk3566.rknn"
	install -D -m 0755 "${src}/files/kray-demo-start" "${SDCARD}/usr/lib/kray-demo/kray-demo-start"
	install -D -m 0755 "${src}/files/kray-demo-lockdown" "${SDCARD}/usr/lib/kray-demo/kray-demo-lockdown"
	install -D -m 0644 "${src}/files/kray-demo.service" "${SDCARD}/usr/lib/systemd/system/kray-demo.service"
	install -D -m 0644 "${src}/files/kray-demo-lockdown.service" \
		"${SDCARD}/usr/lib/systemd/system/kray-demo-lockdown.service"
	install -D -m 0644 "${src}/files/kray-demo.conf" "${SDCARD}/etc/kray-demo.conf"
	chroot_sdcard systemctl enable kray-demo.service kray-demo-lockdown.service

	# Camera overlay on from the first boot.
	if grep -q '^overlays=' "${SDCARD}/boot/armbianEnv.txt"; then
		sed -i -e '/^overlays=/s/ *kray-som-rkc-imx[0-9]*//g' -e '/^overlays=/s/$/ kray-som-rkc-imx219/' \
			"${SDCARD}/boot/armbianEnv.txt"
	else
		echo "overlays=kray-som-rkc-imx219" >> "${SDCARD}/boot/armbianEnv.txt"
	fi
}
