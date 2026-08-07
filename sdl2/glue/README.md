# SDL2 Phoenix GL-context glue

`sdl_phoenix_glctx.c` is the GL-context bring-up for the Phoenix SDL2 video
driver: it creates the V3D `pipe_screen`, the Mesa GL state-tracker context and
the scanout-backed FBO(s), and provides the page-flip present. The SDL video
driver (`src/video/phoenix/`, zlib) calls its plain-C entry points
(`phxgl_init` / `phxgl_make_current` / `phxgl_resolve` / `phxgl_bind_fbo`) as
externs and does **not** compile this file.

## Why it is separate from libSDL2.a

This TU needs Mesa-internal headers (`pipe/*`, `state_tracker/st_context.h`,
`main/mtypes.h`) and a set of Mesa-specific compile flags (endianness defines,
`-include` compat header, many `-I` paths into the Mesa tree). The SDL cmake
cross build never provides those, so this file is compiled by the **client's**
build (with the Mesa flags) and linked alongside `libSDL2.a`,
`libv3d-phoenix.a` and `libGL-phoenix.a`.

## Licensing

This file is **zlib-licensed** (see its SPDX header) and self-contained: it
depends only on Mesa headers and the plain-C entry points named above. Both this
glue and `libSDL2.a` are pure zlib, so it is safe to compile and link into any
client regardless of the client's own license. (The prior GPL-2.0-or-later
caveat, from when this glue was copied from a GPL port, is resolved: the file has
been re-expressed under zlib and no longer carries any GPL attribution.)
