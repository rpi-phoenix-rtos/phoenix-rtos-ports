# SDL2 Phoenix GL-context glue

`sdl_phoenix_glctx.c` is the GL-context bring-up for the Phoenix SDL2 video
driver: it creates the V3D `pipe_screen`, the Mesa GL state-tracker context and
the scanout-backed FBO(s), and provides the page-flip present. The SDL video
driver (`src/video/phoenix/`, zlib) calls its plain-C entry points
(`qsv3d_init` / `qsv3d_make_current` / `qsv3d_resolve` / `qsv3d_bind_fbo`) as
externs and does **not** compile this file.

## Why it is separate from libSDL2.a

This TU needs Mesa-internal headers (`pipe/*`, `state_tracker/st_context.h`,
`main/mtypes.h`) and a set of Mesa-specific compile flags (endianness defines,
`-include` compat header, many `-I` paths into the Mesa tree). The SDL cmake
cross build never provides those, so this file is compiled by the **game's**
build (the same way `tools/quakespasm-port/build-quakespasm-phoenix.py` compiles
`pl_phoenix_glctx.c`) and linked alongside `libSDL2.a`, `libv3d-phoenix.a` and
`libGL-phoenix.a`.

## LICENSING FLAG (unresolved — for the maintainer to decide)

This file is a **verbatim copy** of
`tools/quakespasm-port/platform/pl_phoenix_glctx.c`, which carries a
**GPL-2.0-or-later** header (it is part of the QuakeSpasm port). SDL2 itself is
**zlib**, and `libSDL2.a` stays pure zlib because this file is never compiled
into it — the driver only references its symbols, which is not a derivative
work. But any game that links this glue pulls GPL-2.0-or-later code into its
final binary. That is fine for GPL games (Quake2/Quake3/ioq3), but for a
permissively-licensed SDL2 game it is a licensing consideration. If a
non-GPL glue is required, the Mesa-boilerplate recipe here can be
re-expressed under zlib independently of the QuakeSpasm file. This is FLAGGED,
not resolved.
