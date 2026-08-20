/* Link-closure de-risk stub: all DDX hooks empty. Verifies the kdrive core
 * archive set links into an ELF before the real fbdev backend is written.
 *
 * Copyright 2026 Phoenix Systems  %LICENSE%
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif
#include "kdrive.h"

KdCardFuncs fbdevFuncs;

void InitCard(char *name) { }
void InitOutput(ScreenInfo *pScreenInfo, int argc, char **argv) { KdInitOutput(pScreenInfo, argc, argv); }
void InitInput(int argc, char **argv) { KdInitInput(); }
void CloseInput(void) { KdCloseInput(); }
void ddxUseMsg(void) { KdUseMsg(); }
int ddxProcessArgument(int argc, char **argv, int i) { return KdProcessArgument(argc, argv, i); }
void OsVendorInit(void) { }
#if INPUTTHREAD
void ddxInputThreadInit(void) { }
#endif
