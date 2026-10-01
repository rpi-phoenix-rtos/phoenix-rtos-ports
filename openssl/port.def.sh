#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="openssl"
	# 3.5 is the current LTS line (supported until 2030-04). 4.0 exists but
	# removes engines, the fixed-version TLS method functions and custom
	# EVP_* methods; sscep 0.9.0 calls ENGINE_* unconditionally.
	version="3.5.9"
	desc="TLSv1.3 capable SSL and crypto library"
	cpe23="cpe:2.3:a:openssl:openssl:${version}:*:*:*:*:*:*:*"

	# NOT www.openssl.org/source/: it serves only the CURRENT release of each
	# branch and moves the rest to old/<branch>/, so a pinned version there
	# 404s as soon as the next patch release ships (the 1.1.1 port hit exactly
	# that). The GitHub release asset URL is permanent.
	source="https://github.com/openssl/openssl/releases/download/${name}-${version}/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	sha256="603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"
	size="53279637"

	license="Apache-2.0"
	license_file="LICENSE.txt"

	# Conflicting with its own other versions gives the port a private prefix,
	# versioned-ports/openssl-<version>/ (see dbus): the OpenSSL headers are then
	# visible only to ports that depend on it, never to the configure probes of
	# the rest of the image. Consumers find it with b_dependency_dir openssl or
	# its pkg-config files. Keep phoenix-rtos-build makes/ports.mk
	# (openssl=<version>) in step with version= above.
	conflicts="openssl!=${version}"
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/Makefile" ]; then
		# -fstack-protector-strong: libphoenix provides __stack_chk_guard and
		# __stack_chk_fail. Configure takes CFLAGS from the environment and the
		# phoenix targets add no stack protector of their own.
		CFLAGS+=" -fstack-protector-strong"
		cp "$PREFIX_PORT/30-phoenix.conf" "$PREFIX_PORT_WORKDIR/Configurations/"
		# --libdir=lib: the consumers (and the pkg-config files) expect lib/, and
		# 3.x otherwise picks lib64 on some 64-bit targets.
		# enable-ec_nistp_64_gcc_128 (aarch64 only; untried elsewhere): the
		# constant-time 64-bit-limb P-224/P-384/P-521 code on the compiler's
		# __int128, as the major distributions build 64-bit targets. P-256 keeps
		# the armv8 nistz256 assembler. The sources #error without __int128.
		local extra=()
		case "${TARGET_FAMILY}" in
		aarch64*) extra+=(enable-ec_nistp_64_gcc_128) ;;
		esac
		(cd "${PREFIX_PORT_WORKDIR}" && "${PREFIX_PORT_WORKDIR}/Configure" "phoenix-${TARGET_FAMILY}-${TARGET_SUBFAMILY}" \
			--prefix="$PREFIX_PORT_INSTALL" --libdir=lib --openssldir="/etc/ssl" no-docs "${extra[@]}")
	fi
}

p_build() {
	make -C "$PREFIX_PORT_WORKDIR" all
	make -C "$PREFIX_PORT_WORKDIR" install_sw

	cp -a "$PREFIX_PORT_INSTALL/bin/openssl" "$PREFIX_PROG"
	$STRIP -o "$PREFIX_PROG_STRIPPED/openssl" "$PREFIX_PROG/openssl"

	b_install "$PREFIX_PROG_TO_INSTALL/openssl" /usr/bin/
}
