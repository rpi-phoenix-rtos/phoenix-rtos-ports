#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xorg-fonts"
	version="2.13.2"
	desc="Font + 2D graphics stack for X11 on Phoenix-RTOS (freetype/fontconfig/cairo/libXft)"

	# Aggregate LAYER-2 port (glib-free tier). Depends on xorg-libs (Layer 1) being
	# staged into $PREFIX_BUILD/{lib,include}. Owner decision E4 paused the GTK
	# desktop environments, so the glib-gated text-shaping stack (harfbuzz/pango/
	# fribidi + glib2) is DEFERRED — this port carries only what the current
	# WindowMaker + Xft stack needs: libpng, jpeg, freetype, expat, fontconfig,
	# libXft, cairo. Anchor source = freetype (the tier leaf); the rest are fetched
	# in p_build. See docs/inprogress/x11-ports-migration-spec.md.
	source="https://download.savannah.gnu.org/releases/freetype/"
	archive_filename="freetype-${version}.tar.gz"
	src_path="freetype-${version}/"
	size="3875020"
	sha256="1ac27e16c134a7f2ccea177faba19801131116fd682efc1f5737037c5db224b5"

	license="MIT"   # FreeType(FTL/GPL), fontconfig(MIT), cairo(LGPL/MPL), libpng, jpeg(IJG), expat(MIT)
	license_file="LICENSE.TXT"

	conflicts=""
	depends="xorg-libs zlib"   # X libs (libXrender/libX11/pixman) + zlib

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

	_fetch_extract() {
		local nv=$1 url=$2 attempt
		cd "$SRC" || return 1
		[ -d "$nv" ] && return 0
		for attempt in 1 2 3; do
			timeout 180 curl -fsSL -o "$nv.tar.gz" "$url" && break
			echo "xorg-fonts: $nv fetch $attempt/3 failed; retry" >&2; sleep 5
		done
		[ -s "$nv.tar.gz" ] || b_die "xorg-fonts: $nv download failed from $url"
		tar xf "$nv.tar.gz" || b_die "xorg-fonts: $nv extract failed"
	}

	# --- libpng (needs zlib, already staged) ---
	if [ ! -f "$PREFIX/lib/libpng16.a" ]; then
		_fetch_extract libpng-1.6.40 "https://download.sourceforge.net/libpng/libpng-1.6.40.tar.gz"
		( cd "$SRC/libpng-1.6.40" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CPPFLAGS="--sysroot=$SYSROOT -I$PREFIX/include" CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include" \
		       LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" --with-zlib-prefix="$PREFIX" \
		  && make -j4 && make install ) || b_die "xorg-fonts: libpng failed"
		echo "xorg-fonts: libpng-1.6.40 OK"
	fi

	# --- jpeg (IJG) ---
	if [ ! -f "$PREFIX/lib/libjpeg.a" ] || [ ! -f "$PREFIX/include/jpeglib.h" ]; then
		_fetch_extract jpeg-9e "https://www.ijg.org/files/jpegsrc.v9e.tar.gz"
		( cd "$SRC/jpeg-9e" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" CFLAGS="--sysroot=$SYSROOT" \
		  && make -j4 && make install ) || b_die "xorg-fonts: jpeg failed"
		echo "xorg-fonts: jpeg-9e OK"
	fi

	# --- freetype (minimal: break the freetype<->harfbuzz cycle) ---
	if [ ! -f "$PREFIX/lib/libfreetype.a" ]; then
		_fetch_extract freetype-2.13.2 "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.gz"
		( cd "$SRC/freetype-2.13.2" \
		  && ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		       --without-zlib --without-png --without-harfbuzz --without-bzip2 --without-brotli \
		  && make -j4 && make install ) || b_die "xorg-fonts: freetype failed"
		echo "xorg-fonts: freetype-2.13.2 OK"
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
		_fetch_extract fontconfig-2.14.2 "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.14.2.tar.xz"
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
		       --with-cache-dir=/var/cache/fontconfig --with-default-fonts=/usr/share/fonts --sysconfdir=/etc \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" CC_FOR_BUILD=gcc \
		       ac_cv_func_random=no ac_cv_func_initstate=no ac_cv_func_setstate=no ac_cv_func_random_r=no \
       ac_cv_member_struct_statfs_f_flags=no ac_cv_member_struct_statfs_f_fstypename=no \
       ac_cv_member_struct_statvfs_f_basetype=no ac_cv_member_struct_statvfs_f_fstypename=no \
		       FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" FREETYPE_LIBS="-L$PREFIX/lib -lfreetype" \
		       EXPAT_CFLAGS="-I$PREFIX/include" EXPAT_LIBS="-L$PREFIX/lib -lexpat" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
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
		_fetch_extract libXft-2.3.8 "https://www.x.org/releases/individual/lib/libXft-2.3.8.tar.gz"
		( cd "$SRC/libXft-2.3.8" \
		  && PKG_CONFIG="$PKGC" ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" FREETYPE_LIBS="-L$PREFIX/lib -lfreetype" \
		       FONTCONFIG_CFLAGS="-I$PREFIX/include" FONTCONFIG_LIBS="-L$PREFIX/lib -lfontconfig -lexpat -lfreetype" \
		       XRENDER_CFLAGS="-I$PREFIX/include" XRENDER_LIBS="-L$PREFIX/lib -lXrender -lX11" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		  && make install ) || b_die "xorg-fonts: libXft failed"
		echo "xorg-fonts: libXft-2.3.8 OK"
	fi

	# --- cairo (core lib only: skip the -pthread util) ---
	if [ ! -f "$PREFIX/lib/libcairo.a" ]; then
		_fetch_extract cairo-1.16.0 "https://cairographics.org/releases/cairo-1.16.0.tar.xz"
		( cd "$SRC/cairo-1.16.0" \
		  && ax_cv_c_float_words_bigendian=no \
		     FONTCONFIG_CFLAGS="-I$PREFIX/include" FONTCONFIG_LIBS="-L$PREFIX/lib -lfontconfig" \
		     FREETYPE_CFLAGS="-I$PREFIX/include/freetype2" FREETYPE_LIBS="-L$PREFIX/lib -lfreetype" \
		     png_CFLAGS="-I$PREFIX/include" png_LIBS="-L$PREFIX/lib -lpng16 -lz" \
		     CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		     CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -O2" LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib" \
		     ./configure --host="$XHOST" --prefix="$PREFIX" --disable-shared --enable-static \
		       --enable-ft --enable-fc --enable-png \
		       --disable-xlib --disable-xcb --disable-gl \
		       --disable-script --disable-ps --disable-pdf --disable-svg --disable-interpreter \
		  && make -C src && make -C src install ) || b_die "xorg-fonts: cairo failed"
		# cairo.pc lands from `make -C src install`? ensure it's present
		[ -f "$PREFIX/lib/pkgconfig/cairo.pc" ] || { pc=$(find "$SRC/cairo-1.16.0" -name cairo.pc | head -1); [ -n "$pc" ] && cp "$pc" "$PREFIX/lib/pkgconfig/"; }
		echo "xorg-fonts: cairo-1.16.0 OK"
	fi

	echo "xorg-fonts: LAYER 2 (glib-free tier) complete -> $PREFIX/{lib,include}"
}
