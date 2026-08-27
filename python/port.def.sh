#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="python"
	version="3.14.4"
	desc="CPython 3.14 interpreter (static python3 + curated static C extension modules)"
	cpe23="cpe:2.3:a:python:python:${version}:*:*:*:*:*:*:*"

	source="https://www.python.org/ftp/python/${version}"
	archive_filename="Python-${version}.tar.xz"
	src_path="Python-${version}/"

	size="23855332"
	sha256="d923c51303e38e249136fc1bdf3568d56ecb03214efdef48516176d3d7faaef8"

	# CPython is distributed under the PSF License Agreement (permissive,
	# GPL-compatible) -> safe to ship on Phoenix.
	license="PSF-2.0"
	license_file="LICENSE"

	conflicts=""
	# zlib/sqlite3/libffi are non-conflicting ports -> install into the shared
	# PREFIX_A/PREFIX_H, so python (also non-conflicting, same shared prefix) links
	# them via -I${PREFIX_H} -L${PREFIX_A}. openssl (1.1.1a) is conflictable ->
	# installs to a versioned dir, resolved at build time via b_dependency_dir.
	depends="zlib openssl>=1.1.1a sqlite3 libffi bzip2 xz"

	supports="phoenix>=3.3"
}

# Framework migration of tools/python-port/build.sh — the CPython 3.14.4 cross
# recipe HW-verified on the Pi 4 (a full static `python3`). The hard-won pieces
# (config.site ac_cv_* cache, phoenix-py-compat.h -include shim, curated static
# Setup.local, and the two configure MACHDEP patches teaching CPython about
# Phoenix) live next to this file and are applied verbatim; only the external-lib
# module link lines are re-pointed from the ad-hoc private builds to the framework
# ports (zlib/sqlite3/libffi in the shared prefix, openssl via b_dependency_dir).
#
# CPython cross-build REQUIRES a matching-version (3.14.x) host python for the
# freeze/sysconfig build steps; --with-build-python=/usr/bin/python3 uses the
# host interpreter (this dev host ships 3.14.4). This is a documented host
# prerequisite, identical to the ad-hoc build.

p_common() {
	# Host python of the SAME minor (3.14) drives the cross build steps.
	HOST_PYTHON="/usr/bin/python3"

	# openssl is conflictable -> versioned install dir (has lib/ + include/).
	OSSL="$(b_dependency_dir openssl)"

	# Every TU is compiled with the Phoenix bring-up shim -include'd first (early
	# sys/time.h+resource.h+mman.h for complete struct timeval/rusage, missing
	# _SC_* names, clock_getres/msync no-ops, O_NOFOLLOW=0, SOMAXCONN=128).
	PY_CFLAGS="${CFLAGS} -include ${PREFIX_PORT}/phoenix-py-compat.h"
}

p_prepare() {
	local cfg="${PREFIX_PORT_WORKDIR}"

	# 1. Teach configure about Phoenix: two cross-build MACHDEP blocks otherwise
	#    hard-error "cross build not supported for aarch64-unknown-phoenix".
	#    Idempotent (clean re-extract ships a pristine configure; guard covers
	#    --incremental).
	if ! grep -q 'ac_sys_system=Phoenix' "${cfg}/configure"; then
		perl -0pi -e 's/(\t\*-\*-wasi\*\)\n\t    ac_sys_system=WASI\n\t    ;;\n)/$1\t*-*-phoenix*)\n\t    ac_sys_system=Phoenix\n\t    ;;\n/' "${cfg}/configure"
		perl -0pi -e 's/(\twasm32-\*-\* \| wasm64-\*-\*\)\n\t\t_host_ident=\$host_cpu\n\t\t;;\n)/$1\t*-*-phoenix*)\n\t\t_host_ident=\$host_cpu\n\t\t;;\n/' "${cfg}/configure"
	fi

	# 2. Configure. CONFIG_SITE presets the cross ac_cv_* cache (funcs Phoenix has
	#    but the cross-check can't run to detect) + py_cv_module_*=n/a to suppress
	#    configure's own rules for the external-lib modules we link statically in
	#    step 4 (else they collide with the Setup.local lines). --without-mimalloc:
	#    mimalloc needs madvise/rusage Phoenix lacks -> pymalloc. --disable-shared:
	#    static interpreter.
	if [ ! -f "${cfg}/config.status" ]; then
		(cd "${cfg}" && CONFIG_SITE="${PREFIX_PORT}/config.site" "./configure" \
			--host=aarch64-phoenix --build=x86_64-pc-linux-gnu \
			--with-build-python="${HOST_PYTHON}" \
			--prefix=/usr/local \
			--disable-ipv6 --without-ensurepip --disable-shared --disable-test-modules \
			--without-mimalloc \
			CC="${CROSS}gcc" CXX="${CROSS}g++" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			READELF="${CROSS}readelf" \
			CFLAGS="${PY_CFLAGS}")
	fi

	# 3. The .so extension linker defaults to the host `ld` (wrong arch). Point it
	#    at the cross gcc (only matters if shared modules are built later; the
	#    static interpreter below does not need it, but keep it faithful/hermetic).
	sed -i "s/^LDSHARED=\tld /LDSHARED=\t${CROSS}gcc -shared /; s/^BLDSHARED=\tld /BLDSHARED=\t${CROSS}gcc -shared /" "${cfg}/Makefile"

	# 4. Build the curated pure-C stdlib extension modules STATICALLY into the
	#    interpreter (Phoenix avoids runtime .so loading). makesetup gives
	#    Setup.local priority over the Setup.stdlib *shared* defs, so these link
	#    into `python` instead of as .so.
	cp "${PREFIX_PORT}/Setup.local" "${cfg}/Modules/Setup.local"

	# 4a. zlib module (gzip / zipfile / zipimport) against the framework zlib port.
	grep -q '^zlib ' "${cfg}/Modules/Setup.local" || \
		echo "zlib zlibmodule.c -I${PREFIX_H} -L${PREFIX_A} -lz" >> "${cfg}/Modules/Setup.local"

	# 4b. _sqlite3 (Python + a real SQL DB) against the framework sqlite3 port
	#     (same 3.53.4 amalgamation the ad-hoc build compiled privately). The
	#     _sqlite/*.c sources ship in the CPython tree; MODULE_NAME is defined in
	#     Modules/_sqlite/module.h so no -D needed.
	grep -q '^_sqlite3 ' "${cfg}/Modules/Setup.local" || \
		echo "_sqlite3 _sqlite/blob.c _sqlite/connection.c _sqlite/cursor.c _sqlite/microprotocols.c _sqlite/module.c _sqlite/prepare_protocol.c _sqlite/row.c _sqlite/statement.c _sqlite/util.c -I${PREFIX_H} -L${PREFIX_A} -lsqlite3" >> "${cfg}/Modules/Setup.local"

	# 4c. _decimal (arbitrary-precision Decimal). CPython 3.14 bundles libmpdec
	#     under Modules/_decimal/libmpdec (self-contained, no external download).
	#     NB: makesetup treats ANY Setup line containing '=' as a Makefile var, so
	#     the -D flags are bare (-DCONFIG_64, gcc defines it to 1); mpdecimal.h
	#     tests them with #if defined(...) so the value is irrelevant. config.site
	#     sets py_cv_module__decimal=n/a so configure emits no colliding rules.
	if ! grep -q '^_decimal ' "${cfg}/Modules/Setup.local"; then
		local mpdec="basearith constants context convolute crt difradix2 fnt fourstep io mpalloc mpdecimal mpsignal numbertheory sixstep transpose"
		local decsrcs="_decimal/_decimal.c" m
		for m in ${mpdec}; do decsrcs="${decsrcs} _decimal/libmpdec/${m}.c"; done
		echo "_decimal ${decsrcs} -IModules/_decimal/libmpdec -DCONFIG_64 -DANSI -DHAVE_UINT128_T" >> "${cfg}/Modules/Setup.local"
	fi

	# 4d. _ctypes (Python FFI: ctypes / CDLL) against the framework libffi port.
	#     _ctypes' callproc.c defines its own set_errno/get_errno, which clash with
	#     Phoenix <errno.h>'s `static inline int set_errno(int)`. Rename ONLY the C
	#     functions (definitions + method-table refs) to ctypes_{set,get}_errno; the
	#     quoted "set_errno"/"get_errno" method names Python sees stay intact. (A
	#     global -Dset_errno= would rename errno.h's inline too, and a Setup.local -D
	#     cannot contain '='.) Idempotent guard on the renamed symbol.
	if ! grep -q 'ctypes_set_errno' "${cfg}/Modules/_ctypes/callproc.c"; then
		perl -0pi -e 's/\nset_errno\(PyObject \*self/\nctypes_set_errno(PyObject *self/;
		              s/\nget_errno\(PyObject \*self/\nctypes_get_errno(PyObject *self/;
		              s/\{"set_errno", set_errno,/{"set_errno", ctypes_set_errno,/;
		              s/\{"get_errno", get_errno,/{"get_errno", ctypes_get_errno,/' \
		              "${cfg}/Modules/_ctypes/callproc.c"
	fi
	# -DUSING_MALLOC_CLOSURE_DOT_C: else ctypes.h #defines Py_ffi_closure_free ->
	# ffi_closure_free, so malloc_closure.c's Py_ffi_closure_free redefines libffi's
	# symbol (multiple-def link error). The HAVE_FFI_* macros are normally set by
	# configure's libffi probe; libffi 3.4 has all of them, so supply them directly.
	grep -q '^_ctypes ' "${cfg}/Modules/Setup.local" || \
		echo "_ctypes _ctypes/_ctypes.c _ctypes/callbacks.c _ctypes/callproc.c _ctypes/cfield.c _ctypes/malloc_closure.c _ctypes/stgdict.c -I${PREFIX_H} -L${PREFIX_A} -lffi -DHAVE_FFI_PREP_CIF_VAR -DHAVE_FFI_PREP_CLOSURE_LOC -DHAVE_FFI_CLOSURE_ALLOC -DHAVE_ALLOCA_H -DUSING_MALLOC_CLOSURE_DOT_C" >> "${cfg}/Modules/Setup.local"

	# 4e. _ssl + _hashlib (TLS/HTTPS + OpenSSL-backed hashlib) against the framework
	#     openssl (1.1.1a, conflictable -> versioned dir via b_dependency_dir).
	grep -q '^_ssl ' "${cfg}/Modules/Setup.local" || \
		echo "_ssl _ssl.c -I${OSSL}/include -L${OSSL}/lib -lssl -lcrypto" >> "${cfg}/Modules/Setup.local"
	grep -q '^_hashlib ' "${cfg}/Modules/Setup.local" || \
		echo "_hashlib _hashopenssl.c -I${OSSL}/include -L${OSSL}/lib -lcrypto" >> "${cfg}/Modules/Setup.local"

	# 4f. _blake2 (hashlib.blake2b/blake2s). CPython 3.14 bundles the portable HACL*
	#     Blake2 impl -> self-contained (no external lib). The x86 SIMD variants
	#     compile out on aarch64 (_Py_HACL_CAN_COMPILE_VEC* == 0), so link only the
	#     portable HACL sources + Lib_Memzero0.c (a HACL dep). config.site sets
	#     py_cv_module__blake2=n/a so configure emits no colliding rules.
	grep -q '^_blake2 ' "${cfg}/Modules/Setup.local" || \
		echo "_blake2 blake2module.c _hacl/Hacl_Hash_Blake2s.c _hacl/Hacl_Hash_Blake2b.c _hacl/Lib_Memzero0.c -IModules/_hacl -IModules/_hacl/include -D_BSD_SOURCE -D_DEFAULT_SOURCE" >> "${cfg}/Modules/Setup.local"

	# 4g. _bz2 (bzip2 compression) against the framework bzip2 port (libbz2.a + bzlib.h
	#     in the shared sysroot). config.site sets py_cv_module__bz2=n/a.
	grep -q '^_bz2 ' "${cfg}/Modules/Setup.local" || \
		echo "_bz2 _bz2module.c -I${PREFIX_H} -L${PREFIX_A} -lbz2" >> "${cfg}/Modules/Setup.local"

	# 4h. _lzma (xz/LZMA compression) against the framework xz port (liblzma.a + lzma.h
	#     in the shared sysroot). config.site sets py_cv_module__lzma=n/a.
	grep -q '^_lzma ' "${cfg}/Modules/Setup.local" || \
		echo "_lzma _lzmamodule.c -I${PREFIX_H} -L${PREFIX_A} -llzma" >> "${cfg}/Modules/Setup.local"
}

p_build() {
	# Build JUST the interpreter (not `all`): `make all` would try to link the
	# remaining stdlib extensions as .so with the wrong linker; `make python`
	# links the static interpreter + the Setup.local modules.
	make -C "${PREFIX_PORT_WORKDIR}" python

	"${CROSS}readelf" -h "${PREFIX_PORT_WORKDIR}/python" | grep Machine

	# Install the interpreter as /bin/python3. Keep the NON-stripped binary: the
	# dlopen C-extension recipe (README) resolves the Py C-API + libc against the
	# python binary's .symtab, so PREFIX_PROG_TO_INSTALL (stripped by default) must
	# NOT be used here. A stripped copy is still staged for convention.
	cp "${PREFIX_PORT_WORKDIR}/python" "${PREFIX_PROG}/python3"
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/python3" "${PREFIX_PROG}/python3"
	b_install "${PREFIX_PROG}/python3" /bin

	# Install the pure-Python stdlib at the compiled prefix (--prefix=/usr/local),
	# so the interpreter finds `encodings` etc. at startup. Whole tree, matching
	# the ad-hoc `cp -r Lib/*`. NB: never `make install` — --prefix=/usr/local
	# would target the build host.
	mkdir -p "${PREFIX_FS}/root/usr/local/lib/python3.14"
	cp -a "${PREFIX_PORT_WORKDIR}/Lib/." "${PREFIX_FS}/root/usr/local/lib/python3.14/"
}
