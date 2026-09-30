#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="openvpn"
	version="2.4.7"
	desc="Secure IP/Ethernet tunnel daemon"
	cpe23="cpe:2.3:a:openvpn:openvpn:${version}:*:*:*:community:*:*:*"

	source="https://swupdate.openvpn.org/community/releases/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="1457784"
	sha256="73dce542ed3d6f0553674f49025dfbdff18348eb8a25e6215135d686b165423c"

	# TODO: verify whether it is GPL v2 *only*
	license="GPL-2.0-only"
	license_file="COPYING"

	conflicts=""
	# TODO: openvpn 2.4.x does not compile against OpenSSL 3 (its openssl_compat.h
	# redefines EVP_PKEY_get_id & co. that 3.x provides as macros; verified
	# 2026-09-30 with 2.4.7 and 2.4.12 against 3.5.9). OpenSSL 3 support starts in
	# 2.5/2.6, so this port needs a version bump + a rebase of its Phoenix patch.
	depends="openssl>=3.5 lzo>=2.10"

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "$PREFIX_PORT_WORKDIR/config.h" ]; then
		OPENVPN_CFLAGS="-std=gnu99 -I${PREFIX_H}"
		(cd "$PREFIX_PORT_WORKDIR" && autoreconf -i -v -f)
		(cd "$PREFIX_PORT_WORKDIR" && "./configure" CFLAGS="$CFLAGS $OPENVPN_CFLAGS" LDFLAGS="$LDFLAGS" --host="${HOST}" --sbindir="$PREFIX_PROG")
	fi
}

p_build() {
	make -C "$PREFIX_PORT_WORKDIR"
	make -C "$PREFIX_PORT_WORKDIR" install-exec

	$STRIP -o "$PREFIX_PROG_STRIPPED/openvpn" "$PREFIX_PROG/openvpn"
	b_install "$PREFIX_PROG_TO_INSTALL/openvpn" /sbin/
}
