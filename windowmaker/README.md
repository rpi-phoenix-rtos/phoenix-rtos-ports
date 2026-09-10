# Window Maker (windowmaker) — RPi4 X11 port (DRAFT recipe)

Window Maker 0.95.9, the NeXTSTEP-style X11 window manager. This is the
heaviest window manager ported to the Phoenix-RTOS Raspberry Pi 4 X11 stack
(after twm and JWM).

## Status

`port.def.sh` here is a **draft**. It cannot yet be driven end-to-end by the
phoenix-rtos-ports build, because Window Maker's dependencies are not yet
phoenix-rtos ports themselves:

1. **The X11 client/toolkit lib stack** — `libX11`, `libxcb`, `libXt`,
   `libXmu`, `libXpm`, `libXext`, `libXrender`, `libSM`, `libICE`,
   `libfreetype`, `libz`, … Same blocker as the `xterm` port (feeds task #12).
   On the RPi4 port these are cross-built into `/tmp/x11-phoenix` by the
   coordination repo's `tools/x11-port/build-x11-phoenix.sh`.

2. **The antialiased-font stack** — `expat`, `fontconfig`, `libXft`. Unlike
   twm/JWM/xterm (which use core bitmap fonts), Window Maker requires Xft +
   fontconfig. The coordination repo builds these into `/tmp/wmaker-deps`
   (`tools/x11-port/build-wmaker.sh`), which also snapshots the X11 closure from
   (1) so a single prefix satisfies both.

The **canonical, working build** is therefore
`tools/x11-port/build-wmaker.sh` in the coordination repo, which this recipe
mirrors. Once the lib stacks land as proper ports (so `depends=` can be
satisfied), wire `WMAKER_DEPS` at the prefix the ports build installs them into
and this recipe should drive the same compile.

## Dependencies on libphoenix

The build needed several libphoenix additions / gap-fills:

| Symbol / feature              | Where used            | Fix                                                                 |
|-------------------------------|-----------------------|---------------------------------------------------------------------|
| `_SC_LINE_MAX` (sysconf)      | WINGs/error.c         | **committed to libphoenix** (sysconf returns `_POSIX2_LINE_MAX`)    |
| `nftw()` / `ftw()`            | WINGs/proplist.c      | gap-fill lib (no `<ftw.h>` in libphoenix)                           |
| `scandir()` / `alphasort()`   | util/wmiv, wmgenmenu  | gap-fill lib (absent from libphoenix `<dirent.h>`)                  |
| `nice()`                      | util/wmsetbg          | **libphoenix provides it** (`0961476`, a SCHED_RR no-op) — do NOT stub it |
| `rint()`                      | wcolorpanel, wbrowser | none needed — libphoenix libm implements `rint`/`rintf` (2026-09-04) |

The gap-fill lib sources live **in this port**, at
`files/ftw-phoenix/` (`ftw.c`, `ftw.h`, `wmaker-phoenix-compat.h`) — they used to
be in the coordination repo under `tools/x11-port/`, and that copy is gone.

⚠ **Do not re-add a `nice()` stub here.** libphoenix implements `nice()` as of
`0961476` and declares it in `<unistd.h>`, so a second definition is a hard link
error and it takes the whole X11 showcase down:

    ld: libm.a(sys.o): multiple definition of `nice';
        libftw.a(ftw.o): first defined here
    FAILED: windowmaker: build failed

That was latent from the day libphoenix gained the function until a rebuild
forced a wmaker relink (fixed 2026-09-10). Before adding any shim here, check
whether libphoenix has grown the real function.

Of the remaining gaps, `ftw.h`/`nftw` and `scandir`/`alphasort` are still genuinely
absent and are the ones worth filling upstream.

`fontconfig` itself (a build dependency) also needed two Phoenix source fixes
(non-standard `timercmp()` macro; non-constant static initializer in
`FcRandom`) — see `tools/x11-port/build-wmaker.sh`.

Host build dependency: **gperf** (fontconfig's `fcobjshash.h` codegen).

## Runtime staging (netboot, NFS rootfs at /mnt)

The coordination repo stages onto the NFS export `/srv/phoenix-rpi4-nfs`:

- `bin/wmaker` + the util helpers (`wmsetbg`, `wdwrite`, …)
- the data tree: `share/WindowMaker`, `share/WINGs`, `share/WPrefs`,
  `etc/WindowMaker` (global defaults + root menu)
- a self-contained `etc/fonts/fonts.conf` mapping generic family names
  (`sans serif`, `Sans`, …) to the bundled DejaVu family
- DejaVu TTFs under `usr/share/fonts/truetype/dejavu`
- the fontconfig cache dir `var/cache/fontconfig`

Without a real TTF on the target and the alias mappings, `XftFontOpenName()`
returns NULL and wmaker fails to start — staging the font + `fonts.conf` is part
of the deliverable, not a runtime detail.

## Launch

`wmaker` runs menu/`<exec>` commands via `WMAKER_SHELL` (compiled in as
`/bin/sh`). It finds its background helper via `execlp("wmsetbg", …)`
and dock/menu apps via `execvp`, so **`PATH` must include `/bin`**, and
it computes `~/GNUstep` from `$HOME`, so **`HOME` should be set** to a writable
dir (it degrades to `/` with warnings if unset). On the netboot Pi:

```
HOME=/root PATH=/bin:$PATH /bin/startx wmaker
```
