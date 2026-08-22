#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xorg_apps"
	version="1.1.2"
	desc="Small core-X (Xaw/Xt) demo apps: xcalc, xclock, xlogo, xedit"

	# Aggregate port of the classic Athena-widget X clients that make up the
	# Phoenix X11 showcase alongside xterm + Window Maker. All four are core-X
	# (libXaw/Xmu/Xt/Xext over libX11) autotools apps with a homogeneous build,
	# so they share one recipe. The framework anchor source is xcalc (the DAG
	# leaf, needed anyway); xclock/xlogo/xedit are fetched by p_build from the
	# same x.org release mirror (stable release artifacts). Migrated from the
	# coordination repo's tools/x11-port/build-{xcalc,xclock,xlogo,xedit}.sh.
	#
	# xbill (the one non-autoconf X demo, a bespoke hand-written-config.h build)
	# is a SEPARATE port (../xbill) so its higher breakage risk cannot block
	# staging of these four clean autoconf apps.
	# HTTPS (the http:// /archive/ endpoint is unreliable). xcalc-1.1.2 lives only in
	# the /archive/ tree (not /releases/); same sha256 file, HTTPS is just reliable.
	source="https://www.x.org/archive/individual/app"
	archive_filename="xcalc-1.1.2.tar.gz"
	src_path="xcalc-1.1.2/"
	size="189989"
	sha256="dda2b78bcf9d3721d7f7694dca6f2d38778060cc1ed34d1a5a3f1494d59cbbc7"

	license="MIT"          # X11/MIT across all four apps
	license_file="COPYING"

	conflicts=""
	# depends on xorg_libs (the core-X Xaw client/toolkit closure: libXaw7/Xmu/Xt/
	# SM/ICE/Xpm/Xext/xkbfile/X11/xcb/...) built --without-xft / --without-render, so
	# no font/2D stack (xorg_fonts) is needed. ALSO depends on libiconv: the apps link
	# -liconv (satisfying libX11's Xlocale iconv refs, see _make_app XSYS below), and
	# libiconv is a SEPARATE framework port (xorg_libs does NOT stage it) that is
	# otherwise only pulled transitively by dillo, which is ordered AFTER xorg_apps —
	# so without this explicit dep a clean build reaches xorg_apps before libiconv
	# exists and xcalc fails to link (`cannot find -liconv`). Fixed 2026-08-22.
	depends="xorg_libs libiconv"

	supports="phoenix>=3.3"
}

# No patches applied to the xcalc anchor. The one source patch carried here is
# xedit-specific (files/xedit-1.2.2-*.patch) and is applied by p_build to the
# xedit sub-source only, so p_prepare is a no-op (cf. xorg_libs/xorg_fonts).
p_prepare() {
	:
}

p_build() {
	# Framework env: HOST (aarch64a72-phoenix) is NOT a valid config.sub machine,
	# so use the autotools triplet aarch64-phoenix like the other X ports. The X11
	# client/toolkit stack (xorg_libs) is staged into $PREFIX_BUILD/{lib,include}.
	local XHOST="aarch64-phoenix"
	local PREFIX="${PREFIX_BUILD%/}"
	local SYSROOT="${PREFIX_BUILD%/}/sysroot"
	local SRC="${PREFIX_PORT_BUILD}/appsrc"
	local TCGCC="${CROSS}gcc" TCAR="${CROSS}ar" TCRANLIB="${CROSS}ranlib" TCNM="${CROSS}nm"
	# The app tarballs are SPLIT across x.org trees (xlogo only in /releases/, xedit
	# only in /archive/, xclock in both), and http:// is unreliable — so _fetch_extract
	# tries both HTTPS bases per app. XORG_APPS_BASE overrides the primary (mirror).
	local XBASE_RELEASES="${XORG_APPS_BASE:-https://www.x.org/releases/individual/app}"
	local XBASE_ARCHIVE="https://www.x.org/archive/individual/app"

	# Rootfs staging targets.
	local BINDST="${PREFIX_FS}/root/bin"
	local ADDST="${PREFIX_FS}/root/usr/share/X11/app-defaults"
	mkdir -p "$SRC" "$BINDST" "$ADDST" "${PREFIX_PROG}"

	# X clients resolve their libs via pkg-config from the staged prefix.
	export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
	export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"

	# Shared client CFLAGS (same set the whole X app stack uses: sane
	# MAXHOSTNAMELEN, no O_NOFOLLOW, MT-safe pwd API).
	local APP_CFLAGS="-DMAXHOSTNAMELEN=256 -DO_NOFOLLOW=0 -DXOS_USE_MTSAFE_PWDAPI -D_POSIX_THREAD_SAFE_FUNCTIONS=200809L"
	# Full Xaw toolkit link closure (Xaw<->Xmu<->Xt<->X11 is circular, hence the
	# --start-group/--end-group). -liconv satisfies libX11's Xlocale iconv refs;
	# -lxkbfile (XkbStdBell) is appended only for the apps that need it.
	local XAW="-lXaw7 -lXmu -lXt -lSM -lICE -lXpm -lXext"
	local XSYS="-lX11 -liconv -lxcb -lXau -lXdmcp -lphoenix -lc -lm"
	local XCLOSURE="-Wl,--start-group ${XAW} ${XSYS} -Wl,--end-group"
	local XCLOSURE_XKB="-Wl,--start-group ${XAW} -lxkbfile ${XSYS} -Wl,--end-group"

	local CFLAGS_COMMON="--sysroot=$SYSROOT -I$PREFIX/include $APP_CFLAGS"
	local LDFLAGS_COMMON="--sysroot=$SYSROOT -static -L$PREFIX/lib -L$SYSROOT/lib"

	# --- helpers ---------------------------------------------------------------
	# Fetch+extract an x.org app release tarball (stable artifact) into $SRC.
	_fetch_extract() {
		local nv=$1 base attempt
		cd "$SRC" || return 1
		[ -d "$nv" ] && return 0
		for base in "$XBASE_RELEASES" "$XBASE_ARCHIVE"; do
			for attempt in 1 2 3; do
				timeout 120 curl -fsSL -o "$nv.tar.gz" "$base/$nv.tar.gz" && [ -s "$nv.tar.gz" ] && break 2
				echo "xorg_apps: $nv fetch attempt $attempt/3 from $base failed; retrying" >&2; sleep 5
			done
		done
		[ -s "$nv.tar.gz" ] || b_die "xorg_apps: $nv download failed from all bases ($XBASE_RELEASES, $XBASE_ARCHIVE)"
		tar xf "$nv.tar.gz" || b_die "xorg_apps: $nv extract failed"
	}

	# _configure <srcdir> [extra-args...]  (extra CFLAGS via $XA_CFLAGS)
	_configure() {
		local srcdir=$1; shift
		( cd "$srcdir" && ./configure --host="$XHOST" --prefix="${PREFIX_PORT_INSTALL}" "$@" \
			CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
			CFLAGS="${CFLAGS_COMMON} ${XA_CFLAGS:-}" \
			LDFLAGS="${LDFLAGS_COMMON}" ) || b_die "xorg_apps: configure failed in $srcdir"
	}

	# _make_app <srcdir> <app> <ldadd-closure>
	_make_app() {
		local srcdir=$1 app=$2 closure=$3
		# Force a relink so a rebuilt libphoenix.a / X .a is picked up (the app ELF
		# does not depend on the external archives, so make would otherwise skip it).
		rm -f "$srcdir/$app"
		make -C "$srcdir" "${app}_LDADD=${closure}" \
			CFLAGS="${CFLAGS_COMMON} ${XA_CFLAGS:-}" \
			LDFLAGS="${LDFLAGS_COMMON}" || b_die "xorg_apps: $app build failed"
		[ -x "$srcdir/$app" ] || b_die "xorg_apps: $app binary not produced"
	}

	# _install_bin <srcdir/app> <app>   (into rootfs /bin + framework prog dir)
	_install_bin() {
		install -m 755 "$1" "$BINDST/$2"
		install -m 755 "$1" "${PREFIX_PROG}/$2"
		echo "xorg_apps: staged $2 -> /bin"
	}

	# _install_appdefaults <srcdir> <class-file...>
	_install_appdefaults() {
		local srcdir=$1; shift
		local ad
		for ad in "$@"; do
			[ -f "$srcdir/app-defaults/$ad" ] && install -m 644 "$srcdir/app-defaults/$ad" "$ADDST/$ad" \
				&& echo "xorg_apps: staged app-defaults/$ad"
		done
	}

	# _preflight <bin> <name>  — aarch64 static ELF + 0 undefined symbols
	_preflight() {
		local bin=$1 name=$2 f und
		f="$(file "$bin")"
		case "$f" in
		*"ARM aarch64"*"statically linked"*) ;;
		*) b_die "xorg_apps: $name is not an aarch64 static ELF: $f" ;;
		esac
		und="$("$TCNM" -u "$bin" 2>/dev/null)"
		[ -z "$und" ] || { echo "$und"; b_die "xorg_apps: $name has undefined symbols"; }
		echo "xorg_apps: $name [OK] aarch64 static ELF, 0 undefined symbols"
	}

	# --- xcalc (anchor: already extracted into PREFIX_PORT_WORKDIR) -------------
	local xcalcdir="${PREFIX_PORT_WORKDIR%/}"
	XA_CFLAGS="" _configure "$xcalcdir"
	XA_CFLAGS="" _make_app "$xcalcdir" xcalc "$XCLOSURE"
	_install_bin "$xcalcdir/xcalc" xcalc
	_install_appdefaults "$xcalcdir" XCalc XCalc-color
	_preflight "$xcalcdir/xcalc" xcalc

	# --- xclock (core analog/digital clock; --without-xft, needs libxkbfile) ---
	_fetch_extract xclock-1.1.1
	local xclockdir="$SRC/xclock-1.1.1"
	XA_CFLAGS="" _configure "$xclockdir" --without-xft
	XA_CFLAGS="" _make_app "$xclockdir" xclock "$XCLOSURE_XKB"
	_install_bin "$xclockdir/xclock" xclock
	_install_appdefaults "$xclockdir" XClock
	_preflight "$xclockdir/xclock" xclock

	# --- xlogo (core-X logo; --without-render, needs libxkbfile) ---------------
	_fetch_extract xlogo-1.0.7
	local xlogodir="$SRC/xlogo-1.0.7"
	XA_CFLAGS="" _configure "$xlogodir" --without-render
	XA_CFLAGS="" _make_app "$xlogodir" xlogo "$XCLOSURE_XKB"
	_install_bin "$xlogodir/xlogo" xlogo
	_preflight "$xlogodir/xlogo" xlogo

	# --- xedit (Athena editor + bundled Lisp interpreter) ----------------------
	# The Lisp interpreter loads its module .lsp files from a compiled-in LISPDIR
	# via (require "lisp") at startup; that path MUST be Pi-resident (not the host
	# build prefix), and the .lsp tree must be staged there, or LispBegin crashes.
	local LISPDIR_PI=/usr/lib/X11/xedit/lisp
	local xshim="${PREFIX_PORT}/files/xedit-phoenix-shim.h"
	_fetch_extract xedit-1.2.2
	local xeditdir="$SRC/xedit-1.2.2"

	# xedit-1.2.2 ships a 2014 config.sub/config.guess that predate the "phoenix"
	# OS triplet and reject --host=aarch64-phoenix. The xcalc anchor tree ships a
	# newer, phoenix-aware pair; copy them in so configure accepts the host.
	cp -f "$xcalcdir/config.sub" "$xcalcdir/config.guess" "$xeditdir/"

	# Phoenix Lisp savepackage-NULL guard (see the patch header). Idempotent: a
	# reverse dry-run succeeds only when it is already applied.
	local xpatch="${PREFIX_PORT}/files/xedit-1.2.2-phoenix-lispbegin-savepackage-null.patch"
	if ! patch -p1 -R --dry-run -d "$xeditdir" <"$xpatch" >/dev/null 2>&1; then
		patch -p1 -N -d "$xeditdir" <"$xpatch" >/dev/null 2>&1 || b_die "xorg_apps: xedit patch failed"
	fi

	# -Dfinite=isfinite: the bundled Lisp interpreter uses the obsolete BSD
	# finite(); Phoenix libm has only C99 isfinite. Force-include the port shim
	# (copysign / itimer stubs). --with-lispdir bakes the Pi-resident LISPDIR.
	XA_CFLAGS="-Dfinite=isfinite -include $xshim" _configure "$xeditdir" --with-lispdir="$LISPDIR_PI"
	# Drop cached Lisp objects so the new -DLISPDIR takes effect on recompile.
	rm -f "$xeditdir"/lisp/*.o "$xeditdir/liblisp.a"
	# Keep xedit's bundled static libs (-lre -llisp -lmp) ahead of the X closure;
	# overriding LDADD with only the X libs would drop them and fail the link.
	XA_CFLAGS="-Dfinite=isfinite -include $xshim" \
		_make_app "$xeditdir" xedit "-L. -lre -llisp -lmp $XCLOSURE"
	_install_bin "$xeditdir/xedit" xedit
	_install_appdefaults "$xeditdir" Xedit Xedit-color

	# Stage the Lisp interpreter's module .lsp files at the compiled-in LISPDIR
	# (require.c builds LISPDIR + "/" + name + ".lsp"). Mirror modules/ +
	# modules/progmodes/ exactly.
	local lspsrc="$xeditdir/lisp/modules" lspdst="${PREFIX_FS}/root${LISPDIR_PI}"
	mkdir -p "$lspdst/progmodes"
	cp "$lspsrc"/*.lsp "$lspdst/" 2>/dev/null || true
	cp "$lspsrc"/progmodes/*.lsp "$lspdst/progmodes/" 2>/dev/null || true
	echo "xorg_apps: staged xedit Lisp modules -> ${LISPDIR_PI}"

	_preflight "$xeditdir/xedit" xedit
	# The Lisp interpreter must carry the Pi-resident LISPDIR, not the host prefix.
	strings "$xeditdir/xedit" | grep -q "$LISPDIR_PI" \
		|| b_die "xorg_apps: xedit binary missing LISPDIR=$LISPDIR_PI (stale -DLISPDIR?)"
	echo "xorg_apps: xedit [OK] LISPDIR baked ($LISPDIR_PI)"

	echo "xorg_apps: complete — xcalc xclock xlogo xedit staged into /bin"
}
