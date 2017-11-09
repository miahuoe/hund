/*
 *  Copyright (C) 2017 by Michał Czarnecki <czarnecky@va.pl>
 *
 *  This file is part of the Hund.
 *
 *  The Hund is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  The Hund is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "include/utf8.h"

/* CodePoint To Bytes */
void utf8_cp2b(utf8* const b, codepoint_t cp) {
	if (cp < (codepoint_t) 0x80) {
		b[0] = 0x7f & cp;
	}
	else if (cp < (codepoint_t) 0x0800) {
		b[0] = 0xc0 | ((cp >> 6) & 0x1f);
		b[1] = 0x80 | (cp & 0x3f);
	}
	else if (cp < (codepoint_t) 0x010000) {
		b[0] = 0xe0 | ((cp >> 12) & 0x000f);
		b[1] = 0x80 | ((cp >> 6) & 0x003f);
		b[2] = 0x80 | (cp & 0x003f);
	}
	else if (cp < (codepoint_t) 0x0200000) {
		b[0] = 0xf0 | ((cp >> 18) & 0x000007);
		b[1] = 0x80 | ((cp >> 12) & 0x00003f);
		b[2] = 0x80 | ((cp >> 6) & 0x00003f);
		b[3] = 0x80 | (cp & 0x00003f);
	}
	else {

	}
}

/* Bytes To CodePoint */
codepoint_t utf8_b2cp(const utf8* const b) {
	codepoint_t cp = 0;
	if ((b[0] & 0x80) == 0) {
		cp = b[0];
	}
	else if ((b[0] & 0xe0) == 0xc0) {
		cp |= (b[0] & 0x1f);
		cp <<= 6;
		cp |= (b[1] & 0x3f);
	}
	else if ((b[0] & 0xf0) == 0xe0) {
		cp |= (b[0] & 0x0f);
		cp <<= 6;
		cp |= (b[1] & 0x3f);
		cp <<= 6;
		cp |= (b[2] & 0x3f);
	}
	else if ((b[0] & 0xf8) == 0xf0) {
		cp |= (b[0] & 0x07);
		cp <<= 6;
		cp |= (b[1] & 0x3f);
		cp <<= 6;
		cp |= (b[2] & 0x3f);
		cp <<= 6;
		cp |= (b[3] & 0x3f);
	}
	else {

	}
	return cp;
}

/* Glyph To Number of Bytes
 * how long the current glyph is (in bytes)
 * returns 0 if initial byte is invalid
 */
size_t utf8_g2nb(const utf8* const g) {
	if (!*g) return 0;
	/* It's simple, really
	 * I'm looking at top 5 bits (32 possibilities) of initial byte
	 * 00001xxx -> 1 byte
	 * 00010xxx -> 1 byte
	 * 00011xxx -> 1 byte
	 * 16 x 1 byte
	 * 01111xxx -> 1 byte
	 * 10000xxx -> invalid
	 * 8 x invalid
	 * 10111xxx -> invalid
	 * 11000xxx -> 2 bytes
	 * 4 x 2 bytes
	 * 11011xxx -> 2 bytes
	 * 11100xxx -> 3 bytes
	 * 2 x 3 bytes
	 * 11101xxx -> 3 bytes
	 * 11110xxx -> 4 bytes
	 * 1 x 4 bytes
	 * 11111xxx -> invalid (see impementation)
	 */
	// Top 5 bits To Length
	static const char t2l[32] = {
		1, 1, 1, 1, 1, 1, 1, 1, //00000xxx - 00111xxx
		1, 1, 1, 1, 1, 1, 1, 1, //01000xxx - 01111xxx
		0, 0, 0, 0, 0, 0, 0, 0, //10000xxx - 10111xxx
		2, 2, 2, 2, 3, 3, 4, 0  //11000xxx - 11111xxx
	};
	/* This cast is very important */
	return t2l[(unsigned char)(*g) >> 3];
}

/* CodePoint To Number of Bytes */
size_t utf8_cp2nb(codepoint_t cp) {
	if (cp < (codepoint_t) 0x80) return 1;
	if (cp < (codepoint_t) 0x0800) return 2;
	if (cp < (codepoint_t) 0x010000) return 3;
	if (cp < (codepoint_t) 0x200000) return 4;
	return 0;
}

/* apparent width */
size_t utf8_width(const utf8* const b) {
	size_t g = 0;
	const utf8* p = b;
	size_t s;
	while (*p && (s = utf8_g2nb(p)) != 0) {
		p += s;
		g += 1;
	}
	return g;
}

/* Calculates how much bytes take first g glyphs */
size_t utf8_slice_length(const utf8* const b, size_t g) {
	size_t r = 0;
	for (size_t i = 0; i < g && *(b+r); ++i) {
		r += utf8_g2nb(b+r);
	}
	return r;
}

/* Number of glyphs till some address in that string */
size_t utf8_ng_till(const utf8* const a, const utf8* const b) {
	const utf8* t = a;
	size_t g = 0;
	while (b - t > 0) {
		t += utf8_g2nb(t);
		g += 1;
	}
	return g;
}

bool utf8_validate(const utf8* const b) {
	size_t bl = strlen(b);
	const utf8* t = b;
	size_t i = 0;
	while (i < bl) {
		size_t s = utf8_g2nb(t+i);
		if (!s) break;
		i += s;
	}
	return i == bl;
}

void utf8_insert(utf8* const a, utf8* const b, size_t pos) {
	const size_t bl = strlen(b);
	utf8* t = a;
	size_t i = 0;
	for (; i < pos; ++i) {
		t += utf8_g2nb(t);
	}
	memmove(t+bl, t, strlen(t));
	memcpy(t, b, bl);
}

/* remove glyph at index
 */
void utf8_remove(utf8* const a, size_t j) {
	utf8* t = a;
	for (size_t i = 0; i < j; ++i) {
		t += utf8_g2nb(t);
	}
	size_t rl = utf8_g2nb(t); // Removed glyph Length
	memmove(t, t+rl, strlen(t));
}