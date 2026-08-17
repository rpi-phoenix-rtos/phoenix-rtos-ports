: # SPDX-License-Identifier: BSD-3-Clause
{
	ports_api=1
	name="redis"
	version="7.2.4"
	desc="In-memory data structure store (server + CLI)"
	cpe23="cpe:2.3:a:redis:redis:${version}:*:*:*:*:*:*:*"
	source="https://download.redis.io/releases/"
	archive_filename="redis-${version}.tar.gz"
	src_path="redis-${version}/"
	size="3386861"
	sha256="8d104c26a154b29fd67d6568b4f375212212ad41e0c2caa3d66480e78dbd3b59"
	license="BSD-3-Clause"
	license_file="COPYING"
	conflicts=""
	depends=""
	supports="phoenix>=3.3"
}

# Redis 7.2.4 is BSD-3-Clause (pre-SSPL). Built with the bundled deps and its own
# make, with two Phoenix accommodations:
#   1. patches/7.2.4/ drops the Linux link flags (-rdynamic/-ldl/-pthread/-lrt) --
#      pthread/dl/rt live in libphoenix and -rdynamic is meaningless for a static
#      link. (uname -s runs on the Linux BUILD host, so Redis picks its Linux branch.)
#   2. phoenix-compat.h (-include'd) shims a handful of Linux/glibc divergences that
#      only feed Redis's crash-report/watchdog diagnostics (setcanceltype, setitimer,
#      dladdr, a couple of errno constants) -- not the core data path.
# MALLOC=libc skips jemalloc (hard to cross-compile; libphoenix malloc is fine). The
# event loop auto-falls back to ae_select on Phoenix (no epoll/kqueue).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}" "${version}"
}

p_build() {
	cd "${PREFIX_PORT_WORKDIR}"

	local CF="-DBYTE_ORDER=1234 -DLITTLE_ENDIAN=1234 -DBIG_ENDIAN=4321 -DAF_LOCAL=AF_UNIX"
	CF+=" -include ${PREFIX_PORT}/phoenix-compat.h"

	# Override the framework-exported CFLAGS with CFLAGS= : it carries
	# -I<sysroot>/include, which holds the official lua 5.3.6 port's headers and
	# would shadow Redis's bundled deps/lua (5.1) in eval.c (lua_open /
	# LUA_GLOBALSINDEX). The cross gcc's built-in sysroot still resolves libphoenix.
	make -C . \
		CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
		CFLAGS= \
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
