#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="ncurses"
	version="6.4"
	desc="ncurses terminal library (static, terminfo fallbacks compiled in)"
	cpe23="cpe:2.3:a:gnu:ncurses:${version}:*:*:*:*:*:*:*"

	source="https://ftp.gnu.org/gnu/ncurses/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="3612591"
	sha256="6931283d9ac87c5073f30b6290c4c75f21632bb4fc3603ac8100812bed248159"

	license="X11"
	license_file="COPYING"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

# Phoenix has no on-disk terminfo database, so the common terminal descriptions
# are compiled INTO the library via --with-fallbacks; setupterm() then resolves
# TERM from the built-in set (Pi UART/fbcon console: vt100/linux/ansi; a host
# terminal: xterm variants) with no /usr/share/terminfo needed at runtime.
# Static only, no progs/tests/cxx/ada/manpages — this is a reusable libncurses.a
# for dependent ports (nano, mc, python curses).
p_prepare() {
	if [ ! -f "$PREFIX_PORT_WORKDIR/config.status" ]; then
		(cd "$PREFIX_PORT_WORKDIR" && "./configure" \
			--host="${HOST}" --build="x86_64-pc-linux-gnu" \
			--prefix="$PREFIX_PORT_INSTALL" --libdir="$PREFIX_A" --includedir="$PREFIX_H" \
			--without-shared --without-debug --without-tests --without-progs \
			--without-cxx --without-cxx-binding --without-ada --without-manpages \
			--disable-db-install --enable-termcap --disable-home-terminfo --enable-sp-funcs \
			--without-pkg-config \
			--with-fallbacks="xterm,xterm-256color,vt100,vt220,linux,ansi,dumb,screen" \
			CFLAGS="${CFLAGS} -O2 -fPIC" CPPFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" \
			RANLIB="${CROSS}ranlib")
	fi
}

p_build() {
	make -C "$PREFIX_PORT_WORKDIR"
	make -C "$PREFIX_PORT_WORKDIR" install
	# ncurses (--disable-overwrite default) installs its headers under
	# $includedir/ncurses/. Mirror them to the include root as well so consumers
	# that include <curses.h> (not <ncurses/curses.h>) also resolve.
	for h in curses.h ncurses.h term.h termcap.h unctrl.h ncurses_dll.h eti.h nc_tparm.h; do
		[ -f "${PREFIX_H}/ncurses/${h}" ] && cp -a "${PREFIX_H}/ncurses/${h}" "${PREFIX_H}/" || true
	done
}
