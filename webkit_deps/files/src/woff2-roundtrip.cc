/*
 * woff2-roundtrip: the WOFF2 part of soup-smoke (woff2's API is C++)
 *
 * Compresses a TrueType font to WOFF2 and decompresses it again, as a web page's
 * @font-face would arrive and as WebKit's font loader decodes it.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <woff2/decode.h>
#include <woff2/encode.h>


/* 0: round trip ok, 1: a conversion failed, -1: no such font */
extern "C" int woff2_roundtrip(const char *path, size_t *ttfLen, size_t *woff2Len, size_t *outLen)
{
	std::FILE *f = std::fopen(path, "rb");
	std::vector<uint8_t> ttf;
	uint8_t buf[16384];
	size_t n;

	if (f == nullptr) {
		return -1;
	}
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		ttf.insert(ttf.end(), buf, buf + n);
	}
	std::fclose(f);
	*ttfLen = ttf.size();

	/* brotli quality 5, not woff2's default 11: the check is the format, not the ratio */
	woff2::WOFF2Params params;
	params.brotli_quality = 5;
	std::vector<uint8_t> woff2(woff2::MaxWOFF2CompressedSize(ttf.data(), ttf.size()));
	size_t len = woff2.size();
	if (!woff2::ConvertTTFToWOFF2(ttf.data(), ttf.size(), woff2.data(), &len, params)) {
		return 1;
	}
	*woff2Len = len;

	std::string out(std::min(woff2::ComputeWOFF2FinalSize(woff2.data(), len), woff2::kDefaultMaxSize), '\0');
	woff2::WOFF2StringOut sink(&out);
	if (!woff2::ConvertWOFF2ToTTF(woff2.data(), len, &sink)) {
		return 1;
	}
	*outLen = sink.Size();
	/* an sfnt again: version 1.0 (TrueType outlines) and the same number of tables */
	if (*outLen < 12 || out.compare(0, 4, std::string("\0\1\0\0", 4)) != 0 || out[4] != (char)ttf[4] || out[5] != (char)ttf[5]) {
		return 1;
	}
	return 0;
}
