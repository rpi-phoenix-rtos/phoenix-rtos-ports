# bash — GNU Bourne-Again SHell (5.2.21) for Phoenix-RTOS

A full GNU bash, beyond busybox's ash. Statically linked; verified on aarch64 (RPi4):
`bash --version`, script execution (`bash script.sh`), `for` loops, `export`/variable
read-back, environment propagation to child processes, `fork`/`exec` of external
binaries, and arithmetic all work on hardware with zero faults.

## libphoenix dependency (REQUIRED)

bash exercises POSIX/wide-character surface that older libphoenix lacked. It will
**not build or run** without these libphoenix fixes:

- `_POSIX_VERSION` / `_POSIX2_VERSION` in `<unistd.h>` — else `union wait` picks the
  wrong `WAIT` typedef (posixwait.h) and bash fails to compile.
- `__THROW` self-sufficiency (`<stdio_ext.h>` includes `<sys/cdefs.h>`).
- `<wctype.h>` family + `mbrlen`/`wcwidth`/`wcswidth`/`wcscoll`/`wctob`/`wmemchr`/
  `wcsdup` — bash requires `HANDLE_MULTIBYTE`.
- **crt0 passes `envp` as main's third argument** — bash reads `main(argc, argv, env)`
  as `shell_environment`; without this it faults at startup walking a garbage array.

## Patches

- `0001-posix-signal-select-includes.patch` — readline/lib_sh include `<signal.h>` /
  `<sys/select.h>` (Phoenix has POSIX signals + `select`, not `pselect`); input.c also
  declares its `readfds`/`sigset_t` under `HAVE_SELECT`, not only `HAVE_PSELECT`.
- `0002-termcap-tparam-include-unistd.patch` — `<unistd.h>` for `write`.
- `0003-tmpfile-prefer-mkstemp.patch` — Phoenix lacks the deprecated `mktemp`; use the
  `mkstemp` path.

## Build configuration (`config.cache`)

Cross-compiling, autoconf cannot run target binaries, so `config.cache` supplies the
run-test answers. It also forces the HAVE_* values that make bash use libphoenix's
libc functions instead of its own `lib/sh` fallbacks (otherwise the static link hits
multiple-definition): `bash_cv_getenv_redef=no`, `ac_cv_func_strtoimax=yes`,
`ac_cv_func_wcswidth=yes`. `-fcommon` handles termcap's tentative-definition globals
(PC/UP/BC) under gcc's `-fno-common` default.

## Known limitation — interactive shell

`bash -i` starts and prints its prompt, but psh does not hand its console tty to a
spawned child, so bash gets immediate EOF on stdin and exits. Non-interactive use
(scripts, `bash -c`) works. An interactive shell needs a getty-style bridge that
allocates a pty (`/dev/ptmx` → `/dev/pts/N`, as xterm does) and runs bash on the
slave — tracked separately, outside this port.
