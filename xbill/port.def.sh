#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xbill"
	version="2.1"
	desc="xbill — the classic 'stop the Bill virus' X11 game (Athena backend)"

	# xbill is the one X demo that is NOT a clean autotools app: its stock
	# configure.in is GTK-tilted and fragile under cross-compile, so (like the
	# coordination-repo build-xbill.sh it is migrated from) this recipe BYPASSES
	# ./configure — it writes a minimal config.h and compiles the Athena object
	# set directly with the cross gcc. Because of that bespoke, higher-breakage
	# build it is a standalone port rather than a member of the xorg_apps
	# aggregate, so a break here cannot block the four clean autoconf apps.
	#
	# Source: the alistairmcmillan/Xbill GitHub mirror of xbill 2.1 (has the
	# x11-athena.c backend; the SourceForge 2.1.4 release is GTK-only). Pinned to
	# a specific commit archive and the anchor tarball is committed alongside this
	# recipe, so the build is reproducible and needs no network.
	source="https://github.com/alistairmcmillan/Xbill/archive"
	archive_filename="75f47443380b88d4d4ca9b9a2ad837bfcec39d15.tar.gz"
	src_path="Xbill-75f47443380b88d4d4ca9b9a2ad837bfcec39d15/"
	size="60307"
	sha256="0609e9f33fec84ba9216124ff80532853cdd6f2956f9981e6cbc42830c81562b"

	license="GPL-2.0-or-later"   # xbill.spec: "Copyright: GPL"
	license_file="xbill.spec"

	conflicts=""
	# Athena backend: libXaw/Xmu/Xt/Xpm/Xext over libX11, all staged by xorg_libs
	# into $PREFIX_BUILD/{lib,include}.
	depends="xorg_libs"

	supports="phoenix>=3.3"
}

p_prepare() {
	:
}

p_build() {
	local XHOST="aarch64-phoenix"
	local PREFIX="${PREFIX_BUILD%/}"
	local SYSROOT="${PREFIX_BUILD%/}/sysroot"
	local TCGCC="${CROSS}gcc" TCNM="${CROSS}nm"
	local src="${PREFIX_PORT_WORKDIR%/}"

	local BINDST="${PREFIX_FS}/root/bin"
	mkdir -p "$BINDST" "${PREFIX_PROG}"

	# Runtime asset + score locations on the target rootfs. xbill loads its
	# sprites at runtime from <IMAGES>/{bitmaps,pixmaps} (x11.c) and reads/writes
	# the high-score table at SCOREFILE (Scorelist.c); both are baked in below.
	local IMAGES_DIR=/usr/share/xbill
	local SCOREFILE=/usr/share/xbill/scores

	local shim="${PREFIX_PORT}/files/xbill-phoenix-shim.h"
	local APP_CFLAGS="-DMAXHOSTNAMELEN=256 -DO_NOFOLLOW=0 -DXOS_USE_MTSAFE_PWDAPI -D_POSIX_THREAD_SAFE_FUNCTIONS=200809L -include $shim"
	# Athena toolkit closure (Xaw<->Xmu<->Xt<->X11 circular -> start/end-group).
	local XCLOSURE="-Wl,--start-group -lXaw7 -lXmu -lXt -lSM -lICE -lXpm -lXext -lX11 -liconv -lxcb -lXau -lXdmcp -lphoenix -lc -lm -Wl,--end-group"

	# Athena UI object set (from Makefile.in's WIDGET selection): core game logic
	# + the shared core-X driver (x11.c) + the Athena widget driver
	# (x11-athena.c). gtk.c / x11-motif.c are intentionally excluded.
	local SRCS="Bill.c Bucket.c Cable.c Computer.c Game.c Horde.c Network.c OS.c Scorelist.c Spark.c UI.c util.c x11.c x11-athena.c"

	[ -f "$src/x11-athena.c" ] || b_die "xbill: x11-athena.c missing (wrong source — GTK-only release?)"

	# --- minimal Phoenix config.h (bypasses the GTK-tilted ./configure) ---
	cat > "$src/config.h" <<EOF
/* Minimal Phoenix config.h — Athena backend, written by the xbill port. */
#define STDC_HEADERS 1
#define HAVE_UNISTD_H 1
#define USE_ATHENA 1
/* USE_MOTIF / USE_GTK intentionally undefined: not in the Phoenix X11 stack. */
EOF

	# Runtime paths as a force-included header (not -D) so the embedded C string
	# literals are not mangled by shell quote-stripping. Force-included only into
	# the two TUs that reference them (x11.c: IMAGES, Scorelist.c: SCOREFILE).
	local paths_hdr="$src/phoenix_paths.h"
	cat > "$paths_hdr" <<EOF
/* Phoenix runtime asset/score paths — force-included by the xbill port. */
#ifndef XBILL_PHOENIX_PATHS_H
#define XBILL_PHOENIX_PATHS_H
#define IMAGES    "$IMAGES_DIR"
#define SCOREFILE "$SCOREFILE"
#endif
EOF

	# --- compile each Athena object, then link static (direct cross gcc) ---
	echo "xbill: building (Athena backend, direct compile)"
	local s o paths objs=""
	for s in $SRCS; do
		o="$src/${s%.c}.o"
		paths=""
		case "$s" in
		Scorelist.c|x11.c) paths="-include $paths_hdr" ;;
		esac
		"$TCGCC" --sysroot="$SYSROOT" $APP_CFLAGS $paths -I"$src" -I"$PREFIX/include" \
			-c "$src/$s" -o "$o" || b_die "xbill: compile of $s failed"
		objs="$objs $o"
	done

	# shellcheck disable=2086
	"$TCGCC" --sysroot="$SYSROOT" -static -o "$src/xbill" $objs \
		-L"$PREFIX/lib" -L"$SYSROOT/lib" $XCLOSURE || b_die "xbill: link failed"
	[ -x "$src/xbill" ] || b_die "xbill: binary not produced"

	# --- install binary + runtime assets + a seed score file ---
	install -m 755 "$src/xbill" "$BINDST/xbill"
	install -m 755 "$src/xbill" "${PREFIX_PROG}/xbill"
	local dest="${PREFIX_FS}/root${IMAGES_DIR}"
	mkdir -p "$dest/bitmaps" "$dest/pixmaps"
	cp "$src"/bitmaps/*.xbm "$dest/bitmaps/" 2>/dev/null || true
	cp "$src"/pixmaps/*.xpm "$dest/pixmaps/" 2>/dev/null || true
	# Seed an empty high-score file so the first read succeeds.
	if [ -f "$src/scores" ]; then cp "$src/scores" "$dest/scores"; else : > "$dest/scores"; fi
	echo "xbill: staged /bin/xbill + assets under ${IMAGES_DIR}"

	# --- pre-flight: aarch64 static ELF, 0 undefined symbols, IMAGES baked ---
	local f und
	f="$(file "$src/xbill")"
	case "$f" in
	*"ARM aarch64"*"statically linked"*) ;;
	*) b_die "xbill: not an aarch64 static ELF: $f" ;;
	esac
	und="$("$TCNM" -u "$src/xbill" 2>/dev/null)"
	[ -z "$und" ] || { echo "$und"; b_die "xbill: undefined symbols present"; }
	strings "$src/xbill" | grep -qx "$IMAGES_DIR" || b_die "xbill: IMAGES path not baked in ($IMAGES_DIR)"
	echo "xbill: [OK] aarch64 static ELF, 0 undefined symbols, IMAGES baked ($IMAGES_DIR)"
}
