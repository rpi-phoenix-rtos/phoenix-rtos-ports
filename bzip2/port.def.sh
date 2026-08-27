#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="bzip2"
	version="1.0.8"
	desc="bzip2 block-sorting compression library (static libbz2)"
	cpe23="cpe:2.3:a:bzip:bzip2:${version}:*:*:*:*:*:*:*"

	source="https://sourceware.org/pub/bzip2/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="810029"
	sha256="ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"

	license="bzip2-1.0.6"
	license_file="LICENSE"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# bzip2 ships a plain Makefile (no autoconf); cross-build just the library from
	# its 7 sources (the bzip2/bzip2recover CLIs + dlltest are not part of libbz2)
	# into a static libbz2.a, then install libbz2.a + <bzlib.h> into the shared
	# sysroot so dependents (e.g. Python's _bz2) link -lbz2. -D_FILE_OFFSET_BITS=64
	# matches bzip2's own recommended build (64-bit off_t for the file helpers).
	cd "${PREFIX_PORT_WORKDIR}"
	local objs=""
	local c
	for c in blocksort huffman crctable randtable compress decompress bzlib; do
		"${CROSS}gcc" ${CFLAGS} -O2 -D_FILE_OFFSET_BITS=64 -c "${c}.c" -o "${c}.o"
		objs="${objs} ${c}.o"
	done
	# shellcheck disable=2086
	"${CROSS}ar" rcs libbz2.a ${objs}
	"${CROSS}ranlib" libbz2.a

	mkdir -p "${PREFIX_A}" "${PREFIX_H}"
	cp -a libbz2.a "${PREFIX_A}/libbz2.a"
	cp -a bzlib.h "${PREFIX_H}/bzlib.h"
}
