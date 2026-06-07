#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libnfs"
	version="6.0.2"
	desc="Client library for accessing NFS shares over the network"

	# libnfs ships an autotools build, but its release "tarballs" are GitHub's
	# auto-generated tag archives that do NOT contain a pre-generated ./configure
	# (configure is produced by ./bootstrap -> autoreconf, and this dev host has
	# no autotools). The pre-generated RPC/XDR stubs ARE committed, so no rpcgen
	# is needed. We therefore clone the tag and drive the cross-build with our own
	# Makefile (files/Makefile.phoenix) + a hand-written config.h, bypassing
	# autoconf entirely. git_source mode is gated on the tag tree's hash+size.
	git_rev="libnfs-${version}"
	git_source="https://github.com/sahlberg/libnfs.git"

	src_path="${name}-${git_rev}"

	# `git archive --format=tar HEAD | sha256sum` and the sum of all non-.git file
	# sizes for the libnfs-6.0.2 tag tree (verified host-side; see
	# docs/research/2026-06-07-nfs-rootfs-feasibility/T0-libnfs-build-notes.md).
	size="2112263"
	sha256="8818a12d82f6df874afe669d893808b2d45129710e8ec147467c8def3e651d07"

	# Only the LGPL-2.1 core library is built/shipped (see exclusions in p_build).
	# The repo also carries GPL-3 (sample utils) and BSD bits that are NOT linked.
	license="LGPL-2.1"
	license_file="COPYING"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# Cross-build libnfs.a with the project toolchain. CROSS is derived from the
	# Phoenix tool names; CFLAGS is the port-export CFLAGS the build system hands
	# us (target arch flags, no -Werror). The Makefile compiles the NFSv3+NFSv4
	# sync-API core only and archives it into libnfs.a.
	#
	# Excluded objects (and why): lib/krb5-wrapper.c (HAVE_LIBKRB5 off),
	# lib/multithreading.c (single-threaded sync build, no HAVE_MULTITHREADING),
	# tls/ (HAVE_TLS off), nlm/ nsm/ rquota/ (locking/quota, unused), and all
	# other-OS seams (win32/aros/ps2/ps3) plus examples/utils/tests (the GPL-3
	# programs). nfs/nfsacl.c IS built: nfs_v3.c references rpc_nfsacl3_getacl_task.
	# CROSS is the cross-tool prefix the build system exports (e.g.
	# "aarch64-phoenix-"); the Makefile invokes "${CROSS}gcc"/"${CROSS}ar".
	# (Note: do NOT derive it from HOST -- HOST is the autotools triple
	# "${TARGET_FAMILY}-phoenix" = "aarch64a72-phoenix" here, which is NOT a
	# real tool prefix; only $CROSS matches the installed toolchain.)
	make -f "${PREFIX_PORT}/files/Makefile.phoenix" \
		CROSS="${CROSS}" \
		CFLAGS="${CFLAGS}" \
		LIBNFS_SRC="${PREFIX_PORT_WORKDIR}" \
		CONFIG_H_DIR="${PREFIX_PORT}/files" \
		OUT="${PREFIX_PORT_BUILD}"

	# Stage the static lib + public headers into the Phoenix sysroot staging dirs
	# (same pattern as curl): headers under include/nfsc/, lib as libnfs.a.
	mkdir -p "${PREFIX_H}/nfsc"
	cp -a "${PREFIX_PORT_WORKDIR}/include/nfsc/libnfs.h" \
		"${PREFIX_PORT_WORKDIR}/include/nfsc/libnfs-raw.h" \
		"${PREFIX_PORT_WORKDIR}/include/nfsc/libnfs-zdr.h" \
		"${PREFIX_H}/nfsc/"
	cp -a "${PREFIX_PORT_BUILD}/libnfs.a" "${PREFIX_A}/libnfs.a"
}
