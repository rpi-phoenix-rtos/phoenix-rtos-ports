/*
 * Phoenix-RTOS
 *
 * Xorg-drm: the builtin-module table (xorg-server patch 0002, XORG_BUILTIN_MODULES)
 *
 * NEW GPU LANE, M4 (docs/gpu-new-lane/M4-xorg-modesetting.md). Phoenix programs are
 * statically linked, so the modules Xorg would dlopen() are linked into Xorg-drm and
 * listed here. LoadModule() finds a module by its canonical name and runs its
 * ModuleData exactly as for a loaded one; LoaderSymbolFromModule() resolves the names
 * listed with it (the ones its users look up), LoaderSymbol() searches all lists.
 *
 * The symbol lists mirror the LoaderSymbolFromModule() calls of the modesetting driver
 * (hw/xfree86/drivers/modesetting/driver.c, 21.1.24): a name missing here is logged by
 * the loader as "Builtin module <m>: no symbol <s> in its table" -- keep them in step
 * when rebasing the server.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifdef HAVE_XORG_CONFIG_H
#include <xorg-config.h>
#endif

#include <stddef.h>

#include "xf86Module.h"
#include "loader_builtin.h"

/* <name>ModuleData of each linked module */
extern XF86ModuleData modesettingModuleData;
extern XF86ModuleData glamoreglModuleData;
extern XF86ModuleData shadowModuleData;
extern XF86ModuleData phxhidModuleData;

/* glamoregl: what modesetting's bind_glamor_api() resolves */
extern void glamor_back_pixmap_from_fd(void);
extern void glamor_block_handler(void);
extern void glamor_clear_pixmap(void);
extern void glamor_egl_create_textured_pixmap(void);
extern void glamor_egl_create_textured_pixmap_from_gbm_bo(void);
extern void glamor_egl_exchange_buffers(void);
extern void glamor_egl_get_gbm_device(void);
extern void glamor_egl_init(void);
extern void glamor_finish(void);
extern void glamor_gbm_bo_from_pixmap(void);
extern void glamor_init(void);
extern void glamor_name_from_pixmap(void);
extern void glamor_set_drawable_modifiers_func(void);
extern void glamor_shareable_fd_from_pixmap(void);
extern void glamor_supports_pixmap_import_export(void);
extern void glamor_egl_get_driver_name(void);
extern void glamor_xv_init(void);

/* shadow: what modesetting's PreInit resolves when ShadowFB is used (no glamor) */
extern void shadowSetup(void);
extern void shadowAdd(void);
extern void shadowRemove(void);
extern void shadowUpdate32to24(void);
extern void shadowUpdatePacked(void);

/* server symbols drivers probe for with xf86LoaderCheckSymbol() */
extern void DRI2Version(void);

#define SYM(s) { #s, (void *) s }

static const XF86BuiltinSymbol glamoregl_symbols[] = {
	SYM(glamor_back_pixmap_from_fd),
	SYM(glamor_block_handler),
	SYM(glamor_clear_pixmap),
	SYM(glamor_egl_create_textured_pixmap),
	SYM(glamor_egl_create_textured_pixmap_from_gbm_bo),
	SYM(glamor_egl_exchange_buffers),
	SYM(glamor_egl_get_gbm_device),
	SYM(glamor_egl_init),
	SYM(glamor_finish),
	SYM(glamor_gbm_bo_from_pixmap),
	SYM(glamor_init),
	SYM(glamor_name_from_pixmap),
	SYM(glamor_set_drawable_modifiers_func),
	SYM(glamor_shareable_fd_from_pixmap),
	SYM(glamor_supports_pixmap_import_export),
	SYM(glamor_egl_get_driver_name),
	SYM(glamor_xv_init),
	{ NULL, NULL }
};

static const XF86BuiltinSymbol shadow_symbols[] = {
	SYM(shadowSetup),
	SYM(shadowAdd),
	SYM(shadowRemove),
	SYM(shadowUpdate32to24),
	SYM(shadowUpdatePacked),
	{ NULL, NULL }
};

static const XF86BuiltinSymbol modesetting_symbols[] = {
	SYM(DRI2Version),   /* dri2.c: xf86LoaderCheckSymbol("DRI2Version") */
	{ NULL, NULL }
};

const XF86BuiltinModule xf86BuiltinModules[] = {
	{ "modesetting", &modesettingModuleData, modesetting_symbols },
	{ "glamoregl", &glamoreglModuleData, glamoregl_symbols },
	{ "shadow", &shadowModuleData, shadow_symbols },
	{ "phxhid", &phxhidModuleData, NULL },
	{ NULL, NULL, NULL }
};
