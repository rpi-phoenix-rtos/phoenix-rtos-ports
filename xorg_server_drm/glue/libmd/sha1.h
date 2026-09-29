/*
 * sha1.h — BSD libmd-compatible SHA1 API for the Phoenix-RTOS X server port.
 *
 * Provides exactly the symbols xorg-server's os/xsha1.c expects on its
 * HAVE_SHA1_IN_LIBMD code path (SHA1_CTX, SHA1Init/Update/Final). Backed by
 * Steve Reid's public-domain SHA1 implementation (sha1.c). No external
 * dependency — Phoenix lacks openssl/libgcrypt/libnettle, so this is the
 * portable backend the X server's glyph/PutImage hashing links against.
 *
 * Public domain.
 */
#ifndef PHOENIX_LIBMD_SHA1_H
#define PHOENIX_LIBMD_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t state[5];
	uint32_t count[2];
	uint8_t buffer[64];
} SHA1_CTX;

void SHA1Init(SHA1_CTX *context);
void SHA1Update(SHA1_CTX *context, const uint8_t *data, size_t len);
void SHA1Final(uint8_t digest[20], SHA1_CTX *context);
void SHA1Transform(uint32_t state[5], const uint8_t buffer[64]);

#endif /* PHOENIX_LIBMD_SHA1_H */
