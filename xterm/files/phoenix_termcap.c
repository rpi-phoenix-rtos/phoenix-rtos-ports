/*
 * Phoenix-RTOS — minimal termcap shim for xterm (implementation).
 *
 * See phoenix_termcap.h for rationale. Every entry behaves as "no termcap
 * database present": tgetent() reports not-found, lookups return empty/NULL.
 * xterm degrades gracefully (skips the cosmetic $TERMCAP sync and uses its
 * built-in defaults for the erase key and function keys).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#include "phoenix_termcap.h"

#include <stddef.h>

/* BSD termcap globals xterm/curses code may reference. */
char PC = 0;
char *UP = NULL;
char *BC = NULL;
short ospeed = 0;

int
tgetent(char *bp, const char *name)
{
	(void)name;
	if (bp != NULL)
		bp[0] = '\0';
	return 0; /* 0 == no such entry (xterm treats <=0 as "no termcap") */
}

int
tgetnum(const char *id)
{
	(void)id;
	return -1;
}

int
tgetflag(const char *id)
{
	(void)id;
	return 0;
}

char *
tgetstr(const char *id, char **area)
{
	(void)id;
	(void)area;
	return NULL;
}

char *
tgoto(const char *cap, int col, int row)
{
	(void)col;
	(void)row;
	/* Return the capability verbatim; callers only use this when tgetstr
	 * returned a real cap, which never happens with this stub. */
	return (char *)cap;
}

int
tputs(const char *str, int affcnt, int (*putc_fn)(int))
{
	(void)affcnt;
	if (str != NULL && putc_fn != NULL) {
		while (*str != '\0')
			putc_fn((int)*str++);
	}
	return 0;
}
