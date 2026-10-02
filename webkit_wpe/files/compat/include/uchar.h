/*
 * Phoenix-RTOS browser track C (JavaScriptCore) -- local compat, not part of libphoenix.
 *
 * C11 <uchar.h>, types only. TODO(browser-B1): libphoenix has no <uchar.h>. Without one, the
 * C API header bundled in WTF's simdutf (wtf/simdutf/simdutf_impl.cpp.h) does
 * `#define char16_t uint16_t`, which breaks the C++ keyword for the rest of that translation unit.
 * No conversion functions (mbrtoc16() and friends): nothing in JSC calls them.
 * (tools/gpu-lane/labwc-drm/compat has a fuller one with mbrtoc32/c32rtomb.)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PHOENIX_JSC_COMPAT_UCHAR_H
#define PHOENIX_JSC_COMPAT_UCHAR_H

#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

#endif
