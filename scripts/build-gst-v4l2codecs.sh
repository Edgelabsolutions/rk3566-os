#!/usr/bin/env bash
# Cross-compile GStreamer's v4l2codecs plugin (V4L2 stateless decoders:
# v4l2slh264dec, v4l2slh265dec, ...) from Debian's gst-plugins-bad1.0 source,
# with Debian's patches and the upstream fixes in userpatches/gstreamer/, and package it
# as userpatches/gstreamer/gstreamer1.0-v4l2codecs-kray_<version>_arm64.deb.
#
# Mainline images install it in place of the plugin from
# gstreamer1.0-plugins-bad (dpkg-divert in the package scripts). The patches
# are the upstream commits of GStreamer MR !9315 (in 1.26.4; Debian 13 has
# 1.26.2): v4l2slh265dec hands over 1080p frames without a CPU copy (README,
# "Hardware video codecs").
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ROOT}/.cache/gst-v4l2codecs"
OUT="${ROOT}/userpatches/gstreamer"
SYSROOT="${WORK}/sysroot"
TRIPLET=aarch64-linux-gnu
LIBDIR="usr/lib/${TRIPLET}"
# shellcheck source=../userpatches/hardening/build-flags.sh
. "${ROOT}/userpatches/hardening/build-flags.sh"

# Debian trixie (security) source of the gst-plugins-bad1.0 on the image.
DEB_VERSION="1.26.2-3+deb13u3"
UPSTREAM_VERSION="1.26.2"
ORIG_SHA256="cb116bfc3722c2de53838899006cafdb3c7c0bc69cd769b33c992a8421a9d844"
DEBIAN_SHA256="41ab743a36cba54ee6d9688f482c35397e5ac431752a1481d6a33afb6ab1cbf8"
PKG_VERSION="${DEB_VERSION}+kray2"

log() { echo "==> $*"; }

fetch() { # file sha256
	local f="${WORK}/$1"
	if [[ ! -f "${f}" ]] || ! echo "$2  ${f}" | sha256sum -c --status; then
		log "Downloading $1"
		curl -fsSL -o "${f}" "https://deb.debian.org/debian/pool/main/g/gst-plugins-bad1.0/$1" ||
			curl -fsSL -o "${f}" "https://security.debian.org/debian-security/pool/updates/main/g/gst-plugins-bad1.0/$1"
	fi
	echo "$2  ${f}" | sha256sum -c --quiet
}

mkdir -p "${WORK}" "${OUT}"
fetch "gst-plugins-bad1.0_${UPSTREAM_VERSION}.orig.tar.xz" "${ORIG_SHA256}"
fetch "gst-plugins-bad1.0_${DEB_VERSION}.debian.tar.xz" "${DEBIAN_SHA256}"

log "Unpacking and patching"
SRC="${WORK}/src"
rm -rf "${SRC}"
mkdir -p "${SRC}"
tar -xf "${WORK}/gst-plugins-bad1.0_${UPSTREAM_VERSION}.orig.tar.xz" -C "${SRC}" --strip-components=1
tar -xf "${WORK}/gst-plugins-bad1.0_${DEB_VERSION}.debian.tar.xz" -C "${SRC}"
while read -r p; do
	[[ -z "${p}" || "${p}" == \#* ]] && continue
	patch -d "${SRC}" -p1 -s < "${SRC}/debian/patches/${p}"
done < "${SRC}/debian/patches/series"
# Upstream commits from the GStreamer monorepo: strip subprojects/gst-plugins-bad/.
for p in "${OUT}"/*.patch; do
	log "Applying ${p##*/}"
	patch -d "${SRC}" -p3 -s < "${p}"
done

if [[ ! -f "${SYSROOT}/usr/include/gstreamer-1.0/gst/gst.h" ]]; then
	log "Creating arm64 trixie sysroot"
	rm -rf "${SYSROOT}"
	mmdebstrap --quiet --architectures=arm64 --variant=minbase \
		--include=libgstreamer1.0-dev,libgstreamer-plugins-base1.0-dev,libgudev-1.0-dev,libdrm-dev,libglib2.0-dev \
		trixie "${SYSROOT}" \
		"deb http://deb.debian.org/debian trixie main" \
		"deb http://deb.debian.org/debian trixie-updates main" \
		"deb http://security.debian.org/debian-security trixie-security main"
fi

meson_list() { # words -> 'a', 'b', ...
	local out="" w
	for w in "$@"; do out+="${out:+, }'${w}'"; done
	echo "${out}"
}

# shellcheck disable=SC2086
cat > "${WORK}/cross.ini" <<-INI
	[binaries]
	c = '${TRIPLET}-gcc'
	cpp = '${TRIPLET}-g++'
	ar = '${TRIPLET}-ar'
	strip = '${TRIPLET}-strip'
	pkg-config = 'pkg-config'

	[built-in options]
	c_args = ['--sysroot=${SYSROOT}', $(meson_list ${KRAY_HARDEN_LIB_CFLAGS})]
	c_link_args = ['--sysroot=${SYSROOT}', $(meson_list ${KRAY_HARDEN_LIB_LDFLAGS})]

	[properties]
	sys_root = '${SYSROOT}'
	pkg_config_libdir = '${SYSROOT}/${LIBDIR}/pkgconfig:${SYSROOT}/usr/share/pkgconfig'

	[host_machine]
	system = 'linux'
	cpu_family = 'aarch64'
	cpu = 'cortex-a55'
	endian = 'little'
INI

log "Building the v4l2codecs plugin ${PKG_VERSION}"
export PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"
export PKG_CONFIG_LIBDIR="${SYSROOT}/${LIBDIR}/pkgconfig:${SYSROOT}/usr/share/pkgconfig"
B="${WORK}/build"
rm -rf "${B}"
meson setup "${B}" "${SRC}" --cross-file "${WORK}/cross.ini" --buildtype=release \
	-Dauto_features=disabled -Dv4l2codecs=enabled -Dgpl=disabled -Dorc=disabled \
	-Dexamples=disabled -Dtests=disabled -Dintrospection=disabled -Dnls=disabled >/dev/null
ninja -C "${B}" sys/v4l2codecs/libgstv4l2codecs.so >/dev/null

log "Packaging"
PKG=gstreamer1.0-v4l2codecs-kray
PLUGIN="/${LIBDIR}/gstreamer-1.0/libgstv4l2codecs.so"
STAGE="${WORK}/stage"
rm -rf "${STAGE}"
install -D -m 0644 "${B}/sys/v4l2codecs/libgstv4l2codecs.so" "${STAGE}${PLUGIN}"
"${TRIPLET}-strip" --strip-unneeded "${STAGE}${PLUGIN}"
install -D -m 0644 "${ROOT}/userpatches/gstreamer/"*.patch -t "${STAGE}/usr/share/doc/${PKG}/patches/"
mkdir -p "${STAGE}/DEBIAN"
cat > "${STAGE}/DEBIAN/control" <<-CTL
	Package: ${PKG}
	Version: ${PKG_VERSION}
	Architecture: arm64
	Maintainer: Kray-SoM-RKC OS <o.tyshchenko@eslua.com>
	Depends: gstreamer1.0-plugins-bad (>= ${UPSTREAM_VERSION}), gstreamer1.0-plugins-bad (<< 1.27), libgstreamer-plugins-bad1.0-0 (>= ${UPSTREAM_VERSION}), libgstreamer-plugins-bad1.0-0 (<< 1.27)
	Section: video
	Priority: optional
	Description: GStreamer V4L2 stateless decoders with the 1.26.4 zero-copy fix
	 The v4l2codecs plugin of gst-plugins-bad1.0 ${DEB_VERSION} with the
	 upstream fix from GStreamer 1.26.4 (MR !9315): right/bottom-cropped H.265
	 frames (1080p) are handed over without a CPU copy. Replaces the plugin of
	 gstreamer1.0-plugins-bad through dpkg-divert.
	 Built by rk3566-os scripts/build-gst-v4l2codecs.sh.
CTL
cat > "${STAGE}/DEBIAN/preinst" <<-SH
	#!/bin/sh
	set -e
	if [ "\$1" = install ] || [ "\$1" = upgrade ]; then
		dpkg-divert --package ${PKG} --rename --divert ${PLUGIN}.debian --add ${PLUGIN}
	fi
SH
cat > "${STAGE}/DEBIAN/postrm" <<-SH
	#!/bin/sh
	set -e
	if [ "\$1" = remove ] || [ "\$1" = purge ] || [ "\$1" = abort-install ]; then
		dpkg-divert --package ${PKG} --rename --remove ${PLUGIN}
	fi
SH
chmod 0755 "${STAGE}/DEBIAN/preinst" "${STAGE}/DEBIAN/postrm"
rm -f "${OUT}"/*.deb
dpkg-deb --root-owner-group -Zxz --build "${STAGE}" "${OUT}/${PKG}_${PKG_VERSION}_arm64.deb" >/dev/null
log "Packaged ${OUT}/${PKG}_${PKG_VERSION}_arm64.deb"
