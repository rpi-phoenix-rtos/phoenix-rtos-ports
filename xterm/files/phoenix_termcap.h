/*
 * Phoenix-RTOS — minimal termcap shim for xterm.
 *
 * Phoenix has neither a termcap nor an (n)curses library, so xterm's
 * unconditional `#include <curses.h>` (xtermcap.h) and its handful of
 * tgetent()/tgetstr() calls cannot be satisfied. xterm only consults the
 * termcap database for two optional niceties:
 *   - get_termcap()/set_termcap(): keep $TERMCAP in sync (cosmetic),
 *   - get_tcap_erase() (OPT_INITIAL_ERASE): learn the kb (backspace) capability.
 * Its actual key handling is driven by compiled-in keysym tables, not termcap,
 * and OPT_TCAP_FKEYS / OPT_TCAP_QUERY are disabled in this build. So a stub
 * that reports "no termcap entry" (tgetent -> 0, tgetstr -> NULL) lets xterm
 * link and run; it simply skips the $TERMCAP update and uses its default erase.
 *
 * These are declared (not defined) here; the bodies live in phoenix_termcap.c,
 * which the build adds to xterm's object list. Signatures match BSD <termcap.h>
 * so xtermcap.c's existing prototypes/calls compile unchanged.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef PHOENIX_TERMCAP_H
#define PHOENIX_TERMCAP_H

#ifdef __cplusplus
extern "C" {
#endif

extern char PC;
extern char *UP;
extern char *BC;
extern short ospeed;

extern int tgetent(char *bp, const char *name);
extern int tgetnum(const char *id);
extern int tgetflag(const char *id);
extern char *tgetstr(const char *id, char **area);
extern char *tgoto(const char *cap, int col, int row);
extern int tputs(const char *str, int affcnt, int (*putc_fn)(int));

#ifdef __cplusplus
}
#endif

#endif /* PHOENIX_TERMCAP_H */
