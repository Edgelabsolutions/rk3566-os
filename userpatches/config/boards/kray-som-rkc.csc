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
# brought up to upstream stable 6.1.189 by
# userpatches/kernel/rk35xx-vendor-6.1/9000-linux-stable-6.1.172-to-6.1.189.patch
# (merge and conflict resolutions: docs/kernel-stable-6.1.189-merge.md),
# as in Armbian main (armbian/build 81e8624). The matching kernel config is in
# userpatches/config/kernel/linux-rk35xx-vendor.config. The Unisoc uwe5622 WiFi
# driver that Armbian v26.5.1 injects does not link against rkr7.2 (the board
# has no such chip), so it is skipped.
function post_family_config__kray_som_rkc_vendor_rkr72() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	display_alert "$BOARD" "Using vendor kernel branch rk-6.1-rkr7.2" "info"
	declare -g KERNELBRANCH="branch:rk-6.1-rkr7.2"
	# driver_rtw88 backports newer rtw88 code into 6.1; it no longer applies on
	# top of the merged upstream stable releases (the patch below), and the
	# vendor image uses the out-of-tree 88XXau driver for RTL8821AU instead.
	declare -g -a KERNEL_DRIVERS_SKIP+=(driver_uwe5622 driver_rtw88)
}

# Serial-only console: kernel log, initramfs rescue shell and login on UART2
# (1500000 8N1). HDMI still gets a login prompt on tty1.
function post_customize_image__kray_som_rkc_serial_console() {
	display_alert "$BOARD" "Setting console=serial in armbianEnv.txt" "info"
	sed -i -e 's/^console=.*/console=serial/' -e 's/^verbosity=.*/verbosity=1/' "${SDCARD}"/boot/armbianEnv.txt
}

# Proprietary components (Rockchip RKNN runtime, Hailo-8 firmware) may not be
# redistributed in public images. By default the image leaves them out and
# ships kray-get-proprietary, which downloads them on the board (pinned by
# SHA-256, after the user accepts the license). KRAY_PROPRIETARY=yes puts them
# into the image, for internal and customer images; the demo image always has
# them, since it has to work offline.
function kray_proprietary_in_image() {
	[[ "${KRAY_PROPRIETARY:-no}" == "yes" || "${KRAY_DEMO:-no}" == "yes" ]]
}

function post_customize_image__kray_som_rkc_proprietary_tool() {
	install -D -m 0755 "${USERPATCHES_PATH}/proprietary/kray-get-proprietary" "${SDCARD}/usr/sbin/kray-get-proprietary"
}

# RKNN runtime (librknnrt 2.3.2, matches the vendor kernel's rknpu 0.9.8) and a
# MobileNet smoke test in /usr/share/kray-som-rkc/npu-test. Vendor only: mainline
# has no driver for the RK3566 NPU. The test program is always installed; the
# runtime, headers and test model only with kray_proprietary_in_image.
function post_customize_image__kray_som_rkc_npu_runtime() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	local src="${USERPATCHES_PATH}/npu"
	install -d "${SDCARD}/usr/share/kray-som-rkc/npu-test/model"
	install -m 0755 "${src}/npu-test/npu_test" "${src}/npu-test/run.sh" "${SDCARD}/usr/share/kray-som-rkc/npu-test/"
	install -m 0644 "${src}/npu-test/npu_test.c" "${SDCARD}/usr/share/kray-som-rkc/npu-test/"
	if ! kray_proprietary_in_image; then
		display_alert "$BOARD" "RKNN runtime left out (installed on the board with kray-get-proprietary)" "info"
		return 0
	fi
	display_alert "$BOARD" "Installing RKNN runtime 2.3.2 and NPU test model" "info"
	install -D -m 0644 "${src}/librknnrt.so" "${SDCARD}/usr/lib/librknnrt.so"
	install -d "${SDCARD}/usr/include"
	install -m 0644 "${src}"/include/*.h "${SDCARD}/usr/include/"
	install -m 0644 "${src}"/npu-test/model/* "${SDCARD}/usr/share/kray-som-rkc/npu-test/model/"
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

# Kernel hardening (docs/security.md), for both kernels:
# - memory-safety checks that cost little: usercopy bounds, FORTIFY_SOURCE,
#   hardened and randomised slab free lists, zeroed allocations, KFENCE
#   (kernel stack offset randomisation is built in but off by default);
# - Yama (ptrace limited to a process's own descendants) and Landlock;
# - LKDTM (crashes the kernel on request through debugfs) off;
# - code the board does not need removed: cluster and rarely used file
#   systems, ksmbd, NVMe/SCSI targets and legacy network protocols. These
#   were among the areas with the most CVEs fixed in 6.1.173-6.1.189.
# debugfs stays on: kray-demo reads the NPU load from it.
function custom_kernel_config__kray_som_rkc_hardening() {
	opts_y+=(
		"HARDENED_USERCOPY" "FORTIFY_SOURCE"
		"SLAB_FREELIST_HARDENED" "SLAB_FREELIST_RANDOM" "SHUFFLE_PAGE_ALLOCATOR"
		"INIT_ON_ALLOC_DEFAULT_ON" "RANDOMIZE_KSTACK_OFFSET"
		"SCHED_STACK_END_CHECK" "DEBUG_WX" "KFENCE"
		"IO_STRICT_DEVMEM" "SECURITY_DMESG_RESTRICT" "SECURITY_YAMA" "SECURITY_LANDLOCK"
	)
	opts_n+=(
		# Measured on the board: kernel stack randomisation (about 7%) and the
		# list checks (10-15%) slowed down syscall- and scheduler-heavy
		# micro benchmarks. Kernel stack randomisation can still
		# be switched on with randomize_kstack_offset=on in extraargs.
		"RANDOMIZE_KSTACK_OFFSET_DEFAULT" "BUG_ON_DATA_CORRUPTION" "DEBUG_LIST"
		"SLAB_MERGE_DEFAULT" "LKDTM"
		"OCFS2_FS" "GFS2_FS" "DLM" "JFS_FS" "REISERFS_FS" "NILFS2_FS" "HFS_FS" "HFSPLUS_FS"
		"NTFS_FS" "CEPH_FS" "ECRYPT_FS"
		"SMB_SERVER" "NVME_TARGET" "TARGET_CORE"
		"BATMAN_ADV" "IP_DCCP" "TIPC" "RDS" "ATM" "X25" "LAPB" "PHONET" "IEEE802154" "6LOWPAN"
	)
	opts_val["DEFAULT_MMAP_MIN_ADDR"]="32768"
	# Add Landlock in front of each kernel's LSM list. opts_val loses the
	# quotes a string option needs, so set it with --set-str.
	local lsm="landlock,lockdown,yama,integrity,apparmor"
	[[ "${BRANCH}" == "vendor" ]] && lsm="landlock,lockdown,yama,loadpin,safesetid,integrity,apparmor,bpf"
	kernel_config_modifying_hashes+=("LSM=${lsm}")
	[[ -f .config ]] && ./scripts/config --set-str LSM "${lsm}"
	return 0
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

# Camera sensor drivers from Raspberry Pi's kernel, adapted for the Rockchip
# CSI-2 D-PHY and ISP (userpatches/camera/<driver>/: <driver>.c + Kconfig):
# imx477 (HQ Camera, also IMX378), imx296 (Global Shutter Camera), imx519
# (Arducam 16 MP), arducam_64mp (Arducam 64 MP), imx283 (1-inch 20 MP),
# rpi_imx258, rpi_imx335, rpi_imx415, rpi_imx290 (named apart from the vendor
# kernel's drivers for the same sensors), rpi_ov2311, rpi_mira220.
# Copied into the kernel tree and built in: as modules they would probe after
# the ISP has stopped waiting for its sensor.
function custom_kernel_config__kray_som_rkc_rpi_camera_drivers() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	local cam="${USERPATCHES_PATH}/camera"
	kernel_config_modifying_hashes+=("rpi-cameras=y-$(cat "${cam}"/*/* | sha256sum | cut -c1-16)")
	[[ -f .config ]] || return 0

	local dir="${kernel_work_dir}/drivers/media/i2c" src name sym
	for src in "${cam}"/*/; do
		name="$(basename "${src}")"
		sym="$(sed -n 's/^config \(VIDEO_[A-Z0-9_]*\)$/\1/p' "${src}/Kconfig")"
		[[ -f "${src}/${name}.c" && -n "${sym}" ]] || exit_with_error "Bad camera driver directory ${src}"
		cp "${src}/${name}.c" "${dir}/"
		if ! grep -q "^config ${sym}$" "${dir}/Kconfig"; then
			sed -i "/^config VIDEO_IMX378$/e cat ${src}/Kconfig" "${dir}/Kconfig"
			grep -q "^config ${sym}$" "${dir}/Kconfig" ||
				exit_with_error "Could not add ${sym} to ${dir}/Kconfig"
		fi
		grep -q "CONFIG_${sym})" "${dir}/Makefile" ||
			echo "obj-\$(CONFIG_${sym}) += ${name}.o" >> "${dir}/Makefile"
		opts_y+=("${sym}")
	done
}


# Camera sensor drivers from the vendor tree (Rockchip-style) used by the
# camera overlays: built in, so that they probe before the ISP stops waiting
# for sensors. IMX708 (Camera Module 3) is off in the kernel config;
# IMX327/IMX462 and OV9281 are modules there; OV5647 and OV7251 are already
# built in. Rockchip's imx378, imx258, imx335 and imx415 drivers (4-lane or
# 27/37.125 MHz modules) and the mainline imx290 driver (no Rockchip ISP
# support) are switched off: those sensors are handled by
# Raspberry Pi's drivers (userpatches/camera), which work with the 2-lane,
# 24 MHz Raspberry Pi modules.
#
# Lens focus motors: AK7375 (Arducam 16 MP / IMX519) is built in; the
# mainline dw9807-vcm is switched off for Raspberry Pi's rpi_dw9807_vcm, which
# also drives the DW9817 of Camera Module 3 and the Arducam 64 MP. HDMI input:
# Rockchip's tc35874x TC358743 driver is already built in; the mainline
# tc358743 driver for the same chip is switched off. AV input: the kernel's
# adv7180 is switched off for kray_adv7180 (userpatches/camera), which adds
# what the Rockchip D-PHY and ISP need for the ADV7282-M.
# Upstream 6.1.y (since the 6.1.189 merge) backported two API changes that
# Armbian's out-of-tree Realtek Wi-Fi drivers only expect from later kernels,
# by version number:
# - struct usb_driver lost its drvwrap wrapper for a plain .driver member
#   (8b7648962d41, a dependency of a use-after-free fix; mainline 6.8);
# - cfg80211's tdls_mgmt gained a link_id argument (mainline 6.5).
# Adapt the drivers to the backported API.
function custom_kernel_config__kray_som_rkc_wifi_backport_compat() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	kernel_config_modifying_hashes+=("wifi-backport-compat=2")
	[[ -f .config ]] || return 0
	local f
	for f in $(grep -rl 'drvwrap\.driver' drivers/net/wireless --include='*.c' 2>/dev/null); do
		sed -i 's/drvwrap\.driver/driver/g' "$f"
	done
	for f in $(grep -rl 'int link_id,' drivers/net/wireless --include='*.c' 2>/dev/null); do
		perl -0pi -e 's/#if \(LINUX_VERSION_CODE >= KERNEL_VERSION\(6, 5, 0\)\)[^\n]*\n(\s*int link_id,)/#if 1 \/* tdls_mgmt link_id, backported to 6.1.y *\/\n$1/g' "$f"
	done
}

function custom_kernel_config__kray_som_rkc_camera_drivers() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	kernel_config_modifying_hashes+=("cameras=imx708,imx327,ov9281,ak7375=y;imx378,imx258,imx335,imx415,imx290,dw9807,tc358743,adv7180=n")
	[[ -f .config ]] || return 0
	opts_y+=("VIDEO_IMX708" "VIDEO_IMX327" "VIDEO_OV9281" "VIDEO_AK7375")
	opts_n+=("VIDEO_IMX378" "VIDEO_IMX258" "VIDEO_IMX335" "VIDEO_IMX415" "VIDEO_IMX290"
		"VIDEO_DW9807_VCM" "VIDEO_TC358743" "VIDEO_ADV7180")
}

# Camera helpers for the vendor image:
# - kray-cam-3a.service: auto exposure and white balance for the camera
#   (docs/camera.md), built here with the cross compiler; the demo image
#   keeps it off (kray-demo has its own);
# - kray-hdmi-in-edid.service loads an EDID that fits two CSI-2 lanes when
#   the 2-lane TC358743 overlay is enabled (it does nothing without one).
function post_customize_image__kray_som_rkc_camera_tools() {
	[[ "${BRANCH}" == "vendor" ]] || return 0
	local src="${USERPATCHES_PATH}/camera-tools"
	local cxx="${KRAY_CROSS_COMPILE:-aarch64-linux-gnu-}g++"
	if command -v "${cxx}" > /dev/null; then
		display_alert "$BOARD" "Building kray-cam-3a" "info"
		"${cxx}" -O2 -Wall -std=c++17 -I"${USERPATCHES_PATH}/demo/src/uapi" \
			-o "${SDCARD}/usr/bin/kray-cam-3a" "${src}/kray-cam-3a/kray-cam-3a.cpp" \
			-static-libstdc++ -static-libgcc ||
			exit_with_error "kray-cam-3a did not build"
		"${cxx%g++}strip" "${SDCARD}/usr/bin/kray-cam-3a"
		install -D -m 0644 "${src}/kray-cam-3a/kray-cam-3a.conf" "${SDCARD}/etc/kray-cam-3a.conf"
		install -D -m 0644 "${src}/kray-cam-3a/kray-cam-3a.service" "${SDCARD}/usr/lib/systemd/system/kray-cam-3a.service"
		[[ "${KRAY_DEMO:-no}" == "yes" ]] || chroot_sdcard systemctl enable kray-cam-3a.service
	else
		display_alert "$BOARD" "No ${cxx}: kray-cam-3a left out" "wrn"
	fi
	install -D -m 0755 "${src}/kray-hdmi-in-edid" "${SDCARD}/usr/lib/kray-som-rkc/kray-hdmi-in-edid"
	install -D -m 0644 "${src}/tc358743-edid-1080p30.hex" "${SDCARD}/usr/share/kray-som-rkc/tc358743-edid-1080p30.hex"
	install -D -m 0644 "${src}/kray-hdmi-in-edid.service" "${SDCARD}/usr/lib/systemd/system/kray-hdmi-in-edid.service"
	chroot_sdcard systemctl enable kray-hdmi-in-edid.service
}

# Firmware (only with kray_proprietary_in_image; Hailo's EULA), device node
# permissions, module options and the HailoRT runtime (libhailort, hailortcli).
function post_customize_image__kray_som_rkc_hailo() {
	local src="${USERPATCHES_PATH}/hailo"
	display_alert "$BOARD" "Installing Hailo-8 firmware and HailoRT" "info"
	if kray_proprietary_in_image; then
		install -D -m 0644 "${src}/firmware/hailo8_fw.bin" "${SDCARD}/lib/firmware/hailo/hailo8_fw.bin"
		install -D -m 0644 "${src}/firmware/EULA" "${SDCARD}/lib/firmware/hailo/EULA"
	else
		display_alert "$BOARD" "Hailo-8 firmware left out (installed on the board with kray-get-proprietary)" "info"
	fi
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

# Image hardening (docs/security.md): stricter SSH settings (root keeps
# password login for the first login), no rpcbind and no LLMNR, an nftables
# firewall that only lets in SSH, mDNS, ICMP and DHCP replies, sysctl
# defaults, and no world-writable CEC node.
function post_family_config__kray_som_rkc_hardening_packages() {
	add_packages_to_image nftables
}

function post_customize_image__kray_som_rkc_hardening() {
	local src="${USERPATCHES_PATH}/hardening"
	display_alert "$BOARD" "Hardening: SSH, rpcbind, LLMNR, firewall, sysctl" "info"
	install -D -m 0644 "${src}/sshd-kray.conf" "${SDCARD}/etc/ssh/sshd_config.d/10-kray.conf"
	install -D -m 0644 "${src}/resolved-kray.conf" "${SDCARD}/etc/systemd/resolved.conf.d/10-kray.conf"
	install -D -m 0644 "${src}/sysctl-kray.conf" "${SDCARD}/etc/sysctl.d/90-kray.conf"
	install -D -m 0644 "${src}/90-kray-rk-cec.rules" "${SDCARD}/etc/udev/rules.d/90-kray-rk-cec.rules"
	install -D -m 0644 "${src}/modprobe-kray.conf" "${SDCARD}/etc/modprobe.d/kray-blacklist.conf"
	install -D -m 0755 "${src}/nftables.conf" "${SDCARD}/etc/nftables.conf"
	install -d -m 0755 "${SDCARD}/etc/nftables.d"
	install -D -m 0644 "${src}/nftables-stop.conf" \
		"${SDCARD}/etc/systemd/system/nftables.service.d/10-kray.conf"
	chroot_sdcard systemctl enable nftables.service
	# NFS clients keep NFSv4; NFSv3 needs: systemctl unmask --now rpcbind.socket rpcbind.service
	chroot_sdcard systemctl mask rpcbind.service rpcbind.socket
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
	add_packages_to_image fonts-dejavu-core overlayroot busybox python3
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
	# H.265 live stream of the screen and its web page (port 8080)
	install -D -m 0755 "${src}/files/kray-demo-stream" "${SDCARD}/usr/lib/kray-demo/kray-demo-stream"
	install -D -m 0644 "${src}/files/kray-demo-stream.service" \
		"${SDCARD}/usr/lib/systemd/system/kray-demo-stream.service"
	install -D -m 0644 "${src}/files/www/index.html" "${SDCARD}/usr/share/kray-demo/www/index.html"
	chroot_sdcard systemctl enable kray-demo.service kray-demo-lockdown.service kray-demo-stream.service
	install -D -m 0644 "${USERPATCHES_PATH}/hardening/demo-stream.nft" "${SDCARD}/etc/nftables.d/kray-demo-stream.nft"

	# Camera overlay on from the first boot.
	if grep -q '^overlays=' "${SDCARD}/boot/armbianEnv.txt"; then
		sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays=/s/$/ kray-som-rkc-imx219/' \
			"${SDCARD}/boot/armbianEnv.txt"
	else
		echo "overlays=kray-som-rkc-imx219" >> "${SDCARD}/boot/armbianEnv.txt"
	fi
}
