#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="dbus"
	version="1.16.2"
	desc="D-Bus 1.16 message bus (dbus-daemon, libdbus-1, tools) -- the desktop session bus"
	cpe23="cpe:2.3:a:freedesktop:dbus:${version}:*:*:*:*:*:*:*"

	source="https://dbus.freedesktop.org/releases/dbus/"
	archive_filename="dbus-${version}.tar.xz"
	src_path="dbus-${version}/"

	size="1115644"
	sha256="0ba2a1a4b16afe7bceb2c07e9ce99a8c2c3508e5dec290dbb643384bd6beb7e2"

	# COPYING: "AFL-2.1 or, at your option, GPL-2.0-or-later"; a few standalone
	# programs this port does not build (dbus-cleanup-sockets) are GPL only.
	license="AFL-2.1 OR GPL-2.0-or-later"
	license_file="COPYING"

	# NEW GPU LANE: conflicting with its own other versions only gives the port a
	# private prefix, versioned-ports/<name>-<version>/, instead of the shared
	# _build/<target>/{lib,include} (phoenix-rtos-build port_manager,
	# InstallableCandidate.install_path): nothing it builds is visible to the configure
	# probes of the image's ports. (openssl111 uses the same mechanism.)
	conflicts="dbus!=${version}"
	# expat (libexpat.a + headers) is built by xorg_fonts into the shared prefix.
	depends="xorg_fonts"

	# rootfs: also copy the staging tree (stage/, see p_build) into the image
	# rootfs. Off by default: this port is not in any default ports.yaml, and even
	# a standalone `scripts/build-port.sh dbus` then leaves _fs/root untouched.
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Transcribed from the coordination repo's tools/gpu-lane/dbus/build.sh (M7 stage 3,
# the proven build). The work tree mirrors that script's <out> directory --
# ${PREFIX_PORT_BUILD}/out/{src,deps,dbus-build,destdir,...} -- so meson sees the
# same relative source paths (the __FILE__ strings in the binaries) as the tools
# build.
#
#   ${PREFIX_PORT_INSTALL}/destdir/   `meson install` (libdbus-1.a, headers, dbus-1.pc)
#   ${PREFIX_PORT_INSTALL}/bin/       the programs, unstripped (addr2line) and -stripped
#   ${PREFIX_PORT_INSTALL}/stage/     the files for the target rootfs (+ stage.MANIFEST)
#
# Configuration: unix transport only; no systemd, launchd, X11 autolaunch, SELinux/
# AppArmor/libaudit, epoll/kqueue/inotify (poll() main loop; config reload on SIGHUP),
# tests or docs; traditional (fork/exec) bus activation stays on (XFCE starts xfconfd
# that way). Peer credentials: dbus-sysdeps-unix.c tests SO_PEERCRED with #ifdef (no
# probe). Since kernel master f234ed3e, <sys/socket.h> (via <phoenix/posix-socket.h>)
# defines it and a Linux-layout struct ucred {pid, uid, gid}, so the daemon reads the
# peer's pid and uid with getsockopt(SOL_SOCKET, SO_PEERCRED) and EXTERNAL works
# (files/conf/session-phoenix-external.conf). Built against an older sysroot, it
# compiles the "no credentials mechanism" branch (a #warning) and only ANONYMOUS
# (files/conf/session-phoenix.conf, lab only) can succeed. Binaries built with
# SO_PEERCRED still run on an older kernel: getsockopt() fails, and they fall back to
# that behaviour.
#
# Host tools: meson, ninja, pkg-config.

p_prepare() {
	local t
	for t in meson ninja pkg-config git; do
		command -v "${t}" >/dev/null || b_die "dbus: host tool ${t} not found"
	done

	# The patched source, as its own git repository: `git apply` inside a directory
	# of ANOTHER repository (the buildroot is one) applies relative to that
	# repository's root and silently skips every path.
	local out="${PREFIX_PORT_BUILD}/out"
	local src="${out}/src/dbus"
	local stamp
	stamp="$( { echo "${sha256}"; cat "${PREFIX_PORT}/patches/"*.patch; } | sha256sum | cut -c1-16)"
	if [ "$(cat "${src}.stamp" 2>/dev/null || true)" != "${stamp}" ]; then
		rm -rf "${src}" "${src}.tmp"
		mkdir -p "${out}/src"
		cp -a "${PREFIX_PORT_WORKDIR%/}" "${src}.tmp"
		mv "${src}.tmp" "${src}"
		git -C "${src}" init -q
		git -C "${src}" add -A -f
		git -C "${src}" -c user.name=build -c user.email=build@invalid commit -q -m "${archive_filename}"
		local p
		for p in "${PREFIX_PORT}/patches/"*.patch; do
			[ -e "${p}" ] || continue
			echo "dbus: apply $(basename "${p}")"
			git -C "${src}" apply --whitespace=nowarn "${p}"
			git -C "${src}" add -A -f
			git -C "${src}" -c user.name=build -c user.email=build@invalid commit -q -m "$(basename "${p}")"
		done
		echo "${stamp}" >"${src}.stamp"
		rm -f "${out}/dbus.configured"
	fi
}

p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local SRC="${out}/src/dbus"
	local B S TC PHXCC D
	B="$(b_dependency_dir xorg_fonts)"
	B="${B%/}"
	S="${PREFIX_BUILD%/}/sysroot"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	D="${out}/deps"
	local jobs
	jobs="$(nproc)"
	local PROGS=(dbus-daemon dbus-send dbus-monitor dbus-run-session dbus-uuidgen)

	local p
	for p in "${S}/lib/libphoenix.a" "${TC}-gcc" "${TC}-gcc-ar" "${TC}-nm" "${TC}-strip" "${TC}-readelf" \
		"${B}/lib/libexpat.a" "${B}/include/expat.h"; do
		[ -e "${p}" ] || b_die "dbus: missing ${p}"
	done
	mkdir -p "${out}/bin" "${D}"

	# The aarch64-phoenix gcc rejects -pthread (meson's threads dependency adds it);
	# Phoenix pthreads live in libphoenix, so dropping the flag is exact. Also inside
	# @response files. (tools/gpu-lane/{e7-drm-build,gtk3-wayland}/bin/phx-gcc)
	PHXCC="${out}/bin/phx-gcc"
	cat >"${PHXCC}" <<EOF
#!/bin/sh
# Generated by the phoenix-rtos-ports dbus recipe: ${TC}-gcc without -pthread.
for a; do
	shift
	case "\$a" in
		-pthread) ;;
		@*) f="\${a#@}"
			if [ -f "\$f" ] && grep -q -- '-pthread' "\$f"; then
				sed 's/\\(^\\| \\)-pthread\\( \\|\$\\)/\\1\\2/g' "\$f" > "\$f.nopthread"
				set -- "\$@" "@\$f.nopthread"
			else
				set -- "\$@" "\$a"
			fi ;;
		*) set -- "\$@" "\$a" ;;
	esac
done
exec "${TC}-gcc" "\$@"
EOF
	chmod +x "${PHXCC}"

	# The meson options shared with the tools build's native --host copy (its host
	# test proves THIS configuration).
	local COMMON_OPTS=(-Dmessage_bus=true -Dtools=true -Dtraditional_activation=true -Duser_session=false
		-Depoll=disabled -Dkqueue=disabled -Dinotify=disabled -Dlaunchd=disabled -Dsystemd=disabled
		-Dx11_autolaunch=disabled -Dselinux=disabled -Dapparmor=disabled -Dlibaudit=disabled
		-Dmodular_tests=disabled -Dinstalled_tests=false -Dintrusive_tests=false -Dvalgrind=disabled
		-Ddoxygen_docs=disabled -Dxml_docs=disabled -Dducktype_docs=disabled -Dqt_help=disabled
		-Drelocation=disabled -Dasserts=false -Dchecks=true -Dverbose_mode=true -Dstats=true
		-Dsession_socket_dir=/tmp -Druntime_dir=/var/run -Ddbus_user=root -Dtest_user=root)

	# --- expat view + meson cross file ---
	# A private view: exactly expat's headers and archive. The shared include dir also
	# holds GL/ and X11/ headers and is never put on a search path.
	local expat_ver
	expat_ver="$(sed -n 's/^Version: //p' "${B}/lib/pkgconfig/expat.pc")"
	mkdir -p "${D}/expat/include" "${D}/expat/lib/pkgconfig"
	cp "${B}/include/expat.h" "${B}/include/expat_external.h" "${D}/expat/include/"
	if [ -f "${B}/include/expat_config.h" ]; then
		cp "${B}/include/expat_config.h" "${D}/expat/include/"
	fi
	cp "${B}/lib/libexpat.a" "${D}/expat/lib/"
	printf '%s\n' "prefix=${D}/expat" "Name: expat" "Description: expat from the Phoenix ports prefix" \
		"Version: ${expat_ver}" "Libs: -L\${prefix}/lib -lexpat" "Libs.private: -lm" "Cflags: -I\${prefix}/include" \
		>"${D}/expat/lib/pkgconfig/expat.pc"

	local PKGC="${out}/pkg-config-phoenix"
	cat >"${PKGC}" <<EOF
#!/bin/sh
# pkg-config restricted to the private expat view.
export PKG_CONFIG_LIBDIR=${D}/expat/lib/pkgconfig
unset PKG_CONFIG_PATH
exec /usr/bin/pkg-config --static "\$@"
EOF
	chmod +x "${PKGC}"
	local CROSSF="${out}/phoenix-aarch64.cross"
	# NB: nothing is force-included (-include) into c_args: meson's has_function()
	# probes declare `char f(void)` and a real prototype in scope turns every probe
	# into NO. The constants libphoenix lacks are in patches/0001 instead.
	cat >"${CROSSF}" <<EOF
# Generated by the phoenix-rtos-ports dbus recipe (aarch64-phoenix, Pi 4).
[binaries]
c = '${PHXCC}'
ar = '${TC}-gcc-ar'
nm = '${TC}-nm'
strip = '${TC}-strip'
objcopy = '${TC}-objcopy'
pkg-config = '${PKGC}'

[host_machine]
system = 'phoenix'
cpu_family = 'aarch64'
cpu = 'cortex-a72'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['--sysroot=${S}/', '-B${S}/lib/', '-mcpu=cortex-a72', '-mtune=cortex-a72', '-mstrict-align', '-mno-outline-atomics', '-ffunction-sections', '-fdata-sections']
c_link_args = ['--sysroot=${S}/', '-B${S}/lib/', '-static', '-Wl,--gc-sections', '-Wl,-z,max-page-size=0x1000']
default_library = 'static'
EOF

	# --- target build ---
	local BD="${out}/dbus-build"
	if [ ! -f "${out}/dbus.configured" ] || [ ! -f "${BD}/build.ninja" ]; then
		rm -rf "${BD}"
		meson setup "${BD}" "${SRC}" --cross-file "${CROSSF}" --prefix /usr --sysconfdir /etc --localstatedir /var \
			--libdir lib --buildtype=debugoptimized -Db_staticpic=false -Db_pie=false --wrap-mode=nodownload \
			"${COMMON_OPTS[@]}" >"${out}/dbus-setup.log" 2>&1 || { tail -40 "${out}/dbus-setup.log"; b_die "dbus: meson setup failed"; }
		touch "${out}/dbus.configured"
	fi
	# A probe that silently answers NO changes the code compiled (socketpair() for
	# activation, accept4() for CLOEXEC): refuse the build rather than ship it.
	local f
	for f in socket socketpair accept4 poll getpwnam_r setenv; do
		grep -qE "Checking for function \"${f}\" : YES" "${BD}/meson-logs/meson-log.txt" ||
			b_die "dbus: meson probe for ${f}() answered NO (libphoenix has it): see ${BD}/meson-logs"
	done
	ninja -C "${BD}" -j"${jobs}" >"${out}/dbus-ninja.log" 2>&1 || { grep -E -A6 'error|FAILED' "${out}/dbus-ninja.log" | head -80; b_die "dbus: build failed"; }
	echo "dbus: built ($(grep -c 'warning:' "${out}/dbus-ninja.log" || true) warning line(s))"
	grep -E 'warning: #warning' "${out}/dbus-ninja.log" | sort -u || true
	rm -rf "${out}/destdir"
	DESTDIR="${out}/destdir" ninja -C "${BD}" install >"${out}/dbus-install.log" 2>&1 || { tail -20 "${out}/dbus-install.log"; b_die "dbus: install failed"; }
	local o
	for o in "${PROGS[@]}"; do
		f="$(find "${BD}" -maxdepth 3 -type f -name "${o}" -perm -u+x | head -1)"
		[ -n "${f}" ] || b_die "dbus: ${o} not built"
		cp "${f}" "${out}/bin/${o}"
		"${TC}-strip" -o "${out}/bin/${o}-stripped" "${out}/bin/${o}"
	done
	grep -E 'HAVE_UNIX_FD_PASSING|HAVE_SOCKETPAIR|HAVE_ACCEPT4|HAVE_POLL|DBUS_HAVE_LINUX|HAVE_GETPEEREID|HAVE_CMSGCRED|HAVE_SYSLOG|DBUS_ENABLE_INOTIFY|DBUS_BUS_ENABLE_' \
		"${BD}/config.h" | sed 's/^/dbus: config.h: /' || true

	# --- verification (the tools build's gate, unchanged) ---
	local bad=0 und n interp dyn strs s
	for o in "${PROGS[@]}"; do
		und="$("${TC}-nm" -u "${out}/bin/${o}" || true)"
		n=$(grep -c . <<<"${und}" || true)
		interp="$("${TC}-readelf" -l "${out}/bin/${o}" | grep -c 'INTERP' || true)"
		dyn="$("${TC}-readelf" -d "${out}/bin/${o}" 2>&1 | grep -c 'NEEDED' || true)"
		echo "dbus: ${o}: nm -u ${n}, PT_INTERP ${interp}, DT_NEEDED ${dyn}, $(stat -c %s "${out}/bin/${o}") bytes, stripped $(stat -c %s "${out}/bin/${o}-stripped")"
		if [ "${n}" != 0 ] || [ "${interp}" != 0 ] || [ "${dyn}" != 0 ]; then
			head -10 <<<"${und}"
			bad=1
		fi
	done
	strs="$(strings -a "${out}/bin/dbus-daemon-stripped")"
	for s in 'ANONYMOUS' 'EXTERNAL' 'DBUS_COOKIE_SHA1' 'allow_anonymous' 'DBUS_VERBOSE' 'unix:path=' 'org.freedesktop.DBus'; do
		n=$(grep -cF -- "${s}" <<<"${strs}" || true)
		echo "dbus: dbus-daemon strings '${s}': ${n}"
		[ "${n}" != 0 ] || bad=1
	done
	# The credentials path compiled in: SO_PEERCRED appears as a verbose message only
	# when the sysroot defines it (since kernel f234ed3e: 2).
	echo "dbus: dbus-daemon strings 'SO_PEERCRED' (0 = the sysroot has no SO_PEERCRED): $(grep -cF 'SO_PEERCRED' <<<"${strs}" || true)"
	[ -f "${out}/destdir/usr/lib/libdbus-1.a" ] || { echo "dbus: libdbus-1.a not installed"; bad=1; }
	[ "${bad}" = 0 ] || b_die "dbus: verification failed"

	# --- install: this port's prefix ---
	local I="${PREFIX_PORT_INSTALL%/}"
	rm -rf "${I:?}/destdir" "${I:?}/bin" "${I:?}/stage"
	mkdir -p "${I}/bin"
	cp -a "${out}/destdir" "${I}/destdir"
	for o in "${PROGS[@]}"; do
		cp -a "${out}/bin/${o}" "${out}/bin/${o}-stripped" "${I}/bin/"
	done
	(cd "${I}/bin" && sha256sum "${PROGS[@]/%/-stripped}") >"${I}/SHA256SUMS"
	cat "${I}/SHA256SUMS"

	# --- the staging tree: stage/ mirrors the target rootfs. The session bus configuration
	# the XFCE session starts dbus-daemon with (xfce_wayland's xfce-desktop.sh); the EXTERNAL
	# variant (files/conf/session-phoenix-external.conf) is the tools host test's, not the image's
	local ST="${I}/stage"
	for o in "${PROGS[@]}"; do
		install -D -m 755 "${out}/bin/${o}-stripped" "${ST}/bin/${o}"
	done
	install -D -m 644 "${PREFIX_PORT}/files/conf/session-phoenix.conf" "${ST}/etc/dbus-1/session-phoenix.conf"
	# activatable services directory (xfconfd's .service file comes with xfce_wayland)
	mkdir -p "${ST}/usr/share/dbus-1/services"
	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs sha256sum) >"${I}/stage.MANIFEST"

	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "dbus: staged $(wc -l <"${I}/stage.MANIFEST") file(s) into ${PREFIX_FS}/root"
	fi
}
