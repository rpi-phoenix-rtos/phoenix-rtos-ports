/*
 * sha1.c — public-domain SHA1 (Steve Reid <steve@edmweb.com>, "SHA1.C" 100%
 * public domain), with the BSD libmd-style entry points (SHA1Init/Update/Final)
 * the Phoenix-RTOS X server port links against. Endian-neutral.
 *
 * Public domain.
 */
#include "sha1.h"
#include <string.h>

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/* blk0() and blk() perform the initial expand; little/big-endian neutral. */
static uint32_t blk0(const uint8_t *buffer, int i)
{
	return ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
	       ((uint32_t)buffer[i * 4 + 2] << 8) | ((uint32_t)buffer[i * 4 + 3]);
}

void SHA1Transform(uint32_t state[5], const uint8_t buffer[64])
{
	uint32_t a, b, c, d, e, l[80];
	int i;

	for (i = 0; i < 16; i++) {
		l[i] = blk0(buffer, i);
	}
	for (i = 16; i < 80; i++) {
		l[i] = rol(l[i - 3] ^ l[i - 8] ^ l[i - 14] ^ l[i - 16], 1);
	}

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];

	for (i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | ((~b) & d);
			k = 0x5A827999;
		}
		else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		}
		else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		}
		else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		uint32_t tmp = rol(a, 5) + f + e + k + l[i];
		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = tmp;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

void SHA1Init(SHA1_CTX *context)
{
	context->state[0] = 0x67452301;
	context->state[1] = 0xEFCDAB89;
	context->state[2] = 0x98BADCFE;
	context->state[3] = 0x10325476;
	context->state[4] = 0xC3D2E1F0;
	context->count[0] = 0;
	context->count[1] = 0;
}

void SHA1Update(SHA1_CTX *context, const uint8_t *data, size_t len)
{
	size_t i, j;

	j = (context->count[0] >> 3) & 63;
	if ((context->count[0] += (uint32_t)(len << 3)) < (len << 3)) {
		context->count[1]++;
	}
	context->count[1] += (uint32_t)(len >> 29);

	if ((j + len) > 63) {
		i = 64 - j;
		memcpy(&context->buffer[j], data, i);
		SHA1Transform(context->state, context->buffer);
		for (; i + 63 < len; i += 64) {
			SHA1Transform(context->state, &data[i]);
		}
		j = 0;
	}
	else {
		i = 0;
	}
	memcpy(&context->buffer[j], &data[i], len - i);
}

void SHA1Final(uint8_t digest[20], SHA1_CTX *context)
{
	uint8_t finalcount[8];
	uint8_t c;
	int i;

	for (i = 0; i < 8; i++) {
		finalcount[i] = (uint8_t)((context->count[(i >= 4 ? 0 : 1)] >>
		                          ((3 - (i & 3)) * 8)) & 255);
	}

	c = 0x80;
	SHA1Update(context, &c, 1);
	while ((context->count[0] & 504) != 448) {
		c = 0x00;
		SHA1Update(context, &c, 1);
	}
	SHA1Update(context, finalcount, 8);

	for (i = 0; i < 20; i++) {
		digest[i] = (uint8_t)((context->state[i >> 2] >>
		                      ((3 - (i & 3)) * 8)) & 255);
	}

	memset(context, 0, sizeof(*context));
}
