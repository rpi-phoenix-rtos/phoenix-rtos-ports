#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="supertuxkart_drm"
	version="1.4"
	desc="supertuxkart-drm + stk-drm launcher: the supertuxkart port's build linked on the GPU stack (SDL KMSDRM + Mesa GBM/EGL/GLES)"

	# The supertuxkart port's archive. This clone RELINKS the supertuxkart port's CMake build
	# (it compiles nothing of the game); the archive is extracted only because every framework
	# port has one -- it is the source of what the clone ships, and it carries the licence.
	source="https://github.com/supertuxkart/stk-code/archive/refs/tags"
	archive_filename=("stk-code-${version}.tar.gz" "${version}.tar.gz")
	src_path="stk-code-${version}/"

	size="32646035"
	sha256="40ff14ce0e1fde05fa9f427bfe1f75917a6f4efbf2c1a86421a7f794d05189b9"

	license="GPL-3.0-or-later"
	license_file="COPYING"

	# Private prefix. `supertuxkart` (compiles the game against sdl2_kmsdrm, links and installs
	# nothing) is a dependency so that its CMake build tree (objects + link.txt) exists first;
	# the audio/TLS archives are its link group's.
	conflicts="supertuxkart_drm!=${version}"
	depends="supertuxkart sdl2_kmsdrm mesa_drm[opengl] libdrm_phoenix zlib libogg libvorbis mbedtls"

	# rootfs: install /usr/bin/supertuxkart-drm and its launcher /bin/stk-drm into the image,
	# the launcher also as /bin/stk
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-drm/build-stk-drm.sh:
#   1. compile glue/stkdrm_hooks.c (banner, SDL VIDEO/INPUT DEBUG logging, and the
#      `stk-drm flipstat` frame counter behind -Wl,--wrap=SDL_GL_SwapWindow);
#   2. run CMake's link line (link.txt) from the supertuxkart port's build tree (configured
#      against sdl2_kmsdrm's libSDL2.a) with the Mesa link shape (libgallium whole-archive + one group of
#      EGL/GBM/dri_gbm/GLESv2/glapi/v3d/broadcom/winsys/util + libdrm.a + the compat shim)
#      with -static -Wl,--wrap=mmap -Wl,--wrap=ioctl -Wl,--wrap=SDL_GL_SwapWindow;
#   3. the `stk-drm` launcher: the shipped one (glue/stk-launcher.c, a copy of the coordination
#      repo's tools/supertuxkart-port/stk-launcher.c) with only its exec path and two message
#      prefixes rewritten -- the same default args and seeded config.xml.
# STK is a GLES program (-DUSE_GLES2=ON; Irrlicht asks SDL for an ES 3.0 context and loads
# every gl* through glad + SDL_GL_GetProcAddress = eglGetProcAddress). The Mesa build is the
# desktop-GL one because libSDL2.a was configured against it (it has GLES2 on too).
# PROOFS (b_die): nm -u empty;
# no PT_INTERP; new-stack symbols present, old-lane ones absent; COGLES2Driver ->
# __wrap_SDL_GL_SwapWindow -> SDL_GL_SwapWindow; only the wrappers call ioctl/mmap; submit-first
# swap order; GPU-stack strings present, the first stack's absent; no
# global symbol defined both by STK's inputs and by the new stack; guarded inputs unchanged.

p_prepare() {
	:
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local pfx="${PREFIX_BUILD%/}" sysroot="${NL_SYSROOT}" tcbin
	tcbin="$(dirname "${NL_CC}")"
	local cc="${NL_CC}" nm="${NL_NM}" strip="${NL_STRIP}" readelf="${NL_READELF}" objdump="${NL_OBJDUMP}" size="${NL_SIZE}"
	local stkbuild="${pfx}/port-sources/supertuxkart-1.4/stk-code-1.4/build"
	local linktxt="${stkbuild}/CMakeFiles/supertuxkart.dir/link.txt"
	local launcher_src="${PREFIX_PORT}/glue/stk-launcher.c" hooks_src="${PREFIX_PORT}/glue/stkdrm_hooks.c"
	local SP="${PORT_DEP_sdl2_kmsdrm}" GD="${PORT_DEP_sdl2_kmsdrm}/share/gamedrm"
	local SDL_A="${SP}/lib/libSDL2.a"
	local M="${PORT_DEP_mesa_drm}/gl"
	local MB="${M}/mesa-build"
	local COMPAT_A="${PORT_DEP_mesa_drm}/compat/libmesadrm-compat.a"
	local LDA="${PORT_DEP_libdrm_phoenix}/lib/libdrm.a"
	local zl="${PORT_DEP_zlib}/lib" og="${PORT_DEP_libogg}/lib" vb="${PORT_DEP_libvorbis}/lib" mt="${PORT_DEP_mbedtls}/lib"
	local name="drm"
	local out="${PREFIX_PORT_BUILD}/relink"
	local elf="${out}/supertuxkart-${name}"

	log()  { printf '[stk-drm] %s\n' "$*"; }
	die()  { b_die "[stk-drm] $*"; }
	sha()  { if [ -e "$1" ]; then sha256sum "$1" | cut -d' ' -f1; else echo "absent"; fi; }

	# --- preconditions (fail loud; never fall back) -------------------------------------------
	local TFLAGS=("${NL_TFLAGS[@]}")
	local A=(src/egl/libEGL.a src/gbm/libgbm.a src/gbm/backends/dri/dri_gbm.a src/mesa/glapi/es2api/libGLESv2.a
		src/mesa/glapi/shared-glapi/libglapi.a src/gallium/drivers/v3d/libv3d.a
		src/gallium/drivers/v3d/libv3d-v42.a src/gallium/drivers/v3d/libv3d-v71.a
		src/broadcom/libbroadcom-v42.a src/broadcom/libbroadcom-v71.a src/broadcom/qpu/libbroadcom_qpu.a
		src/broadcom/libv3d_neon.a src/broadcom/perfcntrs/libv3d-perfcntrs-v42.a
		src/broadcom/perfcntrs/libv3d-perfcntrs-v71.a src/gallium/winsys/kmsro/drm/libkmsrowinsys.a
		src/gallium/winsys/v3d/drm/libv3dwinsys.a src/gallium/winsys/vc4/drm/libvc4winsys.a
		src/gallium/winsys/sw/kms-dri/libswkmsdri.a src/gallium/winsys/sw/dri/libswdri.a
		src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a
		src/c11/impl/libmesa_util_c11.a)
	local f a
	for f in "$linktxt" "$sysroot/lib/libphoenix.a" "$launcher_src" "$hooks_src" "$SDL_A" \
			"$SP/include/SDL2/SDL_config.h" "$COMPAT_A" "$LDA" "${zl}/libz.a" "${mt}/libmbedtls.a"; do
		[ -e "$f" ] || die "missing: $f"
	done
	for a in "${A[@]}"; do
		[ -f "${MB}/${a}" ] || die "missing Mesa archive ${MB}/${a} (mesa_drm built without USE opengl?)"
	done
	local GALLIUM_A
	GALLIUM_A="$(ls "${MB}"/src/gallium/targets/dri/libgallium-*.a)"
	[ -f "${GALLIUM_A}" ] || die "no libgallium-*.a in ${MB}"
	local cfg="${SP}/include/SDL2/SDL_config.h" d
	for d in SDL_VIDEO_DRIVER_KMSDRM SDL_VIDEO_OPENGL_EGL SDL_VIDEO_OPENGL_ES2 SDL_INPUT_PHOENIX SDL_AUDIO_DRIVER_PHOENIX; do
		grep -qE "^#define ${d} +1" "${cfg}" || die "sdl2_kmsdrm's SDL_config.h lacks ${d} 1"
	done
	for d in SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC SDL_VIDEO_DRIVER_PHOENIX SDL_LOADSO_DLOPEN; do
		if grep -qE "^#define ${d}( |$)" "${cfg}"; then die "sdl2_kmsdrm's SDL_config.h defines ${d}"; fi
	done
	nl_has_sym "${LDA}" __wrap_ioctl || die "${LDA} has no __wrap_ioctl"

	local guarded=("$linktxt" "$SDL_A" "$GALLIUM_A" "$LDA")
	declare -A before
	for f in "${guarded[@]}"; do before["$f"]="$(sha "$f")"; done

	rm -rf "$out"
	mkdir -p "$out/src" "$out/obj"

	# --- 1. the hooks object -----------------------------------------------------------
	local hooks_o="$out/obj/stkdrm_hooks.o"
	"$cc" -O2 -g -std=gnu17 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${SP}/include" -c "$hooks_src" -o "$hooks_o" \
		|| die "stkdrm_hooks.c compile failed"

	# --- 2. the link --------------------------------------------------------------------
	local linkcmd
	linkcmd="$(cat "$linktxt")"
	[ "$(grep -o ' -o bin/supertuxkart ' "$linktxt" | wc -l)" = 1 ] \
		|| die "link.txt must name ' -o bin/supertuxkart ' exactly once -- the port recipe changed; update this recipe"
	[ "$(grep -oF " ${SDL_A} " "$linktxt" | wc -l)" = 1 ] \
		|| die "link.txt must name ${SDL_A} exactly once -- the supertuxkart port's configure changed; update this recipe"

	local AA="" cmd bad
	for a in "${A[@]}"; do AA="${AA} '${MB}/${a}'"; done
	cmd="${linkcmd/ -o bin\/supertuxkart / -o '${elf}' }"
	cmd="${cmd} '${hooks_o}' -static -Wl,--wrap=mmap -Wl,--wrap=ioctl -Wl,--wrap=SDL_GL_SwapWindow -Wl,-Map,'${elf}.map' \
		-Wl,--whole-archive '${GALLIUM_A}' -Wl,--no-whole-archive \
		-Wl,--start-group '${SDL_A}'${AA} '${LDA}' '${COMPAT_A}' \
		'${zl}/libz.a' '${og}/libogg.a' '${vb}/libvorbis.a' \
		'${vb}/libvorbisfile.a' '${vb}/libvorbisenc.a' \
		'${mt}/libmbedtls.a' '${mt}/libmbedx509.a' '${mt}/libmbedcrypto.a' \
		-Wl,--end-group -lm -Wl,-z,stack-size=8388608"
	printf '%s\n' "$cmd" > "$out/link-cmd.txt"
	log "stk-drm link (KMSDRM SDL + Mesa GBM/EGL/GLES + libdrm-phoenix)"
	rm -f "$elf"
	( cd "$stkbuild" && export PATH="${tcbin}:${PATH}" && eval "$cmd" ) > "$out/link.log" 2>&1 \
		|| { head -60 "$out/link.log" >&2; die "stk-drm link failed"; }
	[ -f "$elf" ] || die "link reported success but produced no ELF"
	"$strip" -o "$elf.stripped" "$elf"

	# --- proofs on the clone --------------------------------------------------------------------
	bad=0
	local und syms s n forbidden calls order stk_in dups
	if "$readelf" -l "$elf" | grep -q INTERP; then log "  PT_INTERP present"; bad=1; fi
	und="$("$nm" -u "$elf" || true)"
	log "  undefined symbols (nm -u): $(grep -c . <<< "${und}" || true)"
	[ -n "${und}" ] && { sed 's/^/[stk-drm]     /' <<< "${und}" | head -20; bad=1; }
	syms="$("$nm" "$elf")"
	for s in KMSDRM_CreateDevice KMSDRM_GLES_SwapWindow KMSDRM_GetWindowWMInfo SDL_EGL_LoadLibrary SDL_PHOENIX_HID_Poll \
			SDL_GL_SwapWindow __wrap_SDL_GL_SwapWindow __wrap_mmap __wrap_ioctl drmPhoenixMmap drm_phoenix_ioctl \
			gbmint_get_backend kmsro_drm_screen_create v3d_drm_screen_create_renderonly eglGetPlatformDisplayEXT \
			eglGetProcAddress _mesa_glapi_get_proc_address; do
		if grep -qE " [TtWw] ${s}\$" <<< "${syms}"; then log "  symbol ${s}: yes"; else log "  symbol ${s}: NO"; bad=1; fi
	done
	forbidden="$(grep -E ' [TtWwDdBbRr] (PHOENIX_bootstrap|PHOENIX_PumpEvents|PHOENIX_GL_[A-Za-z_]*|phxgl_[A-Za-z_]*|phoenix_v3d_ioctl|winsys_init|boPool_take|mboxProp|v3da_connect|v3d_phoenix_flip)$' <<< "${syms}" || true)"
	if [ -n "${forbidden}" ]; then log "  forbidden (first-stack) symbols PRESENT:"; sed 's/^/[stk-drm]     /' <<< "${forbidden}"; bad=1
	else log "  first-stack symbols (SDL phoenix video, phxgl/PHOENIX_GL glue, in-process winsys): none"; fi

	calls="$("$objdump" -d --no-show-raw-insn "$elf" | awk '
		/^[0-9a-f]+ <.*>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
		/\tbl?\t/ && / <(ioctl|mmap|__wrap_SDL_GL_SwapWindow|SDL_GL_SwapWindow|__real_SDL_GL_SwapWindow)>$/ {
			t = $NF; gsub(/[<>]/, "", t); print t, fn }' | sort | uniq -c)"
	printf '%s\n' "$calls" > "$out/call-sites.txt"
	awk '$2 == "ioctl" && $3 != "__wrap_ioctl" { bad = 1 } END { exit bad }' <<< "$calls" \
		|| { log "  real ioctl() called from outside __wrap_ioctl:"; awk '$2 == "ioctl"' <<< "$calls" | sed 's/^/[stk-drm]     /'; bad=1; }
	awk '$2 == "mmap" && $3 != "__wrap_mmap" { bad = 1 } END { exit bad }' <<< "$calls" \
		|| { log "  real mmap() called from outside __wrap_mmap:"; awk '$2 == "mmap"' <<< "$calls" | sed 's/^/[stk-drm]     /'; bad=1; }
	if grep -qE ' __wrap_SDL_GL_SwapWindow _ZN3irr5video13COGLES2Driver' <<< "$calls" \
			&& ! grep -qE ' SDL_GL_SwapWindow _ZN3irr' <<< "$calls" \
			&& grep -qE ' SDL_GL_SwapWindow __wrap_SDL_GL_SwapWindow$' <<< "$calls"; then
		log "  PROOF: COGLES2Driver -> __wrap_SDL_GL_SwapWindow -> SDL_GL_SwapWindow (frame counter in the path)"
	else
		log "  the SDL_GL_SwapWindow wrap is NOT in Irrlicht's swap path:"; sed 's/^/[stk-drm]     /' <<< "$calls"; bad=1
	fi
	if order="$(GAMEDRM_OBJDUMP="$objdump" bash "${GD}/check-swap-order.sh" "$elf")"; then log "  ${order}"
	else log "  ${order} -- the linked libSDL2.a lacks patches/0009"; bad=1; fi

	for s in 'KMS/DRM Video Driver' '/dev/dri/' 'libdrm-phoenix:' 'DRMPHX_TRACE' 'DRMPHX sync' '/dev/kbd0' '/dev/audio0' \
			'EGL_KHR_platform_gbm' 'kmsro' 'stk-drm: new GPU lane' 'stk-drm flipstat' 'stk-drm swapstat'; do
		n="$(grep -acF -- "$s" "$elf.stripped" || true)"
		log "  string '$s': $n"
		[ "$n" != 0 ] || bad=1
	done
	for s in 'v3d-winsys:' 'v3da-winsys:' 'phxgl' 'PHOENIX: GL_CreateContext' '/dev/fb0' 'RPI4FB_GETMODE' 'phoenix_v3d_ioctl' \
			'peek_next_scanout' 'v3d-srv' 'v3d-pool:'; do
		n="$(grep -acF -- "$s" "$elf.stripped" || true)"
		log "  first-stack string '$s': $n"
		[ "$n" = 0 ] || bad=1
	done
	# silent duplicates: global symbols defined by STK's own link inputs AND the new stack
	stk_in="$( cd "$stkbuild" && tr ' ' '\n' < "$linktxt" | grep -E '\.(obj|a)$' | grep -vxF "${SDL_A}" )"
	dups="$( { ( cd "$stkbuild" && while IFS= read -r f; do "$nm" -g --defined-only "$f" 2>/dev/null; done <<< "$stk_in" ) \
			| awk 'NF >= 3 && $2 ~ /[TDBRVW]/ { print $3 }' | LC_ALL=C sort -u > "$out/obj/stk-defs.txt"; \
		for f in "$GALLIUM_A" "${MB}/${A[0]}" "${MB}/${A[1]}" "${MB}/${A[2]}" "${MB}/${A[4]}" "${MB}/src/util/libmesa_util.a" \
				"$LDA" "$SDL_A" "$COMPAT_A" "$hooks_o"; do "$nm" -g --defined-only "$f" 2>/dev/null; done \
			| awk 'NF >= 3 && $2 ~ /[TDBRVW]/ { print $3 }' | LC_ALL=C sort -u > "$out/obj/drm-defs.txt"; \
		LC_ALL=C comm -12 "$out/obj/stk-defs.txt" "$out/obj/drm-defs.txt" | grep -vxF 'DW.ref.__gxx_personality_v0' || true; } )"
	if [ -n "$dups" ]; then
		log "  symbols defined by BOTH STK's inputs and the new stack ($(grep -c . <<< "$dups")):"
		sed 's/^/[stk-drm]     /' <<< "$dups" | head -30
		bad=1
	fi
	rm -f "$out/obj/stk-defs.txt" "$out/obj/drm-defs.txt"

	# --- 3. stk-drm launcher -----------------------------------------------------------------------
	local lsrc="$out/src/stk-$name.c" diff_lines
	sed -e "s|\"/usr/bin/supertuxkart\"|\"/usr/bin/supertuxkart-$name\"|" \
	    -e "s|\"stk: exec /usr/bin/supertuxkart\"|\"stk-$name: exec /usr/bin/supertuxkart-$name\"|" \
	    -e "s|\"stk: DATADIR=|\"stk-$name: DATADIR=|" \
	    "$launcher_src" > "$lsrc"
	[ "$(grep -c "\"/usr/bin/supertuxkart-$name\"" "$lsrc")" = 1 ] || die "launcher exec path rewrite did not match exactly once"
	[ "$(grep -c "\"stk-$name: DATADIR=" "$lsrc")" = 1 ] || die "launcher banner rewrite did not match exactly once"
	[ "$(grep -c '/usr/bin/supertuxkart"' "$lsrc")" = 0 ] || die "launcher still names /usr/bin/supertuxkart"
	grep -qF 'scale_rtts_factor=\"0.75\"' "$lsrc" || die "launcher lost the seeded scale_rtts_factor=0.75"
	diff_lines="$(diff "$launcher_src" "$lsrc" | grep -c '^>' || true)"
	[ "$diff_lines" = 3 ] || die "launcher differs from stk-launcher.c in $diff_lines lines (expected exactly 3)"
	"$cc" -O2 -static -Wall -Wextra --sysroot="${sysroot}/" -B"${sysroot}/lib/" -iprefix "${sysroot}/" \
		-o "$out/stk-$name" "$lsrc" || die "launcher compile failed"
	if "$readelf" -l "$out/stk-$name" 2>/dev/null | grep -q INTERP; then die "stk-$name has a PT_INTERP segment"; fi
	grep -aqF "/usr/bin/supertuxkart-$name" "$out/stk-$name" || die "launcher ELF lacks its exec path"
	[ -z "$("$nm" -u "$out/stk-$name" || true)" ] || die "launcher has undefined symbols"

	# --- provenance + guards ------------------------------------------------------------------------
	"$size" "$elf" | sed 's/^/[stk-drm]   /'
	{
		echo "built:               $(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "stkdrm_hooks.c:      $(sha "$hooks_src")"
		echo "libSDL2.a (KMSDRM):  $(sha "$SDL_A")"
		echo "Mesa (mesa_drm gl):  $(cat "${M}/opengl.txt"); libgallium $(sha "$GALLIUM_A" | cut -c1-16)"
		echo "libdrm-phoenix:      $(sha "$LDA")"
		echo "link.txt:            $(sha "$linktxt")"
		echo "drm unstripped:      $(sha "$elf") ($(stat -c%s "$elf") B)"
		echo "drm stripped:        $(sha "$elf.stripped") ($(stat -c%s "$elf.stripped") B)"
		echo "stk-$name:             $(sha "$out/stk-$name") ($(stat -c%s "$out/stk-$name") B)"
		echo "libphoenix.a:        $(sha "$sysroot/lib/libphoenix.a")"
	} > "$out/BUILD-INFO.txt"
	sed 's/^/[stk-drm]   /' "$out/BUILD-INFO.txt"
	local gbad=0
	for f in "${guarded[@]}"; do
		if [ "${before[$f]}" != "$(sha "$f")" ]; then log "ERROR: shared file CHANGED during this build: $f"; gbad=1; fi
	done
	[ "$gbad" = 0 ] || die "guarded inputs changed"
	[ "$bad" = 0 ] || die "verification failed (see above)"

	local p="${PREFIX_PORT_INSTALL}"
	mkdir -p "${p}/bin" "${p}/prog" "${p}/share/stk-drm"
	install -m 755 "$elf" "${p}/prog/supertuxkart-$name"
	install -m 755 "$elf.stripped" "${p}/bin/supertuxkart-$name"
	install -m 755 "$out/stk-$name" "${p}/bin/stk-$name"
	install -m 644 "${elf}.map" "$out/link-cmd.txt" "$out/call-sites.txt" "$out/BUILD-INFO.txt" "${p}/share/stk-drm/"
	if b_use rootfs; then
		b_install "${p}/bin/supertuxkart-$name" /usr/bin
		b_install "${p}/bin/stk-$name" /bin
		# TODO(TD-26): the plain command name runs this program (GPU migration P1: the default
		# image). P4 gives the programs the plain names themselves.
		install -m 755 "${p}/bin/stk-$name" "${PREFIX_FS}/root/bin/stk"
	fi
}
