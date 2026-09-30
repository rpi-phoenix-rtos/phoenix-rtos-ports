#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="dropbear"
	version="2026.94"
	desc="A smallish SSH server and client"
	cpe23="cpe:2.3:a:dropbear_ssh_project:dropbear_ssh:${version}:*:*:*:*:*:*:*"

	source="https://matt.ucc.asn.au/dropbear/releases/"
	archive_filename="${name}-${version}.tar.bz2"
	src_path="${name}-${version}/"

	size="2386978"
	sha256="e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81"

	license="MIT"
	license_file="LICENSE"

	iuse="zlib"
	depends="zlib? (zlib>=1.2.11)"
	conflicts=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "$PREFIX_PORT_WORKDIR/config.h" ]; then
		cp -a "$PREFIX_PORT/localoptions.h" "$PREFIX_PORT_WORKDIR"

		# -O2: the framework passes no optimisation level, and the post-quantum
		# key exchanges (sntrup761, mlkem768) are slow enough at -O0 to delay
		# every login noticeably.
		DROPBEAR_CFLAGS="-O2 -DENDIAN_LITTLE -DUSE_DEV_PTMX ${DROPBEAR_CUSTOM_CFLAGS}"
		DROPBEAR_LDFLAGS=""

		ENABLE_ZLIB="no"
		b_use "zlib" && ENABLE_ZLIB="yes"

		# shellcheck disable=2153 # CFLAGS, LDFLAGS are externally provided
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure CFLAGS="${CFLAGS} ${DROPBEAR_CFLAGS}" \
			LDFLAGS="${CFLAGS} ${LDFLAGS} ${DROPBEAR_LDFLAGS}" ARFLAGS="-r" \
			--host="${HOST}" --prefix="${PREFIX_PORT_INSTALL}" --enable-zlib="$ENABLE_ZLIB" --enable-static \
			--disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-harden)
	fi
}

p_build() {
	# create multi-binary and hardlinks
	make PROGRAMS="dropbear dbclient dropbearkey scp" -C "${PREFIX_PORT_WORKDIR}" MULTI=1

	$STRIP -o "$PREFIX_PROG_STRIPPED/dropbearmulti" "$PREFIX_PORT_WORKDIR/dropbearmulti"
	cp -a "$PREFIX_PORT_WORKDIR/dropbearmulti" "$PREFIX_PROG/dropbearmulti"

	b_install "$PREFIX_PROG_TO_INSTALL/dropbearmulti" /usr/bin

	mkdir -p "$PREFIX_ROOTFS/usr/sbin"
	ln -vf "$PREFIX_ROOTFS/usr/bin/dropbearmulti" "$PREFIX_ROOTFS/usr/sbin/dropbear"
	ln -vf "$PREFIX_ROOTFS/usr/bin/dropbearmulti" "$PREFIX_ROOTFS/usr/bin/dbclient"
	ln -vf "$PREFIX_ROOTFS/usr/bin/dropbearmulti" "$PREFIX_ROOTFS/usr/bin/scp"
}
