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
#
# --with-terminfo-dirs and --with-default-terminfo-dir: the database search list and the
# default $TERMINFO are compiled into the library and both default to ${datadir}/terminfo
# -- the build host's prefix, which then shipped in every program linking it.
# /usr/share/terminfo is where a database would go; the image has none, so the lookup
# fails and the fallbacks answer, as before.
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
			--with-terminfo-dirs=/usr/share/terminfo \
			--with-default-terminfo-dir=/usr/share/terminfo \
			CFLAGS="${CFLAGS} -O2 -fPIC" CPPFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" \
			RANLIB="${CROSS}ranlib")
	fi
}

p_build() {
	make -C "$PREFIX_PORT_WORKDIR"

	# None of the libraries may compile in a build path (checked on a --strip-debug
	# copy: the debug info names the work tree by design; the needle also catches a
	# path -fmacro-prefix-map left relative).
	local a needle nodebug="${PREFIX_PORT_BUILD}/nodebug.a"
	needle="$(basename "$(dirname "${PREFIX_BUILD%/}")")/$(basename "${PREFIX_BUILD%/}")"
	for a in "$PREFIX_PORT_WORKDIR"/lib/*.a; do
		"${CROSS}strip" --strip-debug -o "${nodebug}" "${a}"
		if grep -qaF "${needle}" "${nodebug}"; then
			b_die "ncurses: $(basename "${a}") compiles in a build path: $(grep -ao -- "[[:print:]]*${needle}[[:print:]]*" "${nodebug}" | head -1)"
		fi
	done
	rm -f "${nodebug}"

	make -C "$PREFIX_PORT_WORKDIR" install
	# ncurses (--disable-overwrite default) installs its headers under
	# $includedir/ncurses/. Mirror them to the include root as well so consumers
	# that include <curses.h> (not <ncurses/curses.h>) also resolve.
	for h in curses.h ncurses.h term.h termcap.h unctrl.h ncurses_dll.h eti.h nc_tparm.h; do
		[ -f "${PREFIX_H}/ncurses/${h}" ] && cp -a "${PREFIX_H}/ncurses/${h}" "${PREFIX_H}/" || true
	done

	# A small terminfo database in /usr/share/terminfo (the compiled-in search path), so a
	# TERM outside the fallbacks above (xterm-color, foot, tmux, ...) works too: with only
	# the fallbacks, `TERM=xterm-color mc` failed with "can't load termcap". Compiled from
	# this release's own terminfo.src by the build host's tic (the cross build makes no
	# programs); the binary format is the same for every ncurses 6.
	if [ -n "${PREFIX_ROOTFS:-}" ]; then
		local ti="${PREFIX_ROOTFS}/usr/share/terminfo"
		local terms="ansi,dumb,linux,vt100,vt102,vt220,xterm,xterm-color,xterm-16color,xterm-256color"
		terms="${terms},xterm-direct,screen,screen-256color,tmux,tmux-256color,foot,foot-direct"
		terms="${terms},alacritty,st-256color,putty,konsole,gnome-256color,rxvt"
		command -v tic >/dev/null || b_die "ncurses: the build host has no tic (Debian/Ubuntu package ncurses-bin)"
		mkdir -p "${ti}"
		tic -x -o "${ti}" -e "${terms}" "${PREFIX_PORT_WORKDIR}/misc/terminfo.src" ||
			b_die "ncurses: tic failed to compile the terminfo database"
		[ -f "${ti}/x/xterm-color" ] || b_die "ncurses: ${ti}/x/xterm-color was not written"
	fi
}
