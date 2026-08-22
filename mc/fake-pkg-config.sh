#!/usr/bin/env bash
#
# Minimal fake pkg-config for cross-builds without a real pkg-config DB. Answers
# just enough for mc's configure (glib/gmodule queries): version checks succeed,
# --cflags/--libs return the values from the matching <MOD>_CFLAGS/<MOD>_LIBS env
# the caller already exported, and --variable=gmodule_supported returns "true".
#
# Copyright 2026 Phoenix Systems
# Author: Witold Bołt
# Only the modules we actually provide are "present". Anything else (ext2fs,
# libssh2, gpm, aspell, check, ...) must report ABSENT so mc disables that
# optional feature instead of trying to compile against missing headers.
have_module() {
	case "$1" in
		*glib-2.0*|*gmodule*|*gthread*|*gobject*) return 0 ;;
		*) return 1 ;;
	esac
}

# Pull the queried module name out of the args (last non-option token).
mod=""
for a in "$@"; do
	case "$a" in -*) ;; *) mod="$a" ;; esac
done

case "$*" in
	*--version)             echo "0.29.2"; exit 0 ;;
	*--variable=gmodule_supported*) echo "true"; exit 0 ;;
	*--variable=glib_genmarshal*)   echo "true"; exit 0 ;;
	*--variable=*)          echo ""; exit 0 ;;
esac

# Module presence / version / flags: succeed only for modules we provide.
case "$*" in
	*--exists*|*--atleast-version*|*--modversion*|*--cflags*|*--libs*)
		if have_module "$mod"; then
			case "$*" in
				*--modversion*) echo "2.56.4" ;;
				*--cflags*)     echo "${GLIB_CFLAGS:-}" ;;
				*--libs*)       echo "${GLIB_LIBS:-}" ;;
			esac
			exit 0
		else
			exit 1   # module not available
		fi
		;;
esac
exit 0
