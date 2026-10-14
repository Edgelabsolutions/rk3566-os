#!/usr/bin/env bash
# Cross-compile the Rockchip hardware video stack for the vendor kernel and
# package it as .deb files in userpatches/multimedia/:
#
#   rockchip-mpp          librockchip_mpp + mpi_* test tools + headers
#   librga-rockchip       librga (RGA 2D engine) + headers
#   ffmpeg-rockchip       static ffmpeg/ffprobe with *_rkmpp codecs and RGA filters
#   gstreamer1.0-rockchip GStreamer MPP/RGA plugins (mppvideodec, mpph264enc, ...)
#
# Builds run against an arm64 Debian trixie sysroot created with mmdebstrap,
# so the binaries link against the same library versions as the image.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ROOT}/.cache/multimedia"
OUT="${ROOT}/userpatches/multimedia"
SYSROOT="${WORK}/sysroot"
STAGE="${WORK}/stage"
TRIPLET=aarch64-linux-gnu
LIBDIR="lib/${TRIPLET}"
JOBS="$(nproc)"
# shellcheck source=../userpatches/hardening/build-flags.sh
. "${ROOT}/userpatches/hardening/build-flags.sh"

# Pinned sources: repository, branch, commit.
MPP_REPO=https://github.com/nyanmisaka/mpp;                MPP_BRANCH=jellyfin-mpp;        MPP_COMMIT=a9380ef333102ac318628f83b5f7a460d377749e
RGA_REPO=https://github.com/nyanmisaka/rk-mirrors;         RGA_BRANCH=jellyfin-rga;        RGA_COMMIT=1d330cc28551943bed3380261a5a9c6fbd58ff53
FFMPEG_REPO=https://github.com/nyanmisaka/ffmpeg-rockchip; FFMPEG_BRANCH=master;           FFMPEG_COMMIT=d90e3a1c18d7929383cf88c1b3da2e2d1c966cbf
GST_REPO=https://github.com/JeffyCN/mirrors;               GST_BRANCH=gstreamer-rockchip;  GST_COMMIT=a0d45af504099b4b82f3d3377019a63d357e7cef

PKG_VERSION="${PKG_VERSION:-1.0~kray1}"

log() { echo "==> $*"; }

fetch() { # name repo branch commit
	local dir="${WORK}/src/$1"
	if [[ ! -d "${dir}/.git" ]]; then
		log "Fetching $1 ($3 @ $4)"
		git -c advice.detachedHead=false clone -q --branch "$3" "$2" "${dir}"
	fi
	git -C "${dir}" -c advice.detachedHead=false checkout -q "$4"
	[[ "$(git -C "${dir}" rev-parse HEAD)" == "$4" ]] || { echo "$1: unexpected commit" >&2; exit 1; }
}

make_sysroot() {
	[[ -d "${SYSROOT}/usr/include/gstreamer-1.0" ]] && return 0
	log "Creating arm64 trixie sysroot"
	mmdebstrap --quiet --architectures=arm64 --variant=minbase \
		--include=libc6-dev,libdrm-dev,libgstreamer1.0-dev,libgstreamer-plugins-base1.0-dev,libglib2.0-dev \
		trixie "${SYSROOT}" http://deb.debian.org/debian
}

meson_list() { # words -> 'a', 'b', ...
	local out="" w
	for w in "$@"; do out+="${out:+, }'${w}'"; done
	echo "${out}"
}

write_cross_files() {
	cat > "${WORK}/toolchain.cmake" <<-EOF
		set(CMAKE_SYSTEM_NAME Linux)
		set(CMAKE_SYSTEM_PROCESSOR aarch64)
		set(CMAKE_C_COMPILER ${TRIPLET}-gcc)
		set(CMAKE_CXX_COMPILER ${TRIPLET}-g++)
		set(CMAKE_SYSROOT ${SYSROOT})
		set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
		set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
		set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
		set(CMAKE_C_FLAGS_INIT "${KRAY_HARDEN_LIB_CFLAGS}")
		set(CMAKE_CXX_FLAGS_INIT "${KRAY_HARDEN_LIB_CFLAGS}")
		set(CMAKE_SHARED_LINKER_FLAGS_INIT "${KRAY_HARDEN_LIB_LDFLAGS}")
		set(CMAKE_EXE_LINKER_FLAGS_INIT "${KRAY_HARDEN_LDFLAGS}")
		set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	EOF
	cat > "${WORK}/cross.ini" <<-EOF
		[binaries]
		c = '${TRIPLET}-gcc'
		cpp = '${TRIPLET}-g++'
		ar = '${TRIPLET}-ar'
		strip = '${TRIPLET}-strip'
		pkg-config = 'pkg-config'

		[built-in options]
		c_args = ['--sysroot=${SYSROOT}', $(meson_list ${KRAY_HARDEN_LIB_CFLAGS})]
		cpp_args = ['--sysroot=${SYSROOT}', $(meson_list ${KRAY_HARDEN_LIB_CFLAGS})]
		c_link_args = ['--sysroot=${SYSROOT}', $(meson_list ${KRAY_HARDEN_LIB_LDFLAGS})]
		cpp_link_args = ['--sysroot=${SYSROOT}', $(meson_list ${KRAY_HARDEN_LIB_LDFLAGS})]

		[properties]
		sys_root = '${SYSROOT}'
		pkg_config_libdir = '${SYSROOT}/usr/${LIBDIR}/pkgconfig:${SYSROOT}/usr/share/pkgconfig'

		[host_machine]
		system = 'linux'
		cpu_family = 'aarch64'
		cpu = 'cortex-a55'
		endian = 'little'
	EOF
}

export PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"
export PKG_CONFIG_LIBDIR="${SYSROOT}/usr/${LIBDIR}/pkgconfig:${SYSROOT}/usr/share/pkgconfig"

# Install a component both into its package staging dir and into the sysroot,
# so that later components find its headers, libraries and .pc files.
install_twice() { # component, then the install command using $DESTDIR
	local name="$1"; shift
	rm -rf "${STAGE:?}/${name}"
	DESTDIR="${STAGE}/${name}" "$@"
	DESTDIR="${SYSROOT}" "$@"
}

build_mpp() {
	log "Building MPP"
	local b="${WORK}/build/mpp"
	# Fresh build dir: CMake only applies the toolchain's *_FLAGS_INIT once.
	rm -rf "${b}"
	cmake -S "${WORK}/src/mpp" -B "${b}" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="${WORK}/toolchain.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DCMAKE_INSTALL_LIBDIR="${LIBDIR}" -DBUILD_TEST=ON >/dev/null
	ninja -C "${b}" -j "${JOBS}" >/dev/null
	install_twice mpp ninja -C "${b}" install >/dev/null
}

build_rga() {
	log "Building RGA"
	local b="${WORK}/build/rga"
	meson setup --wipe "${b}" "${WORK}/src/rga" --cross-file "${WORK}/cross.ini" \
		--prefix=/usr --libdir="${LIBDIR}" --buildtype=release \
		-Dcpp_args="--sysroot=${SYSROOT} ${KRAY_HARDEN_LIB_CFLAGS} -fpermissive" -Dlibdrm=false -Dlibrga_demo=false >/dev/null
	ninja -C "${b}" -j "${JOBS}" >/dev/null
	install_twice rga meson install -C "${b}" --no-rebuild --quiet
}

build_ffmpeg() {
	log "Building ffmpeg-rockchip"
	local b="${WORK}/build/ffmpeg"
	mkdir -p "${b}"
	(cd "${b}" && "${WORK}/src/ffmpeg/configure" \
		--prefix=/usr/local --enable-cross-compile --cross-prefix="${TRIPLET}-" \
		--arch=aarch64 --target-os=linux --sysroot="${SYSROOT}" --pkg-config=pkg-config \
		--enable-gpl --enable-version3 --enable-libdrm --enable-rkmpp --enable-rkrga \
		--enable-static --disable-shared --disable-doc --disable-debug --disable-ffplay \
		--extra-cflags="${KRAY_HARDEN_CFLAGS}" --extra-ldflags="${KRAY_HARDEN_LDFLAGS}" >/dev/null)
	make -C "${b}" -j "${JOBS}" >/dev/null
	rm -rf "${STAGE}/ffmpeg"
	mkdir -p "${STAGE}/ffmpeg/usr/local/bin"
	install -m 0755 "${b}/ffmpeg" "${b}/ffprobe" "${STAGE}/ffmpeg/usr/local/bin/"
	"${TRIPLET}-strip" "${STAGE}/ffmpeg/usr/local/bin/"*
}

build_gst() {
	log "Building gstreamer-rockchip"
	local b="${WORK}/build/gst"
	meson setup --wipe "${b}" "${WORK}/src/gst-rockchip" --cross-file "${WORK}/cross.ini" \
		--prefix=/usr --libdir="${LIBDIR}" --buildtype=release -Drockchipmpp=enabled \
		-Drga=enabled -Drkximage=disabled >/dev/null
	ninja -C "${b}" -j "${JOBS}" >/dev/null
	rm -rf "${STAGE}/gst"
	DESTDIR="${STAGE}/gst" meson install -C "${b}" --no-rebuild --quiet
}

make_deb() { # package stagedir depends description
	local pkg="$1" dir="$2" deps="$3" desc="$4"
	mkdir -p "${dir}/DEBIAN"
	cat > "${dir}/DEBIAN/control" <<-EOF
		Package: ${pkg}
		Version: ${PKG_VERSION}
		Architecture: arm64
		Maintainer: Kray-SoM-RKC OS <o.tyshchenko@eslua.com>
		Depends: ${deps}
		Section: video
		Priority: optional
		Description: ${desc}
		 Built by rk3566-os scripts/build-multimedia.sh for the Rockchip vendor kernel.
	EOF
	dpkg-deb --root-owner-group -Zxz --build "${dir}" "${OUT}/${pkg}_${PKG_VERSION}_arm64.deb" >/dev/null
	log "Packaged ${pkg}"
}

main() {
	mkdir -p "${WORK}/src" "${WORK}/build" "${STAGE}" "${OUT}"
	make_sysroot
	write_cross_files
	fetch mpp "${MPP_REPO}" "${MPP_BRANCH}" "${MPP_COMMIT}"
	fetch rga "${RGA_REPO}" "${RGA_BRANCH}" "${RGA_COMMIT}"
	fetch ffmpeg "${FFMPEG_REPO}" "${FFMPEG_BRANCH}" "${FFMPEG_COMMIT}"
	fetch gst-rockchip "${GST_REPO}" "${GST_BRANCH}" "${GST_COMMIT}"

	build_mpp
	build_rga
	build_ffmpeg
	build_gst

	rm -f "${OUT}"/*.deb
	make_deb rockchip-mpp "${STAGE}/mpp" "libc6" \
		"Rockchip Media Process Platform (MPP) library and test tools"
	make_deb librga-rockchip "${STAGE}/rga" "libc6, libstdc++6" \
		"Rockchip RGA 2D graphics acceleration library"
	make_deb ffmpeg-rockchip "${STAGE}/ffmpeg" "rockchip-mpp, librga-rockchip, libdrm2" \
		"FFmpeg with Rockchip MPP hardware codecs and RGA filters (in /usr/local/bin)"
	make_deb gstreamer1.0-rockchip "${STAGE}/gst" \
		"rockchip-mpp, librga-rockchip, libgstreamer1.0-0, libgstreamer-plugins-base1.0-0" \
		"GStreamer plugins for Rockchip MPP and RGA"
	ls -la "${OUT}"
}

main "$@"
