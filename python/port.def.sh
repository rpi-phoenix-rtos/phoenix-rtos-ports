#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="python"
	version="3.14.4"
	desc="CPython 3.14 interpreter (static python3 + curated static C extension modules + the dlopen'd _curses)"
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
	#
	# ncurses is needed for _curses (see p_build). It is listed here rather than
	# left to a standalone script so that `import curses` exists in any plain
	# --with-ports image; ncurses itself needs no ports.yaml entry, being pulled
	# transitively (same as glib2/fltk for the X11 ports).
	depends="zlib openssl>=1.1.1a sqlite3 libffi bzip2 xz ncurses"

	supports="phoenix>=3.3"
}

# Framework migration of the ad-hoc build script that used to live at
# tools/python-port/build.sh in the coordination repo. That directory was DELETED
# 2026-09-03 once this port was proven to be its complete replacement, so the
# paths named below and further down are historical provenance, not files you can
# open. This recipe is now the only way python3 is built.
#
# The CPython 3.14.4 cross
# recipe HW-verified on the Pi 4 (a full static `python3`). The hard-won pieces
# (config.site ac_cv_* cache, phoenix-py-compat.h -include shim, curated static
# Setup.local, and the two configure MACHDEP patches teaching CPython about
# Phoenix) live next to this file and are applied verbatim; only the external-lib
# module link lines are re-pointed from the ad-hoc private builds to the framework
# ports (zlib/sqlite3/libffi in the shared prefix, openssl via b_dependency_dir).
#
# The _curses step at the end of p_build is likewise a migration, of
# tools/python-port/build-curses.sh (its curses_shim.h is copied verbatim next to
# this file). NOT carried over from that script: the parts that restaged the
# interpreter and the whole stdlib — the port already installs both, and a second
# writer of the same files is exactly the drift this migration removes.
# _curses_panel is deliberately absent: build-curses.sh never built it either, so
# `import curses.panel` fails now as it did before. Adding it is a one-liner
# (Modules/_curses_panel.c) if a consumer ever needs it.
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
		# LDFLAGS is passed EXPLICITLY with --gc-sections stripped. configure
		# otherwise inherits the framework's LDFLAGS from the environment, and
		# --gc-sections then prunes every CPython C-API function the static
		# interpreter does not itself call -- including the ones dlopen'd
		# extension modules need. That is not a curses problem, it breaks any
		# extension using a private API:
		#
		#   ImportError: dlopen: unresolved symbol: _PyLong_UnsignedInt_Converter
		#
		# _PyLong_UnsignedInt_Converter is defined in libpython3.14.a(longobject.o)
		# and longobject.o IS linked, but with -ffunction-sections the unreferenced
		# function lands in its own section and --gc-sections drops it, so the
		# symbol is absent from the interpreter's symtab (Phoenix resolves dlopen
		# against the symtab -- python3 is installed non-stripped for exactly this
		# reason, and a fully static ELF has no .dynsym at all).
		#
		# Upstream solves the same problem with LINKFORSHARED=-Xlinker
		# -export-dynamic. Keeping the API is the right trade: the alternative is
		# a growing list of -u flags, one per symbol, discovered one crash at a
		# time on hardware.
		local py_ldflags="${LDFLAGS//-Wl,--gc-sections/}"

		(cd "${cfg}" && CONFIG_SITE="${PREFIX_PORT}/config.site" "./configure" \
			--host=aarch64-phoenix --build=x86_64-pc-linux-gnu \
			--with-build-python="${HOST_PYTHON}" \
			--prefix=/usr/local \
			--disable-ipv6 --without-ensurepip --disable-shared --disable-test-modules \
			--without-mimalloc \
			CC="${CROSS}gcc" CXX="${CROSS}g++" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			READELF="${CROSS}readelf" \
			CFLAGS="${PY_CFLAGS}" LDFLAGS="${py_ldflags}")
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
	# LDFLAGS must be overridden HERE as well as at configure time. CPython's
	# generated Makefile computes PY_LDFLAGS = $(CONFIGURE_LDFLAGS) $(LDFLAGS),
	# and $(LDFLAGS) is taken from the ENVIRONMENT at make time -- which still
	# carries the framework's --gc-sections. Fixing only configure left the flag
	# on the final link, and the interpreter was still missing the C-API
	# functions dlopen'd extensions need (see the configure comment above:
	# _PyLong_UnsignedInt_Converter, _PyLong_Size_t_Converter, _PyLong_UInt8_Converter
	# were pruned while their referenced neighbours in the SAME object survived).
	# A make command-line assignment overrides the environment.
	make -C "${PREFIX_PORT_WORKDIR}" python LDFLAGS="${LDFLAGS//-Wl,--gc-sections/}"

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

	# _curses — the ONE extension module built as a dlopen-able .so instead of
	# being folded statically into the interpreter like the step-4 modules.
	#
	# Why it is the exception to "Phoenix avoids runtime .so loading":
	#   * this is the shape HW-proven on the Pi 4 (curses.wrapper() draws on the
	#     console). Folding libncurses.a into the `python` link instead would
	#     change the proven interpreter link for everyone, for one optional module.
	#   * the port ALREADY commits to the dlopen seam — /bin/python3 is installed
	#     NON-stripped precisely so a plugin's undefined Py C-API + libc symbols
	#     resolve against its .symtab at load time. No new mechanism is introduced.
	# It lives here, in the port, rather than in a hand-run script: a script that
	# is not part of the port cannot make `import curses` true for a plain
	# --with-ports image, and one that also restages the interpreter silently wins
	# over the port simply by running later.
	#
	# Two Phoenix-specific facts make it link and run. Neither may be dropped:
	#   1. libncurses.a has to be PIC to be folded into a shared object. The
	#      ncurses port already configures with -fPIC, so nothing changes there —
	#      but a "-fPIC is pointless for a static-only library" cleanup over there
	#      would silently break this link.
	#   2. pyconfig.h falsely advertises HAVE_NCURSESW=1 although the ncurses port
	#      is NARROW. curses_shim.h undefs it; see that file for why it must be a
	#      force-include and cannot be a -U on the command line.
	#
	# Flags: PY_CFLAGS so this TU sees the same sysroot/arch/compat-shim setup as
	# every other CPython TU (curses_shim.h must come AFTER phoenix-py-compat.h,
	# hence the append). Deliberately NO ${LDFLAGS}: it carries --gc-sections,
	# which the proven link did not use. -O2 because EXPORT_CFLAGS carries no
	# optimization level. libc and the Py C-API are left UNDEFINED on purpose —
	# linking them in would embed a second copy of the interpreter's state.
	local ext curses_so
	ext="$(awk '/^EXT_SUFFIX=/ { print $2 }' "${PREFIX_PORT_WORKDIR}/Makefile")"
	[ -n "${ext}" ] || b_die "EXT_SUFFIX not found in the configured Makefile"
	curses_so="${PREFIX_PORT_WORKDIR}/_curses${ext}"

	[ -f "${PREFIX_A}/libncurses.a" ] || b_die "ncurses port did not install ${PREFIX_A}/libncurses.a"

	# shellcheck disable=2086 # PY_CFLAGS must word-split
	"${CROSS}gcc" -shared -fPIC -nostartfiles -O2 ${PY_CFLAGS} \
		-include "${PREFIX_PORT}/curses_shim.h" \
		-I "${PREFIX_PORT_WORKDIR}/Include" -I "${PREFIX_PORT_WORKDIR}/Include/internal" \
		-I "${PREFIX_PORT_WORKDIR}" -I "${PREFIX_H}/ncurses" \
		-DPy_BUILD_CORE_MODULE -DHAVE_NCURSES_H -DHAVE_TERM_H \
		"${PREFIX_PORT_WORKDIR}/Modules/_cursesmodule.c" "${PREFIX_A}/libncurses.a" \
		-o "${curses_so}"

	# -D queries the DYNAMIC symbol table — the one dlopen resolves the module
	# through. A .so whose PyInit_ is only in .symtab would import as "not found".
	"${CROSS}nm" -D "${curses_so}" | grep -q PyInit__curses || \
		b_die "PyInit__curses missing from $(basename "${curses_so}")"

	# Onto sys.path (the stdlib dir is the prefix the interpreter was configured
	# with). Installed AFTER the stdlib copy above on purpose: should that copy
	# ever gain delete semantics, an earlier install would be wiped without a
	# word. NOT stripped — the dynamic symbols are the module's whole ABI.
	b_install "${curses_so}" /usr/local/lib/python3.14
}
