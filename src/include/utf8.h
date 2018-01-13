/*
 *  Copyright (C) 2017-2018 by Michał Czarnecki <czarnecky@va.pl>
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

#ifndef UTF8_H
#define UTF8_H

#include <string.h>
#include <stdbool.h>

typedef unsigned int codepoint_t;

void utf8_cp2b(char* const, const codepoint_t);
codepoint_t utf8_b2cp(const char* const);
size_t utf8_g2nb(const char* const);
size_t utf8_cp2nb(const codepoint_t);
size_t utf8_g2w(const codepoint_t);
size_t utf8_width(const char*);
size_t utf8_w2nb(const char* const, size_t);
size_t utf8_wtill(const char*, const char* const);
bool utf8_validate(const char* const);

void utf8_insert(char*, const char* const, const size_t);
void utf8_remove(char* const, const size_t);

void cut_unwanted(const char*, char*, const char, size_t);

struct range {
	codepoint_t s, e;
};

static const struct range zero_width[] = {
	{ 0x00, 0x00 },
	{ 0x200b, 0x200b },
	{ 0x1160, 0x11ff },
	// TODO
};
static const size_t zero_width_len = sizeof(zero_width)/sizeof(struct range);

/*static const struct range double_width[] = {
 *	// TODO
	{ 0xffffff, 0xffffff }, // temponary
};
static const size_t double_width_len = sizeof(double_width)/sizeof(struct range);*/

bool cp_in(const struct range* const, const size_t l, const codepoint_t);

#endif
