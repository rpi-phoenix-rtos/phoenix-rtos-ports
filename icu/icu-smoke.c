/*
 * icu-smoke: a self-test of the ICU port, one tagged line per check:
 *
 *   ICUCHK <name> ok <detail>
 *   ICUCHK <name> FAIL <detail>
 *   ICUCHK RESULT pass <passed>/<total>
 *
 * It exercises what WebKit takes from ICU (collation, the four break
 * iterators, normalization, converters, UTS #46 IDNA, time zones, number and
 * date formatting, locale display names) and checks that the data really is
 * the filtered static package: a kept locale and converter open, an excluded
 * one does not. Exit status 0 only if every check passed.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include <unicode/ubrk.h>
#include <unicode/ucal.h>
#include <unicode/uclean.h>
#include <unicode/ucnv.h>
#include <unicode/ucol.h>
#include <unicode/udat.h>
#include <unicode/uidna.h>
#include <unicode/uloc.h>
#include <unicode/unorm2.h>
#include <unicode/unum.h>
#include <unicode/ures.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>


static int passed, total;


static void check(const char *name, int ok, const char *detail)
{
	total++;
	if (ok) {
		passed++;
	}
	printf("ICUCHK %s %s %s\n", name, ok ? "ok" : "FAIL", detail);
	fflush(stdout);
}


/* UTF-8 -> UTF-16 into buf (NUL-terminated), returns the length or -1 */
static int u16(UChar *buf, int cap, const char *utf8)
{
	UErrorCode st = U_ZERO_ERROR;
	int32_t len;

	u_strFromUTF8(buf, cap, &len, utf8, -1, &st);
	return U_SUCCESS(st) ? len : -1;
}


/* UTF-16 -> UTF-8 into out (NUL-terminated) */
static const char *u8(char *out, int cap, const UChar *s, int len)
{
	UErrorCode st = U_ZERO_ERROR;

	u_strToUTF8(out, cap, NULL, s, len, &st);
	if (U_FAILURE(st)) {
		snprintf(out, cap, "<%s>", u_errorName(st));
	}
	return out;
}


static void checkVersion(void)
{
	UVersionInfo v;
	char s[U_MAX_VERSION_STRING_LENGTH], d[64];

	u_getVersion(v);
	u_versionToString(v, s);
	snprintf(d, sizeof(d), "%s unicode=%s", s, U_UNICODE_VERSION);
	check("version", strcmp(s, U_ICU_VERSION) == 0 && v[0] == 78, d);
}


/* Data package: the default converter is UTF-8 (U_CHARSET_IS_UTF8), a kept
 * locale opens as itself, an excluded one only by falling back to root. */
static void checkData(void)
{
	UErrorCode st = U_ZERO_ERROR, st2 = U_ZERO_ERROR;
	UResourceBundle *rb;
	char d[160];

	snprintf(d, sizeof(d), "default-converter=%s", ucnv_getDefaultName());
	check("charset", strcmp(ucnv_getDefaultName(), "UTF-8") == 0, d);

	rb = ures_open(NULL, "pl", &st);
	if (rb != NULL) {
		ures_close(rb);
	}
	rb = ures_open(NULL, "hi", &st2);
	if (rb != NULL) {
		ures_close(rb);
	}
	snprintf(d, sizeof(d), "pl=%s hi(excluded)=%s", u_errorName(st), u_errorName(st2));
	check("data-filter-locale", st == U_ZERO_ERROR && st2 == U_USING_DEFAULT_WARNING, d);
}


static void checkCollation(void)
{
	static const char *words[] = { "zebra", "\xc5\x82\xc4\x85ka", "lato", "\xc4\x87ma", "cebula", "mak" };
	enum { N = sizeof(words) / sizeof(words[0]) };
	UChar w[N][32], tmp[32];
	UErrorCode st = U_ZERO_ERROR;
	UCollator *pl, *de;
	char d[256], b[64];
	int i, j, ok;

	pl = ucol_open("pl", &st);
	if (U_FAILURE(st)) {
		snprintf(d, sizeof(d), "ucol_open(pl): %s", u_errorName(st));
		check("collation-pl", 0, d);
		return;
	}
	for (i = 0; i < N; i++) {
		u16(w[i], 32, words[i]);
	}
	/* insertion sort with the Polish collator */
	for (i = 1; i < N; i++) {
		for (j = i; j > 0 && ucol_strcoll(pl, w[j], -1, w[j - 1], -1) == UCOL_LESS; j--) {
			memcpy(tmp, w[j], sizeof(tmp));
			memcpy(w[j], w[j - 1], sizeof(tmp));
			memcpy(w[j - 1], tmp, sizeof(tmp));
		}
	}
	d[0] = '\0';
	for (i = 0; i < N; i++) {
		strncat(d, u8(b, sizeof(b), w[i], -1), sizeof(d) - strlen(d) - 2);
		if (i < N - 1) {
			strncat(d, ",", sizeof(d) - strlen(d) - 1);
		}
	}
	/* cebula < ćma < lato < łąka < mak < zebra: ć after c, ł after l */
	check("collation-pl", strcmp(d, "cebula,\xc4\x87ma,lato,\xc5\x82\xc4\x85ka,mak,zebra") == 0, d);
	ucol_close(pl);

	st = U_ZERO_ERROR;
	de = ucol_open("de", &st);
	ok = 0;
	if (U_SUCCESS(st)) {
		UChar a[16], c[16];
		u16(a, 16, "\xc3\x84pfel");  /* Äpfel */
		u16(c, 16, "Birne");
		ok = ucol_strcoll(de, a, -1, c, -1) == UCOL_LESS;
		ucol_close(de);
	}
	snprintf(d, sizeof(d), "\xc3\x84pfel<Birne %s", u_errorName(st));
	check("collation-de", ok, d);
}


static int countBreaks(UBreakIteratorType type, const char *loc, const char *text, int wordsOnly)
{
	UChar t[128];
	UErrorCode st = U_ZERO_ERROR;
	UBreakIterator *bi;
	int len = u16(t, 128, text), n = 0, p;

	bi = ubrk_open(type, loc, t, len, &st);
	if (U_FAILURE(st)) {
		return -1;
	}
	ubrk_first(bi);
	while ((p = ubrk_next(bi)) != UBRK_DONE) {
		if (!wordsOnly || ubrk_getRuleStatus(bi) >= UBRK_WORD_LETTER) {
			n++;
		}
	}
	ubrk_close(bi);
	return n;
}


static void checkBreaks(void)
{
	char d[96];
	int n;

	n = countBreaks(UBRK_WORD, "en", "Hello, wonderful world! It's 2026.", 1);
	snprintf(d, sizeof(d), "en words=%d", n);
	check("brk-word", n == 4, d);

	/* 日本語のテキストです: the dictionary (cjdict) splits it into words */
	n = countBreaks(UBRK_WORD, "ja", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xa7\xe3\x81\x99", 1);
	snprintf(d, sizeof(d), "ja words=%d", n);
	check("brk-word-cjdict", n >= 3, d);

	n = countBreaks(UBRK_LINE, "en", "The quick brown fox jumps.", 0);
	snprintf(d, sizeof(d), "en line-opportunities=%d", n);
	check("brk-line", n == 5, d);

	n = countBreaks(UBRK_SENTENCE, "en", "Hello there. How are you? Fine!", 0);
	snprintf(d, sizeof(d), "en sentences=%d", n);
	check("brk-sentence", n == 3, d);

	/* e + U+0301 COMBINING ACUTE, a, U+1F44D U+1F3FD (thumbs up + skin tone) */
	n = countBreaks(UBRK_CHARACTER, "en", "e\xcc\x81" "a\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", 0);
	snprintf(d, sizeof(d), "graphemes=%d", n);
	check("brk-grapheme", n == 3, d);
}


static void checkNormalization(void)
{
	UErrorCode st = U_ZERO_ERROR;
	const UNormalizer2 *nfc, *nfkc;
	UChar in[16], out[16];
	char d[96], b[32];
	int len, ok;

	nfc = unorm2_getNFCInstance(&st);
	u16(in, 16, "e\xcc\x81");
	len = U_SUCCESS(st) ? unorm2_normalize(nfc, in, -1, out, 16, &st) : -1;
	ok = U_SUCCESS(st) && len == 1 && out[0] == 0x00e9;
	snprintf(d, sizeof(d), "e+U+0301 -> U+%04X len=%d %s", len > 0 ? out[0] : 0, len, u_errorName(st));
	check("nfc", ok, d);

	st = U_ZERO_ERROR;
	nfkc = unorm2_getNFKCInstance(&st);
	u16(in, 16, "\xef\xac\x81\xe2\x91\xa0");  /* U+FB01 (fi ligature), U+2460 (circled 1) */
	len = U_SUCCESS(st) ? unorm2_normalize(nfkc, in, -1, out, 16, &st) : -1;
	u8(b, sizeof(b), out, len > 0 ? len : 0);
	ok = U_SUCCESS(st) && strcmp(b, "fi1") == 0;
	snprintf(d, sizeof(d), "U+FB01U+2460 -> %s %s", b, u_errorName(st));
	check("nfkc", ok, d);
}


/* bytes in the named charset -> UTF-8 */
static int decode(const char *cs, const char *bytes, char *out, int cap, UErrorCode *st)
{
	UChar u[32];
	int len;

	*st = U_ZERO_ERROR;
	UConverter *cnv = ucnv_open(cs, st);
	if (U_FAILURE(*st)) {
		return -1;
	}
	len = ucnv_toUChars(cnv, u, 32, bytes, -1, st);
	ucnv_close(cnv);
	if (U_FAILURE(*st)) {
		return -1;
	}
	u8(out, cap, u, len);
	return 0;
}


/* WebKit's TextCodecICU: every name in its encodingNames[] resolves to a
 * converter name (registerCodecs: by its IANA name, else by opening it), and
 * that converter opens (createICUConverter). ICU has an alias entry for
 * ISO-8859-16 but no table for it (stock data neither), so that one is
 * expected to be missing. */
static void checkWebKitCodecs(void)
{
	static const char *names[] = {
		"ISO-8859-2", "ISO-8859-4", "ISO-8859-5", "ISO-8859-10", "ISO-8859-13",
		"ISO-8859-14", "ISO-8859-15", "ISO-8859-16", "KOI8-R", "macintosh",
		"windows-1250", "windows-1251", "windows-1254", "windows-1256", "windows-1258",
		"x-mac-cyrillic", "x-mac-greek", "x-mac-centraleurroman", "x-mac-turkish", "EUC-TW",
	};
	enum { N = sizeof(names) / sizeof(names[0]) };
	char missing[128] = "", d[192];
	int i, found = 0, isoMissing = 0;

	for (i = 0; i < N; i++) {
		UErrorCode st = U_ZERO_ERROR;
		const char *canon = ucnv_getCanonicalName(names[i], "IANA", &st);
		if (canon == NULL) {
			UConverter *cnv;
			st = U_ZERO_ERROR;
			cnv = ucnv_open(names[i], &st);
			if (U_SUCCESS(st)) {
				canon = ucnv_getName(cnv, &st);
				ucnv_close(cnv);
			}
		}
		if (canon != NULL && U_SUCCESS(st)) {
			UConverter *cnv = ucnv_open(canon, &st);
			if (U_SUCCESS(st)) {
				ucnv_close(cnv);
			}
		}
		if (canon != NULL && U_SUCCESS(st)) {
			found++;
		}
		else {
			isoMissing += strcmp(names[i], "ISO-8859-16") == 0;
			strncat(missing, " ", sizeof(missing) - strlen(missing) - 1);
			strncat(missing, names[i], sizeof(missing) - strlen(missing) - 1);
		}
	}
	snprintf(d, sizeof(d), "%d/%d missing:%s", found, N, missing[0] != '\0' ? missing : " none");
	check("webkit-codecs", found == N - 1 && isoMissing == 1, d);
}


static void checkConverters(void)
{
	static const struct {
		const char *cs, *bytes, *utf8;
	} cases[] = {
		{ "windows-1250", "\xb3\xf3\x64\x9f", "\xc5\x82\xc3\xb3\x64\xc5\xba" },                          /* łódź */
		{ "windows-1251", "\xcf\xf0\xe8\xe2\xe5\xf2", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82" }, /* Привет */
		{ "ISO-8859-2", "\xb3\xf3\x64\xbc", "\xc5\x82\xc3\xb3\x64\xc5\xba" },                            /* łódź */
		{ "KOI8-R", "\xf0\xd2\xc9\xd7\xc5\xd4", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82" },       /* Привет */
		{ "macintosh", "\x8a", "\xc3\xa4" },                                                              /* ä */
		{ "EUC-TW", "\xc4\xa1", "\xe4\xb8\x80" },                                                         /* 一 */
	};
	UErrorCode st;
	char out[64], d[160];
	unsigned i;
	int ok, r;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char name[48];
		r = decode(cases[i].cs, cases[i].bytes, out, sizeof(out), &st);
		ok = r == 0 && strcmp(out, cases[i].utf8) == 0;
		snprintf(name, sizeof(name), "ucnv-%s", cases[i].cs);
		snprintf(d, sizeof(d), "-> %s %s", r == 0 ? out : "-", u_errorName(st));
		check(name, ok, d);
	}

	checkWebKitCodecs();

	/* EBCDIC (ibm-37) is filtered out of the data: it must NOT open */
	r = decode("ibm-37", "\xc1", out, sizeof(out), &st);
	snprintf(d, sizeof(d), "ibm-37(excluded) -> %s", u_errorName(st));
	check("data-filter-converter", r != 0 && st == U_FILE_ACCESS_ERROR, d);
}


static void checkIdna(void)
{
	UErrorCode st = U_ZERO_ERROR;
	UIDNAInfo info = UIDNA_INFO_INITIALIZER;
	UIDNA *idna;
	char out[96], d[160];
	int len;

	idna = uidna_openUTS46(UIDNA_CHECK_BIDI | UIDNA_CHECK_CONTEXTJ | UIDNA_NONTRANSITIONAL_TO_ASCII, &st);
	if (U_FAILURE(st)) {
		snprintf(d, sizeof(d), "uidna_openUTS46: %s", u_errorName(st));
		check("idna-uts46", 0, d);
		return;
	}
	/* BÜcher.Example: mapped (case-folded) and Punycode-encoded */
	len = uidna_nameToASCII_UTF8(idna, "B\xc3\x9c" "cher.Example", -1, out, sizeof(out) - 1, &info, &st);
	if (len >= 0 && U_SUCCESS(st)) {
		out[len] = '\0';
	}
	else {
		strcpy(out, "-");
	}
	snprintf(d, sizeof(d), "B\xc3\x9c" "cher.Example -> %s errors=0x%x %s", out, (unsigned)info.errors, u_errorName(st));
	check("idna-uts46", U_SUCCESS(st) && info.errors == 0 && strcmp(out, "xn--bcher-kva.example") == 0, d);
	uidna_close(idna);
}


static void checkTimeZone(void)
{
	/* 2026-01-15 12:00 UTC and 2026-07-15 12:00 UTC */
	static const UDate winter = 1768478400000.0, summer = 1784116800000.0;
	UChar tz[32], dflt[64];
	UErrorCode st = U_ZERO_ERROR;
	UCalendar *cal;
	char d[160], b[64];
	int32_t w = -1, s = -1;

	u16(tz, 32, "Europe/Warsaw");
	cal = ucal_open(tz, -1, "en", UCAL_GREGORIAN, &st);
	if (U_SUCCESS(st)) {
		ucal_setMillis(cal, winter, &st);
		w = ucal_get(cal, UCAL_ZONE_OFFSET, &st) + ucal_get(cal, UCAL_DST_OFFSET, &st);
		ucal_setMillis(cal, summer, &st);
		s = ucal_get(cal, UCAL_ZONE_OFFSET, &st) + ucal_get(cal, UCAL_DST_OFFSET, &st);
		ucal_close(cal);
	}
	snprintf(d, sizeof(d), "Europe/Warsaw jan=%+dmin jul=%+dmin %s", w / 60000, s / 60000, u_errorName(st));
	check("timezone", U_SUCCESS(st) && w == 3600000 && s == 7200000, d);

	/* informational: the zone ICU detected (TZ, else UTC/whatever the system gives) */
	st = U_ZERO_ERROR;
	ucal_getDefaultTimeZone(dflt, 64, &st);
	printf("ICUCHK info default-timezone=%s\n", U_SUCCESS(st) ? u8(b, sizeof(b), dflt, -1) : u_errorName(st));
}


static void checkFormatting(void)
{
	static const UDate when = 1768478400000.0; /* 2026-01-15 12:00 UTC */
	UChar buf[128], utc[8], pat[32];
	UErrorCode st = U_ZERO_ERROR;
	UNumberFormat *nf;
	UDateFormat *df;
	char d[192], b[128];

	nf = unum_open(UNUM_DECIMAL, NULL, 0, "de", NULL, &st);
	if (U_SUCCESS(st)) {
		unum_formatDouble(nf, 1234567.891, buf, 128, NULL, &st);
		unum_close(nf);
	}
	u8(b, sizeof(b), buf, U_SUCCESS(st) ? -1 : 0);
	snprintf(d, sizeof(d), "de 1234567.891 -> %s %s", b, u_errorName(st));
	check("number-de", U_SUCCESS(st) && strcmp(b, "1.234.567,891") == 0, d);

	st = U_ZERO_ERROR;
	u16(utc, 8, "UTC");
	u16(pat, 32, "d MMMM y");
	df = udat_open(UDAT_PATTERN, UDAT_PATTERN, "pl", utc, -1, pat, -1, &st);
	if (U_SUCCESS(st)) {
		udat_format(df, when, buf, 128, NULL, &st);
		udat_close(df);
	}
	u8(b, sizeof(b), buf, U_SUCCESS(st) ? -1 : 0);
	snprintf(d, sizeof(d), "pl 2026-01-15 -> %s %s", b, u_errorName(st));
	check("date-pl", U_SUCCESS(st) && strcmp(b, "15 stycznia 2026") == 0, d);

	st = U_ZERO_ERROR;
	df = udat_open(UDAT_NONE, UDAT_LONG, "ja", utc, -1, NULL, 0, &st);
	if (U_SUCCESS(st)) {
		udat_format(df, when, buf, 128, NULL, &st);
		udat_close(df);
	}
	u8(b, sizeof(b), buf, U_SUCCESS(st) ? -1 : 0);
	snprintf(d, sizeof(d), "ja long 2026-01-15 -> %s %s", b, u_errorName(st));
	check("date-ja", U_SUCCESS(st) && strcmp(b, "2026\xe5\xb9\xb4" "1\xe6\x9c\x88" "15\xe6\x97\xa5") == 0, d);
}


static void checkDisplayNames(void)
{
	UChar buf[64];
	UErrorCode st = U_ZERO_ERROR, st2 = U_ZERO_ERROR;
	char d[160], b1[64], b2[64];

	uloc_getDisplayLanguage("pl", "en", buf, 64, &st);
	u8(b1, sizeof(b1), buf, U_SUCCESS(st) ? -1 : 0);
	uloc_getDisplayLanguage("de", "fr", buf, 64, &st2);
	u8(b2, sizeof(b2), buf, U_SUCCESS(st2) ? -1 : 0);
	snprintf(d, sizeof(d), "pl@en=%s de@fr=%s", b1, b2);
	check("display-names", U_SUCCESS(st) && U_SUCCESS(st2) &&
		strcmp(b1, "Polish") == 0 && strcmp(b2, "allemand") == 0, d);
}


int main(void)
{
	UErrorCode st = U_ZERO_ERROR;
	char d[64];

	u_init(&st);
	snprintf(d, sizeof(d), "u_init %s", u_errorName(st));
	check("init", U_SUCCESS(st), d);

	checkVersion();
	checkData();
	checkCollation();
	checkBreaks();
	checkNormalization();
	checkConverters();
	checkIdna();
	checkTimeZone();
	checkFormatting();
	checkDisplayNames();

	u_cleanup();
	printf("ICUCHK RESULT pass %d/%d\n", passed, total);
	return passed == total ? 0 : 1;
}
