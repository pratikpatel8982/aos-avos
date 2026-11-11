/*
 * test_id3tag.c -- unit tests for Source/id3tag.c (ID3v1 / ID3v2.2 / 2.3 / 2.4 tag parser).
 *
 * Public API of the real, instrumented id3tag.o (which in turn runs the real file_type.c lookups and
 * awchar.c UTF-16 -> UTF-8 conversion). Inputs are real ID3 bytes assembled by the small builder
 * below following the id3.org specifications, plus the real tag written by FFmpeg/LAME into
 * fixtures/media/sine_id3.mp3 (cross-checked against libavformat's metadata) and the real JPEG /
 * PNG fixtures as attached pictures.
 *
 * ID3V2_Parse() does not clear the tag it fills, so every test starts from a zeroed ID3_TAG.
 */
#include "avos_test.h"
#include "av.h"
#include "debug.h"
#include "id3tag.h"
#include "file_type.h"

#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a tiny ID3v2 tag builder -------------------------------------------------------------- */

typedef struct { unsigned char *b; int n, cap, ver; } TB;

static void tb_put(TB *t, const void *p, int len)
{
	if (t->n + len > t->cap) {
		t->cap = (t->n + len) * 2 + 64;
		t->b = realloc(t->b, t->cap);
		REQUIRE_NOT_NULL(t->b);
	}
	memcpy(t->b + t->n, p, len);
	t->n += len;
}

static void tb_init(TB *t, int ver)
{
	memset(t, 0, sizeof *t);
	t->ver = ver;
	static const unsigned char hdr[10] = { 'I', 'D', '3', 0, 0, 0, 0, 0, 0, 0 };
	tb_put(t, hdr, 10);
	t->b[3] = (unsigned char)ver;
}

/* Append a frame with an exact body; `declared` overrides the size written in the frame header (-1: real). */
static void tb_frame_sz(TB *t, const char *id, const void *body, int len, long declared)
{
	unsigned long sz = declared < 0 ? (unsigned long)len : (unsigned long)declared;
	unsigned char h[10];
	int hl;
	if (t->ver == 2) {
		memcpy(h, id, 3);
		h[3] = (sz >> 16) & 0xFF; h[4] = (sz >> 8) & 0xFF; h[5] = sz & 0xFF;
		hl = 6;
	} else {
		memcpy(h, id, 4);
		if (t->ver >= 4) {
			h[4] = (sz >> 21) & 0x7F; h[5] = (sz >> 14) & 0x7F; h[6] = (sz >> 7) & 0x7F; h[7] = sz & 0x7F;
		} else {
			h[4] = (sz >> 24) & 0xFF; h[5] = (sz >> 16) & 0xFF; h[6] = (sz >> 8) & 0xFF; h[7] = sz & 0xFF;
		}
		h[8] = h[9] = 0;
		hl = 10;
	}
	tb_put(t, h, hl);
	tb_put(t, body, len);
}

static void tb_frame(TB *t, const char *id, const void *body, int len)
{
	tb_frame_sz(t, id, body, len, -1);
}

/* text frame: encoding byte + raw text bytes */
static void tb_text_raw(TB *t, const char *id, int enc, const void *text, int len)
{
	unsigned char *body = malloc(len + 1);
	body[0] = (unsigned char)enc;
	memcpy(body + 1, text, len);
	tb_frame(t, id, body, len + 1);
	free(body);
}

static void tb_text(TB *t, const char *id, const char *text)
{
	tb_text_raw(t, id, 0, text, (int)strlen(text));
}

static void tb_pad(TB *t, int n)
{
	unsigned char z[256] = { 0 };
	while (n > 0) { int k = n > 256 ? 256 : n; tb_put(t, z, k); n -= k; }
}

/* finish: write the synchsafe tag size (excluding the 10 byte header) and the flags */
static unsigned char *tb_done(TB *t, int flags, int *len)
{
	unsigned long sz = t->n - 10;
	t->b[5] = (unsigned char)flags;
	t->b[6] = (sz >> 21) & 0x7F; t->b[7] = (sz >> 14) & 0x7F; t->b[8] = (sz >> 7) & 0x7F; t->b[9] = sz & 0x7F;
	*len = t->n;
	return t->b;
}

static int parse(unsigned char *tag, int len, ID3_TAG *out, APIC *apic)
{
	memset(out, 0, sizeof *out);
	return ID3V2_Parse(out, tag, len, 0, apic);
}

/* UTF-16 helpers: write n code units */
static int utf16(unsigned char *o, const unsigned short *u, int n, int big_endian)
{
	for (int i = 0; i < n; i++) {
		o[2 * i]     = big_endian ? u[i] >> 8 : u[i] & 0xFF;
		o[2 * i + 1] = big_endian ? u[i] & 0xFF : u[i] >> 8;
	}
	return 2 * n;
}

static unsigned char *slurp(const char *rel, int *size)
{
	FILE *f = fopen(fixture_path(rel), "rb");
	REQUIRE_NOT_NULL(f);
	fseek(f, 0, SEEK_END);
	*size = (int)ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc(*size);
	REQUIRE_NOT_NULL(b);
	REQUIRE(fread(b, 1, *size, f) == (size_t)*size);
	fclose(f);
	return b;
}

/* ---- genre table ---------------------------------------------------------------------------- */

TEST(numeric_genre_in_parentheses_is_replaced_by_its_name)
{
	char g[64] = "(13)";
	ID3_set_get_genre_from_ID(g);
	CHECK_EQ_STR(g, "Pop");
	strcpy(g, "(147)");
	ID3_set_get_genre_from_ID(g);
	CHECK_EQ_STR(g, "Synthpop");
	strcpy(g, "(1)");
	ID3_set_get_genre_from_ID(g);
	CHECK_EQ_STR(g, "Classic Rock");
	strcpy(g, "(17)Rock");				/* "(n)Refinement" form: the whole text becomes the name */
	ID3_set_get_genre_from_ID(g);
	CHECK_EQ_STR(g, "Rock");
}

TEST(genre_text_that_is_not_a_known_number_is_left_alone)
{
	static const char *keep[] = { "Rock", "13", "(RX)", "(148)", "(255)", "()", "", "(a)", "[13]" };
	for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++) {
		char g[64];
		strcpy(g, keep[i]);
		ID3_set_get_genre_from_ID(g);
		CHECK_MSG(!strcmp(g, keep[i]), "'%s' became '%s'", keep[i], g);
	}
}

TEST(known_defect_genre_zero_in_parentheses_is_not_translated)
{
	/* ID3v2.3: "(0)" means genre 0, "Blues". The guard `genre[1] >= '1'` excludes it, so the
	   literal "(0)" is shown. Correct result: "Blues". Flip this expectation when fixed. */
	char g[64] = "(0)";
	ID3_set_get_genre_from_ID(g);
	CHECK_EQ_STR(g, "(0)");				/* correct: "Blues" */
}

/* ---- sizes ----------------------------------------------------------------------------------- */

TEST(v2_size_is_the_synchsafe_size_plus_the_ten_byte_header)
{
	unsigned char h[10] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0 };
	CHECK_EQ_INT(ID3V2_GetSize(h), 10);
	h[9] = 0x7F;
	CHECK_EQ_INT(ID3V2_GetSize(h), 10 + 127);
	h[8] = 1; h[9] = 0;
	CHECK_EQ_INT(ID3V2_GetSize(h), 10 + 128);
	h[6] = 0x7F; h[7] = 0x7F; h[8] = 0x7F; h[9] = 0x7F;
	CHECK_EQ_INT(ID3V2_GetSize(h), 10 + 0x0FFFFFFF);
	h[6] = 1; h[7] = 2; h[8] = 3; h[9] = 4;
	CHECK_EQ_INT(ID3V2_GetSize(h), 10 + ((1 << 21) | (2 << 14) | (3 << 7) | 4));
}

TEST(v2_size_is_zero_unless_the_buffer_starts_with_id3)
{
	unsigned char a[10] = { 'I', 'D', '4', 3, 0, 0, 0, 0, 1, 0 };
	unsigned char b[10] = { 'I', 'X', '3', 3, 0, 0, 0, 0, 1, 0 };
	unsigned char c[10] = { 'X', 'D', '3', 3, 0, 0, 0, 0, 1, 0 };
	unsigned char d[10] = { 0 };
	CHECK_EQ_INT(ID3V2_GetSize(a), 0);
	CHECK_EQ_INT(ID3V2_GetSize(b), 0);
	CHECK_EQ_INT(ID3V2_GetSize(c), 0);
	CHECK_EQ_INT(ID3V2_GetSize(d), 0);
}

TEST(v1_size_is_128_for_a_tag_and_zero_otherwise)
{
	CHECK_EQ_INT(ID3V1_GetSize((unsigned char *)"TAG and the rest"), 128);
	CHECK_EQ_INT(ID3V1_GetSize((unsigned char *)"TAX"), 0);
	CHECK_EQ_INT(ID3V1_GetSize((unsigned char *)"tag"), 0);
	CHECK_EQ_INT(ID3V1_GetSize((unsigned char *)""), 0);
}

/* ---- ID3v2.3 text frames --------------------------------------------------------------------- */

TEST(v23_text_frames_fill_every_field)
{
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", "A Title");
	tb_text(&t, "TPE1", "The Artist");
	tb_text(&t, "TALB", "The Album");
	tb_text(&t, "TPE2", "Album Artist");
	tb_text(&t, "TCOM", "The Composer");
	tb_text(&t, "TEXT", "The Lyricist");
	tb_text(&t, "TYER", "1999");
	tb_text(&t, "TCON", "Jazz");
	tb_text(&t, "TLEN", "215000");
	tb_text(&t, "TRCK", "7/12");
	tb_text(&t, "TPOS", "2/3");
	tb_text(&t, "TCMP", "1");
	tb_pad(&t, 32);
	int len; unsigned char *b = tb_done(&t, 0, &len);

	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_INT(tag.valid, 1);
	CHECK_EQ_INT(tag.id3_ver, 2);
	CHECK_EQ_STR(tag.title, "A Title");
	CHECK_EQ_STR(tag.artist, "The Artist");
	CHECK_EQ_STR(tag.album, "The Album");
	CHECK_EQ_STR(tag.album_artist, "Album Artist");
	CHECK_EQ_STR(tag.composer, "The Composer");
	CHECK_EQ_STR(tag.author, "The Lyricist");
	CHECK_EQ_STR(tag.year, "1999");
	CHECK_EQ_STR(tag.genre, "Jazz");
	CHECK_EQ_STR(tag.compilation, "1");
	CHECK_EQ_INT(tag.msec, 215000);			/* TLEN and TRCK/TPOS do not by themselves set `tagfound` */
	CHECK_EQ_INT(tag.track, 7);			/* atoi("7/12") */
	CHECK_EQ_INT(tag.discnumber, 2);
	free(b);
}

TEST(v23_numeric_genre_frame_is_translated)
{
	TB t; tb_init(&t, 3);
	tb_text(&t, "TCON", "(13)");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.genre, "Pop");
	free(b);
}

TEST(track_length_and_disc_alone_do_not_make_the_tag_valid)
{
	TB t; tb_init(&t, 3);
	tb_text(&t, "TLEN", "1000");
	tb_text(&t, "TRCK", "4");
	tb_text(&t, "TPOS", "1");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);
	CHECK_EQ_INT(tag.valid, 0);
	CHECK_EQ_INT(tag.id3_ver, 0);
	CHECK_EQ_INT(tag.msec, 1000);			/* still stored */
	CHECK_EQ_INT(tag.track, 4);
	CHECK_EQ_INT(tag.discnumber, 1);
	free(b);
}

TEST(unknown_and_encoder_frames_are_skipped_without_marking_the_tag_found)
{
	TB t; tb_init(&t, 3);
	tb_text(&t, "TENC", "LAME 3.100");
	tb_text(&t, "XXXX", "mystery");
	tb_text(&t, "COMM", "a comment");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);
	CHECK_EQ_INT(tag.valid, 0);
	CHECK_EQ_STR(tag.title, "");
	free(b);

	/* ...and a known frame after unknown ones is still found */
	tb_init(&t, 3);
	tb_text(&t, "TENC", "LAME");
	tb_text(&t, "XXXX", "mystery");
	tb_text(&t, "TIT2", "After");
	b = tb_done(&t, 0, &len);
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "After");
	free(b);
}

TEST(padding_ends_the_frame_list)
{
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", "Before padding");
	tb_pad(&t, 20);
	tb_text(&t, "TPE1", "Hidden behind padding");	/* must never be read */
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "Before padding");
	CHECK_EQ_STR(tag.artist, "");
	free(b);
}

TEST(a_tag_without_frames_or_with_less_than_one_frame_header_left_finds_nothing)
{
	TB t; tb_init(&t, 3);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);
	CHECK_EQ_INT(tag.valid, 0);
	free(b);

	tb_init(&t, 3);
	tb_pad(&t, 7);					/* fewer than the 8 bytes of a frame header */
	b = tb_done(&t, 0, &len);
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);
	free(b);
}

TEST(a_buffer_that_is_not_an_id3_tag_is_refused_and_left_untouched)
{
	unsigned char junk[32] = "RIFF....WAVEfmt this is no tag";
	ID3_TAG tag;
	memset(&tag, 0x5A, sizeof tag);
	CHECK_EQ_INT(ID3V2_Parse(&tag, junk, sizeof junk, 0, NULL), 1);
	CHECK_EQ_INT(tag.valid, 0x5A5A5A5A);		/* not a single field written */
}

TEST(frame_size_larger_than_the_tag_is_clamped_to_what_is_there)
{
	TB t; tb_init(&t, 3);
	tb_frame_sz(&t, "TIT2", "\0Truncated tit", 14, 1000000);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "Truncated tit");
	free(b);
}

TEST(the_four_byte_frame_size_may_have_its_top_bit_set_in_v23)
{
	/* big-endian, not synchsafe: a 200 byte title needs 0xC8 in the last size byte */
	char title[201];
	memset(title, 'x', 200);
	title[200] = 0;
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", title);
	tb_text(&t, "TPE1", "after");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_INT(strlen(tag.title), 200);
	CHECK_EQ_STR(tag.artist, "after");
	free(b);
}

TEST(entries_longer_than_255_bytes_are_truncated_not_overrun)
{
	char big[600];
	memset(big, 'z', sizeof big - 1);
	big[sizeof big - 1] = 0;
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", big);
	tb_text(&t, "TPE1", "next");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_INT(strlen(tag.title), 255);
	CHECK_EQ_STR(tag.artist, "next");		/* the neighbour field is intact */
	free(b);
}

TEST(year_is_limited_to_four_characters)
{
	TB t; tb_init(&t, 3);
	tb_text(&t, "TYER", "1999-12-31");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.year, "1999");
	free(b);
}

/* ---- text encodings -------------------------------------------------------------------------- */

TEST(iso_8859_1_text_is_converted_to_utf8)
{
	TB t; tb_init(&t, 3);
	tb_text_raw(&t, "TIT2", 0x00, "Caf\xE9 \xFC" "ber", 9);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "Caf\xC3\xA9 \xC3\xBC" "ber");
	free(b);
}

TEST(utf16_with_a_little_endian_bom_is_converted_to_utf8)
{
	static const unsigned short text[] = { 'Z', 'a', 0x017C, 0x00F3, 0x0142, 0x0107 };	/* Zażółć */
	unsigned char body[64];
	int n = 0;
	body[n++] = 0xFF; body[n++] = 0xFE;
	n += utf16(body + n, text, 6, 0);
	TB t; tb_init(&t, 3);
	tb_text_raw(&t, "TIT2", 0x01, body, n);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");
	free(b);
}

TEST(utf16_with_a_big_endian_bom_is_converted_to_utf8)
{
	static const unsigned short text[] = { 'N', 0x00E4, 'r' };
	unsigned char body[64];
	int n = 0;
	body[n++] = 0xFE; body[n++] = 0xFF;
	n += utf16(body + n, text, 3, 1);
	TB t; tb_init(&t, 3);
	tb_text_raw(&t, "TPE1", 0x01, body, n);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.artist, "N\xC3\xA4r");
	free(b);
}

TEST(utf16_big_endian_without_bom_is_converted_to_utf8)
{
	static const unsigned short text[] = { 0x00DF, 'x' };
	unsigned char body[16];
	int n = utf16(body, text, 2, 1);
	TB t; tb_init(&t, 4);
	tb_text_raw(&t, "TALB", 0x02, body, n);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.album, "\xC3\x9F" "x");
	free(b);
}

TEST(utf8_text_is_copied_verbatim)
{
	TB t; tb_init(&t, 4);
	tb_text_raw(&t, "TIT2", 0x03, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 9);	/* 日本語 */
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
	free(b);
}

TEST(long_utf16_and_utf8_entries_are_truncated_at_the_field_size)
{
	unsigned short u[400];
	unsigned char body[1000];
	for (int i = 0; i < 400; i++) u[i] = 'a';
	int n = utf16(body, u, 400, 1);
	TB t; tb_init(&t, 4);
	tb_text_raw(&t, "TIT2", 0x02, body, n);
	char big[500];
	memset(big, 'u', sizeof big);
	tb_text_raw(&t, "TPE1", 0x03, big, sizeof big);
	tb_text(&t, "TALB", "ok");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_INT(strlen(tag.title), 255);
	CHECK_EQ_INT(strlen(tag.artist), 255);
	CHECK_EQ_STR(tag.album, "ok");
	free(b);
}

/* ---- ID3v2.4 and ID3v2.2 --------------------------------------------------------------------- */

TEST(v24_uses_synchsafe_frame_sizes_and_takes_the_year_from_tdrc)
{
	char title[201];
	memset(title, 'q', 200);
	title[200] = 0;
	TB t; tb_init(&t, 4);
	tb_text(&t, "TIT2", title);				/* 201 bytes: synchsafe 0x01 0x49, not 0x00 0xC9 */
	tb_text(&t, "TDRC", "2021-05-06T10:00");
	tb_text(&t, "TPE1", "Four");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_INT(strlen(tag.title), 200);
	CHECK_EQ_STR(tag.year, "2021");
	CHECK_EQ_STR(tag.artist, "Four");
	free(b);
}

TEST(v22_three_character_frame_ids_are_recognised)
{
	TB t; tb_init(&t, 2);
	tb_text(&t, "TT2", "Old Title");
	tb_text(&t, "TP1", "Old Artist");
	tb_text(&t, "TAL", "Old Album");
	tb_text(&t, "TP2", "Old Album Artist");
	tb_text(&t, "TCM", "Old Composer");
	tb_text(&t, "TXT", "Old Writer");
	tb_text(&t, "TYE", "1977");
	tb_text(&t, "TCO", "(17)");
	tb_text(&t, "TLE", "180000");
	tb_text(&t, "TRK", "3");
	tb_text(&t, "TPA", "1/2");
	tb_text(&t, "TCP", "1");
	tb_pad(&t, 16);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "Old Title");
	CHECK_EQ_STR(tag.artist, "Old Artist");
	CHECK_EQ_STR(tag.album, "Old Album");
	CHECK_EQ_STR(tag.album_artist, "Old Album Artist");
	CHECK_EQ_STR(tag.composer, "Old Composer");
	CHECK_EQ_STR(tag.author, "Old Writer");
	CHECK_EQ_STR(tag.year, "1977");
	CHECK_EQ_STR(tag.genre, "Rock");
	CHECK_EQ_STR(tag.compilation, "1");
	CHECK_EQ_INT(tag.msec, 180000);
	CHECK_EQ_INT(tag.track, 3);
	CHECK_EQ_INT(tag.discnumber, 1);
	free(b);
}

TEST(v22_does_not_recognise_four_character_ids)
{
	TB t; tb_init(&t, 2);
	tb_text(&t, "TIT2", "wrong generation");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);
	CHECK_EQ_STR(tag.title, "");
	free(b);
}

/* ---- header features -------------------------------------------------------------------------- */

TEST(an_extended_header_is_skipped_before_the_frames)
{
	TB t; tb_init(&t, 4);
	static const unsigned char ext[6] = { 0, 0, 0, 6, 0x01, 0x20 };	/* synchsafe size 6, flags */
	tb_put(&t, ext, 6);
	tb_text(&t, "TIT2", "After ext header");
	int len; unsigned char *b = tb_done(&t, 0x40, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "After ext header");
	free(b);
}

TEST(the_extended_header_flag_is_what_triggers_the_skip)
{
	/* the same bytes without flag 0x40 make the six ext bytes look like (non-)frames */
	TB t; tb_init(&t, 4);
	static const unsigned char ext[6] = { 0, 0, 0, 6, 0x01, 0x20 };
	tb_put(&t, ext, 6);
	tb_text(&t, "TIT2", "After ext header");
	int len; unsigned char *b = tb_done(&t, 0x00, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);	/* first 4 bytes are zero: parsed as padding */
	CHECK_EQ_STR(tag.title, "");
	free(b);
}

TEST(an_oversized_extended_header_skips_to_the_end_without_overrunning)
{
	TB t; tb_init(&t, 4);
	static const unsigned char ext[8] = { 0x7F, 0x7F, 0x7F, 0x7F, 0, 0, 0, 0 };
	tb_put(&t, ext, 8);
	tb_text(&t, "TIT2", "unreachable");
	int len; unsigned char *b = tb_done(&t, 0x40, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);
	free(b);
}

TEST(a_tag_that_wraps_around_the_end_of_a_circular_buffer_is_parsed)
{
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", "Wrapped title that is long enough to cross the end");
	tb_text(&t, "TPE1", "Wrapped artist");
	tb_text(&t, "TRCK", "5");
	tb_text(&t, "TCON", "(17)");
	int len; unsigned char *b = tb_done(&t, 0, &len);

	const int ring = len + 40;
	/* every start position whose 10 header bytes are still contiguous, so the tag body wraps */
	for (int start = 0; start + 10 <= ring; start += 7) {
		unsigned char *r = calloc(1, ring);
		for (int i = 0; i < len; i++)
			r[(start + i) % ring] = b[i];
		ID3_TAG tag;
		memset(&tag, 0, sizeof tag);
		int found = ID3V2_Parse(&tag, r, ring, start, NULL);
		CHECK_MSG(found == 1, "start %d: found %d", start, found);
		CHECK_MSG(!strcmp(tag.title, "Wrapped title that is long enough to cross the end"), "start %d: title '%s'", start, tag.title);
		CHECK_MSG(!strcmp(tag.artist, "Wrapped artist"), "start %d: artist '%s'", start, tag.artist);
		CHECK_MSG(tag.track == 5, "start %d: track %d", start, tag.track);
		CHECK_MSG(!strcmp(tag.genre, "Rock"), "start %d: genre '%s'", start, tag.genre);
		free(r);
	}
	free(b);
}

TEST(known_defect_an_empty_text_frame_swallows_the_bytes_of_the_following_frames)
{
	/*
	 * A text frame whose body is empty (size 0) is legal. GetEntry() then reads the encoding byte from the
	 * next frame header ('T' = 0x54, i.e. "UTF-8" branch) and copies MIN(max, flen - start - 1) bytes, but
	 * `flen - start - 1` is unsigned and wraps to 0xFFFFFFFF, so up to 255 bytes of the following frames
	 * are copied into the title.
	 * Correct result: title == "". Flip this test when the code is fixed.
	 */
	TB t; tb_init(&t, 3);
	tb_frame(&t, "TIT2", "", 0);
	tb_text(&t, "TPE1", "Artist after empty title");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	parse(b, len, &tag, NULL);
	CHECK(strstr(tag.title, "PE1") != NULL);	/* the next frame's header leaked into the title (correct: title is empty) */
	free(b);
}

/*
 * The same defect is worse than "swallows the next frame": AddressSanitizer reports a heap-buffer-overflow READ
 * (Source/id3tag.c:146, _peek via GetEntry:267). With the wrapped length the copy loop runs up to 255 bytes, which
 * is past the end of the tag when the empty frame is the last thing in it, so bytes of unrelated heap memory end up
 * in tag.title. A crafted tag does this. On a guarded copy the over-read faults.
 * Correct result: title == "" and no read past `len`. When fixed, make this CHECK_SURVIVES.
 */
TEST(known_defect_an_empty_text_frame_at_the_end_of_the_tag_reads_past_the_buffer)
{
	TB t; tb_init(&t, 3);
	tb_frame(&t, "TIT2", "", 0);
	int len; unsigned char *b = tb_done(&t, 0, &len);
	unsigned char *g = avos_guarded_copy(b, len);
	REQUIRE_NOT_NULL(g);
	ID3_TAG tag;
	CHECK_FAULTS(parse(g, len, &tag, NULL));
	free(b);
}

/* ---- attached pictures ------------------------------------------------------------------------ */

/*
 * file_type.c of this build recognises no image type at all (no "image/*" mime, no JPG/PNG extension),
 * and ID3V2_Parse only extracts a picture whose mime type / image format file_type.c recognises.
 * The parser's mechanics (header decoding, description skipping, size limits, copying) are the same
 * for any recognised type, so the tests use a recognised one (MP3's mime "audio/mpeg" / extension "MP3")
 * as the declared type of the real JPEG/PNG bytes. known_defect_* below pins the real-world consequence.
 */
#define APIC_MIME "audio/mpeg"
#define APIC_FMT  "MP3"

static void tb_apic23(TB *t, const char *mime, const char *desc, const unsigned char *img, int imglen, const char *id)
{
	unsigned char *body = malloc(strlen(mime) + strlen(desc) + imglen + 8);
	int n = 0;
	body[n++] = 0;					/* encoding */
	memcpy(body + n, mime, strlen(mime) + 1); n += strlen(mime) + 1;
	body[n++] = 3;					/* picture type: front cover */
	memcpy(body + n, desc, strlen(desc) + 1); n += strlen(desc) + 1;
	memcpy(body + n, img, imglen); n += imglen;
	tb_frame(t, id, body, n);
	free(body);
}

TEST(known_defect_a_real_jpeg_or_png_cover_is_never_extracted_because_no_image_type_is_known)
{
	/*
	 * ID3 cover art is declared with "image/jpeg" / "image/png" (v2.3/2.4) or "JPG" / "PNG" (v2.2).
	 * file_type.c has no image entries, so get_file_type_from_mime_type()/..._from_ext() fail for all of
	 * them and the APIC branch of ID3V2_Parse can never deliver a real picture.
	 * Correct result: apic.valid == 1 and size == image size. Flip this test when image types are added.
	 */
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	for (int variant = 0; variant < 3; variant++) {
		TB t; tb_init(&t, variant == 2 ? 2 : 3);
		if (variant == 0) {
			tb_apic23(&t, "image/jpeg", "", jpg, jlen, "APIC");
		} else if (variant == 1) {
			int plen; unsigned char *png = slurp("media/frame.png", &plen);
			tb_apic23(&t, "image/png", "", png, plen, "APIC");
			free(png);
		} else {
			unsigned char *body = malloc(jlen + 8);
			memcpy(body, "\0JPG\3\0", 6);
			memcpy(body + 6, jpg, jlen);
			tb_frame(&t, "PIC", body, jlen + 6);
			free(body);
		}
		int len; unsigned char *b = tb_done(&t, 0, &len);
		APIC apic;
		memset(&apic, 0, sizeof apic);
		apic.buffer_size = 1 << 20;
		ID3_TAG tag;
		CHECK_MSG(parse(b, len, &tag, &apic) == 0, "variant %d unexpectedly extracted", variant);
		CHECK_MSG(apic.valid == 0, "variant %d: apic.valid", variant);
		free(b);
	}
	free(jpg);
}

TEST(v23_attached_picture_is_extracted_byte_for_byte_into_a_buffer_the_parser_allocates)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	TB t; tb_init(&t, 3);
	tb_apic23(&t, APIC_MIME, "Cover", jpg, jlen, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);

	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 2 * 1024 * 1024;		/* the cap the caller accepts, buffer NULL: please allocate */
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_INT(tag.apic, 1);
	CHECK_EQ_INT(tag.valid, 1);
	CHECK_EQ_INT(apic.valid, 1);
	CHECK_EQ_INT(apic.size, jlen);
	REQUIRE_NOT_NULL(apic.buffer);
	CHECK_EQ_MEM(apic.buffer, jpg, jlen);
	int mime_etype = -1;
	REQUIRE(get_file_type_from_mime_type(APIC_MIME, NULL, &mime_etype) == 0);
	CHECK_EQ_INT(apic.etype, mime_etype);
	free(apic.buffer);
	free(jpg);
	free(b);
}

TEST(v23_attached_picture_into_a_caller_supplied_buffer)
{
	int plen;
	unsigned char *png = slurp("media/frame.png", &plen);
	TB t; tb_init(&t, 4);
	tb_text(&t, "TIT2", "With art");
	tb_apic23(&t, APIC_MIME, "", png, plen, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);

	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer = calloc(1, plen + 100);
	apic.buffer_size = plen + 100;
	unsigned char *mine = apic.buffer;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK(apic.buffer == mine);			/* no reallocation of a supplied buffer */
	CHECK_EQ_INT(apic.buffer_size, plen + 100);
	CHECK_EQ_INT(apic.size, plen);
	CHECK_EQ_MEM(apic.buffer, png, plen);
	CHECK_EQ_STR(tag.title, "With art");
	int mime_etype = -1;
	REQUIRE(get_file_type_from_mime_type(APIC_MIME, NULL, &mime_etype) == 0);
	CHECK_EQ_INT(apic.etype, mime_etype);
	free(apic.buffer);
	free(png);
	free(b);
}

TEST(v22_pic_frame_is_extracted_using_the_three_letter_image_format)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	unsigned char *body = malloc(jlen + 16);
	int n = 0;
	body[n++] = 0;					/* encoding */
	memcpy(body + n, APIC_FMT, 3); n += 3;
	body[n++] = 3;					/* picture type */
	body[n++] = 0;					/* empty description */
	memcpy(body + n, jpg, jlen); n += jlen;

	TB t; tb_init(&t, 2);
	tb_frame(&t, "PIC", body, n);
	int len; unsigned char *b = tb_done(&t, 0, &len);

	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_INT(apic.valid, 1);
	CHECK_EQ_INT(apic.size, jlen);
	CHECK_EQ_MEM(apic.buffer, jpg, jlen);
	free(apic.buffer);
	free(body);
	free(jpg);
	free(b);
}

TEST(the_picture_description_is_skipped)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	TB t; tb_init(&t, 3);
	tb_apic23(&t, APIC_MIME, "A fairly long description of the picture", jpg, jlen, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_INT(apic.size, jlen);
	CHECK_EQ_MEM(apic.buffer, jpg, jlen);
	free(apic.buffer);
	free(jpg);
	free(b);
}

TEST(only_the_first_picture_is_kept)
{
	int jlen, plen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	unsigned char *png = slurp("media/frame.png", &plen);
	TB t; tb_init(&t, 3);
	tb_apic23(&t, APIC_MIME, "", jpg, jlen, "APIC");
	tb_apic23(&t, APIC_MIME, "", png, plen, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_INT(apic.size, jlen);
	CHECK_EQ_MEM(apic.buffer, jpg, jlen);
	free(apic.buffer);
	free(jpg); free(png);
	free(b);
}

TEST(pictures_are_ignored_when_the_caller_does_not_ask_for_them)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	TB t; tb_init(&t, 3);
	tb_apic23(&t, APIC_MIME, "", jpg, jlen, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, NULL), 0);	/* a picture alone does not count without an APIC request */
	CHECK_EQ_INT(tag.apic, 0);
	free(jpg);
	free(b);
}

TEST(a_picture_larger_than_the_callers_limit_is_not_extracted)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	TB t; tb_init(&t, 3);
	tb_apic23(&t, APIC_MIME, "", jpg, jlen, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = jlen - 1;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 0);
	CHECK_EQ_INT(apic.valid, 0);
	CHECK_NULL(apic.buffer);
	CHECK_EQ_INT(tag.apic, 0);

	/* exactly at the limit is fine */
	apic.buffer_size = jlen;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_INT(apic.valid, 1);
	free(apic.buffer);
	free(jpg);
	free(b);
}

TEST(a_picture_of_four_bytes_or_less_is_not_extracted)
{
	static const unsigned char tiny[4] = { 1, 2, 3, 4 };
	TB t; tb_init(&t, 3);
	tb_apic23(&t, APIC_MIME, "", tiny, 4, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 0);
	CHECK_EQ_INT(apic.valid, 0);
	free(b);

	static const unsigned char five[5] = { 1, 2, 3, 4, 5 };
	tb_init(&t, 3);
	tb_apic23(&t, APIC_MIME, "", five, 5, "APIC");
	b = tb_done(&t, 0, &len);
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_INT(apic.size, 5);
	free(apic.buffer);
	free(b);
}

TEST(a_picture_with_an_unknown_mime_type_or_image_format_is_not_extracted)
{
	static const unsigned char data[32] = { 9 };
	TB t; tb_init(&t, 3);
	tb_apic23(&t, "application/x-nothing", "", data, 32, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 0);
	CHECK_EQ_INT(apic.valid, 0);
	free(b);

	unsigned char body[40] = { 0, 'Z', 'Z', 'Z', 3, 0 };	/* v2.2 PIC with a bogus format */
	tb_init(&t, 2);
	tb_frame(&t, "PIC", body, sizeof body);
	b = tb_done(&t, 0, &len);
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 0);
	CHECK_EQ_INT(apic.valid, 0);
	free(b);
}

TEST(a_picture_whose_header_wraps_around_a_circular_buffer_is_still_extracted)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", "Ring");
	const int mime_off = t.n + 10 + 1;			/* where the mime string starts inside the tag */
	tb_apic23(&t, APIC_MIME, "d", jpg, 300, "APIC");
	int len; unsigned char *b = tb_done(&t, 0, &len);

	const int ring = len + 29;
	const int mime_len = (int)strlen(APIC_MIME) + 1;
	int wrapped_mime_starts = 0;
	for (int start = 0; start + 10 <= ring; start++) {
		int mpos = (start + mime_off) % ring;
		if (mpos + mime_len > ring)
			continue;				/* the mime string itself straddles the end: strlen() would run on */
		unsigned char *r = calloc(1, ring);
		for (int i = 0; i < len; i++)
			r[(start + i) % ring] = b[i];
		if (start + mime_off >= ring)
			wrapped_mime_starts++;
		APIC apic;
		memset(&apic, 0, sizeof apic);
		apic.buffer_size = 1 << 20;
		ID3_TAG tag;
		memset(&tag, 0, sizeof tag);
		int found = ID3V2_Parse(&tag, r, ring, start, &apic);
		CHECK_MSG(found == 1 && apic.valid && apic.size == 300 && !memcmp(apic.buffer, jpg, 300),
			  "start %d: found %d valid %u size %u", start, found, apic.valid, apic.size);
		free(apic.buffer);
		free(r);
	}
	CHECK(wrapped_mime_starts > 0);
	free(jpg);
	free(b);
}

TEST(a_picture_does_not_disturb_the_text_frames_around_it)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", "Before");
	tb_apic23(&t, APIC_MIME, "", jpg, jlen, "APIC");
	tb_text(&t, "TPE1", "After");
	int len; unsigned char *b = tb_done(&t, 0, &len);
	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_STR(tag.title, "Before");
	CHECK_EQ_STR(tag.artist, "After");
	CHECK_EQ_INT(apic.valid, 1);
	free(apic.buffer);
	free(jpg);
	free(b);
}

/* ---- debug logging ----------------------------------------------------------------------------- */

TEST(parser_debug_logging_does_not_change_what_is_parsed)
{
	int jlen;
	unsigned char *jpg = slurp("media/frame.jpg", &jlen);
	TB t; tb_init(&t, 3);
	tb_text(&t, "TIT2", "Logged");
	tb_text(&t, "TLEN", "5000");
	tb_text(&t, "TRCK", "2");
	tb_text(&t, "TPOS", "1");
	tb_apic23(&t, APIC_MIME, "", jpg, jlen, "APIC");
	tb_apic23(&t, APIC_MIME, "", jpg, jlen, "APIC");	/* second one: the "ignored" branch */
	tb_apic23(&t, "image/x-weird", "", jpg, 32, "APIC");	/* no usable picture: the Dump() branch */
	tb_pad(&t, 16);
	int len; unsigned char *b = tb_done(&t, 0, &len);

	Debug[DBG_PARSER] = 1;
	APIC apic;
	memset(&apic, 0, sizeof apic);
	apic.buffer_size = 1 << 20;
	ID3_TAG tag;
	CHECK_EQ_INT(parse(b, len, &tag, &apic), 1);
	CHECK_EQ_STR(tag.title, "Logged");
	CHECK_EQ_INT(tag.msec, 5000);
	CHECK_EQ_INT(tag.track, 2);
	CHECK_EQ_INT(tag.discnumber, 1);
	CHECK_EQ_INT(apic.size, jlen);
	free(apic.buffer);

	/* unicode + utf8 + ISO under logging, a genre replacement, v1 and the id3 size */
	TB u; tb_init(&u, 3);
	static const unsigned short us[] = { 'o', 'k' };
	unsigned char body[8];
	int n = 0;
	body[n++] = 0xFF; body[n++] = 0xFE;
	n += utf16(body + n, us, 2, 0);
	tb_text_raw(&u, "TIT2", 1, body, n);
	tb_text_raw(&u, "TPE1", 3, "u8", 2);
	tb_text(&u, "TCON", "(13)");
	int ulen; unsigned char *ub = tb_done(&u, 0, &ulen);
	CHECK_EQ_INT(parse(ub, ulen, &tag, NULL), 1);
	CHECK_EQ_STR(tag.title, "ok");
	CHECK_EQ_STR(tag.genre, "Pop");
	CHECK_EQ_INT(ID3V2_GetSize(ub), ulen);
	char g[16] = "(13)";
	ID3_set_get_genre_from_ID(g);
	CHECK_EQ_STR(g, "Pop");

	unsigned char v1[128];
	memset(v1, 0, sizeof v1);
	memcpy(v1, "TAG", 3);
	CHECK_EQ_INT(ID3V1_Parse(&tag, v1), 0);
	Debug[DBG_PARSER] = 0;

	free(jpg); free(b); free(ub);
}

/* ---- ID3v1 --------------------------------------------------------------------------------------- */

static void make_v1(unsigned char *v, const char *title, const char *artist, const char *album,
		    const char *year, const char *comment, int track, int genre)
{
	memset(v, 0, 128);
	memcpy(v, "TAG", 3);
	strncpy((char *)v + 3, title, 30);
	strncpy((char *)v + 33, artist, 30);
	strncpy((char *)v + 63, album, 30);
	memcpy(v + 93, year, 4);
	strncpy((char *)v + 97, comment, 30);
	if (track >= 0) { v[125] = 0; v[126] = (unsigned char)track; }
	v[127] = (unsigned char)genre;
}

TEST(v1_tag_is_decoded_field_by_field)
{
	unsigned char v[128];
	make_v1(v, "Song Title", "Some Artist", "Some Album", "2003", "nice one", 9, 13);
	ID3_TAG tag;
	memset(&tag, 0xFF, sizeof tag);			/* the parser clears the tag itself */
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	CHECK_EQ_STR(tag.title, "Song Title");
	CHECK_EQ_STR(tag.artist, "Some Artist");
	CHECK_EQ_STR(tag.album, "Some Album");
	CHECK_EQ_STR(tag.year, "2003");
	CHECK_EQ_STR(tag.comment, "nice one");
	CHECK_EQ_STR(tag.genre, "Pop");
	CHECK_EQ_INT(tag.track, 9);
	CHECK_EQ_INT(tag.id3_ver, 1);
	CHECK_EQ_INT(tag.valid, 1);
	CHECK_EQ_INT(tag.apic, 0);
	CHECK_EQ_INT(tag.msec, 0);
	CHECK_EQ_INT(tag.discnumber, 0);
}

TEST(v1_fields_of_exactly_thirty_characters_are_not_overrun)
{
	unsigned char v[128];
	char thirty[31];
	memset(thirty, 'T', 30); thirty[30] = 0;
	make_v1(v, thirty, thirty, thirty, "1999", thirty, -1, 0);
	ID3_TAG tag;
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	CHECK_EQ_INT(strlen(tag.title), 30);
	CHECK_EQ_INT(strlen(tag.artist), 30);
	CHECK_EQ_INT(strlen(tag.album), 30);
	CHECK_EQ_INT(strlen(tag.comment), 30);
	CHECK_EQ_STR(tag.genre, "Blues");		/* genre byte 0 is "Blues" */
}

TEST(v1_track_exists_only_when_the_comment_ends_with_a_zero_byte_marker)
{
	unsigned char v[128];
	make_v1(v, "t", "a", "b", "2000", "c", 5, 1);
	ID3_TAG tag;
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	CHECK_EQ_INT(tag.track, 5);			/* v1.1 */

	v[125] = 'x';					/* 30 char comment: v1.0, byte 126 is not a track */
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	CHECK_EQ_INT(tag.track, 0);
}

TEST(v1_genre_out_of_range_gives_an_empty_genre)
{
	unsigned char v[128];
	make_v1(v, "t", "a", "b", "2000", "c", -1, 147);
	ID3_TAG tag;
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	CHECK_EQ_STR(tag.genre, "Synthpop");
	v[127] = 148;
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	CHECK_EQ_STR(tag.genre, "");
	v[127] = 255;					/* "no genre" */
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	CHECK_EQ_STR(tag.genre, "");
	CHECK_EQ_INT(tag.valid, 1);
}

TEST(v1_parse_refuses_a_buffer_without_the_tag_marker_and_clears_the_tag)
{
	unsigned char v[128];
	make_v1(v, "t", "a", "b", "2000", "c", 1, 1);
	v[0] = 'X';
	ID3_TAG tag;
	memset(&tag, 0xFF, sizeof tag);
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 1);
	CHECK_EQ_INT(tag.valid, 0);
	CHECK_EQ_STR(tag.title, "");
	CHECK_EQ_INT(tag.id3_ver, 0);
}

TEST(show_tag_prints_without_modifying_the_tag)
{
	unsigned char v[128];
	make_v1(v, "Shown", "Artist", "Album", "2010", "c", 3, 17);
	ID3_TAG tag, copy;
	CHECK_EQ_INT(ID3V1_Parse(&tag, v), 0);
	copy = tag;
	ID3_show_tag(&tag);
	CHECK_EQ_MEM(&tag, &copy, sizeof tag);
}

/* ---- real encoder output -------------------------------------------------------------------------- */

TEST(the_tag_written_by_ffmpeg_into_sine_id3_mp3_is_read_back_like_libavformat_reads_it)
{
	int size;
	unsigned char *file = slurp("media/sine_id3.mp3", &size);
	ID3_TAG tag;
	CHECK_EQ_INT(ID3V2_GetSize(file) > 10, 1);
	CHECK_EQ_INT(parse(file, size, &tag, NULL), 1);

	AVFormatContext *ic = NULL;
	REQUIRE(avformat_open_input(&ic, fixture_path("media/sine_id3.mp3"), NULL, NULL) == 0);
	const AVDictionaryEntry *t = av_dict_get(ic->metadata, "title", NULL, 0);
	const AVDictionaryEntry *a = av_dict_get(ic->metadata, "artist", NULL, 0);
	const AVDictionaryEntry *l = av_dict_get(ic->metadata, "album", NULL, 0);
	REQUIRE(t && a && l);
	CHECK_EQ_STR(tag.title, t->value);
	CHECK_EQ_STR(tag.artist, a->value);
	CHECK_EQ_STR(tag.album, l->value);
	CHECK_EQ_STR(tag.title, "Test Tone");
	avformat_close_input(&ic);

	/* the size says where the audio starts: a sync word (0xFF 0xEx) is found right there or soon after */
	unsigned v2 = ID3V2_GetSize(file);
	CHECK(v2 < (unsigned)size);
	free(file);
}

TEST(an_untagged_mp3_has_no_id3v2_tag)
{
	int size;
	unsigned char *file = slurp("media/sine.mp3", &size);
	ID3_TAG tag;
	/* sine.mp3 is written by the same encoder but without metadata: LAME still writes a tag header of
	   zero frames or none; either way no field is produced */
	CHECK_EQ_INT(parse(file, size, &tag, NULL), 0);
	CHECK_EQ_INT(tag.valid, 0);
	CHECK_EQ_STR(tag.title, "");
	free(file);
}
