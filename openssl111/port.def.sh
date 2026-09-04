#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="openssl"
	version="1.1.1w"
	desc="TLSv1.3 capable SSL and crypto library"
	cpe23="cpe:2.3:a:openssl:openssl:${version}:-:*:*:*:*:*:*"

	# 1.1.1 is EOL, so www.openssl.org/source/ serves only the CURRENT release of
	# each supported branch; every 1.1.1 tarball lives under old/1.1.1/. This URL
	# must move with version= or the fetch 404s.
	source="https://www.openssl.org/source/old/1.1.1/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	sha256="cf3098950cb4d853ad95c0841f1f9c6d3dc102dccfcacd521d93925208b76ac8"
	size="9893384"

	license="OpenSSL"
	license_file="LICENSE"

	conflicts="openssl3>=0.0"
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/Makefile" ]; then
		cp "$PREFIX_PORT/30-phoenix.conf" "$PREFIX_PORT_WORKDIR/Configurations/"
		(cd "${PREFIX_PORT_WORKDIR}" && "${PREFIX_PORT_WORKDIR}/Configure" "phoenix-${TARGET_FAMILY}-${TARGET_SUBFAMILY}" --prefix="$PREFIX_PORT_INSTALL" --openssldir="/etc/ssl")
	fi
}

p_build() {
	make -C "$PREFIX_PORT_WORKDIR" all
	make -C "$PREFIX_PORT_WORKDIR" install_sw

	cp -a "$PREFIX_PORT_INSTALL/bin/openssl" "$PREFIX_PROG"
	$STRIP -o "$PREFIX_PROG_STRIPPED/openssl" "$PREFIX_PROG/openssl"

	b_install "$PREFIX_PROG_TO_INSTALL/openssl" /usr/bin/
}
