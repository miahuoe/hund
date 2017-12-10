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

#ifndef FILE_VIEW_H
#define FILE_VIEW_H

#include "file.h"
#include "utf8.h"

struct file_view {
	char wd[PATH_MAX];
	struct file_record** file_list;
	fnum_t num_files;
	fnum_t num_hidden;
	fnum_t selection;
	sorting_foo sorting;
	bool show_hidden;
};

bool hidden(const struct file_view* const, const fnum_t);
bool visible(const struct file_view* const, const fnum_t);

void next_entry(struct file_view*);
void first_entry(struct file_view*);
void prev_entry(struct file_view*);
void last_entry(struct file_view*);

void delete_file_list(struct file_view*);
bool file_on_list(struct file_view*, const utf8* const);
void file_highlight(struct file_view*, const utf8* const);

bool file_find(struct file_view*, const char* const, fnum_t, fnum_t);

int file_view_enter_selected_dir(struct file_view*);
int file_view_up_dir(struct file_view*);

void file_view_afterdel(struct file_view*);
void file_view_toggle_hidden(struct file_view*);

utf8* file_view_path_to_selected(struct file_view*);

#endif
