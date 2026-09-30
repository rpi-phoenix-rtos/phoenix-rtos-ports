#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libdrm_phoenix"
	# upstream libdrm 2.4.134-16-gb97cbde (main, 16 commits after the 2.4.134 tag); the
	# resolver needs a dotted version, the exact commit is in `commit`/archive_filename.
	version="2.4.134"
	desc="libdrm (upstream) + the Phoenix-RTOS backend: DRM ioctls served by rpi4-kms / rpi4-v3d-async"

	# GitLab commit archive (content-addressed by the commit; checked byte-identical to
	# `git archive b97cbde` of the freedesktop repository).
	commit="b97cbde15c5c3abfe44d78e8f57139e50f612fec"
	source="https://gitlab.freedesktop.org/mesa/drm/-/archive/${commit}"
	archive_filename="drm-${commit}.tar.gz"
	src_path="libdrm-${commit}/"

	size="570781"
	sha256="379d0881f6e7509ba5c0dca6ce045d7e68ffcbb4bed4cee5b598ab843fcb8cb2"

	# libdrm is MIT; the Phoenix backend (glue/phoenix/) is our code, BSD-3-Clause.
	license="MIT AND BSD-3-Clause"
	license_file="LICENSES/MIT.txt"

	# NEW GPU LANE: conflicting with its own other versions only gives the port a private
	# install prefix (versioned-ports/libdrm_phoenix-<ver>/): nothing of the new lane may
	# land in the shared ports prefix the old lane's ports compile and link from.
	conflicts="libdrm_phoenix!=${version}"
	depends=""

	# rootfs: also install /bin/drmprobe into the image (opt-in; see gpu_lane.md)
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/libdrm-phoenix/build.sh (same patches,
# same backend sources, same meson options and compiler flags). What this port builds:
#
#   ${PREFIX_PORT_INSTALL}/lib/libdrm.a          upstream libdrm + the Phoenix backend
#   ${PREFIX_PORT_INSTALL}/include/{xf86drm*.h, libdrm/...}, lib/pkgconfig/libdrm.pc
#   ${PREFIX_PORT_INSTALL}/share/phoenix-newlane/   the build helpers every new-lane port uses
#   ${PREFIX_PORT_INSTALL}/bin/drmprobe          the probe (stripped; unstripped in prog/)
#
# Every program linking libdrm.a must be linked -Wl,--wrap=mmap -Wl,--wrap=ioctl: the
# backend resolves BO-token mmap()s and emulates the sync_file ioctls in-process.
#
# The backend (glue/phoenix/) and the two server wire headers it speaks (v3da_proto.h of
# rpi4-v3d-async, kms_proto.h of rpi4-kms) are vendored copies of the coordination repo's
# tools/gpu-lane/{libdrm-phoenix/src,libdrm-phoenix/include,v3d-async,kms}; the copies are
# kept identical by scripts/check-gpu-lane-ports-sync.sh there. They move with the
# servers when those move to phoenix-rtos-devices (MIGRATION.md section 4 item 7).

p_prepare() {
	# 0001 drm.h/drm_mode.h: Phoenix ioctl layout; 0002 xf86drm.c: backend hooks;
	# 0003 meson: build phoenix/ when host_machine.system() == 'phoenix';
	# 0004 meson: the ioctl interposer (__wrap_ioctl, sync_file emulation).
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
	mkdir -p "${PREFIX_PORT_WORKDIR}/phoenix"
	cp -a "${PREFIX_PORT}/glue/phoenix/." "${PREFIX_PORT_WORKDIR}/phoenix/"
}

p_build() {
	# shellcheck source=libdrm_phoenix/glue/newlane.subr
	. "${PREFIX_PORT}/glue/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	# pkg-config sees nothing: libdrm needs no library of the ports prefix here
	nl_pkgconfig "${PREFIX_PORT_BUILD}/nl/pkg-config" "/nonexistent"
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross.txt" "${PREFIX_PORT_BUILD}/nl/pkg-config" "" ""

	local bd="${PREFIX_PORT_BUILD}/build"
	if [ ! -f "${bd}/build.ninja" ]; then
		meson setup "${bd}" "${PREFIX_PORT_WORKDIR}" --cross-file "${PREFIX_PORT_BUILD}/nl/cross.txt" \
			--prefix "${PREFIX_PORT_INSTALL}" --libdir lib --buildtype=debugoptimized \
			-Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
			-Domap=disabled -Dexynos=disabled -Dtegra=disabled -Dvc4=enabled -Detnaviv=disabled \
			-Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dudev=false -Dtests=false \
			-Dinstall-test-programs=false
	fi
	ninja -C "${bd}"
	ninja -C "${bd}" install

	# the shared build helpers, for the ports that depend on this one (their
	# `depends` makes a change here rebuild them: port_manager's staleness propagates)
	mkdir -p "${PREFIX_PORT_INSTALL}/share/phoenix-newlane"
	install -m 644 "${PREFIX_PORT}/glue/newlane.subr" "${PREFIX_PORT_INSTALL}/share/phoenix-newlane/"

	local a="${PREFIX_PORT_INSTALL}/lib/libdrm.a" s
	for s in drmIoctl drm_phoenix_ioctl drmPhoenixMmap __wrap_mmap __wrap_ioctl; do
		nl_has_sym "${a}" "${s}" || b_die "libdrm.a has no ${s} (backend not built in?)"
	done

	# --- drmprobe -----------------------------------------------------------------------------
	local o="${PREFIX_PORT_BUILD}/drmprobe-obj" p="${PREFIX_PORT_INSTALL}"
	mkdir -p "${o}" "${p}/bin" "${p}/prog"
	local cf=(-O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${PREFIX_PORT}/glue/phoenix"
		-I"${p}/include" -I"${p}/include/libdrm" -I"${PREFIX_PORT}/glue/drmprobe")
	"${NL_CC}" "${cf[@]}" -c "${PREFIX_PORT}/glue/drmprobe/drmprobe.c" -o "${o}/drmprobe.o"
	"${NL_CC}" "${cf[@]}" -c "${PREFIX_PORT}/glue/drmprobe/v3da_clgen.c" -o "${o}/v3da_clgen.o"
	"${NL_CC}" --sysroot="${NL_SYSROOT}/" -B"${NL_SYSROOT}/lib/" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 \
		-Wl,--wrap=mmap -Wl,--wrap=ioctl -o "${p}/prog/drmprobe" "${o}/drmprobe.o" "${o}/v3da_clgen.o" "${a}"
	"${NL_STRIP}" -o "${p}/bin/drmprobe" "${p}/prog/drmprobe"
	nl_no_undefined "${p}/prog/drmprobe"

	if b_use rootfs; then
		b_install "${p}/bin/drmprobe" /bin
	fi
}
