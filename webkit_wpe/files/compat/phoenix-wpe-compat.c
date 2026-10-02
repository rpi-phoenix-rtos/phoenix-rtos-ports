/*
 * Phoenix-RTOS browser track D (WPE WebKit) -- local compat, not part of libphoenix.
 *
 * libphoenix functions WPE needs and libphoenix lacks, beyond track C's set
 * (tools/browser/jsc/compat). Each definition is WEAK, so a libphoenix that gains the function
 * wins without a change here; delete the entry then.
 *
 *   nextafterf()  libm has nextafter() but not the float variant (WebCore layout/rendering,
 *                 LayoutUnit/FloatRect edge handling). TODO(browser-B1): add to libphoenix libm.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <string.h>

__attribute__((weak)) float nextafterf(float x, float y)
{
	uint32_t ux, uy;

	if (x != x || y != y) {
		return x + y; /* NaN */
	}
	if (x == y) {
		return y;
	}
	memcpy(&ux, &x, sizeof(ux));
	memcpy(&uy, &y, sizeof(uy));
	if ((ux & 0x7fffffffU) == 0U) {
		ux = (uy & 0x80000000U) | 1U; /* smallest subnormal towards y */
	}
	else if ((x < y) == ((ux & 0x80000000U) == 0U)) {
		ux++; /* away from zero */
	}
	else {
		ux--; /* towards zero */
	}
	memcpy(&x, &ux, sizeof(x));
	return x;
}
