#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xkeyboard_config"
	version="2.48"
	desc="xkeyboard-config: the XKB keyboard data (rules, keycodes, types, compat, symbols) at /usr/share/X11/xkb"

	# The keyboard database libxkbcommon (wayland_phoenix) reads when a program asks for
	# a keymap by rules/model/layout (xkb_keymap_new_from_names): GTK (GDK's Wayland
	# backend), XFCE, labwc, SDL. Without it every GTK program logs XKB-338 / XKB-822
	# ("Couldn't find file rules/evdev") and falls back to its built-in US keymap.
	#
	# Data only: nothing is compiled for the target. The build runs upstream's meson
	# build natively -- project() declares no language, so no compiler is probed -- to
	# generate the evdev and base rules files (a Python >= 3.11 generator, stdlib only)
	# and installs into a scratch DESTDIR; p_build then stages the part libxkbcommon
	# reads. Host tools: meson, ninja, python3 >= 3.11, perl (rules/xml2lst.pl, which
	# upstream's build requires).
	source="https://www.x.org/releases/individual/data/xkeyboard-config/"
	archive_filename="xkeyboard-config-${version}.tar.xz"
	src_path="xkeyboard-config-${version}/"

	size="953316"
	sha256="b77041324f0109f77161ee43743fe04baa485866af8460d31e476ad3f7648fd5"

	# COPYING: the MIT/X11 notices of the individual contributors ("MIT/Expat" in
	# upstream's meson.build). p_build installs it with the data.
	license="MIT"
	license_file="COPYING"

	conflicts=""
	depends=""

	# rootfs: also copy the staging tree (stage/, see p_build) into the image rootfs.
	iuse="rootfs"

	supports="phoenix>=3.3"
}

p_prepare() {
	local t
	for t in meson ninja python3 perl; do
		command -v "${t}" >/dev/null || b_die "xkeyboard_config: host tool ${t} not found"
	done
	python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' ||
		b_die "xkeyboard_config: host python3 is older than 3.11 (the rules generator needs it)"
}

# ${PREFIX_PORT_BUILD}/out/
#   build/      the native meson build
#   destdir/    upstream's install: usr/share/xkeyboard-config-2/ + the X11/xkb symlink
#   stage/      the files for the target rootfs (+ stage.MANIFEST next to it)
#
# The image gets a real directory /usr/share/X11/xkb (libxkbcommon's
# DFLT_XKB_CONFIG_ROOT, see wayland_phoenix), not upstream's versioned
# /usr/share/xkeyboard-config-2 behind a symlink: labwc_desktop also stages
# /usr/share/X11/xkb/keymap/us.xkb as a file, and a real directory does not depend on
# symlink fidelity through the ext2 packer, the NFS export and Phoenix's path lookup.
#
# Staged: rules/{evdev,base}, keycodes/, types/, compat/, symbols/ -- what a keymap
# compile from rules/model/layout reads. Not staged: geometry/ (libxkbcommon ignores
# geometry) and rules/*.xml, *.lst, xkb.dtd, xfree98, README (the layout registry for
# libxkbregistry, libxklavier and keyboard-settings dialogs, none of which the image
# has: wayland_phoenix builds libxkbcommon with -Denable-xkbregistry=false).
p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local bd="${out}/build" dd="${out}/destdir" ST="${out}/stage"
	local X="${dd}/usr/share/xkeyboard-config-2" XKB="${ST}/usr/share/X11/xkb"

	# data from sources in a second: always rebuilt, so the stage matches this recipe
	rm -rf "${bd}" "${dd}" "${ST}"
	mkdir -p "${out}"
	meson setup "${bd}" "${PREFIX_PORT_WORKDIR%/}" --prefix /usr --wrap-mode=nodownload -Dnls=false \
		>"${out}/setup.log" 2>&1 || { tail -30 "${out}/setup.log"; b_die "xkeyboard_config: meson setup failed"; }
	ninja -C "${bd}" >"${out}/ninja.log" 2>&1 || { tail -30 "${out}/ninja.log"; b_die "xkeyboard_config: build failed"; }
	DESTDIR="${dd}" meson install -C "${bd}" --no-rebuild >"${out}/install.log" 2>&1 ||
		{ tail -20 "${out}/install.log"; b_die "xkeyboard_config: install failed"; }

	local d f
	for d in rules keycodes types compat symbols; do
		[ -d "${X}/${d}" ] || b_die "xkeyboard_config: ${X}/${d} not installed"
	done
	mkdir -p "${XKB}/rules"
	for f in evdev base; do
		install -m 644 "${X}/rules/${f}" "${XKB}/rules/${f}"
	done
	cp -a "${X}/keycodes" "${X}/types" "${X}/compat" "${X}/symbols" "${XKB}/"
	install -D -m 644 "${PREFIX_PORT_WORKDIR%/}/COPYING" "${ST}/usr/share/licenses/xkeyboard_config/COPYING"

	# --- verification ---
	f="$(find "${ST}" -type l -print -quit)"
	[ -z "${f}" ] || b_die "xkeyboard_config: symlink in the stage: ${f}"
	for f in rules/evdev keycodes/evdev types/complete compat/complete symbols/us symbols/pc symbols/inet; do
		[ -s "${XKB}/${f}" ] || b_die "xkeyboard_config: ${f} not staged"
	done
	# the evdev rules resolve the default model and layout (pc105, us)
	grep -q 'pc105' "${XKB}/rules/evdev" || b_die "xkeyboard_config: rules/evdev has no pc105 model"

	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs sha256sum) >"${out}/stage.MANIFEST"
	echo "xkeyboard_config: staged $(wc -l <"${out}/stage.MANIFEST") file(s), $(du -sk --apparent-size "${ST}" | cut -f1) KiB"

	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		# a real directory replaces anything an earlier build left there (a symlink too)
		if [ -L "${PREFIX_FS}/root/usr/share/X11/xkb" ]; then
			rm -f "${PREFIX_FS}/root/usr/share/X11/xkb"
		fi
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "xkeyboard_config: staged into ${PREFIX_FS}/root/usr/share/X11/xkb"
	fi
}
