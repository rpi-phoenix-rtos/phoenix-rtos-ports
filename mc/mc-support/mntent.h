/*
 * Phoenix-RTOS — minimal <mntent.h> for Midnight Commander's mountlist.c.
 *
 * Phoenix ships an EMPTY <mntent.h> placeholder (no struct, no prototypes). This
 * supplies the glibc-compatible struct mntent + the getmntent/setmntent/endmntent
 * prototypes; the implementations (mntent-stub.c) report no mounts.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef _PHOENIX_MNTENT_H
#define _PHOENIX_MNTENT_H

#include <stdio.h>

#define MOUNTED      "/etc/mtab"
#define MNTTAB       "/etc/fstab"

#define MNTTYPE_IGNORE "ignore"
#define MNTTYPE_NFS    "nfs"
#define MNTTYPE_SWAP   "swap"

#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO       "ro"
#define MNTOPT_RW       "rw"
#define MNTOPT_SUID     "suid"
#define MNTOPT_NOSUID   "nosuid"
#define MNTOPT_NOAUTO   "noauto"

#ifdef __cplusplus
extern "C" {
#endif

struct mntent {
	char *mnt_fsname; /* device or server for filesystem */
	char *mnt_dir;    /* directory mounted on */
	char *mnt_type;   /* type of filesystem */
	char *mnt_opts;   /* comma-separated options */
	int   mnt_freq;   /* dump frequency (in days) */
	int   mnt_passno; /* pass number on parallel fsck */
};

FILE *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
int endmntent(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif /* _PHOENIX_MNTENT_H */
