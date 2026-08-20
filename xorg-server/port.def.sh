#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xorg-server"
	version="1.20.14"
	desc="X.Org kdrive fbdev server (Xphoenix) for Phoenix-RTOS"

	# Aggregate LAYER-3 port of the hybrid X11 migration. Builds the xorg-server
	# 1.20.14 kdrive CORE archives, then hand-ld-links the Phoenix fbdev DDX
	# (Xphoenix) from them + the X libs staged by xorg-libs (L1) + xorg-fonts (L2).
	# The DDX backend, the XKB compiled-in keymap, and libmd (SHA1) are carried in
	# files/ (source-of-truth, migrated from tools/x11-port). This is the CURRENT
	# fbdev-DDX server (the working interim); modernizing to a glamor/modesetting
	# path is the future goal G-XORG-MODERN. Recipe validated end-to-end
	# (XPHOENIX-LINK-OK). See docs/inprogress/x11-ports-migration-spec.md + MASTER
	# plan §J.
	source="https://www.x.org/releases/individual/xserver/"
	archive_filename="xorg-server-${version}.tar.gz"
	src_path="xorg-server-${version}/"
	size="9416754"
	sha256="54b199c9280ff8bf0f73a54a759645bd0eeeda7255d1c99310d5b7595f3ac066"

	license="MIT"
	license_file="COPYING"

	conflicts=""
	depends="xorg-libs xorg-fonts zlib"

	supports="phoenix>=3.3"
}

# Apply the RECORD malloc(0)->NULL assert-guard patch to the xorg-server tree
# (record/ early-return when numContexts==0; WindowMaker client disconnect trips it).
p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	local XHOST="aarch64-phoenix"
	local PREFIX="${PREFIX_BUILD%/}"
	local SYSROOT="${PREFIX_BUILD%/}/sysroot"
	local KD="${PREFIX_PORT_WORKDIR}"           # the framework-extracted xorg-server tree
	local FILES="${PREFIX_PORT}/files"
	local TCGCC="${CROSS}gcc" TCAR="${CROSS}ar" TCRANLIB="${CROSS}ranlib"

	# --- 1. libmd (SHA1) — --with-sha1=libmd avoids openssl/nettle/gcrypt ---
	if [ ! -f "$PREFIX/lib/libmd.a" ]; then
		( cd "$FILES/libmd" \
		  && "$TCGCC" --sysroot="$SYSROOT" -O2 -c sha1.c -o sha1.o \
		  && "$TCAR" rcs libmd.a sha1.o && "$TCRANLIB" libmd.a \
		  && cp libmd.a "$PREFIX/lib/" && cp sha1.h "$PREFIX/include/" ) \
		  || b_die "xorg-server: libmd build failed"
		echo "xorg-server: libmd OK"
	fi

	# --- 2. configure + build the kdrive CORE archives ---
	if [ ! -f "$KD/dix/.libs/libdix.a" ]; then
		( cd "$KD" \
		  && PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig" \
		     PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig" \
		     ./configure --host="$XHOST" --prefix="$PREFIX" \
		       --enable-kdrive --disable-xephyr --with-sha1=libmd \
		       --disable-xorg --disable-xwayland --disable-xnest --disable-xvfb --disable-dmx \
		       --disable-glamor --disable-dri --disable-dri2 --disable-dri3 --disable-glx \
		       --disable-int10-module --disable-vgahw --disable-vbe --disable-xdmcp \
		       --disable-xinerama --without-dtrace --disable-systemd-logind --disable-secure-rpc \
		       --disable-config-udev --disable-config-hal --without-systemd-daemon --disable-unit-tests \
		       CC="$TCGCC" AR="$TCAR" RANLIB="$TCRANLIB" \
		       CFLAGS="--sysroot=$SYSROOT -I$PREFIX/include -DMAXHOSTNAMELEN=256 -DXOS_USE_MTSAFE_PWDAPI -D_POSIX_THREAD_SAFE_FUNCTIONS=200809L -DO_NOFOLLOW=0 -DSI_USER=0 -DHAVE_CBRT=1" \
		       LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/lib" ) \
		  || b_die "xorg-server: configure failed"
		# --disable-xephyr = no server binary linked here; make only builds the libs.
		# A trailing no-op target can exit non-zero, so the archive check is the gate.
		( cd "$KD" && make -j"$(nproc)" ) || echo "xorg-server: make non-zero, verifying archives"
		[ -f "$KD/dix/.libs/libdix.a" ] || b_die "xorg-server: core archives missing after make"
		echo "xorg-server: core archives OK"
	fi

	# --- 3. compile the Phoenix fbdev DDX + the XKB-no-xkbcomp ddxLoad ---
	local DDX="$KD/hw/kdrive/fbdev"
	local XKBDIR="$FILES/xkb"
	mkdir -p "$DDX"
	cp "$FILES/ddx/fbdev.c" "$FILES/ddx/hid_evdev_map.h" "$DDX/"
	local CF="--sysroot=$SYSROOT -fno-strict-aliasing -D_DEFAULT_SOURCE -D_BSD_SOURCE \
-DHAS_FCHOWN -DHAS_STICKY_DIR_BIT -DMAXHOSTNAMELEN=256 -DXOS_USE_MTSAFE_PWDAPI \
-D_POSIX_THREAD_SAFE_FUNCTIONS=200809L -DO_NOFOLLOW=0 -DSI_USER=0 \
-I$PREFIX/include -I$PREFIX/include/pixman-1 -I$PREFIX/include/freetype2"
	local INCS="-DHAVE_DIX_CONFIG_H -DHAVE_CONFIG_H -I$KD/include \
-I$KD/Xext -I$KD/composite -I$KD/damageext -I$KD/xfixes -I$KD/Xi -I$KD/mi \
-I$KD/miext/sync -I$KD/miext/shadow -I$KD/miext/damage \
-I$KD/render -I$KD/randr -I$KD/fb -I$KD/dbe -I$KD/present \
-I$KD/hw/kdrive/src -I$KD/hw/kdrive/linux -I$DDX"
	# shellcheck disable=2086
	$TCGCC $CF $INCS -c "$DDX/fbdev.c" -o "$DDX/fbdev.o" || b_die "xorg-server: fbdev.c compile failed"
	# shellcheck disable=2086
	$TCGCC $CF $INCS -I"$KD/xkb" -I"$XKBDIR" -c "$FILES/ddx/ddxLoad.c" -o "$DDX/ddxLoad.o" \
		|| b_die "xorg-server: ddxLoad.c compile failed"

	# --- 4. hand-ld link Xphoenix (circular core refs -> --start-group; ddxLoad.o
	#        BEFORE the group so its XkbDDX* win; -L$SYSROOT/lib first for fresh libc) ---
	local core_la=(dix/.libs/libmain.a dix/.libs/libdix.a hw/kdrive/src/.libs/libkdrive.a \
fb/.libs/libfb.a mi/.libs/libmi.a xfixes/.libs/libxfixes.a Xext/.libs/libXext.a \
Xext/.libs/libXvidmode.a Xext/.libs/libhashtable.a dbe/.libs/libdbe.a record/.libs/librecord.a \
randr/.libs/librandr.a render/.libs/librender.a damageext/.libs/libdamageext.a present/.libs/libpresent.a \
miext/sync/.libs/libsync.a miext/damage/.libs/libdamage.a miext/shadow/.libs/libshadow.a \
Xi/.libs/libXi.a Xi/.libs/libXistubs.a xkb/.libs/libxkb.a xkb/.libs/libxkbstubs.a \
composite/.libs/libcomposite.a config/.libs/libconfig.a os/.libs/libos.a)
	local GROUP=""; local a
	for a in "${core_la[@]}"; do GROUP="$GROUP $KD/$a"; done
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	# shellcheck disable=2086
	$TCGCC --sysroot="$SYSROOT" -o "${PREFIX_PROG}/Xphoenix" "$DDX/fbdev.o" "$DDX/ddxLoad.o" \
		-L$SYSROOT/lib -Wl,--start-group $GROUP -Wl,--end-group \
		-L$PREFIX/lib -lpixman-1 -lXfont2 -lfontenc -lfreetype -lz -lXau -lXdmcp -lxkbfile -lmd -lm \
		|| b_die "xorg-server: Xphoenix link failed"
	"${CROSS}readelf" -h "${PREFIX_PROG}/Xphoenix" | grep -q AArch64 || b_die "xorg-server: Xphoenix not aarch64"
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/Xphoenix" "${PREFIX_PROG}/Xphoenix"
	b_install "${PREFIX_PROG_TO_INSTALL}/Xphoenix" /usr/bin
	echo "xorg-server: Xphoenix linked + installed (/usr/bin/Xphoenix)"
}
