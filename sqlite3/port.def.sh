: # SPDX-License-Identifier: BSD-3-Clause
{
	ports_api=1
	name="sqlite3"
	version="3.53.4"
	desc="Self-contained, serverless, zero-configuration SQL database engine"
	cpe23="cpe:2.3:a:sqlite:sqlite:${version}:*:*:*:*:*:*:*"
	source="https://www.sqlite.org/2026/"
	archive_filename="sqlite-autoconf-3530400.tar.gz"
	src_path="sqlite-autoconf-3530400/"
	size="3283177"
	sha256="0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c"
	license="blessing"
	license_file="sqlite3.h"
	conflicts=""
	depends=""
	supports="phoenix>=3.3"
}

# The autoconf tarball ships the pre-amalgamated sqlite3.c/sqlite3.h + the shell
# (shell.c). We compile it directly with the cross toolchain and skip the bundled
# ./configure entirely: no autoconf on-target, and Phoenix needs none of the
# feature probes (the flags below are the curated, HW-verified set).
#
# Without configure nothing defines the HAVE_* macros, and the amalgamation only
# guesses them for __linux__/__APPLE__. So they are named here, for functions
# libphoenix really has: HAVE_PREAD/HAVE_PWRITE make the unix VFS read and write
# a page with ONE positional syscall (libphoenix pread/pwrite pass the offset to
# sys_read/sys_write) instead of lseek()+read(); HAVE_LOCALTIME_R makes
# date('now','localtime') thread-safe. SQLITE_ENABLE_MATH_FUNCTIONS adds the SQL
# math functions (sqrt, pow, log, ln, exp, trig, ceil, floor, pi, ...; libphoenix
# libm has every one, including acosh/asinh/atanh). SQLITE_DEFAULT_MEMSTATUS=0
# drops the global memory-statistics mutex from every malloc (the documented
# speed-up; sqlite3_memory_used() then reports 0).
SQLITE_FEATURES="\
	-DSQLITE_THREADSAFE=1 \
	-DSQLITE_OMIT_LOAD_EXTENSION \
	-DSQLITE_ENABLE_FTS5 \
	-DSQLITE_ENABLE_JSON1 \
	-DSQLITE_ENABLE_RTREE \
	-DSQLITE_ENABLE_MATH_FUNCTIONS \
	-DSQLITE_DEFAULT_MEMSTATUS=0 \
	-DHAVE_PREAD=1 \
	-DHAVE_PWRITE=1 \
	-DHAVE_LOCALTIME_R=1 \
	-DHAVE_READLINE=0"

p_prepare() {
	# No patches: the amalgamation compiles unmodified on Phoenix.
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	cd "${PREFIX_PORT_WORKDIR}"

	# libsqlite3.a + public headers (so other ports, e.g. CPython's _sqlite3
	# module, can link against the official SQLite port instead of a private copy).
	"${CROSS}gcc" ${CFLAGS} -O2 ${SQLITE_FEATURES} -c sqlite3.c -o sqlite3.o
	"${CROSS}ar" rcs libsqlite3.a sqlite3.o
	"${CROSS}ranlib" libsqlite3.a
	mkdir -p "${PREFIX_A}" "${PREFIX_H}"
	cp -a libsqlite3.a "${PREFIX_A}/"
	cp -a sqlite3.h sqlite3ext.h "${PREFIX_H}/"

	# The sqlite3 command-line shell -> /usr/bin.
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	"${CROSS}gcc" ${CFLAGS} ${LDFLAGS} -O2 ${SQLITE_FEATURES} \
		shell.c sqlite3.c -o "${PREFIX_PROG}/sqlite3" -lm
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/sqlite3" "${PREFIX_PROG}/sqlite3"
	b_install "${PREFIX_PROG_TO_INSTALL}/sqlite3" /usr/bin
}
