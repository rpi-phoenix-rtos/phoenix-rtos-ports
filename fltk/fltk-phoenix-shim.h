/*
 * Phoenix-RTOS — force-included compatibility shim for cross-building FLTK 1.3.x.
 *
 * Phoenix libm/libphoenix provides round()/roundf() but NOT the C99 rint()/
 * rintf() family. FLTK's drawing code (Fl_Chart, Fl_Arc, the vertex/transform
 * helpers) calls rint() in ~29 places for pixel-coordinate rounding. rint()
 * rounds to nearest using the current FPU rounding mode (default: round-half-to-
 * even); round() rounds half-away-from-zero. For integer pixel coordinates the
 * one-ULP difference at exact .5 boundaries is visually irrelevant, so we alias
 * rint -> round here rather than add a new function to libphoenix (which would
 * require a full toolchain/sysroot rebuild).
 *
 * Included via -include on the FLTK CXXFLAGS (see build-fltk.sh).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef FLTK_PHOENIX_SHIM_H
#define FLTK_PHOENIX_SHIM_H

#include <math.h>

#ifndef rint
#define rint(x) round(x)
#endif
#ifndef rintf
#define rintf(x) roundf(x)
#endif

#endif /* FLTK_PHOENIX_SHIM_H */
