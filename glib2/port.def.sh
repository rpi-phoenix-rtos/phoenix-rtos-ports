#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="glib2"
	version="2.56.4"
	desc="GLib 2.56 — GLib/GObject/GModule/GThread core (static; last autotools glib)"
	cpe23="cpe:2.3:a:gnome:glib:${version}:*:*:*:*:*:*:*"

	# 2.56 is the LAST autotools (./configure) glib series — glib went meson-only at
	# 2.60. 2.56 also still bundles PCRE1 (--with-pcre=internal), so no system pcre.
	source="https://download.gnome.org/sources/glib/2.56"
	archive_filename="glib-${version}.tar.xz"
	src_path="glib-${version}/"

	size="7029768"
	sha256="27f703d125efb07f8a743666b580df0b4095c59fc8750e8890132c91d437504c"

	license="LGPL-2.1-or-later"
	license_file="COPYING"

	conflicts=""
	# libiconv (real GNU libiconv 1.18) supplies iconv.h + libiconv.a; libffi 3.4.6
	# supplies ffi.h + libffi.a (gobject gclosure marshalling HARD-requires it in
	# 2.56); zlib supplies zlib.h + libz.a. All land in PREFIX_H/PREFIX_A. PCRE is
	# bundled (--with-pcre=internal); gettext/resolver/nameser are stubbed below.
	depends="libiconv libffi zlib"

	supports="phoenix>=3.3"
}

p_prepare() {
	# No source patches; but glib 2.56 ships an OLD (2016) config.sub/guess that does
	# not know the `phoenix` triplet, so ./configure would reject `aarch64-phoenix`.
	# Copy a phoenix-aware pair from a dep already extracted under port-sources
	# (libffi/zlib/libiconv are all built before glib2). config.sub is the
	# load-bearing one (we pass --build, so config.guess is never invoked), but we
	# refresh both to match the ad-hoc recipe.
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	local psrc donor
	psrc="$(dirname "${PREFIX_PORT_BUILD}")"
	donor="$(grep -l phoenix "${psrc}"/*/*/config.sub 2>/dev/null | grep -v glib | head -1)"
	if [ -n "${donor}" ]; then
		cp "${donor}" "${PREFIX_PORT_WORKDIR}/config.sub"
		[ -f "$(dirname "${donor}")/config.guess" ] && \
			cp "$(dirname "${donor}")/config.guess" "${PREFIX_PORT_WORKDIR}/config.guess"
	fi

	# Stage the three compatibility stubs into the shared PREFIX so configure's
	# header/link probes resolve them (the framework CFLAGS carry -I${PREFIX_H}
	# once we add it in p_build; these headers must exist there beforehand):
	#   <libintl.h>       — identity gettext macros (glib HARD-requires a gettext
	#                        provider even with --disable-nls)
	#   <arpa/nameser.h>  — DNS class/type constants (configure needs C_IN defined)
	#   <resolv.h> + libresolv.a — res_query() link probe (gio gresolver only; not
	#                        built for the mc-critical libglib-2.0). Stub fails
	#                        cleanly at runtime.
	cp "${PREFIX_PORT}/libintl-stub/libintl.h" "${PREFIX_H}/libintl.h"
	mkdir -p "${PREFIX_H}/arpa"
	cp "${PREFIX_PORT}/nameser-stub/arpa/nameser.h" "${PREFIX_H}/arpa/nameser.h"
	cp "${PREFIX_PORT}/resolv-stub/resolv.h" "${PREFIX_H}/resolv.h"

	if [ ! -f "${PREFIX_A}/libresolv.a" ]; then
		"${CROSS}gcc" ${CFLAGS} -I"${PREFIX_PORT}/resolv-stub" \
			-c "${PREFIX_PORT}/resolv-stub/resolv-stub.c" -o "${PREFIX_PORT_WORKDIR}/resolv-stub.o"
		"${CROSS}ar" rcs "${PREFIX_A}/libresolv.a" "${PREFIX_PORT_WORKDIR}/resolv-stub.o"
	fi

	# Fresh copy of the pre-seeded autoconf cache (AC_TRY_RUN cross probes) into the
	# workdir; configure loads it via --cache-file=glib2.cache (relative).
	cp "${PREFIX_PORT}/glib2.cache" "${PREFIX_PORT_WORKDIR}/glib2.cache"
}

p_build() {
	# Framework paths: deps' headers/libs live in PREFIX_H/PREFIX_A and the
	# framework CFLAGS do NOT auto-add -I${PREFIX_H}, so add it (+ the force-included
	# shim: P_tmpdir, LC_MESSAGES, NLS identity fallbacks). CPPFLAGS mirrors it for
	# configure's preprocessor probes; LDFLAGS gains -L${PREFIX_A}.
	local xcflags="${CFLAGS} -I${PREFIX_H} -O2 -include ${PREFIX_PORT}/glib-phoenix-shim.h"
	local xcppflags="${CFLAGS} -I${PREFIX_H}"
	local xldflags="${LDFLAGS} -L${PREFIX_A}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu --prefix="${PREFIX_PORT_INSTALL}" \
			--cache-file=glib2.cache \
			--enable-static --disable-shared --disable-nls --disable-libmount \
			--disable-selinux --disable-dtrace --disable-systemtap --disable-coverage \
			--disable-installed-tests --with-pcre=internal --with-libiconv=maybe \
			--with-threads=posix \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${xcflags}" CPPFLAGS="${xcppflags}" \
			LDFLAGS="${xldflags}" LIBS="-liconv" \
			PKG_CONFIG=/bin/false \
			ZLIB_CFLAGS="-I${PREFIX_H}" ZLIB_LIBS="-L${PREFIX_A} -lz" \
			LIBFFI_CFLAGS="-I${PREFIX_H}" LIBFFI_LIBS="-L${PREFIX_A} -lffi")
	fi

	# The mc-critical deliverable: the glib/ core static library. MUST succeed.
	make -C "${PREFIX_PORT_WORKDIR}/glib"

	local gliba="${PREFIX_PORT_WORKDIR}/glib/.libs/libglib-2.0.a"
	[ -f "${gliba}" ] || b_die "libglib-2.0.a not produced"

	# The rest, best-effort (with real libffi these should build; don't fail the
	# port if a helper-program link in one of them trips).
	make -C "${PREFIX_PORT_WORKDIR}/gthread" || echo "[glib2] warn: gthread build issues"
	make -C "${PREFIX_PORT_WORKDIR}/gmodule" || echo "[glib2] warn: gmodule build issues"
	make -C "${PREFIX_PORT_WORKDIR}/gobject" || echo "[glib2] warn: gobject build issues"

	# Stage libraries into PREFIX_A.
	local la
	for la in glib/.libs/libglib-2.0.a gthread/.libs/libgthread-2.0.a \
	          gmodule/.libs/libgmodule-2.0.a gobject/.libs/libgobject-2.0.a; do
		[ -f "${PREFIX_PORT_WORKDIR}/${la}" ] && cp -a "${PREFIX_PORT_WORKDIR}/${la}" "${PREFIX_A}/"
	done

	# Headers: glib's `make install` runs an install-data-local hook that fails in
	# this cross env and aborts header recursion, so stage explicitly + deterministic
	# in glib's canonical layout:
	#   ${PREFIX_H}/glib-2.0/{glib.h,glib-unix.h,glib-object.h,gmodule.h,gi18n*.h}
	#   ${PREFIX_H}/glib-2.0/glib/*.h  (+ glib/deprecated/*.h)
	#   ${PREFIX_H}/glib-2.0/gobject/*.h
	#   ${PREFIX_A}/glib-2.0/include/glibconfig.h   (the generated config header)
	local ginc="${PREFIX_H}/glib-2.0"
	rm -rf "${ginc}"
	mkdir -p "${ginc}/glib/deprecated" "${ginc}/gobject" "${PREFIX_A}/glib-2.0/include"

	local w="${PREFIX_PORT_WORKDIR}"
	# Top-level public headers (glibinclude_HEADERS) live directly under glib-2.0/.
	cp -a "${w}"/glib/glib.h "${w}"/glib/glib-unix.h "${w}"/glib/glib-object.h \
		"${w}"/glib/gi18n.h "${w}"/glib/gi18n-lib.h "${ginc}/" 2>/dev/null || true
	cp -a "${w}"/gmodule/gmodule.h "${ginc}/" 2>/dev/null || true
	local h
	for h in "${w}"/glib/*.h; do cp -a "${h}" "${ginc}/glib/"; done
	cp -a "${w}"/glib/deprecated/*.h "${ginc}/glib/deprecated/" 2>/dev/null || true
	cp -a "${w}"/gobject/*.h "${ginc}/gobject/" 2>/dev/null || true
	# glibconfig.h is generated at $workdir/glib/glibconfig.h (config_commands).
	[ -f "${w}/glib/glibconfig.h" ] && \
		cp -a "${w}/glib/glibconfig.h" "${PREFIX_A}/glib-2.0/include/glibconfig.h"
}
