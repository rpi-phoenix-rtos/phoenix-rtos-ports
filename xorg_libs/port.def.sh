#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xorg_libs"
	version="2023.2"
	desc="X11 client library stack (Xlib/XCB + toolkit libs) for Phoenix-RTOS"

	# Aggregate LAYER-1 port: the X protocol headers + core client/toolkit libraries.
	# The framework anchor source is xorgproto (the DAG leaf, needed first anyway);
	# the remaining ~23 tarballs are fetched by p_build (see docs/inprogress/
	# x11-ports-migration-spec.md for the full DAG, URLs and flags). Migrated from
	# the coordination repo's tools/x11-port/build-x11-phoenix.sh (proven build
	# logic) into the framework, staging into $PREFIX_BUILD/{lib,include} so
	# dependent ports (xorg-fonts, xorg-server, xterm, windowmaker, ...) find the
	# .a/headers/.pc there instead of the old /tmp/x11-phoenix prefix.
	source="https://www.x.org/releases/individual/proto/"
	archive_filename="xorgproto-${version}.tar.gz"
	src_path="xorgproto-${version}/"
	size="1150326"
	sha256="c791aad9b5847781175388ebe2de85cb5f024f8dabf526d5d699c4f942660cc3"

	license="MIT"          # X11/MIT across the stack
	# xorgproto ships per-protocol COPYING-* files (no single COPYING); the core
	# X11 protocol license is representative of the MIT-licensed stack.
	license_file="COPYING-x11proto"

	conflicts=""
	depends=""             # Layer 1 has no external port deps

	supports="phoenix>=3.3"
}

# NOTE: patches are per-sublibrary (libxcb/libX11/libICE), not for the xorgproto
# anchor tree, so we do NOT call b_port_apply_patches here — _xbuild applies the
# matching patches/<name-version>*.patch to each sublib after it is extracted.
p_prepare() {
	:
}

p_build() {
	# Install prefix = the framework's shared staging dir: lib -> $PREFIX_A,
	# include -> $PREFIX_H, pkgconfig -> $PREFIX_A/pkgconfig. Dependent ports
	# already look in $PREFIX_A / $PREFIX_H (cf. pcre --libdir/--includedir).
	local XHOST="aarch64-phoenix"   # X11 config.sub rejects the framework HOST (aarch64a72-phoenix)
	local PREFIX="${PREFIX_BUILD%/}"
	local SYSROOT="${PREFIX_BUILD%/}/sysroot"
	local SRC="${PREFIX_PORT_BUILD}/x11src"
	local TCGCC="${CROSS}gcc" TCAR="${CROSS}ar" TCRANLIB="${CROSS}ranlib"
	local XBASE="https://www.x.org/releases/individual"
	local XARCHIVE="https://xorg.freedesktop.org/archive/individual"
	local XCBB="https://xcb.freedesktop.org/dist"

	mkdir -p "$SRC" "$PREFIX/lib/pkgconfig" "$PREFIX/share/pkgconfig" "$PREFIX/include"
	# xorgproto/xcb-proto .pc land in share/pkgconfig; everything else in lib/pkgconfig.
	export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"

	# --- helpers (transplanted from tools/x11-port/build-x11-phoenix.sh) ---
	_fetch_extract() {
		local nv=$1 url=$2 attempt
		cd "$SRC" || return 1
		[ -d "$nv" ] && return 0
		for attempt in 1 2 3; do
			timeout 120 curl -fsSL -o "$nv.tar.gz" "$url" && break
			echo "xorg-libs: $nv fetch attempt $attempt/3 failed; retrying" >&2; sleep 5
		done
		[ -s "$nv.tar.gz" ] || b_die "xorg-libs: $nv download failed from $url"
		tar xf "$nv.tar.gz" || b_die "xorg-libs: $nv extract failed"
	}
	_apply_patches() {
		local nv=$1 p
		for p in "${PREFIX_PORT}/patches/$nv"*.patch; do
			[ -f "$p" ] || continue
			# Idempotent: skip if already applied. The framework runs p_build under
			# `set -ex`, and `patch -N` exits non-zero when it refuses a re-apply, so a
			# second pass (e.g. this port built again as a dependency of xorg_fonts/
			# xorg_server/xterm, or a developer rebuild) would otherwise abort here. A
			# reverse dry-run succeeds only when the change is already present.
			if patch -p1 -R --dry-run -d "$SRC/$nv" <"$p" >/dev/null 2>&1; then
				continue
			fi
			patch -p1 -N -d "$SRC/$nv" <"$p" >/dev/null 2>&1 || b_die "xorg-libs: $nv patch $(basename "$p") failed"
		done
	}
	# _xbuild <name-version> <url> [extra-configure-args]  (autotools, static, into $PREFIX)
	_xbuild() {
		local nv=$1 url=$2 extra=${3:-} srcdir
		if [ "$nv" = "xorgproto-${version}" ]; then
			srcdir="${PREFIX_PORT_WORKDIR}"    # framework already fetched+extracted the anchor
		else
			_fetch_extract "$nv" "$url"; _apply_patches "$nv"; srcdir="$SRC/$nv"
		fi
		cd "$srcdir" || b_die "xorg-libs: no srcdir $srcdir"
		if [ ! -f config.status ]; then
			# shellcheck disable=2086
			./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
				CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
				CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include ${XCFLAGS_EXTRA:-}" \
				LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" $extra \
				|| b_die "xorg-libs: $nv configure failed"
		fi
		make install || b_die "xorg-libs: $nv build failed"
		echo "xorg-libs: $nv OK"
	}
	# _hostbuild <name-version> <url>  (native host tool, e.g. xcb-proto python codegen)
	_hostbuild() {
		local nv=$1 url=$2
		_fetch_extract "$nv" "$url"
		cd "$SRC/$nv" || b_die "xorg-libs: no $nv"
		[ -f config.status ] || ./configure --prefix="$PREFIX" || b_die "xorg-libs: $nv host configure failed"
		make install || b_die "xorg-libs: $nv host build failed"
		echo "xorg-libs: $nv OK (host)"
	}
	# _libonly_pc <name-version> — copy an in-tree .pc into the prefix (lib-only installs)
	_copy_pc() { local pc; pc=$(find "$SRC/$1" -name "$2" 2>/dev/null | head -1); [ -n "$pc" ] && cp "$pc" "$PREFIX/lib/pkgconfig/"; }

	local PWD_DEFS="-DMAXHOSTNAMELEN=256 -DO_NOFOLLOW=0 -DXOS_USE_MTSAFE_PWDAPI -D_POSIX_THREAD_SAFE_FUNCTIONS=200809L"

	# ---- base proto/aux tier ----
	_xbuild "xorgproto-${version}" ""
	_xbuild libXau-1.0.11  "$XBASE/lib/libXau-1.0.11.tar.gz"
	_xbuild xtrans-1.5.0   "$XBASE/lib/xtrans-1.5.0.tar.gz"
	_xbuild libXdmcp-1.1.5 "$XBASE/lib/libXdmcp-1.1.5.tar.gz"

	# ---- XCB tier (xcb-proto is a HOST build: python codegen) ----
	_hostbuild xcb-proto-1.16.0     "$XARCHIVE/proto/xcb-proto-1.16.0.tar.xz"
	_xbuild libpthread-stubs-0.5    "$XARCHIVE/lib/libpthread-stubs-0.5.tar.xz"
	_xbuild libxcb-1.16             "$XARCHIVE/lib/libxcb-1.16.tar.xz" "--disable-mitshm"

	# ---- libX11 (core Xlib) ----
	XCFLAGS_EXTRA="-DMAXHOSTNAMELEN=256 -DXOS_USE_MTSAFE_PWDAPI -D_POSIX_THREAD_SAFE_FUNCTIONS=200809L" \
		_xbuild libX11-1.8.7 "$XBASE/lib/libX11-1.8.7.tar.gz" \
		"--without-xmlto --disable-specs --disable-devel-docs xorg_cv_malloc0_returns_null=no"

	# ---- extension / render libs ----
	_xbuild libXext-1.3.5     "$XBASE/lib/libXext-1.3.5.tar.gz"     "xorg_cv_malloc0_returns_null=no"
	_xbuild libXrender-0.9.11 "$XBASE/lib/libXrender-0.9.11.tar.gz" "xorg_cv_malloc0_returns_null=no"
	_xbuild libXrandr-1.5.4   "$XBASE/lib/libXrandr-1.5.4.tar.gz"   "xorg_cv_malloc0_returns_null=no"
	_xbuild libxkbfile-1.1.3  "$XBASE/lib/libxkbfile-1.1.3.tar.gz"  "xorg_cv_malloc0_returns_null=no"

	# ---- xcb-util family ----
	_xbuild xcb-util-0.4.1             "$XCBB/xcb-util-0.4.1.tar.gz"
	_xbuild xcb-util-image-0.4.1       "$XCBB/xcb-util-image-0.4.1.tar.gz"
	_xbuild xcb-util-renderutil-0.3.10 "$XCBB/xcb-util-renderutil-0.3.10.tar.gz"
	_xbuild xcb-util-keysyms-0.4.1     "$XCBB/xcb-util-keysyms-0.4.1.tar.gz"
	_xbuild xcb-util-wm-0.4.2          "$XCBB/xcb-util-wm-0.4.2.tar.gz"

	# ---- pixman (software rasteriser; lib-only: test utils clash with sys/time.h) ----
	if [ ! -f "$PREFIX/lib/libpixman-1.a" ]; then
		_fetch_extract pixman-0.42.2 "$XBASE/lib/pixman-0.42.2.tar.gz"
		( cd "$SRC/pixman-0.42.2" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static --disable-gtk \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make -C pixman install ) || b_die "xorg-libs: pixman build failed"
		_copy_pc pixman-0.42.2 pixman-1.pc
		echo "xorg-libs: pixman-0.42.2 OK (lib only)"
	fi

	# ---- toolkit base: ICE/SM/Xt/Xmu/Xpm/Xaw ----
	# libXt/libXmu MUST use xorg_cv_malloc0_returns_null=yes (adds -DMALLOC_0_RETURNS_NULL
	# -DXTMALLOC_BC so XtMalloc(0) is bumped to 1; =no aborts Xt apps "Cannot perform malloc").
	XCFLAGS_EXTRA="-DMAXHOSTNAMELEN=256 -DO_NOFOLLOW=0" \
		_xbuild libICE-1.1.1 "$XBASE/lib/libICE-1.1.1.tar.gz" "xorg_cv_malloc0_returns_null=no"
	XCFLAGS_EXTRA="-DMAXHOSTNAMELEN=256 -DO_NOFOLLOW=0" \
		_xbuild libSM-1.2.4  "$XBASE/lib/libSM-1.2.4.tar.gz"  "xorg_cv_malloc0_returns_null=no --without-libuuid"
	XCFLAGS_EXTRA="$PWD_DEFS" \
		_xbuild libXt-1.3.0  "$XBASE/lib/libXt-1.3.0.tar.gz"  "xorg_cv_malloc0_returns_null=yes ac_cv_lib_m_hypot=yes"
	XCFLAGS_EXTRA="$PWD_DEFS" \
		_xbuild libXmu-1.2.1 "$XBASE/lib/libXmu-1.2.1.tar.gz" "xorg_cv_malloc0_returns_null=yes ac_cv_lib_m_hypot=yes"

	# libXpm (lib-only: sxpm/cxpm tools need getpwuid_r)
	if [ ! -f "$PREFIX/lib/libXpm.a" ]; then
		_fetch_extract libXpm-3.5.17 "$XBASE/lib/libXpm-3.5.17.tar.gz"
		( cd "$SRC/libXpm-3.5.17" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       xorg_cv_malloc0_returns_null=no CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include $PWD_DEFS" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make -C src install && make install-data ) || b_die "xorg-libs: libXpm build failed"
		_copy_pc libXpm-3.5.17 xpm.pc
		echo "xorg-libs: libXpm-3.5.17 OK (lib only)"
	fi

	# libXaw (Athena widgets; lib-only: tools pull deferred libc syms)
	if [ ! -f "$PREFIX/lib/libXaw7.a" ]; then
		_fetch_extract libXaw-1.0.16 "$XBASE/lib/libXaw-1.0.16.tar.gz"
		( cd "$SRC/libXaw-1.0.16" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       xorg_cv_malloc0_returns_null=no ac_cv_lib_m_hypot=yes CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include $PWD_DEFS" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make install ) || b_die "xorg-libs: libXaw build failed"
		[ -f "$PREFIX/lib/libXaw7.a" ] && echo "xorg-libs: libXaw-1.0.16 OK" || b_die "xorg-libs: libXaw did not install"
	fi

	echo "xorg-libs: LAYER 1 complete (staged into $PREFIX/{lib,include})"
}
