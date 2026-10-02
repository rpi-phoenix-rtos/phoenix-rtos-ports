/*
 * Phoenix-RTOS browser track C (JavaScriptCore) -- local compat, not part of libphoenix.
 *
 * <stdint.h> with UINT8_MAX and UINT16_MAX of type int. TODO(browser-B1): libphoenix defines them
 * as (0xffU) and (0xffffU), i.e. unsigned int, but C11 7.20.2 requires "the same type as would an
 * expression that is an object of the corresponding type converted according to the integer
 * promotions": int (as glibc/musl/newlib define them). The unsigned type silently turns signed
 * comparisons into unsigned ones -- `int16_t t = -5; if (t > UINT8_MAX) ...` is TRUE -- which is
 * exactly the shape of WTF's SIMDe saturation code and JSC's PropertyTable::canFitInCompact()
 * (`offset <= UINT8_MAX`, offsets can be -1). GCC 16 flags 397 such comparisons in this build.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PHOENIX_JSC_COMPAT_STDINT_H
#define PHOENIX_JSC_COMPAT_STDINT_H

#include_next <stdint.h>

#undef UINT8_MAX
#undef UINT16_MAX
#define UINT8_MAX  (255)
#define UINT16_MAX (65535)

#endif
