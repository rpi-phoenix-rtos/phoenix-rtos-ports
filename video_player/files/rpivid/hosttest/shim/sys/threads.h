/* rpivid host test shim (SPDX-License-Identifier: BSD-3-Clause) */
#ifndef RPIVID_SHIM_THREADS_H
#define RPIVID_SHIM_THREADS_H
#include <time.h>
#include "phoenix_shim.h"
int mutexCreate(handle_t *h);
int mutexLock(handle_t h);
int mutexUnlock(handle_t h);
int condCreate(handle_t *h);
int condWait(handle_t h, handle_t m, time_t timeout);
int condSignal(handle_t h);
int resourceDestroy(handle_t h);
#endif
