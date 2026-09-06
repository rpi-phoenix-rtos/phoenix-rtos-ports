#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="ca_certificates"
	# certifi's version IS the Mozilla store snapshot date (YYYY.M.D), so the port
	# version tracks the trust store, not a piece of software.
	version="2026.7.22"
	desc="Mozilla trusted root CA store (PEM bundle) for /etc/ssl"

	# The bundle is Mozilla's NSS root store, converted to a single PEM file by
	# the same mk-ca-bundle pipeline curl uses for cacert.pem. We take it from
	# the `certifi` sdist rather than curl.se/ca/cacert.pem because the ports
	# framework unconditionally `tar xf`s the fetched artifact and requires a
	# license file inside the extracted tree (port_prepare.sh) — a bare .pem
	# satisfies neither. certifi ships exactly one data file (certifi/cacert.pem)
	# plus the MPL-2.0 LICENSE, and every release is an immutable, hash-pinned
	# PyPI sdist, so this is reproducible in a way a "latest" URL is not.
	source="https://files.pythonhosted.org/packages/a3/c2/24167ea9858356b47a87a50d39908bfdb72ceeefe0041586e704e5376b3a"
	archive_filename="certifi-${version}.tar.gz"
	src_path="certifi-${version}/"

	size="138112"
	sha256="741e2c3b351ddf169a738da9f2c048608ff7f2c5cc02f1ebc6b118bb090d5d55"

	# MPL-2.0 covers the Mozilla store itself; certifi redistributes it under the
	# same terms. This is DATA shipped in the rootfs, not code in a Phoenix core
	# repo, and phoenix-rtos-ports may carry copyleft recipes (see dillo, GPL-3.0).
	# MPL-2.0 sec. 3.2 wants the license to travel with the distribution, so
	# p_build installs LICENSE next to the bundle.
	license="MPL-2.0"
	license_file="LICENSE"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# No patches; the artifact is data. p_prepare is called unconditionally by
	# port_prepare.sh (unlike p_common), so it has to exist.
	:
}

p_build() {
	local bundle="${PREFIX_PORT_WORKDIR}/certifi/cacert.pem"
	local rootfs="${PREFIX_FS}/root"

	[ -f "${bundle}" ] || b_die "certifi/cacert.pem missing from ${PREFIX_PORT_WORKDIR}"

	# TWO real copies, no symlink and no exploded hash-link directory:
	#
	#   /etc/ssl/certs/ca-certificates.crt
	#       The Debian/Ubuntu path. It is ca_files[0] in dillo's
	#       src/IO/tls_mbedtls.c Tls_load_certificates(), i.e. the first bundle
	#       Dillo tries, and it is also what curl's CURL_CA_BUNDLE points at
	#       (see curl/port.def.sh).
	#
	#   /etc/ssl/cert.pem
	#       OpenSSL's X509_get_default_cert_file() == OPENSSLDIR "/cert.pem",
	#       and openssl111/port.def.sh configures --openssldir=/etc/ssl. This is
	#       what `openssl s_client`, python3's ssl module, and anything else
	#       calling SSL_CTX_set_default_verify_paths() reads.
	#
	# A symlink would depend on symlink fidelity through the ext2 packer, the NFS
	# export and Phoenix's own path resolution; 200 kB of duplication buys that
	# whole question away. The per-certificate `<hash>.0` CApath layout that
	# distributions also ship is deliberately NOT recreated: OpenSSL's CAfile
	# covers every consumer we have, and ~150 tiny files under /etc/ssl/certs/ on
	# an NFS root is the fontconfig walk-the-export trap all over again.
	mkdir -p "${rootfs}/etc/ssl/certs"
	install -m 644 "${bundle}" "${rootfs}/etc/ssl/certs/ca-certificates.crt"
	install -m 644 "${bundle}" "${rootfs}/etc/ssl/cert.pem"

	mkdir -p "${rootfs}/usr/share/licenses/ca_certificates"
	install -m 644 "${PREFIX_PORT_WORKDIR}/LICENSE" \
		"${rootfs}/usr/share/licenses/ca_certificates/LICENSE"
}
