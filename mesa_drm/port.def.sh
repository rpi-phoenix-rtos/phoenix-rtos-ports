#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="mesa_drm"
	version="26.2.0"
	desc="Mesa 26.2 on the DRM path: gallium v3d/vc4+kmsro, GBM, EGL, GLES/GL, v3dv — static"

	# The release tarball (checked identical to `git archive mesa-26.2.0` = 9f0a761020b, the
	# base of the old lane's fork).
	source="https://archive.mesa3d.org"
	archive_filename="mesa-${version}.tar.xz"
	src_path="mesa-${version}/"

	size="68461648"
	sha256="efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef"

	license="MIT"
	license_file="docs/license.rst"

	# NEW GPU LANE: private install prefix (see libdrm_phoenix); the old lane's Mesa fork
	# (external/mesa + tools/.gpu-libs) is a different thing and stays untouched.
	conflicts="mesa_drm!=${version}"

	# USE flags select EXTRA builds of the same patched source, each in its own meson build
	# directory and install prefix (a static program links exactly one of them):
	#   (always)  gles/     GBM + EGL (drm, surfaceless) + GLES2/3          tools: mesa-drm/build-out
	#   opengl    gl/       the same + desktop GL + libglapi_bridge.a + the EGL wayland platform
	#                       (EGL on GBM for full screen AND on Wayland for a desktop window: the
	#                       SDL games and players, sdl2_kmsdrm)       tools: mesa-drm/build-out-wayland-gl
	#   wayland   wayland/  GLES + EGL wayland platform (dma-buf import)    tools: mesa-drm/build-out-wayland
	#   x11       x11/      GLES + EGL X11 platform (DRI3/Present)          tools: mesa-drm/build-out-x11
	#   vulkan    vulkan/   v3dv as a static ICD (no GL)                    tools: mesa-drm/build-out-vulkan
	iuse="opengl wayland x11 vulkan"
	depends="libdrm_phoenix zlib opengl? ( wayland_phoenix ) wayland? ( wayland_phoenix ) x11? ( xorg_libs libxshmfence_phoenix )"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/mesa-drm/build.sh: the same 16 patches
# (patches/, = tools/gpu-lane/mesa-drm/patches/mesa/), the same compat shim
# (glue/compat/, = tools/gpu-lane/mesa-drm/compat/), the same meson options and compiler
# flags per variant. kmscube (the default build's test program there) is the kmscube_drm port.
#
# What a consumer finds in ${PORT_DEP_mesa_drm}:
#   <v>/prefix/          `ninja install`: EGL/GLES/KHR/gbm(/GL, vulkan) headers, lib/*.a, *.pc
#   <v>/mesa-build       the meson build directory (a symlink into this port's work tree): the
#                        internal static libraries programs link (meson makes them THIN
#                        archives and bundles their objects into the installed ones, so a
#                        program links libgallium whole + the per-driver archives, see link.txt)
#   <v>/link-gles.txt    the archives a GLES program links, in order; the first line is
#                        "--whole-archive <libgallium-*.a>"; libdrm.a, libmesadrm-compat.a and
#                        libz.a are listed at the end (wayland/x11: + their platform archives)
#   gl/link-gl.txt       the same for desktop GL (libglapi_bridge.a instead of libGLESv2.a);
#                        gl/ has both lists, each with libwayland_drm.a; a gl/ program also links
#                        the Wayland client libraries (sdl2_kmsdrm's link-inputs.txt names them)
#   vulkan/link.txt      "--whole-archive <libvulkan_broadcom.a>", libdrm, compat, zlib
#   vulkan/phxvk/        phxvk_loader.{c,h}: the static Vulkan-loader stand-in (glue/phxvk/,
#                        = tools/gpu-lane/vulkan-drm/phxvk/), compiled into each Vulkan program
#   <v>/opengl.txt       opengl=true|false (the Mesa build's desktop-GL setting)
#   compat/libmesadrm-compat.a, compat/include (Mesa + apps), compat/app-include (apps only)
#   src-include/         the Mesa source tree's include/ (GL/, GLES*/, EGL/, KHR/, vulkan/)
#   zlib-prefix/, x11-prefix/ (x11)   the private dependency views Mesa was configured with
# Every program links -Wl,--wrap=mmap -Wl,--wrap=ioctl (libdrm-phoenix, see libdrm_phoenix).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

# the archives a GBM/EGL/GLES program links, in order (tools: mesa-drm build.sh A=(...))
_mesa_drm_gl_archives() {
	local a
	for a in src/egl/libEGL.a src/gbm/libgbm.a src/gbm/backends/dri/dri_gbm.a "$@" \
			src/mesa/glapi/shared-glapi/libglapi.a src/gallium/drivers/v3d/libv3d.a \
			src/gallium/drivers/v3d/libv3d-v42.a src/gallium/drivers/v3d/libv3d-v71.a \
			src/broadcom/libbroadcom-v42.a src/broadcom/libbroadcom-v71.a src/broadcom/qpu/libbroadcom_qpu.a \
			src/broadcom/libv3d_neon.a src/broadcom/perfcntrs/libv3d-perfcntrs-v42.a \
			src/broadcom/perfcntrs/libv3d-perfcntrs-v71.a src/gallium/winsys/kmsro/drm/libkmsrowinsys.a \
			src/gallium/winsys/v3d/drm/libv3dwinsys.a src/gallium/winsys/vc4/drm/libvc4winsys.a \
			src/gallium/winsys/sw/kms-dri/libswkmsdri.a src/gallium/winsys/sw/dri/libswdri.a \
			src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a \
			src/c11/impl/libmesa_util_c11.a; do
		echo "${a}"
	done
}

# _mesa_drm_variant <gles|gl|wayland|x11|vulkan>
_mesa_drm_variant() {
	local v="$1" out="${PREFIX_PORT_INSTALL}/$1" mb="${PREFIX_PORT_BUILD}/$1-build"
	local ldpc="${PORT_DEP_libdrm_phoenix}/lib/pkgconfig" zp="${PREFIX_PORT_INSTALL}/zlib-prefix"
	local libdir="${ldpc}:${zp}/lib/pkgconfig" opengl=false platforms="" api_opts=() extra_targets=()
	case "${v}" in
		gl | wayland)
			# gl: desktop GL + GLES, EGL on GBM (drm, surfaceless) and on Wayland -- one build
			# for an SDL program full screen on KMS and in a window of the desktop
			platforms=wayland
			[ "${v}" = gl ] && opengl=true
			# wayland_phoenix's libwayland/ view: libwayland alone (its prefix/ has libudev.pc,
			# which would turn on Mesa's HAVE_LIBUDEV)
			local wo="${PORT_DEP_wayland_phoenix:?}"
			wo="${wo%/}/libwayland"
			libdir="${libdir}:${wo}/lib/pkgconfig:${wo}/share/pkgconfig:${wo}/deps/libffi/lib/pkgconfig" ;;
		x11)
			# EGL on X11 through DRI3/Present (loader_dri3); GLX stays off, and so does
			# xlib-lease (a Vulkan extension that would pull libXrandr)
			platforms=x11
			api_opts+=(-Dxlib-lease=disabled)
			libdir="${libdir}:${PREFIX_PORT_INSTALL}/x11-prefix/lib/pkgconfig" ;;
	esac
	if [ "${v}" = vulkan ]; then
		# v3dv only. VK_KHR_display (wsi_common_display.c) is built whenever the system has
		# KMS/DRM (patch 0001 puts phoenix there); no x11/wayland platform.
		api_opts=(-Dgallium-drivers= -Dvulkan-drivers=broadcom -Dvulkan-layers= -Dvulkan-beta=false
			-Degl=disabled -Dgbm=disabled -Dglx=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled)
	else
		api_opts=(-Dgallium-drivers=v3d,vc4 -Dvulkan-drivers=
			-Degl=enabled -Dgbm=enabled -Dglx=disabled -Dopengl=${opengl} -Dgles1=disabled -Dgles2=enabled "${api_opts[@]}")
	fi
	[ "${opengl}" = true ] && extra_targets+=(src/mesa/glapi/glapi/libglapi_bridge.a)

	mkdir -p "${out}"
	nl_pkgconfig "${PREFIX_PORT_BUILD}/nl/pkg-config-${v}" "${libdir}"
	# compat/include goes on -I (never -include: a force-included header flips meson's probes,
	# E7 section 3.2). posix_memalign: a gcc builtin, so meson's __has_builtin probe passes
	# although libphoenix has no posix_memalign; with NO, Mesa's os_memory_aligned.h uses its
	# own over-allocating fallback.
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross-${v}.txt" "${PREFIX_PORT_BUILD}/nl/pkg-config-${v}" \
		"'-I${PREFIX_PORT}/glue/compat/include'" "'-L${PREFIX_BUILD%/}/lib'" "" \
		"has_function_posix_memalign = false"
	if [ ! -f "${mb}/build.ninja" ]; then
		meson setup "${mb}" "${PREFIX_PORT_WORKDIR}" --cross-file "${PREFIX_PORT_BUILD}/nl/cross-${v}.txt" \
			--prefix "${out}/prefix" --buildtype=debugoptimized -Db_ndebug=true --wrap-mode=nodownload \
			"${api_opts[@]}" -Dplatforms="${platforms}" \
			-Dllvm=disabled -Dspirv-tools=disabled -Dvideo-codecs= -Dgallium-va=disabled \
			-Dshader-cache=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dzstd=disabled \
			-Dlibunwind=disabled -Dvalgrind=disabled -Dlmsensors=disabled -Dperfetto=false \
			-Dbuild-tests=false -Dtools=
	fi
	ninja -C "${mb}" all "${extra_targets[@]}"
	ninja -C "${mb}" install
	ln -sfn "${mb}" "${out}/mesa-build"
	echo "opengl=${opengl}" > "${out}/opengl.txt"

	local tail_libs=("${PORT_DEP_libdrm_phoenix}/lib/libdrm.a" "${PREFIX_PORT_INSTALL}/compat/libmesadrm-compat.a"
		"${PORT_DEP_zlib}/lib/libz.a") a s
	if [ "${v}" = vulkan ]; then
		local icd="${out}/prefix/lib/libvulkan_broadcom.a"
		[ -f "${icd}" ] || b_die "${icd} not installed (patch 0010 missing?)"
		rm -rf "${out}/prefix/include/vulkan" "${out}/prefix/include/vk_video"
		mkdir -p "${out}/prefix/include"
		cp -a "${PREFIX_PORT_WORKDIR}/include/vulkan" "${PREFIX_PORT_WORKDIR}/include/vk_video" "${out}/prefix/include/"
		# The installed archive bundles meson's internal static libraries (vulkan runtime, wsi,
		# util, NIR, SPIR-V, broadcom compiler); it goes in whole (the generated dispatch
		# tables reference the entry points WEAKLY, and a weak reference pulls no member).
		printf '%s\n' "--whole-archive ${icd}" "${tail_libs[@]}" > "${out}/link.txt"
		# phxvk: the static stand-in for the Vulkan loader in front of this ICD (there is no
		# loader dlopen on Phoenix); a program compiles phxvk_loader.c itself (vkcube_drm,
		# vkquake_drm), as the tools builds do
		mkdir -p "${out}/phxvk"
		install -m 644 "${PREFIX_PORT}/glue/phxvk/phxvk_loader.c" "${PREFIX_PORT}/glue/phxvk/phxvk_loader.h" "${out}/phxvk/"
		for s in vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion v3dv_GetInstanceProcAddr \
				wsi_CreateDisplayPlaneSurfaceKHR wsi_display_init_wsi; do
			nl_has_sym "${icd}" "${s}" || b_die "${icd}: no ${s}"
		done
		return 0
	fi

	# link-gles.txt: GLES through libGLESv2 (stk-drm, yquake2-drm, kmscube, Xorg-drm's shape);
	# link-gl.txt (opengl builds): desktop GL through libglapi_bridge.a INSTEAD of libGLESv2
	# (whose gl* would clash; quakespasm-drm, quake3-drm). wayland/x11 add their platform
	# archives (+ the xcb/X11 archives and xshmfence for x11) to the GLES list.
	local extra=() gallium lists=(gles)
	case "${v}" in gl | wayland) extra=(src/egl/wayland/wayland-drm/libwayland_drm.a) ;; esac
	[ "${v}" = x11 ] && extra=(src/x11/libloader_x11.a)
	[ "${opengl}" = true ] && lists+=(gl)
	gallium="$(ls "${mb}"/src/gallium/targets/dri/libgallium-*.a)"
	local l
	for l in "${lists[@]}"; do
		{
			echo "--whole-archive ${gallium}"
			while IFS= read -r a; do
				if [ -f "${mb}/${a}" ]; then echo "${mb}/${a}"; else echo "mesa_drm: (archive not built: ${a})" >&2; fi
			done < <(
				# desktop GL: the bridge FIRST (where the tools links put it, right after
				# libSDL2.a), GLES: libGLESv2 after dri_gbm -- the member order decides the layout
				if [ "${l}" = gl ]; then echo src/mesa/glapi/glapi/libglapi_bridge.a; _mesa_drm_gl_archives
				else _mesa_drm_gl_archives src/mesa/glapi/es2api/libGLESv2.a; fi
				[ "${#extra[@]}" = 0 ] || printf '%s\n' "${extra[@]}")
			if [ "${v}" = x11 ]; then
				for a in X11-xcb X11 xcb-dri3 xcb-present xcb-sync xcb-xfixes xcb-randr xcb-shm xcb-render xcb-shape \
						xcb-keysyms xcb Xau Xdmcp; do
					[ -f "${PORT_DEP_xorg_libs}/lib/lib${a}.a" ] && echo "${PORT_DEP_xorg_libs}/lib/lib${a}.a"
				done
				echo "${PORT_DEP_libxshmfence_phoenix}/lib/libxshmfence.a"
			fi
			printf '%s\n' "${tail_libs[@]}"
		} > "${out}/link-${l}.txt"
	done
	nl_has_sym "${mb}/src/gbm/backends/dri/dri_gbm.a" gbmint_get_backend || b_die "${v}: dri_gbm.a has no gbmint_get_backend"
	nl_has_sym "${mb}/src/egl/libEGL.a" dri2_initialize_drm || b_die "${v}: libEGL.a has no dri2_initialize_drm"
	if [ "${opengl}" = true ]; then
		[ -f "${mb}/src/mesa/glapi/glapi/libglapi_bridge.a" ] || b_die "gl: libglapi_bridge.a not built"
	fi
	case "${v}" in
		gl | wayland) nl_has_sym "${mb}/src/egl/libEGL.a" dri2_initialize_wayland || b_die "${v}: no dri2_initialize_wayland" ;;
		x11) nl_has_sym "${mb}/src/egl/libEGL.a" dri2_initialize_x11 || b_die "x11: no dri2_initialize_x11" ;;
	esac
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"
	nl_host_python "${PREFIX_PORT_BUILD}/nl" mako yaml
	nl_has_libc sys_fdpath || b_die "${NL_SYSROOT}/lib/libphoenix.a has no sys_fdpath (stale sysroot)"

	# --- compat shim (stand-ins only while libphoenix lacks the symbol) ------------------------
	local c="${PREFIX_PORT_INSTALL}/compat" defs=()
	nl_has_libc open_memstream || defs+=(-DMESADRM_NEED_OPEN_MEMSTREAM)
	nl_has_libc getopt_long_only || defs+=(-DMESADRM_NEED_GETOPT_LONG_ONLY)
	nl_has_libc sincos || defs+=(-DMESADRM_NEED_SINCOS)
	rm -rf "${c}"
	mkdir -p "${c}"
	cp -a "${PREFIX_PORT}/glue/compat/include" "${PREFIX_PORT}/glue/compat/app-include" "${c}/"
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${PREFIX_PORT}/glue/compat/include" \
		-I"${PREFIX_PORT}/glue/compat/app-include" "${defs[@]}" -c "${PREFIX_PORT}/glue/compat/mesadrm_compat.c" \
		-o "${PREFIX_PORT_BUILD}/mesadrm_compat.o"
	"${NL_AR}" rcs "${c}/libmesadrm-compat.a" "${PREFIX_PORT_BUILD}/mesadrm_compat.o"
	echo "mesa_drm: compat stand-ins: ${defs[*]:-none}"

	# --- zlib (Mesa's one hard port dependency), through a private prefix -----------------------
	# The ports include/ also holds other ports' GL/ and X11/ headers: never on Mesa's search
	# path. (--wrap-mode=nodownload: without a zlib.pc meson silently builds its own zlib.)
	local zp="${PREFIX_PORT_INSTALL}/zlib-prefix" zver
	rm -rf "${zp}"
	mkdir -p "${zp}/include" "${zp}/lib/pkgconfig"
	cp "${PORT_DEP_zlib}/include/zlib.h" "${PORT_DEP_zlib}/include/zconf.h" "${zp}/include/"
	zver="$(sed -n 's/^#define ZLIB_VERSION "\(.*\)"/\1/p' "${zp}/include/zlib.h")"
	printf '%s\n' "Name: zlib" "Description: zlib from the Phoenix ports prefix" "Version: ${zver}" \
		"Libs: -L${PORT_DEP_zlib}/lib -lz" "Cflags: -I${zp}/include" > "${zp}/lib/pkgconfig/zlib.pc"

	# --- X11/xcb for x11 (same reason: a private prefix with only X11/ and xcb/ headers; the
	# .pc files are the ports' own with includedir moved here, libdir stays the ports lib/) ------
	if b_use x11; then
		local xp="${PREFIX_PORT_INSTALL}/x11-prefix" pc xl="${PORT_DEP_xorg_libs}" xs="${PORT_DEP_libxshmfence_phoenix}"
		rm -rf "${xp}"
		mkdir -p "${xp}/include" "${xp}/lib/pkgconfig"
		cp -a "${xl}/include/X11" "${xl}/include/xcb" "${xp}/include/"
		for pc in "${xl}"/lib/pkgconfig/xcb*.pc "${xl}"/lib/pkgconfig/x11*.pc "${xl}"/lib/pkgconfig/xau.pc \
				"${xl}"/lib/pkgconfig/xdmcp.pc "${xl}"/lib/pkgconfig/xext.pc "${xl}"/lib/pkgconfig/xrandr.pc \
				"${xl}"/lib/pkgconfig/xrender.pc "${xl}"/lib/pkgconfig/pthread-stubs.pc \
				"${xl}"/share/pkgconfig/*proto.pc; do
			sed -e "s|^prefix=.*|prefix=${xp}|" -e "s|^libdir=.*|libdir=${xl}/lib|" \
				-e "s|^includedir=.*|includedir=${xp}/include|" "${pc}" > "${xp}/lib/pkgconfig/$(basename "${pc}")"
		done
		cp "${xs}/include/X11/xshmfence.h" "${xp}/include/X11/"
		printf '%s\n' "Name: xshmfence" "Description: X shared memory fences (${xs})" "Version: 1.3.2" \
			"Libs: ${xs}/lib/libxshmfence.a" "Cflags: -I${xp}/include" > "${xp}/lib/pkgconfig/xshmfence.pc"
	fi

	# the source tree's include/ (GL, GLES, EGL, KHR, vulkan): what the tools builds put on
	# their programs' -I as <mesa out>/mesa-src/include (quakespasm_drm)
	rm -rf "${PREFIX_PORT_INSTALL}/src-include"
	cp -a "${PREFIX_PORT_WORKDIR}/include" "${PREFIX_PORT_INSTALL}/src-include"

	_mesa_drm_variant gles
	if b_use opengl; then _mesa_drm_variant gl; fi
	if b_use wayland; then _mesa_drm_variant wayland; fi
	if b_use x11; then _mesa_drm_variant x11; fi
	if b_use vulkan; then _mesa_drm_variant vulkan; fi
}
