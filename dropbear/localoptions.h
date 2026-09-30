#ifndef DROPBEAR_LOCALOPTIONS_H
#define DROPBEAR_LOCALOPTIONS_H

/*
 * Phoenix-RTOS build options. Everything not set here keeps the upstream
 * default from src/default_options.h, which already enables ed25519 host and
 * user keys and the curve25519, sntrup761x25519 and mlkem768x25519 key
 * exchanges that current OpenSSH clients prefer.
 */

/*
 * libphoenix has no O_NOFOLLOW. Three of its four uses here are combined with
 * O_CREAT | O_EXCL, which already refuses an existing path; the fourth is scp
 * writing a received file. Build without the flag until libphoenix gains it.
 */
#include <fcntl.h>
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/*
 * Host keys live in /local, the writable persistent directory on every
 * Phoenix-RTOS target (/etc is read-only on the flash-based ones). With -R the
 * server creates the key a client asks for on first use.
 */
#define RSA_PRIV_FILENAME     "/local/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME   "/local/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME "/local/dropbear_ed25519_host_key"

/*
 * Dropping privileges after authentication needs setresuid()/setresgid(),
 * which libphoenix does not provide. Unix-socket forwarding is only safe with
 * that privilege drop, so it is disabled along with it.
 */
#define DROPBEAR_SVR_DROP_PRIVS 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0

#endif
