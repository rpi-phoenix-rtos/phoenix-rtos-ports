# xterm port (RPi4 X11 stack)

`xterm` is the standard X Window System terminal emulator. On the Raspberry Pi 4
Phoenix-RTOS port it is the first terminal emulator to run on the in-tree X11
stack (the `Xphoenix` kdrive fbdev DDX server), and it validates the full
keyboard path end-to-end: HID keyboard -> `usbkbd` -> `/dev/kbd0` -> X server ->
xterm -> shell on a pty (task #30).

## Status: structural recipe — not yet end-to-end buildable by `phoenix-rtos-ports`

This `port.def.sh` captures the complete, working build (patch + Phoenix source
shims + configure flags + static link closure). It is, however, **blocked from
running unattended through the normal ports build** by one dependency:

* **The X11 client/toolkit library stack is not (yet) a set of phoenix-rtos
  ports.** xterm links against `libXaw libXmu libXt libSM libICE libXext libXpm
  libXrender libX11 libxcb libXau libXdmcp`. On this port those are cross-built
  by hand into a prefix (`/tmp/x11-phoenix`) by the coordination repo script
  `tools/x11-port/build-x11-phoenix.sh`, not produced by `phoenix-rtos-ports`.
  Until that stack is itself ported (tracked as **task #12**), the recipe's
  `--x-includes`/`--x-libraries` must point at that external prefix
  (`XLIB_PREFIX`, default `/tmp/x11-phoenix`) and the ports build cannot resolve
  the `depends=` automatically.

The authoritative, reproducible build today is the coordination-repo wrapper
**`tools/x11-port/build-xterm.sh`** (idempotent: fetch/extract/patch/configure/
build/pre-flight/stage). This recipe mirrors that wrapper so the work is not
lost and so it can be promoted to a first-class port once the X11 libs land.

## What the Phoenix adaptation does

`patches/xterm-396-phoenix.patch` (canonical copy in the coordination repo at
`tools/x11-port/patches/`) touches `main.c`, `xterm_io.h`, `xtermcap.h`, all
keyed on the `__phoenix__` predefine:

* **pty**: `get_pty()` opens the SVR4 `/dev/ptmx` multiplexor (Phoenix posixsrv
  provides it), then `unlockpt()` + `ptsname()` to get the `/dev/pts/N` slave —
  instead of the BSD `/dev/ptyXX` search or `openpty()` (neither exists here).
* **termios**: define `USE_POSIX_TERMIOS` so `<termios.h>` (and, transitively,
  `struct winsize`) is included; without a matching platform branch xterm
  hits `#error Neither termio or termios is enabled`.
* **process groups**: define `USE_SYSV_PGRP` so xterm uses POSIX
  `setsid()`/`setpgrp(void)` and not the 2-arg BSD `setpgrp(pid,pgrp)` +
  `TIOCSPGRP`, which Phoenix does not provide.
* **shell**: `resetShell()` falls back to a compile-time `DEFSHELL_NAME` (guarded
  with `#ifndef`) instead of a hardcoded `/bin/sh`. See the netboot note below.

Three drop-in files (`files/`) cover libraries Phoenix lacks:

* `phoenix_termcap.[ch]` — Phoenix has no curses/termcap; these provide
  `tgetent()`/`tgetstr()` "no termcap database" stubs so xterm links and degrades
  gracefully (its keysym tables, not termcap, drive the keyboard).
* `files/include/wctype.h` — a minimal `<wctype.h>` (the `isw*`/`tow*` family
  over the narrow `<ctype.h>` for ASCII); libphoenix has none.
* `files/xterm-phoenix-fdset-shim.h` — force-included so Xlib's `Xpoll.h` finds
  `fd_mask` and the `__fds_bits` member alias on Phoenix's `fd_set`.

## libphoenix prerequisites

Two small libphoenix additions were made for this port (committed in the
`libphoenix` sibling repo):

* `setgrent`/`endgrent`/`getgrent` group-iteration stubs (xterm calls
  `endgrent()` after a `getgrnam("tty")` lookup).
* `wcslen()` (it was declared in `<wchar.h>` but never implemented; libXaw's
  Text widget needs it).

## Netboot launch + keyboard test

On the RPi4 netboot image the rootfs (including the shell) is mounted at
`/nfstest`, not `/`, so the wrapper builds with
`XTERM_DEFSHELL=/bin/sh`. Launch via the X session launcher:

```
/bin/startx term      # twm (window manager, for focus) + xterm
```

A window manager is required, not cosmetic: with no WM the server uses
PointerRoot focus, so keystrokes go to whatever window the pointer is over;
`twm` gives xterm click-to-focus so typed keys reliably reach the shell. In the
xterm window, type e.g. `ls /bin` and Enter to confirm input round-trips
through HID -> X -> pty -> shell.
