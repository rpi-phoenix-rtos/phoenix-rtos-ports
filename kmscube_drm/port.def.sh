#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="kmscube_drm"
	version="0.0.1"
	desc="kmscube (upstream, MIT) on the new GPU lane: GBM + EGL + GLES3 on rpi4-kms / rpi4-v3d-async -- the lane's smoke test"

	# GitLab commit archive (checked identical to `git archive` of the pinned commit)
	commit="f60e50e887d3c49e91ac9b06d8199b36152632fa"
	source="https://gitlab.freedesktop.org/mesa/kmscube/-/archive/${commit}"
	archive_filename="kmscube-${commit}.tar.gz"
	src_path="kmscube-${commit}/"

	size="163714"
	sha256="e676b733d351d345b9cbf919dfaf84a5ef16e71f328c542f5614027bd4ca2e05"

	license="MIT"
	license_file="COPYING"

	conflicts="kmscube_drm!=${version}"
	depends="mesa_drm libdrm_phoenix zlib"

	# rootfs: install /bin/kmscube into the image
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/mesa-drm/build.sh (its kmscube part): the
# source list of kmscube's meson.build with GLES3 (shadertoy) and without libpng/gstreamer,
# mesa_drm's compat headers (compat/include + compat/app-include), the E7 link shape
# (libgallium whole-archive, the GBM/EGL/GLES archives in one group, -Wl,--wrap=mmap) and the
# same checks (new-lane symbols/strings present, old-lane strings absent).

p_prepare() {
	:
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local M="${PORT_DEP_mesa_drm}" LD="${PORT_DEP_libdrm_phoenix}" src="${PREFIX_PORT_WORKDIR}"
	local KSRCS=(common.c cube-smooth.c cube-gears.c cube-tex.c cube-shadertoy.c drm-atomic.c drm-common.c
		drm-legacy.c drm-offscreen.c esTransform.c frame-512x512-NV12.c frame-512x512-RGBA.c kmscube.c perfcntrs.c)
	local KFLAGS=(-O2 -g -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
		"${NL_TFLAGS[@]}" -DHAVE_GLES3 -I"${M}/compat/include" -I"${M}/compat/app-include" -I"${M}/gles/prefix/include"
		-I"${LD}/include" -I"${LD}/include/libdrm")
	local kobj="${PREFIX_PORT_BUILD}/kmscube-obj" f
	rm -rf "${kobj}"
	mkdir -p "${kobj}"
	for f in "${KSRCS[@]}"; do
		"${NL_CC}" "${KFLAGS[@]}" -c "${src}/${f}" -o "${kobj}/${f%.c}.o"
	done

	# link = gles/link-gles.txt: libgallium whole, then the GBM/EGL/GLESv2 archives (which bundle
	# their own copies of loader/util objects -- never pulled twice) and libdrm/compat/zlib in one
	# group. -Wl,--wrap=mmap: Mesa's BO maps do mmap(drm_fd, token), libdrm-phoenix resolves them.
	local gallium="" MA=() l out="${PREFIX_PORT_BUILD}/out"
	while IFS= read -r l; do
		case "${l}" in "--whole-archive "*) gallium="${l#--whole-archive }" ;; *) MA+=("${l}") ;; esac
	done < "${M}/gles/link-gles.txt"
	[ -f "${gallium}" ] || b_die "no libgallium in ${M}/gles/link-gles.txt"
	mkdir -p "${out}"
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,--wrap=mmap \
		-Wl,-Map,"${out}/kmscube.map" -o "${out}/kmscube" "${kobj}"/*.o \
		-Wl,--whole-archive "${gallium}" -Wl,--no-whole-archive \
		-Wl,--start-group "${MA[@]}" -Wl,--end-group -lm
	"${NL_STRIP}" -o "${out}/kmscube-stripped" "${out}/kmscube"

	nl_no_undefined "${out}/kmscube"
	local s bad=0
	for s in __wrap_mmap drmPhoenixMmap drm_phoenix_ioctl gbmint_get_backend v3d_drm_screen_create_renderonly \
			vc4_drm_screen_create kmsro_drm_screen_create; do
		nl_has_sym "${out}/kmscube" "${s}" || { echo "symbol ${s}: NO"; bad=1; }
	done
	# reported, as the tools build does (only the old-lane strings fail it)
	for s in /dev/kms /dev/v3d-async /kmsbuf /dev/dri/card0 'libdrm-phoenix:' kmsro v3d vc4 'V3D 4.2' EGL_KHR_platform_gbm; do
		echo "kmscube strings '${s}': $(nl_count_strings "${out}/kmscube-stripped" "${s}")"
	done
	nl_forbid_old_lane "${out}/kmscube-stripped"
	[ "${bad}" = 0 ] || b_die "kmscube: verification failed"

	local p="${PREFIX_PORT_INSTALL}"
	mkdir -p "${p}/bin" "${p}/prog"
	install -m 755 "${out}/kmscube" "${p}/prog/kmscube"
	install -m 755 "${out}/kmscube-stripped" "${p}/bin/kmscube"
	if b_use rootfs; then
		b_install "${p}/bin/kmscube" /bin
	fi
}
