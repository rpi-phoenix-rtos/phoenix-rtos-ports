#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xz"
	version="5.4.7"
	desc="XZ Utils LZMA compression — static liblzma + the xz/unxz/xzcat CLI"
	cpe23="cpe:2.3:a:tukaani:xz:${version}:*:*:*:*:*:*:*"

	source="https://github.com/tukaani-project/xz/releases/download/v${version}/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="2798247"
	sha256="8db6664c48ca07908b92baedcfe7f3ba23f49ef2476864518ab5db6723836e71"

	license="0BSD"
	license_file="COPYING"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# configure + build both live in p_build (CROSS available there, libiconv pattern).
	:
}

p_build() {
	# Two products from one tree:
	#   1. static liblzma.a + <lzma.h> — what Python's _lzma and every other xz
	#      consumer links against. This is why the port existed at first.
	#   2. the `xz` CLI, installed as xz/unxz/xzcat.
	#
	# (2) is new (2026-09-20). The recipe used to pass --disable-xz and build only
	# src/liblzma, so the image's `unxz` and `xzcat` were BUSYBOX applets, not XZ
	# Utils — which is also true of bzip2, and is the shape of the whole
	# busybox-shadows-the-real-tool problem we are unwinding. `.tar.xz` is the
	# common archive format today, so the CLI earns its ~200 KB.
	#
	# Still off: xzdec/lzmadec/lzmainfo (niche), the xzdiff/xzgrep/... shell
	# SCRIPTS (they need a /bin/sh, which is exactly what is going away with
	# busybox), nls, docs, and threads (single-threaded target).
	local xcflags="${CFLAGS} -I${PREFIX_H}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" \
			--prefix="${PREFIX_PORT_INSTALL}" --libdir="${PREFIX_A}" --includedir="${PREFIX_H}" \
			--enable-static --disable-shared \
			--enable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-lzma-links \
			--disable-scripts --disable-doc --disable-nls --disable-threads \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${xcflags}" CPPFLAGS="${xcflags}" LDFLAGS="${LDFLAGS} -static -L${PREFIX_A}")
	fi

	# The library first: the CLI links it, and other ports consume it.
	make -C "${PREFIX_PORT_WORKDIR}/src/liblzma"
	make -C "${PREFIX_PORT_WORKDIR}/src/liblzma" install

	make -C "${PREFIX_PORT_WORKDIR}/src/xz"

	# Install the CLI under all three names it dispatches on. xz selects
	# compress/decompress/cat from argv[0], so unxz and xzcat are the SAME binary
	# upstream; copies rather than links, to match how every other tool lands in
	# this rootfs (busybox's own applets are hardlinks, and the SD image preserves
	# them, but a port has no hook to express that here).
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	local xzbin="${PREFIX_PORT_WORKDIR}/src/xz/xz"
	[ -f "${xzbin}" ] || b_die "xz: CLI was not built (${xzbin} missing)"
	"${CROSS}readelf" -h "${xzbin}" | grep -q 'AArch64' || b_die "xz: CLI is not an aarch64 binary"

	local name
	for name in xz unxz xzcat; do
		cp -a "${xzbin}" "${PREFIX_PROG}/${name}"
		${STRIP} -o "${PREFIX_PROG_STRIPPED}/${name}" "${PREFIX_PROG}/${name}"
		b_install "${PREFIX_PROG_TO_INSTALL}/${name}" /usr/bin
	done
	echo "xz: installed liblzma.a + xz/unxz/xzcat"
}
