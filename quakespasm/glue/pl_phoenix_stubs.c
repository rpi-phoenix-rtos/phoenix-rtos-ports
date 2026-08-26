/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for QuakeSpasm (QuakeSpasm is Copyright (C) id
 * Software, Inc. and the QuakeSpasm developers, GPL-2.0-or-later). It implements
 * the QuakeSpasm platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * pl_phoenix_stubs.c — small Phoenix shims for symbols Quakespasm's excluded
 * platform TUs (net_bsd.c, sys_sdl_unix.c, pl_*.c) used to provide. First-light:
 * no networking (empty driver tables -> single-player local play still works via
 * the loopback path registered by net_main when present; expand if a demo needs
 * net_loop), no clipboard, no console input.
 */
#include "quakedef.h"
typedef int sys_socket_t;       /* normally from net_sys.h (platform-gated) */
#include "net_defs.h"
#include "net_loop.h"
#include "net_dgrm.h"
#include "net_udp.h"

#include <time.h>
#include <pthread.h>

/* --- network driver tables (were in net_bsd.c, which this port excludes). ---
 * Register the Loopback driver (single-player "map"/"newgame" via Loop_Connect) AND
 * the Datagram net driver over the UDP LAN driver, so LAN multiplayer works (task #26).
 *
 * Earlier this was Loopback-only: enabling the UDP landriver hung the single-player
 * flagship because Quake's per-frame net poll services incoming UDP, and stray LAN
 * broadcast traffic on the BCM2711 made UDP_CheckNewConnections call ioctl(FIONREAD),
 * which the Pi's lwIP build leaves unimplemented (LWIP_SO_RCVBUF==0) -> the syscall
 * errored and upstream turned that into a fatal Sys_Error. That FIONREAD dependency is
 * now removed app-side (net_udp.c UDP_CheckNewConnections peeks the non-blocking socket
 * with MSG_PEEK instead) -- no lwIP/kernel change, so the working NFS/render path is
 * untouched. Loop_* live in net_loop.c, Datagram_* in net_dgrm.c, UDP_* in net_udp.c
 * (all CORE). */
net_driver_t net_drivers[] =
{
	{	"Loopback",
		false,
		Loop_Init,
		Loop_Listen,
		Loop_SearchForHosts,
		Loop_Connect,
		Loop_CheckNewConnections,
		Loop_GetMessage,
		Loop_SendMessage,
		Loop_SendUnreliableMessage,
		Loop_CanSendMessage,
		Loop_CanSendUnreliableMessage,
		Loop_Close,
		Loop_Shutdown
	},

	{	"Datagram",
		false,
		Datagram_Init,
		Datagram_Listen,
		Datagram_SearchForHosts,
		Datagram_Connect,
		Datagram_CheckNewConnections,
		Datagram_GetMessage,
		Datagram_SendMessage,
		Datagram_SendUnreliableMessage,
		Datagram_CanSendMessage,
		Datagram_CanSendUnreliableMessage,
		Datagram_Close,
		Datagram_Shutdown
	}
};
const int      net_numdrivers = Q_COUNTOF(net_drivers);

net_landriver_t net_landrivers[] =
{
	{	"UDP",
		false,
		0,
		UDP_Init,
		UDP_Shutdown,
		UDP_Listen,
		UDP_OpenSocket,
		UDP_CloseSocket,
		UDP_Connect,
		UDP_CheckNewConnections,
		UDP_Read,
		UDP_Write,
		UDP_Broadcast,
		UDP_AddrToString,
		UDP_StringToAddr,
		UDP_GetSocketAddr,
		UDP_GetNameFromAddr,
		UDP_GetAddrFromName,
		UDP_AddrCompare,
		UDP_GetSocketPort,
		UDP_SetSocketPort
	}
};
const int      net_numlandrivers = Q_COUNTOF(net_landrivers);

/* --- throttle cvar (was in main_sdl.c; sys_ticrate is owned by host.c) --- */
cvar_t sys_throttle = { "sys_throttle", "0.02", CVAR_ARCHIVE };

/* --- platform odds & ends --- */
char *PL_GetClipboardData(void)
{
	return NULL;
}

const char *Sys_ConsoleInput(void)
{
	return NULL;
}

/* Phoenix libc lacks pthread_getcpuclockid (referenced by Mesa's thread utils);
 * a monotonic-clock stand-in is fine for Mesa's timing. */
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id)
{
	(void)thread;
	if (clock_id)
		*clock_id = CLOCK_MONOTONIC;
	return 0;
}
