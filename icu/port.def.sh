#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="icu"
	version="78.3"
	desc="ICU4C (International Components for Unicode): static libicuuc/libicui18n/libicuio + a filtered static libicudata"
	cpe23="cpe:2.3:a:unicode:international_components_for_unicode:${version}:*:*:*:*:c\/c\+\+:*:*"

	source="https://github.com/unicode-org/icu/releases/download/release-${version}"
	archive_filename="icu4c-${version}-sources.tgz"
	src_path="icu/source/"

	# = the sha256 digest GitHub publishes for the release asset, and the md5 in
	# icu4c-78.3-sources.md5 (a7b736b570ef0e180c96a31715a00c78).
	size="27977255"
	sha256="3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0"

	# ICU itself: Unicode License v3. The LICENSE file also carries the notices of
	# the third-party parts built into the libraries: the old ICU license (code
	# from ICU <= 57), cjdict (BSD-3-Clause, with IPADIC data under NAIST-2003),
	# the Thai/Lao/Burmese/Khmer dictionaries and double-conversion (BSD-3-Clause),
	# and the tz database (public domain). All permissive. The GPL-with-exception
	# texts in it cover only autotools build scripts, which nothing ships.
	license="Unicode-3.0 AND ICU AND BSD-3-Clause AND NAIST-2003"
	license_file="../LICENSE"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

# What the port installs (into the shared ports prefix):
#   lib/libicuuc.a libicui18n.a libicuio.a libicudata.a
#   lib/pkgconfig/icu-uc.pc icu-i18n.pc icu-io.pc
#   include/unicode/*.h
#   /usr/bin/icu-smoke (rootfs): the Pi smoke test, icu-smoke.c next to this file
#
# The data is linked in (--with-data-packaging=static: libicudata.a holds the
# icudt78l common data as one symbol), so nothing is looked up on the file system
# at run time. It is built from the full data SOURCES (icu4c-78.3-data.zip;
# the sources tarball only carries a prebuilt, unfiltered .dat) through
# data-filter.json -- see that file for what is in and why.
#
# ICU cross-compiles in two steps: a native build for the BUILD machine first
# (${PREFIX_PORT_BUILD}/host: the data tools genrb/makeconv/pkgdata/icupkg/...,
# which build the data), then the target build in the source tree, configured
# --with-cross-build pointing at it. The host build comes first and out of tree
# because autoconf refuses an out-of-tree configure once the source tree holds a
# config.status; the target build is in tree so its config.status is where the
# framework looks when the libc API changes (b_port_invalidate_stale_configure).
#
# Host prerequisites: a native gcc/g++ (C++17), make, python3 (ICU's data build
# tool; it also unpacks the data archive).

ICU_DATA_ZIP="icu4c-78.3-data.zip"
ICU_DATA_ZIP_SIZE="20187983"
ICU_DATA_ZIP_SHA256="9d8b3899096aeb83e4e21ef8a40fec9e03b28db18c48452efac882ce25a91e27"

# libicudata.a must stay below this (filtered it is 11.2 MB; ICU's stock data is
# 33.1 MB, so a filter that silently stops applying fails the build).
ICU_DATA_MAX_BYTES=$((12 * 1024 * 1024))

p_common() {
	ICU_HOST_BUILD="${PREFIX_PORT_BUILD}/host"
	ICU_DESTDIR="${PREFIX_PORT_BUILD}/destdir"
	ICU_FILTER="${PREFIX_PORT}/data-filter.json"
}

p_prepare() {
	local src="${PREFIX_PORT_WORKDIR}" zip got

	b_port_apply_patches "${src}"

	# The data sources replace the tarball's data/ (whose data/in/icudt78l.dat,
	# if left, would be repackaged as is and the filter ignored). Stamped, so an
	# incremental build keeps the tree.
	if [ ! -f "${src}/data/.phoenix-data-sources" ]; then
		b_port_download "${source}/" "${ICU_DATA_ZIP}"
		zip="${PREFIX_PORT}/${ICU_DATA_ZIP}"
		got="$(wc -c <"${zip}")"
		[ "${got}" = "${ICU_DATA_ZIP_SIZE}" ] || b_die "icu: ${ICU_DATA_ZIP}: size ${got}, expected ${ICU_DATA_ZIP_SIZE}"
		echo "${ICU_DATA_ZIP_SHA256}  ${zip}" | sha256sum -c --quiet - || b_die "icu: ${ICU_DATA_ZIP}: sha256 mismatch"

		rm -rf "${src}/data"
		python3 -m zipfile -e "${zip}" "${src}"
		[ ! -e "${src}/data/in/icudt78l.dat" ] || b_die "icu: ${ICU_DATA_ZIP} carries a prebuilt .dat; the filter would not apply"
		touch "${src}/data/.phoenix-data-sources"
	fi

	# 1. The native build. Full `make`: --with-cross-build needs the host tools
	#    AND config/icucross.{mk,inc}, which the end of a host `make` writes. The
	#    filter applies here too, so the host's data build stays short. The
	#    framework's cross CC/CFLAGS/... are in the environment and must not leak.
	if [ ! -f "${ICU_HOST_BUILD}/config/icucross.mk" ]; then
		mkdir -p "${ICU_HOST_BUILD}"
		(cd "${ICU_HOST_BUILD}" && env -u CPPFLAGS -u CXXFLAGS \
			CC=gcc CXX=g++ AR=ar RANLIB=ranlib LD=ld AS=as \
			CFLAGS="-O2" CXXFLAGS="-O2" LDFLAGS="" \
			ICU_DATA_FILTER_FILE="${ICU_FILTER}" \
			"${src}/configure" --enable-static --disable-shared \
			--disable-samples --disable-tests --disable-extras --disable-layoutex)
		(cd "${ICU_HOST_BUILD}" && env -u CPPFLAGS -u CXXFLAGS -u CFLAGS -u LDFLAGS \
			CC=gcc CXX=g++ AR=ar RANLIB=ranlib LD=ld AS=as make)
		[ -f "${ICU_HOST_BUILD}/config/icucross.mk" ] || b_die "icu: host build wrote no config/icucross.mk"
	fi

	# 2. The target build.
	#    C++ uses the framework's C++ flags (CFLAGS carries -std=gnu17) plus the
	#    section flags ICU adds itself only for the hosts it knows.
	#    --prefix=/usr: the paths ICU bakes in (U_ICU_DATA_DEFAULT_DIR) are the
	#    target's; installing goes through DESTDIR (p_build).
	#    --disable-dyload: no plugins. --disable-tools: the host's are used.
	if [ ! -f "${src}/config.status" ]; then
		[ -n "${EXPORT_CXXFLAGS:-}" ] || b_die "icu: EXPORT_CXXFLAGS is not set"
		(cd "${src}" && ICU_DATA_FILTER_FILE="${ICU_FILTER}" ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu \
			--with-cross-build="${ICU_HOST_BUILD}" \
			--prefix=/usr \
			--enable-static --disable-shared \
			--with-data-packaging=static \
			--disable-dyload --disable-tools --disable-samples --disable-tests \
			--disable-extras --disable-layoutex \
			CC="${CROSS}gcc" CXX="${CROSS}g++" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${CFLAGS}" \
			CXXFLAGS="${EXPORT_CXXFLAGS} -ffunction-sections -fdata-sections" \
			LDFLAGS="${LDFLAGS}")
		grep -q "icu_cv_host_frag=mh-linux" "${src}/config.log" ||
			b_die "icu: configure did not take mh-linux for ${HOST} (patches/01-phoenix-host.patch)"
	fi
}

p_build() {
	local src="${PREFIX_PORT_WORKDIR}" lib n data_bytes

	make -C "${src}"

	# Install through DESTDIR (prefix=/usr) and take only the libraries, headers
	# and pkg-config files: the rest (icu-config, share/icu/78.3/ build makefiles,
	# lib/icu/78.3/pkgdata.inc) is for building ICU-based data packages on the
	# target, and would put bin/ and share/ into the shared ports prefix.
	rm -rf "${ICU_DESTDIR}"
	make -C "${src}" install DESTDIR="${ICU_DESTDIR}"

	mkdir -p "${PREFIX_A}/pkgconfig" "${PREFIX_H}"
	for lib in icuuc icui18n icuio icudata; do
		[ -f "${ICU_DESTDIR}/usr/lib/lib${lib}.a" ] || b_die "icu: lib${lib}.a was not built"
		cp -a "${ICU_DESTDIR}/usr/lib/lib${lib}.a" "${PREFIX_A}/"
	done
	rm -rf "${PREFIX_H}/unicode"
	cp -a "${ICU_DESTDIR}/usr/include/unicode" "${PREFIX_H}/"
	# Static C consumers also need the C++ runtime ICU is written against.
	for n in icu-uc icu-i18n icu-io; do
		sed -e "s|^prefix = .*|prefix = ${PREFIX_PORT_INSTALL%/}|" \
			"${ICU_DESTDIR}/usr/lib/pkgconfig/${n}.pc" >"${PREFIX_A}/pkgconfig/${n}.pc"
	done
	echo "Libs.private: -lstdc++" >>"${PREFIX_A}/pkgconfig/icu-uc.pc"

	# Phoenix's own definitions (patches/03-phoenix-platform-h.patch) are in the
	# installed headers, so every consumer sees what ICU was built with.
	grep -q "defined(__phoenix__)" "${PREFIX_H}/unicode/platform.h" ||
		b_die "icu: the installed unicode/platform.h lacks the Phoenix definitions"

	# The data is really linked in, and really filtered.
	"${CROSS}nm" "${PREFIX_A}/libicudata.a" | grep -q " R icudt78_dat$" ||
		b_die "icu: libicudata.a does not define icudt78_dat (not static data?)"
	data_bytes="$(stat -c %s "${PREFIX_A}/libicudata.a")"
	[ "${data_bytes}" -le "${ICU_DATA_MAX_BYTES}" ] ||
		b_die "icu: libicudata.a is ${data_bytes} bytes, over ${ICU_DATA_MAX_BYTES} (is data-filter.json applied?)"
	echo "icu: libicudata.a ${data_bytes} bytes, $(wc -l <"${src}/data/out/tmp/icudata.lst") data items (data/out/tmp/icudata.lst)"

	# The smoke test: ICUCHK lines for collation, break iteration, normalization,
	# conversion, IDNA, time zones, formatting and the data filter itself.
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	# shellcheck disable=2086 # CFLAGS/LDFLAGS must word-split
	"${CROSS}gcc" ${CFLAGS} -I"${PREFIX_H}" "${PREFIX_PORT}/icu-smoke.c" \
		-o "${PREFIX_PROG}/icu-smoke" ${LDFLAGS} -L"${PREFIX_A}" \
		-licui18n -licuuc -licudata -lm -lstdc++ -lpthread
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/icu-smoke" "${PREFIX_PROG}/icu-smoke"
	b_install "${PREFIX_PROG_TO_INSTALL}/icu-smoke" /usr/bin
}
