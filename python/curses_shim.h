/*
 * Phoenix-RTOS — force-include shim for building CPython's _curses against the
 * NARROW ncurses port (libncurses.a).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * The Phoenix cross-build's pyconfig.h falsely reports HAVE_NCURSESW=1 (a bad
 * configure probe — the target has no ncursesw at all; the ncurses port builds
 * only the narrow libncurses.a), so _cursesmodule.c would compile the wide-char
 * paths (setcchar/wadd_wch/wget_wch/…) that narrow libncurses.a does not
 * provide. A command-line -UHAVE_NCURSESW cannot fix this: pyconfig.h
 * re-#defines the macro every time Python.h is included.
 *
 * Compile with `-include curses_shim.h`: this pulls in Python.h (and thus
 * pyconfig.h, with its include guard) FIRST, then undefs the false macro. The
 * module's own later `#include "Python.h"` is a guarded no-op, so the undef
 * sticks and _cursesmodule.c's `#ifdef HAVE_NCURSESW` guards take the narrow
 * path. Pair with -DHAVE_NCURSES_H so py_curses.h includes <ncurses.h>
 * (pyconfig.h leaves HAVE_NCURSES_H unset, but only as a comment, so a
 * command-line define persists).
 *
 * Order matters twice over: this shim must come AFTER the port's own
 * phoenix-py-compat.h on the command line (that one has to be the first thing
 * every CPython TU sees), and the #undef must come after the #include, not
 * before it.
 */
#ifndef PHOENIX_PY_CURSES_SHIM_H
#define PHOENIX_PY_CURSES_SHIM_H

#include <Python.h>
#undef HAVE_NCURSESW

#endif /* PHOENIX_PY_CURSES_SHIM_H */
