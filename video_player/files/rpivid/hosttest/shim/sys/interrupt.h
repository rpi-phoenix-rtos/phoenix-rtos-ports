/* rpivid host test shim (SPDX-License-Identifier: BSD-3-Clause) */
#ifndef RPIVID_SHIM_INTERRUPT_H
#define RPIVID_SHIM_INTERRUPT_H
#include "phoenix_shim.h"
int interrupt(unsigned int n, int (*f)(unsigned int, void *), void *arg, handle_t queue, handle_t *handle);
#endif
