#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xorg_fonts"
	version="2.13.2"
	desc="Font + 2D graphics stack for X11 on Phoenix-RTOS (freetype/fontconfig/cairo/libXft)"

	# Aggregate LAYER-2 port (glib-free tier). Depends on xorg-libs (Layer 1) being
	# staged into $PREFIX_BUILD/{lib,include}. Owner decision E4 paused the GTK
	# desktop environments, so the glib-gated text-shaping stack (harfbuzz/pango/
	# fribidi + glib2) is DEFERRED — this port carries only what the current
	# WindowMaker + Xft stack needs: libpng, freetype, expat, fontconfig,
	# libXft, cairo. Anchor source = freetype (the tier leaf); the rest are fetched
	# in p_build. See docs/inprogress/x11-ports-migration-spec.md.
	# Anchor = freetype (the tier leaf). Served from the SourceForge CDN mirror:
	# the upstream savannah.gnu.org host is frequently unreachable/slow and the
	# phoesys ports mirror does not cache freetype, so both b_port_download hops
	# would fail. SourceForge ships the byte-identical release (sha256 verified).
	source="https://downloads.sourceforge.net/freetype/"
	archive_filename="freetype-${version}.tar.gz"
	src_path="freetype-${version}/"
	size="3875020"
	sha256="1ac27e16c134a7f2ccea177faba19801131116fd682efc1f5737037c5db224b5"

	license="MIT"   # FreeType(FTL/GPL), fontconfig(MIT), cairo(LGPL/MPL), libpng, expat(MIT)
	license_file="LICENSE.TXT"

	conflicts=""
	depends="xorg_libs zlib"   # X libs (libXrender/libX11/pixman) + zlib

	supports="phoenix>=3.3"
}

p_prepare() {
	:
}

p_build() {
	local XHOST="aarch64-phoenix"
	local PREFIX="${PREFIX_BUILD%/}"
	local SYSROOT="${PREFIX_BUILD%/}/sysroot"
	local SRC="${PREFIX_PORT_BUILD}/fontsrc"
	local TCGCC="${CROSS}gcc" TCAR="${CROSS}ar" TCRANLIB="${CROSS}ranlib"

	command -v gperf >/dev/null 2>&1 || b_die "xorg-fonts: host 'gperf' missing (fontconfig codegen) — apt-get install gperf"

	mkdir -p "$SRC" "$PREFIX/lib/pkgconfig" "$PREFIX/include"
	export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
	local PKGC="pkg-config --static"

	# x.org tarballs come from the artfiles.org mirror by default — x.org's own CDN
	# (xorg.freedesktop.org) is frequently slow or unreachable. Override with
	# XORG_XBASE (e.g. XORG_XBASE=https://www.x.org/releases/individual).
	local XMIRROR="https://artfiles.org/x.org/pub/xorg/individual"
	local XBASE="${XORG_XBASE:-$XMIRROR}"
	# Persistent shared tarball cache (owner-requested), keyed by real tarball name so
	# it is SHARED with xorg_libs/xorg_server (same $DISTFILES). A cached tarball is
	# reused with no network (offline-reproducible, survives buildroot wipes); a cache
	# miss downloads then populates. Override the dir with PHOENIX_DISTFILES.
	local DISTFILES="${PHOENIX_DISTFILES:-$HOME/.phoenix-distfiles}/xorg"
	# Args after $nv are URLs tried in order (primary then fallback mirrors) so a
	# flaky/down CDN falls back to a mirror — a fresh build (empty cache, e.g. the
	# Docker release container) survives one CDN outage. Cache-first: a cached
	# tarball (keyed by basename) skips the network entirely. curl -o writes to the
	# primary's basename regardless of a mirror's remote name.
	_fetch_extract() {
		local nv=$1; shift
		local fname="${1##*/}" u attempt
		cd "$SRC" || return 1
		[ -d "$nv" ] && return 0
		mkdir -p "$DISTFILES"
		if [ -s "$DISTFILES/$fname" ]; then
			echo "xorg-fonts: $nv from distfiles cache ($DISTFILES)" >&2
			cp "$DISTFILES/$fname" "$SRC/$fname"
		else
			for u in "$@"; do
				for attempt in 1 2 3; do
					timeout 180 curl -fsSL -o "$SRC/$fname" "$u" && break 2
					echo "xorg-fonts: $nv fetch $attempt/3 from $u failed; retry" >&2; sleep 5
				done
			done
			[ -s "$SRC/$fname" ] || b_die "xorg-fonts: $nv download failed (tried: $*)"
			cp "$SRC/$fname" "$DISTFILES/$fname"
		fi
		# Verify BEFORE extracting, so the check covers the $DISTFILES cache path as
		# well as a fresh download -- the cache is outside the buildroot, keyed by
		# basename and reused forever, and nothing validated it until now.
		local _want _have
		_want="$(awk -v f="$fname" '$1 !~ /^#/ && $2 == f { print $1; exit }' \
			"${PREFIX_PORT}/distfiles.sha256" 2>/dev/null || true)"
		if [ -n "$_want" ]; then
			_have="$(sha256sum "$SRC/$fname" | cut -d" " -f1)"
			[ "$_want" = "$_have" ] || b_die "xorg-fonts: $fname sha256 MISMATCH (want $_want, got $_have) -- refusing to extract; delete the cached copy in $DISTFILES and retry"
		else
			echo "xorg-fonts: WARNING $fname has no recorded sha256 (add it to distfiles.sha256)" >&2
		fi
		tar xf "$SRC/$fname" || b_die "xorg-fonts: $nv extract failed"
	}

	# --- libpng (needs zlib, already staged) ---
	if [ ! -f "$PREFIX/lib/libpng16.a" ]; then
		_fetch_extract libpng-1.6.40 "https://download.sourceforge.net/libpng/libpng-1.6.40.tar.gz"
		( cd "$SRC/libpng-1.6.40" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CPPFLAGS="--sysroot=$SYSROOT -I$PREFIX/include" CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17" \
		       LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" --with-zlib-prefix="$PREFIX" \
		  && make -j4 && make install ) || b_die "xorg-fonts: libpng failed"
		echo "xorg-fonts: libpng-1.6.40 OK"
	fi

	# --- jpeg: DELIBERATELY NOT BUILT HERE (2026-09-04) ---
	# This port used to build IJG jpeg-9e into $PREFIX/lib/libjpeg.a +
	# $PREFIX/include/jpeglib.h -- the SAME two paths the `libjpeg` port fills with
	# libjpeg-turbo 3.0.4, with no dependency edge between them, so which
	# implementation a consumer got depended on build order. They are not
	# ABI-compatible (turbo defaults to the libjpeg 6.2 ABI, IJG 9e is 90, and
	# jpeg_decompress_struct grew fields across 7/8/9), so a consumer could compile
	# against one header and link the other archive: silent corruption, not a link
	# error.
	#
	# Nothing here uses jpeg -- it was built for the downstream Xft/WindowMaker
	# stack -- and every real consumer already depends on the libjpeg port
	# (fltk:27, supertuxkart:36, dillo via fltk), so removing it leaves turbo as
	# the single producer and needs no `depends` change. See
	# docs/misc/2026-09-04-port-determinism-audit.md finding 1.

	# --- freetype (minimal: break the freetype<->harfbuzz cycle) ---
	if [ ! -f "$PREFIX/lib/libfreetype.a" ]; then
		_fetch_extract freetype-2.13.2 "https://downloads.sourceforge.net/freetype/freetype-2.13.2.tar.gz"
		( cd "$SRC/freetype-2.13.2" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		       --without-zlib --without-png --without-harfbuzz --without-bzip2 --without-brotli \
		  && make -j4 && make install ) || b_die "xorg-fonts: freetype failed"
		echo "xorg-fonts: freetype-2.13.2 OK"
	fi

	# --- libfontenc (server-side font-encoding lib; needs zlib + xorgproto from L1) ---
	if [ ! -f "$PREFIX/lib/libfontenc.a" ]; then
		_fetch_extract libfontenc-1.1.8 "$XBASE/lib/libfontenc-1.1.8.tar.gz"
		( cd "$SRC/libfontenc-1.1.8" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make -j4 && make install ) || b_die "xorg-fonts: libfontenc failed"
		echo "xorg-fonts: libfontenc-1.1.8 OK"
	fi

	# --- libXfont2 (server-side font lib; needed by xorg-server). Lib-only: its
	#     in-tree font tools fail to link (deferred libc syms), so install .a + headers.
	#     -DO_NOFOLLOW=0 -DNOFILES_MAX=256 + the cross malloc0/hypot run-test cache. ---
	if [ ! -f "$PREFIX/lib/libXfont2.a" ]; then
		_fetch_extract libXfont2-2.0.6 "$XBASE/lib/libXfont2-2.0.6.tar.gz"
		( cd "$SRC/libXfont2-2.0.6" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       ac_cv_lib_m_hypot=yes xorg_cv_malloc0_returns_null=no \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17 -DO_NOFOLLOW=0 -DNOFILES_MAX=256" \
		       LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make ) || true   # tools may fail to link; the .a is what we need
		[ -f "$SRC/libXfont2-2.0.6/.libs/libXfont2.a" ] || b_die "xorg-fonts: libXfont2.a not produced"
		cp "$SRC/libXfont2-2.0.6/.libs/libXfont2.a" "$PREFIX/lib/"
		( cd "$SRC/libXfont2-2.0.6" && make install-data >/dev/null 2>&1 ) || true
		echo "xorg-fonts: libXfont2-2.0.6 OK (lib only)"
	fi

	# --- expat (fontconfig's XML parser) ---
	if [ ! -f "$PREFIX/lib/libexpat.a" ]; then
		_fetch_extract expat-2.5.0 "https://github.com/libexpat/libexpat/releases/download/R_2_5_0/expat-2.5.0.tar.bz2"
		( cd "$SRC/expat-2.5.0" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" CFLAGS="--sysroot=$SYSROOT" \
		       --without-docbook --without-examples --without-tests \
		  && make -j4 && make install ) || b_die "xorg-fonts: expat failed"
		echo "xorg-fonts: expat-2.5.0 OK"
	fi

	# --- fontconfig (needs freetype + expat; two Phoenix inline patches; needs gperf) ---
	if [ ! -f "$PREFIX/lib/libfontconfig.a" ]; then
		_fetch_extract fontconfig-2.14.2 "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.14.2.tar.xz" "https://mirrors.mit.edu/macports/distfiles/fontconfig/fontconfig-2.14.2.tar.xz"
		local fc="$SRC/fontconfig-2.14.2"
		# (a) fccache.c: libphoenix <sys/time.h> defines a non-standard VALUE-based
		#     timercmp(); fontconfig passes struct-timeval POINTERS — redefine standard.
		if ! grep -q 'Phoenix-RTOS port: libphoenix' "$fc/src/fccache.c"; then
			perl -0pi -e 's{(#ifndef O_BINARY\n#define O_BINARY 0\n#endif\n)}{$1\n/* Phoenix-RTOS port: pointer-based timercmp */\n#ifdef timercmp\n#undef timercmp\n#endif\n#define timercmp(a, b, CMP) \\\n\t(((a)->tv_sec == (b)->tv_sec) ? \\\n\t\t((a)->tv_usec CMP (b)->tv_usec) : \\\n\t\t((a)->tv_sec CMP (b)->tv_sec))\n}' "$fc/src/fccache.c"
		fi
		# (b) fccompat.c: a static initializer must be constant — time(NULL) is not;
		#     seed FcRandom lazily on first call.
		if ! grep -q 'Phoenix-RTOS port: a static initializer' "$fc/src/fccompat.c"; then
			perl -0pi -e 's{    static unsigned int seed = time \(NULL\);\n\n    result = rand_r \(&seed\);}{    /* Phoenix-RTOS port: lazy seed (static init must be constant) */\n    static unsigned int seed = 0;\n    static FcBool seeded = FcFalse;\n\n    if (seeded != FcTrue)\n    \{\n\tseed = (unsigned int) time (NULL);\n\tseeded = FcTrue;\n    \}\n    result = rand_r (&seed);}' "$fc/src/fccompat.c"
		fi
		local fcstage="${PREFIX_PORT_BUILD}/fc-stage"
		rm -rf "$fcstage"
		( cd "$fc" \
		  && PKG_CONFIG="$PKGC" ./configure --host="$XHOST" --build=x86_64-pc-linux-gnu \
		       --prefix="$PREFIX" --disable-shared --enable-static --disable-docs \
		       --with-cache-dir=/var/cache/fontconfig --with-default-fonts=/usr/share/fonts/truetype --sysconfdir=/etc \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" CC_FOR_BUILD=gcc \
		       ac_cv_func_random=no ac_cv_func_initstate=no ac_cv_func_setstate=no ac_cv_func_random_r=no \
       ac_cv_member_struct_statfs_f_flags=no ac_cv_member_struct_statfs_f_fstypename=no \
       ac_cv_member_struct_statvfs_f_basetype=no ac_cv_member_struct_statvfs_f_fstypename=no \
		       FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" FREETYPE_LIBS="-L$PREFIX/lib -lfreetype" \
		       EXPAT_CFLAGS="-I$PREFIX/include" EXPAT_LIBS="-L$PREFIX/lib -lexpat" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make \
		  && make DESTDIR="$fcstage" install ) || b_die "xorg-fonts: fontconfig failed"
		# Land lib + headers + .pc into $PREFIX (DESTDIR install avoids the on-host
		# fc-cache run; the .pc prefix must be rewritten from the DESTDIR path).
		cp "$fcstage$PREFIX/lib/libfontconfig.a" "$PREFIX/lib/" || b_die "xorg-fonts: no libfontconfig.a"
		cp -r "$fcstage$PREFIX/include/fontconfig" "$PREFIX/include/"
		cp "$fcstage$PREFIX/lib/pkgconfig/fontconfig.pc" "$PREFIX/lib/pkgconfig/"
		echo "xorg-fonts: fontconfig-2.14.2 OK"
	fi

	# --- libXft (freetype + fontconfig + libXrender + libX11, all staged) ---
	if [ ! -f "$PREFIX/lib/libXft.a" ]; then
		_fetch_extract libXft-2.3.8 "$XBASE/lib/libXft-2.3.8.tar.gz"
		( cd "$SRC/libXft-2.3.8" \
		  && PKG_CONFIG="$PKGC" ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" FREETYPE_LIBS="-L$PREFIX/lib -lfreetype" \
		       FONTCONFIG_CFLAGS="-I$PREFIX/include" FONTCONFIG_LIBS="-L$PREFIX/lib -lfontconfig -lexpat -lfreetype" \
		       XRENDER_CFLAGS="-I$PREFIX/include" XRENDER_LIBS="-L$PREFIX/lib -lXrender -lX11" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make install ) || b_die "xorg-fonts: libXft failed"
		echo "xorg-fonts: libXft-2.3.8 OK"
	fi

	# --- cairo (core lib only: skip the -pthread util) ---
	if [ ! -f "$PREFIX/lib/libcairo.a" ]; then
		_fetch_extract cairo-1.16.0 "https://cairographics.org/releases/cairo-1.16.0.tar.xz" "https://mirrors.mit.edu/macports/distfiles/cairo/cairo-1.16.0.tar.xz"
		( cd "$SRC/cairo-1.16.0" \
		  && ax_cv_c_float_words_bigendian=no \
		     FONTCONFIG_CFLAGS="-I$PREFIX/include" FONTCONFIG_LIBS="-L$PREFIX/lib -lfontconfig" \
		     FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" FREETYPE_LIBS="-L$PREFIX/lib -lfreetype" \
		     png_CFLAGS="-I$PREFIX/include" png_LIBS="-L$PREFIX/lib -lpng16 -lz" \
		     CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		     CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -std=gnu17 -O2" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		     ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       --enable-ft --enable-fc --enable-png \
		       --disable-xlib --disable-xcb --disable-gl \
		       --disable-script --disable-ps --disable-pdf --disable-svg --disable-interpreter \
		  && make -C src && make -C src install ) || b_die "xorg-fonts: cairo failed"
		# cairo.pc lands from `make -C src install`? ensure it's present
		[ -f "$PREFIX/lib/pkgconfig/cairo.pc" ] || { pc=$(find "$SRC/cairo-1.16.0" -name cairo.pc | head -1); [ -n "$pc" ] && cp "$pc" "$PREFIX/lib/pkgconfig/"; }
		echo "xorg-fonts: cairo-1.16.0 OK"
	fi

	# --- X11 CORE FONT DATA (noarch) ------------------------------------------
	#
	# Until 2026-09-05 the rootfs shipped NO X11 font files at all: this port
	# builds the font LIBRARIES (freetype, fontconfig, libXft, libfontenc), and
	# nothing installed font DATA. The desktop still rendered text, but only from
	# libXfont2's built-in `fixed` compiled into Xphoenix -- i.e. the whole X
	# server had exactly one font. Measured the same day: adding a fontconfig
	# config alone changed nothing, because the clients take the CORE-font path,
	# so what is missing is PCFs plus a fonts.dir, not a config.
	#
	# These two packages are ARCHITECTURE-INDEPENDENT data. They are built with
	# the HOST toolchain on purpose (no --host): the build runs bdftopcf and
	# mkfontdir/mkfontscale, which must execute here, and everything installed is
	# .pcf.gz / .dir / .alias text consumed by the target at runtime.
	local FONTSTAGE="$PREFIX_PORT_BUILD/fontdata"
	if [ ! -f "$FONTSTAGE/usr/share/fonts/X11/misc/fonts.dir" ]; then
		command -v bdftopcf >/dev/null 2>&1 ||
			b_die "xorg-fonts: host 'bdftopcf' missing (X core fonts) — apt-get install xfonts-utils"
		command -v mkfontdir >/dev/null 2>&1 ||
			b_die "xorg-fonts: host 'mkfontdir' missing (X core fonts) — apt-get install xfonts-utils"

		# encodings/: the .enc maps the server's font path already references
		# (Xphoenix looks for .../fonts/X11/encodings/encodings.dir).
		_fetch_extract encodings-1.0.7 "$XBASE/font/encodings-1.0.7.tar.gz"
		( cd "$SRC/encodings-1.0.7" \
		  && ./configure --prefix=/usr --with-fontrootdir=/usr/share/fonts/X11 \
		  && make && make install DESTDIR="$FONTSTAGE" ) || b_die "xorg-fonts: encodings failed"

		# misc-fixed: the classic X core bitmap family (6x13, 9x15, 10x20, ...),
		# which is what an X client asking for "fixed" or a plain XLFD gets.
		_fetch_extract font-misc-misc-1.1.3 "$XBASE/font/font-misc-misc-1.1.3.tar.gz"
		( cd "$SRC/font-misc-misc-1.1.3" \
		  && ./configure --prefix=/usr --with-fontrootdir=/usr/share/fonts/X11 \
		  && make && make install DESTDIR="$FONTSTAGE" ) || b_die "xorg-fonts: font-misc-misc failed"

		# font-alias: the fonts.alias files that map SHORT names to XLFDs. Without
		# them the Xt apps fail with `Cannot convert string "8x13" to type
		# FontStruct` even though 8x13.pcf.gz is installed -- the PCFs answer a
		# full XLFD, the aliases answer the names applications actually ask for
		# (fixed, 8x13, 9x15, ...). Measured on hardware 2026-09-05.
		_fetch_extract font-alias-1.0.5 "$XBASE/font/font-alias-1.0.5.tar.gz"
		( cd "$SRC/font-alias-1.0.5" \
		  && ./configure --prefix=/usr --with-fontrootdir=/usr/share/fonts/X11 \
		  && make && make install DESTDIR="$FONTSTAGE" ) || b_die "xorg-fonts: font-alias failed"

		# adobe-75dpi: the -adobe-* XLFDs the Xaw/Xt widgets request by name --
		# xcalc asks for "-adobe-symbol-*-*-*-*-*-120-*" and rendered nothing
		# without it. Also gives Helvetica/Times/Courier at 75dpi for xterm, xedit
		# and the rest of the app set.
		# cursor-misc: the "cursor" font. Every X client that calls
		# XCreateFontCursor (the WM for its root/resize cursors, xterm, ...) opens
		# it by name, and it lives in its own package -- font-misc-misc does NOT
		# contain it. Before this, the port's misc/ replaced the host-staged one
		# and silently dropped cursor.pcf.gz, leaving a font path with no cursor
		# font at all.
		_fetch_extract font-cursor-misc-1.0.4 "$XBASE/font/font-cursor-misc-1.0.4.tar.gz"
		( cd "$SRC/font-cursor-misc-1.0.4" \
		  && ./configure --prefix=/usr --with-fontrootdir=/usr/share/fonts/X11 \
		  && make && make install DESTDIR="$FONTSTAGE" ) || b_die "xorg-fonts: font-cursor-misc failed"

		_fetch_extract font-adobe-75dpi-1.0.4 "$XBASE/font/font-adobe-75dpi-1.0.4.tar.gz"
		( cd "$SRC/font-adobe-75dpi-1.0.4" \
		  && ./configure --prefix=/usr --with-fontrootdir=/usr/share/fonts/X11 \
		  && make && make install DESTDIR="$FONTSTAGE" ) || b_die "xorg-fonts: font-adobe-75dpi failed"
	fi

	if [ -d "$FONTSTAGE/usr/share/fonts/X11" ]; then
		mkdir -p "${PREFIX_FS}/root/usr/share/fonts"
		cp -a "$FONTSTAGE/usr/share/fonts/X11" "${PREFIX_FS}/root/usr/share/fonts/"
		echo "xorg-fonts: staged X11 core fonts -> /usr/share/fonts/X11 ($(find "$FONTSTAGE/usr/share/fonts/X11" -name '*.pcf.gz' | wc -l) pcf, $(du -sh "$FONTSTAGE/usr/share/fonts/X11" | cut -f1))"
	fi

	echo "xorg-fonts: LAYER 2 (glib-free tier) complete -> $PREFIX/{lib,include}"
}
