#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xorg_server_drm"
	version="21.1.24"
	desc="Xorg-drm: X.Org server (hw/xfree86) + modesetting + glamor on GBM/EGL + DRI3/Present, static; the startx desktop launcher"

	source="https://www.x.org/releases/individual/xserver/"
	archive_filename="xorg-server-${version}.tar.xz"
	src_path="xorg-server-${version}/"

	size="5072780"
	sha256="1a4eb36ca65cc3b1b936566d677a9786e13c11cd5806e951ac55f3f5ce3984af"

	# X.Org MIT; libxcvt (built in here) MIT; our glue (glue/src, glue/compat) BSD-3-Clause;
	# glue/libmd (SHA1, built in here) public domain
	license="MIT AND BSD-3-Clause"
	license_file="COPYING"

	# Private install prefix (see libdrm_phoenix).
	conflicts="xorg_server_drm!=${version}"

	# rootfs:   install /bin/Xorg-drm, /bin/startx (the desktop launcher) and
	#           /etc/X11/xorg-drm.conf into the image
	# x11demo:  also build eglx11-demo, the GLES-in-an-X-window DRI3/Present client
	#           (tools/gpu-lane/x11-drm): pulls mesa_drm's x11 build. Folded in here because a
	#           framework port needs an upstream archive and the demo is our source only.
	iuse="rootfs x11demo"

	# The X11 libraries come from the shared ports prefix (archives + .pc; pixman, xkbfile,
	# Xau, xtrans, xorgproto: xorg_libs; Xfont2, fontenc, freetype: xorg_fonts). libmd (SHA1
	# for -Dsha1=libmd: glue/libmd) and libxcvt are built in here.
	depends="libdrm_phoenix mesa_drm libepoxy libxshmfence_phoenix xorg_libs xorg_fonts zlib x11demo? ( mesa_drm[x11] )"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/xorg-drm/build.sh in the configuration of
# the Pi-proven Xorg-drm-noshim (m4n-noshim; build-out-noshim): the default mesa_drm GLES
# build (glamor on GLES), the Phoenix-RTOS xshmfence backend (libxshmfence_phoenix; the tools'
# --xshmfence-prefix x11-drm/build-out/xshmfence-prefix), libdrm_phoenix. Same 8 xorg-server
# patches, same meson options, the modesetting driver's own compile flags for our objects,
# the same hand link and checks. eglx11-demo (x11demo) = tools/gpu-lane/x11-drm/build.sh's.
#
#   ${PREFIX_PORT_INSTALL}/bin/Xorg-drm        stripped;  prog/Xorg-drm + Xorg-drm.map (addr2line)
#   ${PREFIX_PORT_INSTALL}/bin/startx          the desktop launcher (bash script, glue/pi/)
#   ${PREFIX_PORT_INSTALL}/etc/X11/xorg-drm.conf
#   ${PREFIX_PORT_INSTALL}/bin/eglx11-demo     (x11demo) stripped; prog/eglx11-demo
# Not ported: pi/xorg-drm-m4a.sh (a one-off pre-registered Pi-cycle script, not a launcher).

p_prepare() {
	# 0001 static module archives, 0002 builtin module table (no dlopen), 0003 builtin XKB
	# keymap, 0004 larger client receive buffer, 0005 os-support for Phoenix-RTOS,
	# 0006 modesetting without libpciaccess, 0007 glamor_glx.c only with GLX epoxy,
	# 0008 framebuffer-slot probe without libpciaccess.
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	# libxcvt 0.1.2 (MIT): VESA CVT modelines; one source file
	b_port_download "https://www.x.org/releases/individual/lib/" "libxcvt-0.1.2.tar.xz"
	echo "0561690544796e25cfbd71806ba1b0d797ffe464e9796411123e79450f71db38  ${PREFIX_PORT}/libxcvt-0.1.2.tar.xz" \
		| sha256sum -c --quiet - || b_die "libxcvt-0.1.2.tar.xz: sha256 mismatch"
	if [ ! -d "${PREFIX_PORT_BUILD}/libxcvt-0.1.2" ]; then
		tar xJf "${PREFIX_PORT}/libxcvt-0.1.2.tar.xz" -C "${PREFIX_PORT_BUILD}"
	fi
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local B="${PORT_DEP_xorg_libs%/}" M="${PORT_DEP_mesa_drm}" LDP="${PORT_DEP_libdrm_phoenix}"
	local EPX="${PORT_DEP_libepoxy}" SHMF="${PORT_DEP_libxshmfence_phoenix}"
	local MESA_PREFIX="${M}/gles/prefix" MB="${M}/gles/mesa-build" COMPAT_INC="${PREFIX_PORT}/glue/compat/include"
	local XS="${PREFIX_PORT_WORKDIR%/}" XB="${PREFIX_PORT_BUILD}/xorg-build" DP="${PREFIX_PORT_BUILD}/deps-prefix"
	local P="${PREFIX_PORT_INSTALL}" OBJ="${PREFIX_PORT_BUILD}/obj" p

	# The DRM_CAP_PRIME fix (mesa_drm patch 0008): without it u_init_pipe_screen_caps() never
	# asks for PRIME, caps.dmabuf stays 0, GBM makes scan-out buffers without a DRI image and
	# Mesa NULL-dereferences in dri2_allocate_textures -- glamor on GBM allocates exactly that
	# way. A call to drmGetCap in the function (its CALL26 relocation) = fixed.
	local u_screen_o n_getcap=0
	u_screen_o="$(find -L "${MB}/src" -name '*u_screen.c.o' 2>/dev/null | head -1)"
	[ -n "${u_screen_o}" ] && n_getcap=$("${NL_OBJDUMP}" -dr "${u_screen_o}" \
		| awk '/<u_init_pipe_screen_caps>:/,/^$/' | grep -c 'CALL26.*drmGetCap' || true)
	[ "${n_getcap}" -ge 1 ] || b_die "${MB} lacks the DRM_CAP_PRIME fix (mesa_drm patch 0008)"
	echo "xorg_server_drm: Mesa DRM_CAP_PRIME query present (${n_getcap} call(s) to drmGetCap in u_init_pipe_screen_caps)"

	for p in "${B}/lib/libpixman-1.a" "${B}/lib/libXfont2.a" "${B}/lib/libxkbfile.a" \
			"${B}/share/pkgconfig/xproto.pc" "${B}/share/pkgconfig/xtrans.pc" \
			"${MESA_PREFIX}/lib/libEGL.a" "${MESA_PREFIX}/lib/libgbm.a" "${LDP}/lib/libdrm.a" \
			"${M}/zlib-prefix/lib/pkgconfig/zlib.pc" "${EPX}/lib/libepoxy.a" "${SHMF}/lib/libxshmfence.a"; do
		[ -e "${p}" ] || b_die "missing ${p}"
	done

	# The X server's config scanner reads numbers with isdigit(c = buf[pos++]); libphoenix's
	# ctype macros used to evaluate their argument more than once, which parsed
	# "DefaultDepth 24" as 2 (m4b). libphoenix 156422a fixed them at source; refuse a sysroot
	# that predates the fix.
	local ctype_probe
	ctype_probe="$(printf '#include <ctype.h>\nint f(const char *p) { return isdigit(*p++); }\n' \
		| "${NL_CC}" "${NL_TFLAGS[@]}" -I"${COMPAT_INC}" -E -P -x c - 2>/dev/null | sed -n '/^int f(/,$p' | tr -s ' \n' ' ')"
	[ "$(grep -o '\*p++' <<< "${ctype_probe}" | wc -l)" -eq 1 ] \
		|| b_die "sysroot <ctype.h> evaluates the argument more than once (need libphoenix >= 156422a): ${ctype_probe}"

	# --- libxcvt: one source file, and its meson.build hard-codes shared_library(): compile it
	local xcvt="${PREFIX_PORT_BUILD}/libxcvt-0.1.2"
	rm -rf "${DP}"
	mkdir -p "${DP}/lib/pkgconfig" "${DP}/include/libxcvt" "${PREFIX_PORT_BUILD}/xcvt-obj"
	"${NL_CC}" -O2 -g -Wall "${NL_TFLAGS[@]}" -I"${xcvt}/include" \
		-c "${xcvt}/lib/libxcvt.c" -o "${PREFIX_PORT_BUILD}/xcvt-obj/libxcvt.o"
	"${NL_AR}" rcs "${DP}/lib/libxcvt.a" "${PREFIX_PORT_BUILD}/xcvt-obj/libxcvt.o"
	cp "${xcvt}/include/libxcvt/"*.h "${DP}/include/libxcvt/"
	# shellcheck disable=SC2016 # ${prefix}-style variables are pkg-config's
	printf '%s\n' "prefix=${DP}" 'libdir=${prefix}/lib' 'includedir=${prefix}/include' '' \
		'Name: libxcvt' 'Description: VESA CVT modelines (static)' "Version: 0.1.2" \
		'Libs: -L${libdir} -lxcvt -lm' 'Cflags: -I${includedir}' > "${DP}/lib/pkgconfig/libxcvt.pc"

	# --- libmd: SHA1 with the BSD libmd entry points (SHA1Init/Update/Final), one source file.
	# meson's -Dsha1=libmd finds it by cc.find_library('md') (the -L below), os/xsha1.c
	# includes <sha1.h> (the -I below); the hand link names the archive.
	mkdir -p "${PREFIX_PORT_BUILD}/md-obj"
	"${NL_CC}" -O2 -g -Wall "${NL_TFLAGS[@]}" -c "${PREFIX_PORT}/glue/libmd/sha1.c" -o "${PREFIX_PORT_BUILD}/md-obj/sha1.o"
	"${NL_AR}" rcs "${DP}/lib/libmd.a" "${PREFIX_PORT_BUILD}/md-obj/sha1.o"
	cp "${PREFIX_PORT}/glue/libmd/sha1.h" "${DP}/include/"

	# The pkg-config every configure step sees: only these prefixes, in this order (never the
	# shared sysroot; the ports prefix only for the X11 libraries -- its include/ must have no
	# GL/EGL/GLES/gbm/drm headers that could shadow ours, checked below). The tools build had
	# libxcvt, xshmfence and libepoxy in one deps prefix; here they are three prefixes, in that
	# place of the order.
	local libdir="${DP}/lib/pkgconfig:${SHMF}/lib/pkgconfig:${EPX}/lib/pkgconfig:${LDP}/lib/pkgconfig"
	libdir="${libdir}:${MESA_PREFIX}/lib/pkgconfig:${M}/zlib-prefix/lib/pkgconfig:${B}/lib/pkgconfig:${B}/share/pkgconfig"
	nl_pkgconfig "${PREFIX_PORT_BUILD}/nl/pkg-config" "${libdir}"
	local h
	for h in EGL GLES2 GLES3 KHR gbm.h xf86drm.h libdrm epoxy; do
		[ ! -e "${B}/include/${h}" ] || b_die "${B}/include/${h} would shadow the new-lane headers"
	done
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross.txt" "${PREFIX_PORT_BUILD}/nl/pkg-config" \
		"'-I${COMPAT_INC}', '-I${DP}/include'" "'-L${DP}/lib', '-L${B}/lib'"

	# --- xorg-server configure + build (static archives; the executable is linked below) ------
	local deps_stamp
	deps_stamp="$(printf '%s\n' "${M}" "$(sha256sum "${LDP}/lib/libdrm.a" "${EPX}/lib/libepoxy.a" \
		"${PREFIX_PORT}/glue/libmd/sha1.c" "${PREFIX_PORT}/glue/libmd/sha1.h" | cut -c1-64)" | sha256sum | cut -c1-16)"
	if [ "$(cat "${PREFIX_PORT_BUILD}/xorg-deps.stamp" 2>/dev/null || true)" != "${deps_stamp}" ]; then
		rm -rf "${XB}"   # headers/pkg-config paths of another Mesa or libdrm: reconfigure
	fi
	if [ ! -f "${XB}/build.ninja" ]; then
		meson setup "${XB}" "${XS}" --cross-file "${PREFIX_PORT_BUILD}/nl/cross.txt" \
			--prefix /usr --sysconfdir /etc --localstatedir /var \
			--buildtype=debugoptimized -Db_ndebug=false -Db_staticpic=false --wrap-mode=nodownload \
			-Dxorg=true -Dxephyr=false -Dxnest=false -Dxvfb=false -Dxwin=false -Dxquartz=false \
			-Dglamor=true -Dglx=false -Ddri1=false -Ddri2=true -Ddri3=true -Ddrm=true \
			-Dudev=false -Dudev_kms=false -Dhal=false -Dsystemd_logind=false -Dpciaccess=false \
			-Dint10=false -Dvgahw=false -Ddga=false -Dagp=false -Dlinux_apm=false -Dlinux_acpi=false \
			-Dxdmcp=false -Dxdm-auth-1=false -Dsecure-rpc=false -Dxselinux=false -Dxcsecurity=false \
			-Dmitshm=false -Dxv=true -Dxvmc=false -Dxinerama=false -Dxf86-input-inputtest=false \
			-Dsha1=libmd -Dinput_thread=false -Dlisten_tcp=false -Dsuid_wrapper=false -Dlibunwind=false \
			-Ddocs=false -Ddevel-docs=false -Ddocs-pdf=false \
			-Ddefault_font_path=/usr/share/fonts/X11/misc,/usr/share/fonts/X11/75dpi \
			-Dxkb_dir=/usr/share/X11/xkb -Dxkb_output_dir=/tmp -Dlog_dir=/tmp \
			-Dfallback_input_driver=phxhid \
			-Dbuilder_string="Phoenix-RTOS (Xorg-drm)"
		echo "${deps_stamp}" > "${PREFIX_PORT_BUILD}/xorg-deps.stamp"
	fi
	# Every static library the server + the builtin modules need; the meson Xorg executable
	# itself is never linked (Xorg-drm is linked below with the modules).
	local xtargets
	xtargets=$(ninja -C "${XB}" -t targets all | sed -n 's/^\([^:]*\.a\): .*/\1/p' | sort -u)
	# shellcheck disable=2086
	ninja -C "${XB}" -k 0 ${xtargets}

	# --- Xorg-drm's own objects: builtin-module table, phxhid input driver, compat ------------
	# Compiled exactly like the modesetting driver (a module of this server): its command from
	# meson's compile database, minus the output/dependency arguments.
	rm -rf "${OBJ}"
	mkdir -p "${OBJ}"
	local xcflags=()
	mapfile -t xcflags < <(python3 - "${XB}/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('hw/xfree86/drivers/modesetting/driver.c'):
        a = shlex.split(e['command'])[1:]
        out, skip = [], False
        for x in a:
            if skip: skip = False; continue
            if x in ('-o', '-MQ', '-MF', '-c'): skip = True; continue
            if x in ('-MD',) or x.endswith('driver.c') or x.startswith('-fdiagnostics-color'): continue
            out.append(x)
        print('\n'.join(out)); break
PY
)
	[ "${#xcflags[@]}" -gt 10 ] || b_die "no compile command for modesetting driver.c in ${XB}"
	local f
	for f in xorg_drm_builtin.c phxhid.c; do
		( cd "${XB}" && "${NL_PHXCC}" "${xcflags[@]}" -Wextra -Wno-unused-parameter -Wno-sign-compare \
			-Wno-missing-field-initializers -Werror -I"${PREFIX_PORT}/glue/src" -c "${PREFIX_PORT}/glue/src/${f}" \
			-o "${OBJ}/${f%.c}.o" ) || b_die "${f} compile failed"
	done
	# libphoenix gaps that only show at link time (stand-ins compiled only while libphoenix
	# lacks the symbol; see glue/compat/xorg_drm_compat.c for the list)
	local compat_defs=() fn
	for fn in ${XORG_DRM_COMPAT_FNS:-}; do
		nl_has_libc "${fn}" || compat_defs+=(-DXORG_DRM_NEED_"$(tr '[:lower:]' '[:upper:]' <<< "${fn}")")
	done
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${COMPAT_INC}" "${compat_defs[@]}" \
		-c "${PREFIX_PORT}/glue/compat/xorg_drm_compat.c" -o "${OBJ}/xorg_drm_compat.o"
	echo "xorg_server_drm: compat stand-ins: ${compat_defs[*]:-none}"

	# --- link -------------------------------------------------------------------------------
	local mesa_a=(src/egl/libEGL.a src/gbm/libgbm.a src/gbm/backends/dri/dri_gbm.a src/mesa/glapi/es2api/libGLESv2.a
		src/mesa/glapi/shared-glapi/libglapi.a src/mesa/glapi/glapi/libglapi_bridge.a
		src/gallium/drivers/v3d/libv3d.a src/gallium/drivers/v3d/libv3d-v42.a src/gallium/drivers/v3d/libv3d-v71.a
		src/broadcom/libbroadcom-v42.a src/broadcom/libbroadcom-v71.a src/broadcom/qpu/libbroadcom_qpu.a
		src/broadcom/libv3d_neon.a src/broadcom/perfcntrs/libv3d-perfcntrs-v42.a
		src/broadcom/perfcntrs/libv3d-perfcntrs-v71.a src/gallium/winsys/kmsro/drm/libkmsrowinsys.a
		src/gallium/winsys/v3d/drm/libv3dwinsys.a src/gallium/winsys/vc4/drm/libvc4winsys.a
		src/gallium/winsys/sw/kms-dri/libswkmsdri.a src/gallium/winsys/sw/dri/libswdri.a
		src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a
		src/c11/impl/libmesa_util_c11.a)
	local ma=() a gallium
	for a in "${mesa_a[@]}"; do [ -f "${MB}/${a}" ] && ma+=("${MB}/${a}"); done
	gallium="$(ls "${MB}"/src/gallium/targets/dri/libgallium-*.a)"
	local xsrv=("${XB}/hw/xfree86/libxorgserver_static.a")
	local xlibc=("${XB}/os/liblibxlibc.a")   # the server's own fallbacks for functions libc lacks
	local xmod=("${XB}/hw/xfree86/drivers/modesetting/libmodesetting_drv.a" "${XB}/hw/xfree86/glamor_egl/libglamoregl.a"
		"${XB}/hw/xfree86/dixmods/libshadow.a")
	mkdir -p "${P}/bin" "${P}/prog" "${P}/etc/X11"
	local X="${P}/prog/Xorg-drm"
	# The server archive goes in whole (as meson's link_whole for the Xorg executable: modules
	# resolve server symbols the server itself never calls); Mesa's gallium target whole as for
	# kmscube; --gc-sections drops what nobody references. -Wl,--wrap=mmap: libdrm-phoenix's
	# __wrap_mmap resolves the MAP_DUMB/MMAP_BO tokens of modesetting's dumb buffers, GBM's and
	# Mesa's BO maps (M3 section 2.6).
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,--wrap=mmap \
		-Wl,-Map,"${X}.map" -o "${X}" "${OBJ}"/xorg_drm_builtin.o "${OBJ}"/phxhid.o \
		-Wl,--whole-archive "${xsrv[@]}" "${gallium}" -Wl,--no-whole-archive \
		-Wl,--start-group "${xmod[@]}" "${xlibc[@]}" "${EPX}/lib/libepoxy.a" "${DP}/lib/libxcvt.a" "${SHMF}/lib/libxshmfence.a" \
		"${ma[@]}" "${LDP}/lib/libdrm.a" "${M}/compat/libmesadrm-compat.a" "${OBJ}/xorg_drm_compat.o" \
		"${B}/lib/libpixman-1.a" "${B}/lib/libXfont2.a" "${B}/lib/libfontenc.a" "${B}/lib/libfreetype.a" \
		"${B}/lib/libxkbfile.a" "${B}/lib/libXau.a" "${DP}/lib/libmd.a" "${PORT_DEP_zlib}/lib/libz.a" \
		-Wl,--end-group -lm > "${PREFIX_PORT_BUILD}/Xorg-drm-link.log" 2>&1 \
		|| { grep -v '^/.*: warning: ' "${PREFIX_PORT_BUILD}/Xorg-drm-link.log" | head -80; b_die "Xorg-drm link failed"; }
	"${NL_STRIP}" -o "${P}/bin/Xorg-drm" "${X}"
	echo "xorg_server_drm: Xorg-drm $(stat -c %s "${X}") bytes; stripped $(stat -c %s "${P}/bin/Xorg-drm") bytes"

	# --- verification -------------------------------------------------------------------------
	local und syms s n bad=0
	"${NL_SIZE}" "${X}"
	und="$("${NL_NM}" -u "${X}" || true)"
	echo "xorg_server_drm: undefined symbols (nm -u): $(grep -c . <<< "${und}" || true)"
	[ -n "${und}" ] && head -20 <<< "${und}"
	syms="$("${NL_NM}" "${X}")"
	for s in modesettingModuleData glamoreglModuleData shadowModuleData phxhidModuleData xf86BuiltinModules \
			LoaderBuiltinFind glamor_egl_init glamor_init ms_present_screen_init dri3_screen_init \
			present_screen_init __wrap_mmap drmPhoenixMmap drm_phoenix_ioctl gbmint_get_backend \
			kmsro_drm_screen_create v3d_drm_screen_create_renderonly epoxy_static_proc_address \
			xshmfence_map_shm libxcvt_gen_mode_info ReadFdFromClient WriteFdToClient _XSERVTransRecvFd; do
		if grep -qE " [TtDdRrBbWw] ${s}\$" <<< "${syms}"; then echo "  symbol ${s}: yes"; else echo "  symbol ${s}: NO"; fi
	done
	# DRI3 (open, PixmapFromBuffers, FenceFromFD) passes descriptors over the X socket: xtrans
	# must have been built with fd passing.
	grep -q '^#define XTRANS_SEND_FDS 1' "${XB}/include/dix-config.h" \
		|| b_die "xorg-server built without XTRANS_SEND_FDS (DRI3 fd passing)"
	# The Phoenix-RTOS xshmfence backend (G16), not upstream's pthread one: which members linked.
	local nphx npth
	nphx=$(grep -c 'libxshmfence.a(xshmfence_phoenix.o)' "${X}.map" || true)
	npth=$(grep -c 'libxshmfence.a(xshmfence_pthread.o)' "${X}.map" || true)
	echo "xorg_server_drm: xshmfence members: phoenix=${nphx} pthread=${npth} (want >0 / 0)"
	{ [ "${nphx}" -gt 0 ] && [ "${npth}" = 0 ]; } || b_die "wrong xshmfence backend linked"
	for s in 'modesetting' 'glamor' 'PHXHID dev=' 'linked into the server' 'builtin keymap' \
			/dev/dri/card0 /dev/dri/renderD128 /kmsbuf 'libdrm-phoenix:' DRMPHX_TRACE kmsro 'V3D 4.2' \
			EGL_KHR_platform_gbm EGL_MESA_platform_gbm 'DRI3' 'Present' 'X.Org X Server' 'Xorg-drm'; do
		echo "  strings '${s}': $(nl_count_strings "${P}/bin/Xorg-drm" "${s}")"
	done
	# xorg-server patch 0008: without libpciaccess xf86PostProbe() must not abort a
	# framebuffer-slot (legacy Probe) screen -- the m4a failure; the FatalError string is then
	# dead code and gone.
	n="$(nl_count_strings "${P}/bin/Xorg-drm" 'Cannot run in framebuffer mode')"
	echo "  fb-slot abort string (must be 0, patch 0008): ${n}"
	[ "${n}" = 0 ] || bad=1
	if grep -qE ' [Tt] dlopen$' <<< "${syms}"; then
		echo "  note: dlopen is linked (the loader's fallback for modules outside the builtin table; libepoxy never calls it)"
	fi
	[ "${bad}" = 0 ] || b_die "Xorg-drm verification failed (see above)"
	sha256sum "${X}" "${P}/bin/Xorg-drm"

	install -m 755 "${PREFIX_PORT}/glue/pi/startx" "${P}/bin/startx"
	install -m 644 "${PREFIX_PORT}/glue/conf/xorg-drm.conf" "${P}/etc/X11/xorg-drm.conf"

	if b_use x11demo; then
		_xorg_server_drm_eglx11_demo
	fi

	if b_use rootfs; then
		# startx starts /bin/Xorg-drm -config /etc/X11/xorg-drm.conf and its clients (the GL
		# one: /bin/eglx11-demo-x).
		b_install "${P}/bin/Xorg-drm" /bin
		b_install "${P}/bin/startx" /bin
		mkdir -p "${PREFIX_FS}/root/etc/X11"
		install -m 644 "${P}/etc/X11/xorg-drm.conf" "${PREFIX_FS}/root/etc/X11/xorg-drm.conf"
		if b_use x11demo; then
			cp "${P}/bin/eglx11-demo" "${PREFIX_PORT_BUILD}/eglx11-demo-x"
			b_install "${PREFIX_PORT_BUILD}/eglx11-demo-x" /bin
		fi
	fi
}

# eglx11-demo: a static GLES2-in-an-X-window client (EGL on X11 through DRI3/Present), linked
# from mesa_drm's x11 build (x11/link-gles.txt: gallium whole, the Mesa archives, the X11/xcb
# archives, libxshmfence_phoenix's archive -- the same one Xorg-drm links -- libdrm, compat,
# zlib). = tools/gpu-lane/x11-drm/build.sh's eglx11-demo step.
_xorg_server_drm_eglx11_demo() {
	local M="${PORT_DEP_mesa_drm}" P="${PREFIX_PORT_INSTALL}" o="${PREFIX_PORT_BUILD}/eglx11-obj"
	local lt="${M}/x11/link-gles.txt" e="${PREFIX_PORT_INSTALL}/prog/eglx11-demo"
	[ -f "${lt}" ] || b_die "no ${lt} (mesa_drm[x11])"
	mkdir -p "${o}"
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter "${NL_TFLAGS[@]}" \
		-I"${M}/x11/prefix/include" -I"${M}/x11-prefix/include" -c "${PREFIX_PORT}/glue/eglx11/eglx11_demo.c" \
		-o "${o}/eglx11_demo.o"
	local link=() whole="" l
	while IFS= read -r l; do
		case "${l}" in
			"--whole-archive "*) whole="${l#--whole-archive }" ;;
			*/libxshmfence.a) link+=("${PORT_DEP_libxshmfence_phoenix}/lib/libxshmfence.a") ;;
			*) link+=("${l}") ;;
		esac
	done < "${lt}"
	[ -n "${whole}" ] || b_die "no --whole-archive entry in ${lt}"
	# -Wl,--wrap=mmap: libdrm-phoenix resolves Mesa's BO tokens (M3 section 2.6);
	# -Wl,--wrap=ioctl: the in-process sync-file ioctls (M5 section 9.3).
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 \
		-Wl,--wrap=mmap -Wl,--wrap=ioctl -Wl,-Map,"${e}.map" -o "${e}" "${o}/eglx11_demo.o" \
		-Wl,--whole-archive "${whole}" -Wl,--no-whole-archive -Wl,--start-group "${link[@]}" -Wl,--end-group -lm \
		> "${o}/link.log" 2>&1 || { grep -v ': warning: ' "${o}/link.log" | head -60; b_die "eglx11-demo link failed"; }
	"${NL_STRIP}" -o "${P}/bin/eglx11-demo" "${e}"

	local und syms s bad=0
	und="$("${NL_NM}" -u "${e}" || true)"
	echo "eglx11-demo: undefined symbols (nm -u): $(grep -c . <<< "${und}" || true)"
	[ -n "${und}" ] && head -20 <<< "${und}"
	syms="$("${NL_NM}" "${e}")"
	for s in __wrap_mmap __wrap_ioctl drmPhoenixMmap drm_phoenix_ioctl dri2_initialize_x11 dri3_x11_connect \
			loader_dri3_swap_buffers_msc x11_dri3_open xcb_dri3_open xcb_dri3_pixmap_from_buffers xcb_present_pixmap \
			xshmfence_alloc_shm xshmfence_phoenix_alloc_shm kmsro_drm_screen_create v3d_drm_screen_create_renderonly; do
		if grep -qE " [TtWw] ${s}\$" <<< "${syms}"; then echo "  symbol ${s}: yes"; else echo "  symbol ${s}: NO"; bad=1; fi
	done
	if grep -qE ' [TtWw] pthread_condattr_setpshared$' <<< "${syms}" && grep -q 'xshmfence_pthread' "${e}.map"; then
		echo "  the pthread xshmfence backend got linked"; bad=1
	fi
	for s in 'XDEMO ' /dev/dri/card0 /dev/dri/renderD128 /kmsbuf /shm 'libdrm-phoenix:' kmsro 'V3D 4.2' \
			EGL_KHR_platform_x11 EGL_EXT_platform_xcb 'xshmfence: no shmsrv'; do
		echo "  strings '${s}': $(nl_count_strings "${P}/bin/eglx11-demo" "${s}")"
	done
	for s in 'v3d-winsys:' phoenix_v3d_ioctl peek_next_scanout v3d-srv; do
		[ "$(nl_count_strings "${P}/bin/eglx11-demo" "${s}")" = 0 ] || { echo "  old-lane string '${s}'"; bad=1; }
	done
	[ "${bad}" = 0 ] || b_die "eglx11-demo verification failed (see above)"
	sha256sum "${e}" "${P}/bin/eglx11-demo"
}
