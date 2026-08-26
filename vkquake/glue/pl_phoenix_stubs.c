/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for vkQuake (vkQuake is Copyright (C) id
 * Software, Inc. and the vkQuake developers, GPL-2.0-or-later). It implements
 * the vkQuake platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * pl_phoenix_stubs.c — small Phoenix shims for symbols vkQuake's excluded platform TUs
 * (net_bsd.c, pl_linux.c) used to provide. First-light: Loopback-only networking (so
 * single-player "map"/"newgame" via Loop_Connect works; no UDP/LAN — see the quakespasm
 * port's rationale below), and the Mesa clock helper libphoenix lacks.
 *
 * Adapted from quakespasm-port/platform/pl_phoenix_stubs.c. vkQuake's net_driver_t has a
 * different field set than quakespasm's (it adds QueryAddresses + QGetAnyMessage and
 * renames Get/SendMessage to QGet/QSendMessage), so we use DESIGNATED initializers keyed
 * to net_defs.h's field names — robust to the layout change.
 */
#include "quakedef.h"
typedef int sys_socket_t;       /* normally from net_sys.h (platform-gated) */
#include "net_defs.h"
#include "net_loop.h"

#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

/* --- network driver tables (were in net_bsd.c, which this port excludes). ---
 * Register ONLY the Loopback driver (single-player "map"/"newgame" via Loop_Connect).
 *
 * The Datagram + UDP LAN driver is intentionally NOT registered: enabling it makes
 * Quake's per-frame net poll service incoming UDP, and on the BCM2711 LAN + Phoenix lwIP
 * that path hit FIONREAD/ENOSYS faults and regressed NFS/rendering in the GL port. LAN
 * multiplayer needs a careful combined FIONREAD + NFS-safe fix (deferred). Loopback-only
 * keeps single-player solid. Loop_* live in net_loop.c (in the engine build). */
net_driver_t net_drivers[] = {
	{
		.name = "Loopback",
		.initialized = false,
		.Init = Loop_Init,
		.Listen = Loop_Listen,
		.QueryAddresses = Loop_QueryAddresses,     /* NULL macro in net_loop.h */
		.SearchForHosts = Loop_SearchForHosts,
		.Connect = Loop_Connect,
		.CheckNewConnections = Loop_CheckNewConnections,
		.QGetAnyMessage = Loop_GetAnyMessage,
		.QGetMessage = Loop_GetMessage,
		.QSendMessage = Loop_SendMessage,
		.SendUnreliableMessage = Loop_SendUnreliableMessage,
		.CanSendMessage = Loop_CanSendMessage,
		.CanSendUnreliableMessage = Loop_CanSendUnreliableMessage,
		.Close = Loop_Close,
		.Shutdown = Loop_Shutdown,
	},
};
const int net_numdrivers = countof(net_drivers);

/* No LAN drivers (UDP/BSD sockets excluded — see above). */
net_landriver_t net_landrivers[1];
const int       net_numlandrivers = 0;

/* --- Mesa clock helper libphoenix lacks (referenced by Mesa's thread utils) --- */
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id)
{
	(void)thread;
	if (clock_id != NULL)
		*clock_id = CLOCK_MONOTONIC;
	return 0;
}

/* --- Dead Mesa-runtime entrypoints pulled by --whole-archive on libv3dv-phoenix.a ---
 *
 * Whole-archiving the ICD (required so the vk_common_* dispatch fallbacks — reachable only
 * through WEAK relocs in the generated dispatch tables — are actually linked) also drags in
 * two runtime objects this build never reaches at runtime:
 *
 *   vk_texcompress_astc.c  -> the software ASTC-decode meta path. V3D 4.2 decodes ASTC in
 *                             hardware, so V3DV never registers/calls the emulated decoder;
 *                             its LUT helpers (_mesa_{init_astc_decoder_luts,
 *                             get_astc_decoder_partition_table}) live in src/util and are
 *                             not in the linked archives.
 *   vk_rmv_exporter.c      -> the Radeon Memory Visualizer trace dump (vk_dump_rmv_capture),
 *                             a debug-tooling path; its util_get_process_name dep is unused.
 *
 * Nothing references these objects' exported symbols (verified via nm + the prior link
 * reaching staging-buffer init without pulling them), so they are inert. We satisfy their
 * three dangling deps with TRAP stubs that Sys_Error (already linked) rather than spin: an
 * unexpected reach prints to UART instead of silently hanging (the orchestrator reads UART). */
void _mesa_init_astc_decoder_luts(void *holder)
{
	(void)holder;
	Sys_Error ("ASTC emulation path reached — unexpected on V3D 4.2 (HW ASTC)");
}

void *_mesa_get_astc_decoder_partition_table(uint32_t block_width, uint32_t block_height,
                                             unsigned *lut_width, unsigned *lut_height)
{
	(void)block_width; (void)block_height; (void)lut_width; (void)lut_height;
	Sys_Error ("ASTC emulation path reached — unexpected on V3D 4.2 (HW ASTC)");
	return NULL;
}

const char *util_get_process_name(void)
{
	return "vkquake-phoenix";
}
