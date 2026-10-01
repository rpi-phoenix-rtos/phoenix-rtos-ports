/* rpivid host test shim (SPDX-License-Identifier: BSD-3-Clause) */
#ifndef RPIVID_SHIM_LIBVCMBOX_H
#define RPIVID_SHIM_LIBVCMBOX_H
#include <stdint.h>
int vcmbox_call(uint32_t tag, uint32_t valBufSize, const uint32_t *in, uint32_t nIn, uint32_t *out, uint32_t nOut);
#endif
