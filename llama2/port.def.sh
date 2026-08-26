#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="llama2"
	version="20240529"
	desc="llama2.c — pure-C CPU inference for Llama-2 language models (single-threaded)"

	# Upstream (Andrej Karpathy's llama2.c) has no tagged releases, so the source is
	# pinned to a specific commit via GitHub's commit-archive tarball. GitHub serves
	# the tree at /archive/<ref>.tar.gz; the ref here is the full commit SHA, so the
	# fetched bytes are content-addressed and reproducible. `version` is the commit
	# date (port_manager's resolver wants dotted/numeric versions); the SHA below is
	# the authoritative provenance and also appears in archive_filename/src_path.
	commit="350e04fe35433e6d2941dce5a1f53308f87058eb"
	source="https://github.com/karpathy/llama2.c/archive"
	archive_filename="${commit}.tar.gz"
	src_path="llama2.c-${commit}/"

	size="700335"
	sha256="9210e104192375f2c156fd24a84d2692c6b199f8ebbc2337551842c03cc1ddce"

	license="MIT"
	license_file="LICENSE"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

# Framework migration of tools/llama2-port (the "ML inference on Pi4, phase 1 CPU"
# recipe). Builds the single-file, single-threaded fp32 inference engine `run.c`
# into /usr/bin/run-llama2. The only Phoenix change is in patches/: read_checkpoint()
# reads the whole checkpoint into RAM via malloc()+fread() instead of mmap()ing the
# file (a file-backed MAP_PRIVATE mapping, esp. over NFS, is not relied upon on this
# port). Model + tokenizer files are runtime data, staged separately (not shipped).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	cd "${PREFIX_PORT_WORKDIR}"

	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"

	# Static, single-threaded (no OpenMP on Phoenix), -O3 -lm — the HW-verified
	# configuration from tools/llama2-port/build.sh. libphoenix's libm already
	# covers the full math surface (expf/exp/sqrtf/sinf/cosf/powf).
	"${CROSS}gcc" ${CFLAGS} ${LDFLAGS} -O3 -static run.c -o "${PREFIX_PROG}/run-llama2" -lm
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/run-llama2" "${PREFIX_PROG}/run-llama2"

	b_install "${PREFIX_PROG_TO_INSTALL}/run-llama2" /usr/bin
}
