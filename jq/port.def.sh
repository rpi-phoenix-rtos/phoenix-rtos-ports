: # SPDX-License-Identifier: BSD-3-Clause
{
	ports_api=1
	name="jq"
	version="1.8.2"
	desc="Command-line JSON processor"
	cpe23="cpe:2.3:a:jqlang:jq:${version}:*:*:*:*:*:*:*"
	source="https://github.com/jqlang/jq/releases/download/jq-${version}/"
	archive_filename="jq-${version}.tar.gz"
	src_path="jq-${version}/"
	size="1959950"
	sha256="71b8d6e8f5fe81f6c6d0d110e3892251f6ce76ed095abd315e26e6e1193af3af"
	license="MIT"
	license_file="COPYING"
	conflicts=""
	depends="oniguruma"
	supports="phoenix>=3.3"
}

# jq's release tarball ships the pre-generated parser.c/lexer.c (no bison/flex) and
# a bundled decNumber (under vendor/ since 1.8), and has NO config.h -- ./configure
# normally emits the HAVE_* feature macros as -D flags. We compile directly with the
# cross toolchain and bake a curated, Phoenix-valid macro set (a native ./configure
# minus the functions libphoenix lacks). Regex builtins (test/match/sub/gsub/splits/scan)
# are ENABLED via the oniguruma port (depends=oniguruma): -DHAVE_LIBONIG + -I${PREFIX_H}
# at compile, -L${PREFIX_A} -lonig at link (libonig.a + <oniguruma.h> in the sysroot).
# The bundled vendor/oniguruma is NOT used.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	# BUILT_SOURCES that ./configure/make would normally generate.
	cd "${PREFIX_PORT_WORKDIR}"
	# Since 1.8 builtin.inc is a list of octal byte values (jq's Makefile.am rule),
	# not a string literal; keep the same od/sed pipeline.
	od -v -A n -t o1 -- src/builtin.jq |
		sed -e 's/$/ /' \
			-e 's/\([0123456789]\) /\1, /g' \
			-e 's/ $//' \
			-e 's/ 0/  0/g' \
			-e 's/ \([123456789]\)/ 0\1/g' >src/builtin.inc
	printf '#define JQ_CONFIG "phoenix-direct (+oniguruma)"\n' >src/config_opts.inc
	printf '#define JQ_VERSION "%s"\n' "${version}" >src/version.h
}

p_build() {
	cd "${PREFIX_PORT_WORKDIR}"

	# Curated HAVE_* set: only functions libphoenix actually provides (checked against
	# libphoenix.a symbols). Omitted: gamma, j0/j1/jn/y0/y1/yn, arc4random -- absent from
	# libphoenix; they gate obscure jq math builtins only. No HAVE___THREAD: libphoenix has
	# no TLS, so jv_alloc.c falls back to pthread keys.
	local DEFS=(
		-DPACKAGE_NAME='"jq"' -DPACKAGE_VERSION='"1.8.2"' -DPACKAGE_STRING='"jq 1.8.2"'
		-DPACKAGE='"jq"' -DVERSION='"1.8.2"'
		-D_GNU_SOURCE=1 -DHAVE_MEMMEM=1 -DUSE_DECNUM=1 -DHAVE_ALLOCA_H=1 -DHAVE_ALLOCA=1 -DHAVE_ISATTY=1
		-DHAVE_STRPTIME=1 -DHAVE_STRFTIME=1 -DHAVE_SETENV=1 -DHAVE_TIMEGM=1 -DHAVE_GMTIME_R=1 -DHAVE_GMTIME=1
		-DHAVE_LOCALTIME_R=1 -DHAVE_LOCALTIME=1 -DHAVE_GETTIMEOFDAY=1 -DHAVE_TM_TM_GMT_OFF=1 -DHAVE_SETLOCALE=1 -DHAVE_ATEXIT=1
		-DHAVE_ACOS=1 -DHAVE_ASIN=1 -DHAVE_ATAN2=1 -DHAVE_ATAN=1 -DHAVE_CEIL=1 -DHAVE_COPYSIGN=1 -DHAVE_COS=1 -DHAVE_COSH=1
		-DHAVE_ERF=1 -DHAVE_ERFC=1 -DHAVE_EXP2=1 -DHAVE_EXP=1 -DHAVE_FABS=1 -DHAVE_FDIM=1 -DHAVE_FLOOR=1 -DHAVE_FMA=1
		-DHAVE_FMAX=1 -DHAVE_FMIN=1 -DHAVE_FMOD=1 -DHAVE_HYPOT=1 -DHAVE_LOG10=1 -DHAVE_LOG2=1 -DHAVE_LOG=1 -DHAVE_MODF=1
		-DHAVE_NEARBYINT=1 -DHAVE_POW=1 -DHAVE_RINT=1 -DHAVE_ROUND=1 -DHAVE_SCALBN=1 -DHAVE_SIN=1 -DHAVE_SINH=1 -DHAVE_SQRT=1
		-DHAVE_TAN=1 -DHAVE_TANH=1 -DHAVE_TRUNC=1 -DIEEE_8087=1
		-DHAVE_ACOSH=1 -DHAVE_ASINH=1 -DHAVE_ATANH=1 -DHAVE_CBRT=1 -DHAVE_EXPM1=1 -DHAVE_LOG1P=1
		-DHAVE_FREXP=1 -DHAVE_LDEXP=1 -DHAVE_ILOGB=1 -DHAVE_LOGB=1 -DHAVE_NEXTAFTER=1 -DHAVE_NEXTTOWARD=1
		-DHAVE_SCALB=1 -DHAVE_SCALBLN=1 -DHAVE_SIGNIFICAND=1 -DHAVE_REMAINDER=1 -DHAVE_DREM=1 -DHAVE_EXP10=1
		-DHAVE_LGAMMA=1 -DHAVE_LGAMMA_R=1 -DHAVE_TGAMMA=1 -DHAVE_GETENTROPY=1
		-DHAVE_PTHREAD=1 -DHAVE_PTHREAD_KEY_CREATE=1 -DHAVE_PTHREAD_ONCE=1
		# Regex builtins via the oniguruma port (libonig.a + <oniguruma.h> in the sysroot).
		-DHAVE_LIBONIG=1
	)
	local SRCS=(
		src/builtin.c src/bytecode.c src/compile.c src/execute.c src/jv.c src/jv_alloc.c src/jv_aux.c
		src/jv_dtoa.c src/jv_dtoa_tsd.c src/jv_file.c src/jv_parse.c src/jv_print.c src/jv_unicode.c src/lexer.c
		src/linker.c src/locfile.c src/main.c src/parser.c src/util.c src/jq_test.c
		vendor/decNumber/decNumber.c vendor/decNumber/decContext.c
	)

	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	# -Wno-incompatible-pointer-types: jq's cfunction dispatch table stores
	# different-arity fn pointers in one slot (arity checked at runtime); GCC 14
	# turns the cast into an error by default. Benign for jq.
	"${CROSS}gcc" ${CFLAGS} ${LDFLAGS} -O2 -Wno-incompatible-pointer-types \
		-Isrc -Ivendor -I. -I"${PREFIX_H}" "${DEFS[@]}" "${SRCS[@]}" \
		-o "${PREFIX_PROG}/jq" -L"${PREFIX_A}" -lonig -lm
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/jq" "${PREFIX_PROG}/jq"
	b_install "${PREFIX_PROG_TO_INSTALL}/jq" /usr/bin
}
