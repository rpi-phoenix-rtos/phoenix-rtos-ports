: # SPDX-License-Identifier: BSD-3-Clause
{
	ports_api=1
	name="redis"
	version="7.2.16"
	desc="In-memory data structure store (server + CLI)"
	cpe23="cpe:2.3:a:redis:redis:${version}:*:*:*:*:*:*:*"
	source="https://download.redis.io/releases/"
	archive_filename="redis-${version}.tar.gz"
	src_path="redis-${version}/"
	size="3410977"
	sha256="960a8ec15e34ff40e57ff16837b26b33bd81f2da6d24497bb63de532a323a18e"
	license="BSD-3-Clause"
	license_file="COPYING"
	conflicts=""
	depends=""
	supports="phoenix>=3.3"
}

# Redis 7.2.x is BSD-3-Clause (pre-SSPL; 7.4 changed the license, so this port
# follows the 7.2 patch releases -- 7.2.11+ fixes CVE-2025-49844, the Lua
# use-after-free RCE). Built with the bundled deps and its own make, with two
# Phoenix accommodations:
#   1. patches/${version}/ drops the Linux link flags (-rdynamic/-ldl/-pthread/-lrt) --
#      pthread/dl/rt live in libphoenix and -rdynamic is meaningless for a static
#      link. (uname -s runs on the Linux BUILD host, so Redis picks its Linux branch.)
#      02 maps EAI_FAMILY to EAFNOSUPPORT so an IPv6 listen on an IPv4-only stack is
#      skipped; 03 restores SIG_DFL on entry to the crash handler (harmless where
#      SA_RESETHAND works) and skips INFO when crashing before initServer().
#   2. phoenix-compat.h (-include'd) shims a handful of Linux/glibc divergences that
#      only feed Redis's crash-report/watchdog diagnostics (setcanceltype, setitimer,
#      dladdr, a couple of errno constants) -- not the core data path. (A
#      setcancelstate guard for the --daemonize child is no longer needed:
#      libphoenix keeps pthread_self() valid across fork() since f36de2b.)
# MALLOC=libc skips jemalloc (hard to cross-compile; libphoenix malloc is fine). The
# event loop auto-falls back to ae_select on Phoenix (no epoll/kqueue).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}" "${version}"
}

p_build() {
	cd "${PREFIX_PORT_WORKDIR}"

	local CF="-DBYTE_ORDER=1234 -DLITTLE_ENDIAN=1234 -DBIG_ENDIAN=4321 -DAF_LOCAL=AF_UNIX"
	CF+=" -include ${PREFIX_PORT}/phoenix-compat.h"

	# Override the framework-exported CFLAGS on the command line: it carries
	# -I<sysroot>/include, which holds the official lua 5.3.6 port's headers and
	# would shadow Redis's bundled deps/lua (5.1) in eval.c (lua_open /
	# LUA_GLOBALSINDEX). libphoenix then comes from the toolchain's bundled copy, which
	# rebuild-rpi4b-fast.sh refreshes from the sysroot right after the core stage.
	# What it is replaced with reaches redis AND its bundled deps (hiredis, lua,
	# linenoise, hdr_histogram, fpconv), unlike REDIS_CFLAGS:
	# -fstack-protector-strong, whose __stack_chk_guard/__stack_chk_fail libphoenix
	# provides.
	make -C . \
		CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
		CFLAGS="-fstack-protector-strong" \
		MALLOC=libc BUILD_TLS=no USE_SYSTEMD=no \
		OPTIMIZATION=-O2 LDFLAGS="-static" \
		REDIS_CFLAGS="${CF}" -j4

	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	local b
	for b in redis-server redis-cli; do
		cp -a "src/${b}" "${PREFIX_PROG}/${b}"
		${STRIP} -o "${PREFIX_PROG_STRIPPED}/${b}" "${PREFIX_PROG}/${b}"
		b_install "${PREFIX_PROG_TO_INSTALL}/${b}" /usr/bin
	done
}
