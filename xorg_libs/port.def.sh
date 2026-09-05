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
	# Upstream tarball bases. The canonical x.org / freedesktop CDNs are flaky (observed
	# 2026-08-22: served broken stubs for hours, blocking clean builds), so default to a
	# reliable full mirror: artfiles.org carries the whole x.org "individual" tree —
	# lib/*.tar.{gz,xz}, proto/*, and the xcb-util-*.tar.gz under lib/ (owner-suggested,
	# HTTP-200 verified 2026-08-23). Override with XORG_XBASE / XORG_XARCHIVE / XORG_XCBB
	# to use a different mirror; e.g. XORG_XBASE=https://www.x.org/releases/individual.
	local XMIRROR="https://artfiles.org/x.org/pub/xorg/individual"
	local XBASE="${XORG_XBASE:-$XMIRROR}"
	local XARCHIVE="${XORG_XARCHIVE:-$XMIRROR}"
	local XCBB="${XORG_XCBB:-$XMIRROR/lib}"
	# Persistent tarball cache (owner-requested): downloaded tarballs are kept here so a
	# clean rebuild reuses them instead of re-downloading. Populated on first miss; survives
	# buildroot wipes (it lives outside the buildroot). Override the dir with PHOENIX_DISTFILES.
	local DISTFILES="${PHOENIX_DISTFILES:-$HOME/.phoenix-distfiles}/xorg"
	mkdir -p "$DISTFILES" 2>/dev/null || true

	mkdir -p "$SRC" "$PREFIX/lib/pkgconfig" "$PREFIX/share/pkgconfig" "$PREFIX/include"
	# xorgproto/xcb-proto .pc land in share/pkgconfig; everything else in lib/pkgconfig.
	export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"

	# --- helpers (transplanted from tools/x11-port/build-x11-phoenix.sh) ---
	_fetch_extract() {
		local nv=$1 url=$2 attempt
		cd "$SRC" || return 1
		[ -d "$nv" ] && return 0
		# 1) Reuse the cached tarball if we have one (no network on a clean rebuild).
		if [ -s "$DISTFILES/$nv.tar.gz" ]; then
			cp "$DISTFILES/$nv.tar.gz" "$nv.tar.gz"
			echo "xorg-libs: $nv from distfiles cache ($DISTFILES)" >&2
		else
			# 2) Cache miss: download (retry), then populate the cache for next time.
			for attempt in 1 2 3; do
				timeout 120 curl -fsSL -o "$nv.tar.gz" "$url" && break
				echo "xorg-libs: $nv fetch attempt $attempt/3 failed; retrying" >&2; sleep 5
			done
			[ -s "$nv.tar.gz" ] || b_die "xorg-libs: $nv download failed from $url"
			cp "$nv.tar.gz" "$DISTFILES/$nv.tar.gz" 2>/dev/null || true
		fi
		# Verify BEFORE extracting, so the check covers the $DISTFILES cache path as
		# well as a fresh download: that cache lives outside the buildroot, is keyed by
		# basename and was reused forever with nothing validating it.
		local _want _have
		_want="$(awk -v f="$nv.tar.gz" '$1 !~ /^#/ && $2 == f { print $1; exit }' \
			"${PREFIX_PORT}/distfiles.sha256" 2>/dev/null || true)"
		if [ -n "$_want" ]; then
			_have="$(sha256sum "$nv.tar.gz" | cut -d" " -f1)"
			[ "$_want" = "$_have" ] || b_die "xorg-libs: $nv.tar.gz sha256 MISMATCH (want $_want, got $_have) -- refusing to extract; delete the cached copy and retry"
		else
			echo "xorg-libs: WARNING $nv.tar.gz has no recorded sha256 (add it to distfiles.sha256)" >&2
		fi
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
				CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17 ${XCFLAGS_EXTRA:-}" \
				LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" $extra \
				|| b_die "xorg-libs: $nv configure failed"
		fi
		# shellcheck disable=2086 -- XMAKE_VARS holds make variable overrides, one
		# word each, and must stay unquoted so make parses them as assignments.
		make install ${XMAKE_VARS:-} || b_die "xorg-libs: $nv build failed"
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
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
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
		_xbuild libXt-1.3.1  "$XBASE/lib/libXt-1.3.1.tar.gz"  "xorg_cv_malloc0_returns_null=yes ac_cv_lib_m_hypot=yes"
	# BITMAP_DEFINES override: libXmu bakes -DBITMAPDIR="$(includedir)/X11/bitmaps"
	# into the library, and includedir is the HOST buildroot prefix -- a path that
	# does not exist on the Pi, so XmuLocateBitmapFile never finds a pixmap and
	# xlogo/xcalc warn `Cannot convert string "xlogo32" to type Pixmap`. Point it at
	# the target path where the xbitmaps data above is staged (measured 2026-09-05).
	XCFLAGS_EXTRA="$PWD_DEFS" \
	XMAKE_VARS='BITMAP_DEFINES=-DBITMAPDIR=\"/usr/include/X11/bitmaps\"' \
		_xbuild libXmu-1.2.1 "$XBASE/lib/libXmu-1.2.1.tar.gz" "xorg_cv_malloc0_returns_null=yes ac_cv_lib_m_hypot=yes"

	# libXpm (lib-only: sxpm/cxpm tools need getpwuid_r)
	if [ ! -f "$PREFIX/lib/libXpm.a" ]; then
		_fetch_extract libXpm-3.5.17 "$XBASE/lib/libXpm-3.5.17.tar.gz"
		( cd "$SRC/libXpm-3.5.17" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       xorg_cv_malloc0_returns_null=no CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17 $PWD_DEFS" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
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
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17 $PWD_DEFS" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make install ) || b_die "xorg-libs: libXaw build failed"
		[ -f "$PREFIX/lib/libXaw7.a" ] && echo "xorg-libs: libXaw-1.0.16 OK" || b_die "xorg-libs: libXaw did not install"
	fi

	# ---- runtime DATA the libraries need on the target ----
	#
	# Both of these are installed by the packages above into $PREFIX, and both are
	# read at RUNTIME by path — so a rootfs with the libraries but without them
	# produces warnings that look like missing fonts:
	#
	#   Warning: locale not supported by Xlib, locale set to C
	#   Warning: X locale modifiers not supported, using default
	#   Warning: Unable to load any usable fontset
	#
	# all three from libX11 failing to find its XLC database (XLOCALEDIR, which the
	# launcher points at /usr/share/X11/locale). Measured on hardware 2026-09-05.
	if [ -f "$PREFIX/share/X11/locale/locale.dir" ]; then
		mkdir -p "${PREFIX_FS}/root/usr/share/X11"
		# rm first: `cp -a src dst` with dst present NESTS as dst/locale/locale.
		rm -rf "${PREFIX_FS}/root/usr/share/X11/locale"
		cp -a "$PREFIX/share/X11/locale" "${PREFIX_FS}/root/usr/share/X11/locale"
		echo "xorg-libs: staged XLC locale DB -> /usr/share/X11/locale ($(du -sh "$PREFIX/share/X11/locale" | cut -f1))"
	else
		echo "xorg-libs: WARN no XLC locale DB in $PREFIX/share/X11 — Xlib will fall back to C"
	fi

	# xbitmaps: the .xbm pixmaps Xt/Xmu apps load BY NAME at runtime through
	# XmuLocateBitmapFile, which searches /usr/include/X11/bitmaps. Without them
	# xlogo and xcalc warn `Cannot convert string "xlogo32"/"calculator" to type
	# Pixmap` and run without their icon. Architecture-independent data.
	if [ ! -f "$PREFIX/include/X11/bitmaps/xlogo32" ]; then
		_fetch_extract xbitmaps-1.1.3 "$XARCHIVE/data/xbitmaps-1.1.3.tar.gz"
		( cd "$SRC/xbitmaps-1.1.3" \
		  && ./configure --prefix="$PREFIX" \
		  && make install ) || b_die "xorg-libs: xbitmaps build failed"
	fi
	if [ -d "$PREFIX/include/X11/bitmaps" ]; then
		mkdir -p "${PREFIX_FS}/root/usr/include/X11"
		rm -rf "${PREFIX_FS}/root/usr/include/X11/bitmaps"
		cp -a "$PREFIX/include/X11/bitmaps" "${PREFIX_FS}/root/usr/include/X11/bitmaps"
		echo "xorg-libs: staged xbitmaps -> /usr/include/X11/bitmaps ($(ls -1 "$PREFIX/include/X11/bitmaps" | wc -l | tr -d ' ') pixmaps)"
	fi

	echo "xorg-libs: LAYER 1 complete (staged into $PREFIX/{lib,include})"
}
