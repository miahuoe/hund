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

#ifndef _DEFAULT_SOURCE
	#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <limits.h>

#include "ui.h"
#include "task.h"

/*
 * GENERAL TODO
 * - Dir scanning via task?
 * - Creating links: offer relative or absolute link path
 * - Keybindings must make more sense
 * - cache login/group names or entire /etc/passwd
 * - Use piped less more
 * - Display input buffer
 * - Jump to file pointed by symlink (and return)
 * - Change symlink target
 * - simplify empty dir handling - maybe show . or .. ?
 */

static char* ed[] = {"$VISUAL", "$EDITOR", "vi", NULL};
static char* pager[] = {"$PAGER", "less", NULL};
static char* sh[] = {"$SHELL", "sh", NULL};
static char* opener[] = {"$OPEN", NULL};

char* xgetenv(char* q[]) {
	char* r = NULL;
	while (*q && !r) {
		if (**q == '$') {
			r = getenv(*q+1);
		}
		else {
			return *q;
		}
		q += 1;
	}
	return r;
}

int pager_spawn(int* const fd, pid_t* const pid) {
	int pipefd[2];
	if (pipe(pipefd) || (*pid = fork()) == -1) {
		return errno;
	}
	if (*pid == 0) {
		close(pipefd[1]);
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);
		char* arg[] = { xgetenv(pager), "-", NULL };
		execvp(arg[0], arg);
		exit(EXIT_FAILURE);
	}
	*fd = pipefd[1];
	return 0;
}

void pager_done(int fd, const pid_t pid) {
	int status;
	close(fd);
	waitpid(pid, &status, 0);
}

struct mark_path {
	size_t len;
	char data[];
};

struct marks {
	struct mark_path* AZ['Z'-'A'+1];
	struct mark_path* az['z'-'a'+1];
};

void marks_free(struct marks* const);
struct mark_path** marks_get(struct marks* const, const char);
struct mark_path* marks_set(struct marks* const, const char,
		const char* const, size_t, const char* const, size_t);
void marks_input(struct ui* const, struct marks* const);
void marks_jump(struct ui* const, struct marks* const);

void marks_free(struct marks* const M) {
	struct mark_path** mp;
	for (char c = ' '; c < 0x7f; ++c) { // TODO
		mp = marks_get(M, c);
		if (!mp || !*mp) continue;
		free(*mp);
		*mp = NULL;
	}
	memset(M, 0, sizeof(struct marks));
}

struct mark_path** marks_get(struct marks* const M, const char C) {
	if (UPPERCASE(C)) {
		return &M->AZ[C-'A'];
	}
	else if (LOWERCASE(C)) {
		return &M->az[C-'a'];
	}
	return NULL;
}

struct mark_path* marks_set(struct marks* const M, const char C,
		const char* const wd, size_t wdlen,
		const char* const n, size_t nlen) {
	struct mark_path** mp = marks_get(M, C);
	if (!mp) return NULL;
	*mp = realloc(*mp, sizeof(struct mark_path)+wdlen+1+nlen+1);
	memcpy((*mp)->data, wd, wdlen+1);
	memcpy((*mp)->data+wdlen+1, n, nlen+1);
	(*mp)->data[wdlen] = '/';
	(*mp)->len = wdlen+1+nlen;
	return *mp;
}

void marks_input(struct ui* const i, struct marks* const m) {
	const struct input in = get_input(-1);
	if (in.t != I_UTF8) return;
	const struct file* f = hfr(i->pv);
	if (!f) return;
	marks_set(m, in.utf[0], i->pv->wd,
		strnlen(i->pv->wd, PATH_MAX_LEN), f->name, f->nl);
}

void marks_jump(struct ui* const i, struct marks* const m) {
	const struct input in = get_input(-1);
	if (in.t != I_UTF8) return;
	const char C = in.utf[0];
	struct mark_path** mp = marks_get(m, C);
	if (!mp || !*mp) {
		failed(i, "jump to mark", "Unknown mark");
		return;
	}
	if (access((*mp)->data, F_OK)) {
		int e = errno;
		failed(i, "jump to mark", strerror(e));
		return;
	}
	if (UPPERCASE(C)) {
		memcpy(i->pv->wd, (*mp)->data, (*mp)->len+1);
		i->pv->wdlen = (*mp)->len;
		if (ui_rescan(i, i->pv, NULL)) {
			first_entry(i->pv);
		}
	}
	else {
		char* const file = (*mp)->data+current_dir_i((*mp)->data);
		const size_t flen = strlen(file);
		const size_t wdlen = (*mp)->len-flen-1;
		memcpy(i->pv->wd, (*mp)->data, wdlen);
		i->pv->wdlen = wdlen;
		if (ui_rescan(i, i->pv, NULL)) {
			file_highlight(i->pv, file);
		}
	}
}

int marks_to_fd(struct marks* const m, const int fd) {
	struct mark_path** mp;
	for (int i = ' '; i < 0x7f; ++i) {
		if (!(mp = marks_get(m, i)) || !*mp) continue;
		char H[2] = { i, ' ' };
		if (write(fd, H, 2) == -1
		|| write(fd, (*mp)->data, (*mp)->len) == -1
		|| write(fd, "\n", 1) == -1) {
			return errno;
		}
	}
	return 0;
}

static inline void list_marks(struct ui* const i, struct marks* const m) {
	int fd, e;
	pid_t p;
	if ((e = pager_spawn(&fd, &p))
	|| (e = marks_to_fd(m, fd))) {
		failed(i, "pager", strerror(e));
		return;
	}
	pager_done(fd, p);
}

static int open_file_with(char* const p, char* const f) {
	if (!p) return 0;
	char* const arg[] = { p, f, NULL };
	return spawn(arg, 0);
}

static void open_help(struct ui* const i) {
	int e, fd = -1;
	pid_t p;
	if ((e = pager_spawn(&fd, &p))) {
		failed(i, "help", strerror(e));
		return;
	}
	help_to_fd(i, fd);
	pager_done(fd, p);
}

static int edit_list(struct string_list* const in,
		struct string_list* const out) {
	int err = 0, tmpfd;
	char tmpn[] = "/tmp/hund.XXXXXXXX";
	if ((tmpfd = mkstemp(tmpn)) == -1) return errno;
	if ((err = list_to_file(in, tmpfd))
	|| (err = open_file_with(xgetenv(ed), tmpn))
	|| (err = file_to_list(tmpfd, out))) {
		// Failed; One operation failed, the rest was skipped.
	}
	if (close(tmpfd) && !err) err = errno;
	if (unlink(tmpn) && !err) err = errno;
	return err;
}

static void open_selected_with(struct ui* const i, char* const w) {
	char* path;
	int err;
	if ((path = panel_path_to_selected(i->pv))) {
		if ((err = open_file_with(w, path))) {
			failed(i, "open", strerror(err));
		}
		free(path);
	}
}

static void cmd_find(struct ui* const i) {
	if (!i->pv->num_files) return;
	char t[NAME_BUF_SIZE];
	char* t_top = t;
	memset(t, 0, sizeof(t));
	memcpy(i->prch, "/", 2);
	i->prompt = t;
	int r;
	const fnum_t S = i->pv->selection;
	const fnum_t N = i->pv->num_files;
	struct input o;
	fnum_t s = 0; // Start
	fnum_t e = N-1; // End
	for (;;) {
		i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
		ui_draw(i);
		r = fill_textbox(i, t, &t_top, NAME_MAX_LEN, &o);
		if (!r) {
			break;
		}
		else if (r == -1) {
			i->pv->selection = S;
			break;
		}
		else if (r == 2 || t_top != t) {
			if (IS_CTRL(o, 'V')) {
				panel_select_file(i->pv);
				continue;
			}
			else if (IS_CTRL(o, 'N') && i->pv->selection < N-1) {
				s = i->pv->selection+1;
				e = N-1;
			}
			else if (IS_CTRL(o, 'P') && i->pv->selection > 0) {
				s = i->pv->selection-1;
				e = 0;
			}
		}
		file_find(i->pv, t, s, e);
	}
	i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
	i->prompt = NULL;
}

/*
 * Returns:
 * true - success and there are files to work with (skipping may empty list)
 * false - failure or aborted
 */
inline static bool _solve_name_conflicts_if_any(struct ui* const i,
		struct string_list* const s, struct string_list* const r) {
	static const char* const question = "Conflicting names.";
	static const struct select_option o[] = {
		{ KUTF8("r"), "rename" },
		{ KUTF8("m"), "merge" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("a"), "abort" }
	};
	int err;
	bool was_conflict = false;
	list_copy(r, s);
	while (conflicts_with_existing(i->sv, r)) {
		was_conflict = true;
		switch (ui_ask(i, question, o, 4)) {
		case 0: // rename
			if ((err = edit_list(r, r))) {
				failed(i, "editor", strerror(err));
				return false;
			}
			break;
		case 1: // merge
			// merge policy is chosen after estimating
			// (if there are any conflicts in the tree)
			list_free(r);
			return true;
		case 2: // skip
			remove_conflicting(i->sv, s);
			list_copy(r, s);
			return s->len != 0;
		case 3: // abort
		default:
			return false;
		}
	}
	if (!was_conflict) {
		list_free(r);
	}
	return true;
}

static void prepare_task(struct ui* const i, struct task* const t,
		const enum task_type tt) {
	static const struct select_option o[] = {
		{ KUTF8("y"), "yes" },
		{ KUTF8("n"), "no" },
	};
	struct string_list S = { NULL, 0 }; // Selected
	struct string_list R = { NULL, 0 }; // Renamed
	panel_selected_to_list(i->pv, &S);
	if (!S.len) return;
	if (tt & (TASK_MOVE | TASK_COPY)) {
		if (!_solve_name_conflicts_if_any(i, &S, &R)) {
			list_free(&S);
			list_free(&R);
			return;
		}
	}
	enum task_flags tf = 0;
	if (tt == TASK_CHMOD && S_ISDIR(i->perm[0])
	&& !ui_ask(i, "Apply recursively?", o, 2)) {
		tf |= TF_RECURSIVE_CHMOD;
	}
	task_new(t, tt, tf, i->pv->wd, i->sv->wd, &S, &R);
	if (tt == TASK_CHMOD) {
		t->chp = i->plus;
		t->chm = i->minus;
		t->cho = ((i->o[0] == i->o[1]) ? (uid_t)-1 : i->o[1]);
		t->chg = ((i->g[0] == i->g[1]) ? (gid_t)-1 : i->g[1]);
		chmod_close(i);
	}
}

/*
 * op = Old Path
 * np = New Path
 * on = Old Name
 * nn = New Name
 */
static int _rename(char* const op, size_t* const opl,
		char* const np, size_t* const npl,
		const struct string* const on,
		const struct string* const nn) {
	int err = 0;
	if ((err = pushd(op, opl, on->str, on->len))) {
		return err;
	}
	if ((err = pushd(np, npl, nn->str, nn->len))) {
		popd(np, npl);
		return err;
	}
	if (rename(op, np)) {
		err = errno;
	}
	popd(np, npl);
	popd(op, opl);
	return err;
}

/*
 */
static int rename_trivial(const char* const wd, const size_t wdl,
		struct string_list* const S, struct string_list* const R) {
	int err = 0;
	char* op = malloc(wdl+1+NAME_BUF_SIZE);
	size_t opl = wdl;
	if (!op) {
		return ENOMEM;
	}
	char* np = malloc(wdl+1+NAME_BUF_SIZE);
	size_t npl = wdl;
	if (!np) {
		free(op);
		return ENOMEM;
	}
	memcpy(op, wd, wdl+1);
	memcpy(np, wd, wdl+1);
	for (fnum_t f = 0; f < S->len; ++f) {
		if (!S->arr[f]) {
			continue;
		}
		if ((err = _rename(op, &opl, np, &npl, S->arr[f], R->arr[f]))) {
			break;
		}
	}
	free(op);
	free(np);
	return err;
}

static int rename_interdependent(const char* const wd, const size_t wdl,
		struct string_list* const N,
		struct assign* const A, const fnum_t Al) {
	int err = 0;

	char* op = malloc(wdl+1+NAME_BUF_SIZE);
	size_t opl = wdl;
	memcpy(op, wd, wdl+1);

	char* np = malloc(wdl+1+NAME_BUF_SIZE);
	size_t npl = wdl;
	memcpy(np, wd, wdl+1);

	static const char* const tmpn = ".hund.rename.tmpdir.";
	size_t tmpnl = strlen(tmpn);
	struct string* tn = malloc(sizeof(struct string)+tmpnl+8+1);
	tn->len = tmpnl+8;
	snprintf(tn->str, tmpnl+8+1, "%s%08x", tmpn, getpid());

	fnum_t tc; // Temponary file Content
	for (;;) {
		fnum_t i = 0;
		while (i < Al && A[i].to == (fnum_t)-1) {
			i += 1;
		}
		if (i == Al) break;

		tc = A[i].to;
		const fnum_t tv = A[i].to;
		if ((err = _rename(op, &opl, np, &npl, N->arr[A[i].from], tn))) {
			break;
		}
		fnum_t from = A[i].from;
		do {
			fnum_t j = (fnum_t)-1;
			for (fnum_t f = 0; f < Al; ++f) {
				if (A[f].to == from) {
					j = f;
					break;
				}
			}
			if (j == (fnum_t)-1) break;
			if ((err = _rename(op, &opl, np, &npl,
				N->arr[A[j].from], N->arr[A[j].to]))) {
				free(op);
				free(np);
				free(tn);
				return err;
			}
			from = A[j].from;
			A[j].from = A[j].to = (fnum_t)-1;
		} while (from != tv);
		if ((err = _rename(op, &opl, np, &npl, tn, N->arr[tc]))) {
			break;
		}
		A[i].from = A[i].to = (fnum_t)-1;
	}
	free(op);
	free(np);
	free(tn);
	return err;
}

static void cmd_rename(struct ui* const i) {
	int err;
	struct string_list S = { NULL, 0 }; // Selected files
	struct string_list R = { NULL, 0 }; // Renamed files
	struct string_list N = { NULL, 0 };
	struct assign* a = NULL;
	fnum_t al = 0;
	static const struct select_option o[] = {
		{ KUTF8("y"), "yes" },
		{ KUTF8("n"), "no" },
		{ KUTF8("a"), "abort" }
	};
	bool ok = true;
	panel_selected_to_list(i->pv, &S);
	do {
		if ((err = edit_list(&S, &R))) {
			failed(i, "edit", strerror(err));
			list_free(&S);
			list_free(&R);
			return;
		}
		const char* msg = "There are conflicts. Retry?";
		if (blank_lines(&R)) {
			msg = "File contains blank lines. Retry?";
			ok = false;
		}
		else if (S.len > R.len) {
			msg = "File does not contain enough lines. Retry?";
			ok = false;

		}
		else if (S.len < R.len) {
			msg = "File contains too much lines. Retry?";
			ok = false;
		}
		if ((!ok || !(ok = rename_prepare(i->pv, &S, &R, &N, &a, &al)))
		&& ui_ask(i, msg, o, 2) == 1) {
			list_free(&S);
			list_free(&R);
			return;
		}
	} while (!ok);
	if ((err = rename_trivial(i->pv->wd, i->pv->wdlen, &S, &R))
	|| (err = rename_interdependent(i->pv->wd, i->pv->wdlen, &N, a, al))) {
		failed(i, "rename", strerror(err));
	}
	list_free(&S);
	free(a);
	if (ui_rescan(i, i->pv, NULL)) {
		select_from_list(i->pv, &R);
		select_from_list(i->pv, &N);
	}
	list_free(&N);
	list_free(&R);
}

inline static void cmd_cd(struct ui* const i) {
	// TODO
	// TODO buffers
	int err = 0;
	struct stat s;
	char* path = malloc(PATH_BUF_SIZE);
	size_t pathlen = i->pv->wdlen;
	memcpy(path, i->pv->wd, pathlen+1);
	char* cdp = calloc(PATH_BUF_SIZE, sizeof(char));
	if (prompt(i, cdp, cdp, PATH_MAX_LEN)
	|| (err = cd(path, &pathlen, cdp, strnlen(cdp, PATH_MAX_LEN)))
	|| (access(path, F_OK) ? (err = errno) : 0)
	|| (stat(path, &s) ? (err = errno) : 0)
	|| (err = ENOTDIR, !S_ISDIR(s.st_mode))) {
		if (err) failed(i, "cd", strerror(err));
	}
	else {
		xstrlcpy(i->pv->wd, path, PATH_BUF_SIZE);
		i->pv->wdlen = pathlen;
		ui_rescan(i, i->pv, NULL);
	}
	free(path);
	free(cdp);
}

inline static void cmd_mkdir(struct ui* const i) {
	int err;
	struct string_list F = { NULL, 0 };
	const size_t wdl = strnlen(i->pv->wd, PATH_MAX_LEN);
	char* const P = malloc(wdl+1+NAME_BUF_SIZE);
	memcpy(P, i->pv->wd, wdl+1);
	size_t Pl = wdl;
	if ((err = edit_list(&F, &F))) {
		failed(i, "mkdir", strerror(err));
	}
	for (fnum_t f = 0; f < F.len; ++f) {
		if (!F.arr[f]) continue;
		if (memchr(F.arr[f]->str, '/', F.arr[f]->len+1)) {
			failed(i, "mkdir", "name contains '/'");
		}
		else if ((err = pushd(P, &Pl, F.arr[f]->str, F.arr[f]->len))
		|| (mkdir(P, MKDIR_DEFAULT_PERM) ? (err = errno) : 0)) {
			failed(i, "mkdir", strerror(err));
		}
		popd(P, &Pl);
	}
	free(P);
	list_free(&F);
	ui_rescan(i, i->pv, NULL);
}

inline static void cmd_change_sorting(struct ui* const i) {
	// TODO cursor

	char old[FV_ORDER_SIZE];
	memcpy(old, i->pv->order, FV_ORDER_SIZE);
	char* buf = i->pv->order;
	char* top = i->pv->order + strlen(buf);
	int r;
	i->mt = MSG_INFO;
	xstrlcpy(i->msg, "-- SORT --", MSG_BUFFER_SIZE);
	i->dirty |= DIRTY_BOTTOMBAR | DIRTY_STATUSBAR;
	ui_draw(i);
	for (;;) {
		r = fill_textbox(i, buf, &top, FV_ORDER_SIZE, NULL);
		if (!r) break;
		else if (r == -1) {
			memcpy(i->pv->order, old, FV_ORDER_SIZE);
			return;
		}
		i->mt = MSG_INFO;
		i->dirty |= DIRTY_BOTTOMBAR | DIRTY_STATUSBAR;
		ui_draw(i);
	}
	for (size_t j = 0; j < FV_ORDER_SIZE; ++j) {
		bool v = false;
		for (size_t k = 0; k < strlen(compare_values); ++k) {
			v = v || (compare_values[k] == i->pv->order[j]);
		}
		if (!v) {
			i->pv->order[j] = 0;
		}
	}
	for (size_t j = 0; j < FV_ORDER_SIZE; ++j) {
		if (i->pv->order[0]) continue;
		memmove(&i->pv->order[0], &i->pv->order[1], FV_ORDER_SIZE-1);
		i->pv->order[FV_ORDER_SIZE-1] = 0;
	}
	panel_sorting_changed(i->pv);
}

static void cmd_mklnk(struct ui* const i) {
	// TODO conflicts
	//char tmpn[] = "/tmp/hund.XXXXXXXX";
	//int tmpfd = mkstemp(tmpn);
	int err;
	struct string_list sf = { NULL, 0 };
	panel_selected_to_list(i->pv, &sf);
	size_t target_l = strnlen(i->pv->wd, PATH_MAX_LEN);
	size_t slpath_l = strnlen(i->sv->wd, PATH_MAX_LEN);
	char* target = malloc(target_l+1+NAME_BUF_SIZE);
	char* slpath = malloc(slpath_l+1+NAME_BUF_SIZE);
	memcpy(target, i->pv->wd, target_l+1);
	memcpy(slpath, i->sv->wd, slpath_l+1);
	for (fnum_t f = 0; f < sf.len; ++f) {
		pushd(target, &target_l, sf.arr[f]->str, sf.arr[f]->len);
		pushd(slpath, &slpath_l, sf.arr[f]->str, sf.arr[f]->len);
		if (symlink(target, slpath)) {
			err = errno;
			failed(i, "mklnk", strerror(err));
		}
		popd(target, &target_l);
		popd(slpath, &slpath_l);
	}
	free(target);
	free(slpath);
	//close(tmpfd);
	//unlink(tmpn);
	ui_rescan(i, i->sv, NULL);
	select_from_list(i->sv, &sf);
	list_free(&sf);
}

static void cmd_quick_chmod_plus_x(struct ui* const i) {
	char* path = NULL;
	struct stat s;
	if (!(path = panel_path_to_selected(i->pv))) return;
	if (stat(path, &s) || !S_ISREG(s.st_mode)) return;
	if (chmod(path, s.st_mode | (S_IXUSR | S_IXGRP | S_IXOTH))) {
		int e = errno;
		failed(i, "chmod", strerror(e));
	}
	ui_rescan(i, i->pv, NULL);
}

static void interpreter(struct ui* const i, struct task* const t,
		struct marks* const m, char* const line, size_t linesize) {
	/* TODO document it */
	static char* anykey = \
		"; read -n1 -r -p \"Press any key to continue...\" key\n"
		"if [ \"$key\" != '' ]; then echo; fi";
	int e;
	(void)(t);
	const size_t line_len = strlen(line);
	if (!line[0] || line[0] == '\n' || line[0] == '#') {
		return;
	}
	if (!strcmp(line, "q")) {
		i->run = false;
	}
	else if (!strcmp(line, "h") || !strcmp(line, "help")) {
		open_help(i);
	}
	else if (!strcmp(line, "+x")) {
		i->dirty |= DIRTY_STATUSBAR; // TODO
		cmd_quick_chmod_plus_x(i);
	}
	else if (!strcmp(line, "lm")) {
		list_marks(i, m);
	}
	else if (!strcmp(line, "sh")) {
		if (chdir(i->pv->wd)) {
			e = errno;
			failed(i, "chdir", strerror(e));
			return;
		}
		char* const arg[] = { xgetenv(sh), "-i", NULL };
		spawn(arg, 0);
	}
	else if (!memcmp(line, "sh ", 3)) {
		if (chdir(i->pv->wd)) {
			e = errno;
			failed(i, "chdir", strerror(e));
			return;
		}
		xstrlcpy(line+line_len, anykey, linesize-line_len);
		char* const arg[] = { xgetenv(sh), "-i", "-c", line+3, NULL };
		spawn(arg, 0);
	}
	else if (!memcmp(line, "o ", 2)) {
		open_selected_with(i, line+2);
	}
	else if (!memcmp(line, "mark ", 5)) { // TODO
		if (line[6] != ' ') {
			failed(i, "mark", ""); // TODO
			return;
		}
		char* path = line+7;
		const size_t f = current_dir_i(path);
		*(path+f-1) = 0;
		const size_t wdl = strlen(path);
		const size_t fl = strlen(path+f);
		if (!marks_set(m, line[5], path, wdl, path+f, fl)) {
			failed(i, "mark", ""); // TODO
		}
	}
	else if (!memcmp(line, "map ", 4)) {
		// ...
	}
	else if (!memcmp(line, "set ", 4)) {
		// ...
	}
	else if (!strcmp(line, "noh") || !strcmp(line, "nos")) {
		i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR;
		panel_unselect_all(i->pv);
	}
	else {
		failed(i, "interpreter", "Unrecognized command"); // TODO
	}
}

static void _perm(struct ui* const i, const bool unset, const int mask) {
	mode_t* m[2] = { &i->plus, &i->minus, };
	mode_t* tmp;
	bool minus;
	struct input in = get_input(i->timeout);
	if (in.t != I_UTF8) return;
	#define REL(M) do { \
		*m[0] |= (mask & (M)); \
		*m[1] &= ~(mask & (M)); \
	} while (0);
	#define SET(M) do { \
		*m[0] = (~mask & *m[0]) | (mask & (M)); \
		*m[1] = (~mask & *m[1]) | (mask & ~(M)); \
	} while (0);
	minus = in.utf[0] == '-';
	if (in.utf[0] == '+' || in.utf[0] == '-') {
		in = get_input(i->timeout);
		if (in.t != I_UTF8) return;
		if (unset || minus) {
			tmp = m[0];
			m[0] = m[1];
			m[1] = tmp;
		}
		switch (in.utf[0]) {
		case '0': REL(00000); break;
		case 'x':
		case '1': REL(00111); break;
		case 'w':
		case '2': REL(00222); break;
		case '3': REL(00333); break;
		case 'r':
		case '4': REL(00444); break;
		case '5': REL(00555); break;
		case '6': REL(00666); break;
		case '7': REL(00777); break;
		case 't': REL(07000); break;
		}
	}
	else {
		if (unset) {
			tmp = m[0];
			m[0] = m[1];
			m[1] = tmp;
		}
		switch (in.utf[0]) {
		case '0': SET(00000); break;
		case '1': SET(00111); break;
		case '2': SET(00222); break;
		case '3': SET(00333); break;
		case '4': SET(00444); break;
		case '5': SET(00555); break;
		case '6': SET(00666); break;
		case '7': SET(00777); break;
		case 'r': REL(00444); break;
		case 'w': REL(00222); break;
		case 'x': REL(00111); break;
		case 't': REL(07000); break;
		}
	}
	#undef REL
	#undef SET
}

static void chg_column(struct ui* const i) {
	xstrlcpy(i->msg, "-- COLUMN --", MSG_BUFFER_SIZE);
	i->mt = MSG_INFO;
	i->dirty |= DIRTY_BOTTOMBAR;
	ui_draw(i);
	struct input in = get_input(i->timeout);
	if (in.t != I_UTF8) return;
	switch (in.utf[0]) {
	case 't': i->pv->column = COL_NONE; break;
	case 'i': i->pv->column = COL_INODE; break;
	case 'S': i->pv->column = COL_LONGSIZE; break;
	case 's': i->pv->column = COL_SHORTSIZE; break;
	case 'P': i->pv->column = COL_LONGPERM; break;
	case 'p': i->pv->column = COL_SHORTPERM; break;
	case 'u': i->pv->column = COL_UID; break;
	case 'U': i->pv->column = COL_USER; break;
	case 'g': i->pv->column = COL_GID; break;
	case 'G': i->pv->column = COL_GROUP; break;
	case 'A': i->pv->column = COL_LONGATIME; break;
	case 'a': i->pv->column = COL_SHORTATIME; break;
	case 'C': i->pv->column = COL_LONGCTIME; break;
	case 'c': i->pv->column = COL_SHORTCTIME; break;
	case 'M': i->pv->column = COL_LONGMTIME; break;
	case 'm': i->pv->column = COL_SHORTMTIME; break;
	default: break;
	}
	i->dirty |= DIRTY_PANELS | DIRTY_BOTTOMBAR;
}

static void cmd_command(struct ui* const i, struct task* const t,
		struct marks* const m) {
	(void)(t);
	char cmd[1024];
	memset(cmd, 0, sizeof(cmd));
	char* t_top = cmd;
	i->prompt = cmd;

	memcpy(i->prch, ":", 2);
	i->prompt_cursor_pos = 1;
	int r = 1;
	struct input o;
	for (;;) {
		i->dirty |= DIRTY_BOTTOMBAR;
		ui_draw(i);
		r = fill_textbox(i, cmd, &t_top, sizeof(cmd)-1, &o);
		if (!r) {
			break;
		}
		if (r == -1) {
			i->prompt = NULL;
			return;
		}
	}
	i->dirty |= DIRTY_BOTTOMBAR;
	i->prompt = NULL;
	interpreter(i, t, m, cmd, sizeof(cmd));
}

static void process_input(struct ui* const i, struct task* const t,
		struct marks* const m) {
	char *s = NULL;
	struct panel* tmp = NULL;
	int err = 0;
	fnum_t f;
	i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR;
	if (i->m == MODE_CHMOD) {
		i->dirty |= DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
	}
	switch (get_cmd(i)) {
	/* CHMOD */
	case CMD_RETURN:
		chmod_close(i);
		break;
	case CMD_CHANGE:
		i->dirty |= DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
		prepare_task(i, t, TASK_CHMOD);
		break;
	case CMD_CHOWN:
		/* TODO in $VISUAL */
		s = calloc(LOGIN_BUF_SIZE, sizeof(char));
		if (!prompt(i, s, s, LOGIN_MAX_LEN)) {
			errno = 0;
			struct passwd* pwd = getpwnam(s);
			if (!pwd) {
				err = errno;
				failed(i, "chown", strerror(err));
			}
			else {
				i->o[1] = pwd->pw_uid;
				xstrlcpy(i->user, pwd->pw_name, LOGIN_BUF_SIZE);
			}
		}
		free(s);
		break;
	case CMD_CHGRP:
		/* TODO in $VISUAL */
		s = calloc(LOGIN_BUF_SIZE, sizeof(char));
		if (!prompt(i, s, s, LOGIN_BUF_SIZE)) {
			errno = 0;
			struct group* grp = getgrnam(s);
			if (!grp) {
				err = errno;
				failed(i, "chgrp", strerror(err));
			}
			else {
				i->g[1] = grp->gr_gid;
				xstrlcpy(i->group, grp->gr_name, LOGIN_BUF_SIZE);
			}
		}
		free(s);
		break;

	case CMD_A: _perm(i, false, 00777); break;
	case CMD_U: _perm(i, false, S_ISUID | 0700); break;
	case CMD_G: _perm(i, false, S_ISGID | 0070); break;
	case CMD_O: _perm(i, false, S_ISVTX | 0007); break;
	case CMD_PL: _perm(i, false, 0777); break;
	case CMD_MI: _perm(i, true, 0777); break;

	/* WAIT */
	case CMD_TASK_QUIT:
		//t->done = true; // TODO
		break;
	case CMD_TASK_PAUSE:
		t->ts = TS_PAUSED;
		break;
	case CMD_TASK_RESUME:
		t->ts = TS_RUNNING;
		break;
	/* MANAGER */
	case CMD_QUIT:
		i->run = false;
		break;
	case CMD_HELP:
		open_help(i);
		break;
	case CMD_SWITCH_PANEL:
		tmp = i->pv;
		i->pv = i->sv;
		i->sv = tmp;
		if (!visible(i->pv, i->pv->selection)) {
			first_entry(i->pv);
		}
		break;
	case CMD_DUP_PANEL:
		memcpy(i->sv->wd, i->pv->wd, PATH_BUF_SIZE);
		i->sv->wdlen = i->pv->wdlen;
		if (ui_rescan(i, i->sv, NULL)) {
			i->sv->selection = i->pv->selection;
			i->sv->show_hidden = i->pv->show_hidden;
		}
		break;
	case CMD_SWAP_PANELS:
		tmp = malloc(sizeof(struct panel));
		memcpy(tmp, i->pv, sizeof(struct panel));
		memcpy(i->pv, i->sv, sizeof(struct panel));
		memcpy(i->sv, tmp, sizeof(struct panel));
		free(tmp);
		i->dirty |= DIRTY_PATHBAR;
		break;
	case CMD_ENTRY_DOWN:
		jump_n_entries(i->pv, 1);
		break;
	case CMD_ENTRY_UP:
		jump_n_entries(i->pv, -1);
		break;
	case CMD_SCREEN_DOWN:
		jump_n_entries(i->pv, i->ph-1);
		break;
	case CMD_SCREEN_UP:
		jump_n_entries(i->pv, -(i->ph-1));
		break;
	case CMD_ENTER_DIR:
		err = panel_enter_selected_dir(i->pv);
		if (err) {
			if (err == ENOTDIR) {
				open_selected_with(i, xgetenv(opener));
			}
			else  {
				failed(i, "enter dir", strerror(err));
			}
		}
		i->dirty |= DIRTY_PATHBAR;
		break;
	case CMD_UP_DIR:
		if ((err = panel_up_dir(i->pv))) {
			failed(i, "up dir", strerror(err));
		}
		i->dirty |= DIRTY_PATHBAR;
		break;
	case CMD_COPY:
		prepare_task(i, t, TASK_COPY);
		break;
	case CMD_MOVE:
		prepare_task(i, t, TASK_MOVE);
		break;
	case CMD_REMOVE:
		prepare_task(i, t, TASK_REMOVE);
		break;
	case CMD_COMMAND:
		cmd_command(i, t, m);
		break;
	case CMD_CD:
		cmd_cd(i);
		break;
	case CMD_CREATE_DIR:
		cmd_mkdir(i);
		break;
	case CMD_RENAME:
		cmd_rename(i);
		break;
	case CMD_LINK:
		cmd_mklnk(i);
		break;
	case CMD_DIR_VOLUME:
		//estimate_volume_for_selected(i->pv);
		break;
	case CMD_SELECT_FILE:
		if (panel_select_file(i->pv)) {
			jump_n_entries(i->pv, 1);
		}
		break;
	case CMD_SELECT_ALL:
		i->pv->num_selected = 0;
		for (fnum_t f = 0; f < i->pv->num_files; ++f) {
			if (visible(i->pv, f)) {
				i->pv->file_list[f]->selected = true;
				i->pv->num_selected += 1;
			}
		}
		break;
	case CMD_SELECT_NONE:
		panel_unselect_all(i->pv);
		break;
	case CMD_SELECTED_NEXT:
		if (!i->pv->num_selected
		|| i->pv->selection == i->pv->num_files-1) break;
		f = i->pv->selection+1;
		while (f < i->pv->num_files) {
			if (i->pv->file_list[f]->selected) {
				i->pv->selection = f;
				break;
			}
			f += 1;
		}
		break;
	case CMD_SELECTED_PREV:
		if (!i->pv->num_selected || !i->pv->selection) break;
		f = i->pv->selection;
		while (f) {
			f -= 1;
			if (i->pv->file_list[f]->selected) {
				i->pv->selection = f;
				break;
			}
		}
		break;
	case CMD_MARK_NEW:
		marks_input(i, m);
		break;
	case CMD_MARK_JUMP:
		marks_jump(i, m);
		break;
	case CMD_FIND:
		cmd_find(i);
		break;
	case CMD_ENTRY_FIRST:
		first_entry(i->pv);
		break;
	case CMD_ENTRY_LAST:
		last_entry(i->pv);
		break;
	case CMD_CHMOD:
		if ((s = panel_path_to_selected(i->pv))) {
			i->dirty = DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
			chmod_open(i, s);
		}
		else {
			failed(i, "chmod", strerror(ENAMETOOLONG));
		}
		break;
	case CMD_TOGGLE_HIDDEN:
		panel_toggle_hidden(i->pv);
		break;
	case CMD_REFRESH:
		ui_rescan(i, i->pv, NULL);
		break;
	case CMD_SORT_REVERSE:
		i->pv->scending = -i->pv->scending;
		panel_sorting_changed(i->pv);
		break;
	case CMD_SORT_CHANGE:
		cmd_change_sorting(i);
		break;
	case CMD_COL:
		chg_column(i);
		break;
	default:
		i->dirty = 0;
		break;
	}
}

static void task_progress(struct ui* const i,
		struct task* const t,
		const char* const S) {
	i->mt = MSG_INFO;
	i->dirty |= DIRTY_BOTTOMBAR;
	int n = snprintf(i->msg, MSG_BUFFER_SIZE,
			"%s %d/%df, %d/%dd", S,
			t->files_done, t->files_total,
			t->dirs_done, t->dirs_total);
	if (t->t & (TASK_REMOVE | TASK_MOVE | TASK_COPY)) {
		char sdone[SIZE_BUF_SIZE];
		char stota[SIZE_BUF_SIZE];
		pretty_size(t->size_done, sdone);
		pretty_size(t->size_total, stota);
		n += snprintf(i->msg+n, MSG_BUFFER_SIZE-n,
			", %s/%s", sdone, stota);
	}
}

static void task_execute(struct ui* const i, struct task* const t) {
	task_action ta = NULL;
	char msg[512]; // TODO
	char psize[SIZE_BUF_SIZE];
	static const struct select_option remove_o[] = {
		{ KUTF8("y"), "yes" },
		{ KUTF8("n"), "no" },
	};
	// TODO skip all errors, skip same errno
	static const struct select_option error_o[] = {
		{ KUTF8("t"), "try again" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("a"), "abort" },
	};
	static const struct select_option manual_o[] = {
		{ KUTF8("o"), "overwrite" },
		{ KUTF8("O"), "overwrite all" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("S"), "skip all" },
	};
	static const struct select_option conflict_o[] = {
		{ KUTF8("i"), "ask" },
		{ KUTF8("o"), "overwrite all" },
		{ KUTF8("s"), "skip all" },
		{ KUTF8("a"), "abort" },
	};
	static const struct select_option symlink_o[] = {
		{ KUTF8("r"), "raw" },
		{ KUTF8("c"), "recalculate" },
		{ KUTF8("d"), "dereference" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("a"), "abort" },
	};
	static const char* const symlink_q = "There are symlinks";
	switch (t->ts) {
	case TS_CLEAN:
		break;
	case TS_ESTIMATE:
		i->timeout = 500;
		i->m = MODE_WAIT;
		task_progress(i, t, "--");
		task_do(t, task_action_estimate, TS_CONFIRM);
		if (t->err) t->ts = TS_FAILED;
		if (t->tw.tws == AT_LINK && !(t->tf & (TF_ANY_LINK_METHOD))) {
			if (t->t & (TASK_COPY | TASK_MOVE)) {
				switch (ui_ask(i, symlink_q, symlink_o, 5)) {
				case 0: t->tf |= TF_RAW_LINKS; break;
				case 1: t->tf |= TF_RECALCULATE_LINKS; break;
				case 2: t->tf |= TF_DEREF_LINKS; break;
				case 3: t->tf |= TF_SKIP_LINKS; break;
				case 4: t->ts = TS_FINISHED; break;
				default: break;
				}
			}
			else if (t->t & TASK_REMOVE) {
				t->tf |= TF_RAW_LINKS; // TODO
			}
		}
		break;
	case TS_CONFIRM:
		t->ts = TS_RUNNING;
		if (t->t == TASK_REMOVE) {
			pretty_size(t->size_total, psize);
			snprintf(msg, sizeof(msg),
				"Remove %u files? (%s)",
				t->files_total, psize);
			if (ui_ask(i, msg, remove_o, 2)) {
				t->ts = TS_FINISHED;
			}
		}
		else if (t->conflicts && t->t & (TASK_COPY | TASK_MOVE)) {
			snprintf(msg, sizeof(msg),
				"There are %d conflicts", t->conflicts);
			switch (ui_ask(i, msg, conflict_o, 4)) {
			case 0: t->tf |= TF_ASK_CONFLICTS; break;
			case 1: t->tf |= TF_OVERWRITE_CONFLICTS; break;
			case 2: t->tf |= TF_SKIP_CONFLICTS; break;
			case 3: t->ts = TS_FINISHED; break;
			default: break;
			}
		}
		task_progress(i, t, "==");
		break;
	case TS_RUNNING:
		i->timeout = 500;
		if (t->t & (TASK_REMOVE | TASK_COPY | TASK_MOVE)) {
			ta = task_action_copyremove;
		}
		else if (t->t == TASK_CHMOD) {
			ta = task_action_chmod;
		}
		task_progress(i, t, ">>");
		task_do(t, ta, TS_FINISHED);
		break;
	case TS_PAUSED:
		i->timeout = -1;
		task_progress(i, t, "||");
		break;
	case TS_FAILED:
		snprintf(msg, sizeof(msg), "@ %s\r\n(%d) %s.",
			t->tw.path, t->err, strerror(t->err));
		if (t->err == EEXIST) {
			switch (ui_ask(i, msg, manual_o, 4)) {
			case 0:
				t->tf |= TF_OVERWRITE_ONCE;
				break;
			case 1:
				t->tf &= ~TF_ASK_CONFLICTS;
				t->tf |= TF_OVERWRITE_CONFLICTS;
				break;
			case 2:
				t->err = tree_walk_step(&t->tw);
				break;
			case 3:
				t->tf &= ~TF_ASK_CONFLICTS;
				t->tf |= TF_SKIP_CONFLICTS;
				break;
			}
			t->ts = TS_RUNNING;
		}
		else {
			switch (ui_ask(i, msg, error_o, 3)) {
			case 0:
				t->ts = TS_RUNNING;
				break;
			case 1:
				t->err = tree_walk_step(&t->tw);
				t->ts = TS_RUNNING;
				break;
			case 2:
				t->ts = TS_FINISHED;
				break;
			}
		}
		t->err = 0;
		break;
	case TS_FINISHED:
		i->timeout = -1;
		if (ui_rescan(i, i->pv, i->sv)) {
			if (t->t == TASK_MOVE) {
				jump_n_entries(i->pv, -1);
			}
			pretty_size(t->size_done, psize);
			i->mt = MSG_INFO;
			snprintf(i->msg, MSG_BUFFER_SIZE,
				"processed %u files, %u dirs; %s",
				t->files_done, t->dirs_done, psize);
		}
		task_clean(t);
		i->m = MODE_MANAGER;
		break;
	default:
		break;
	}
}

void read_config(struct ui* const i, struct task* const t,
		struct marks* const m, const char* const path) {
	char buf[BUFSIZ];
	size_t linelen;
	ssize_t rem = 0;
	char* z;
	int fd = open(path, O_RDONLY);
	if (fd == -1) return;
	while ((rem = read(fd, buf, sizeof(buf))), rem && rem != -1) {
		buf[rem] = '\n';
		while (rem) {
			z = memchr(buf, '\n', rem);
			*z = 0;
			linelen = z - buf;
			interpreter(i, t, m, buf, sizeof(buf));
			memmove(buf, z+1, rem-linelen);
			rem -= linelen+1;
		}
	}
	close(fd);
}

extern struct ui* I;

int main(int argc, char* argv[]) {
	static const char* const help = \
	"Usage: hund [OPTION...] [left panel] [right panel]\n"
	"Options:\n"
	"  -c, --config          use suppiled file as a config\n"
	"  --no-config           do not load any config files\n"
	"  -h, --help            display this help message\n"
	"Type `?` while in hund for more help\n";
	int o, opti = 0, err;
	int no_config = 0;
	const char* config = NULL;
	static const char sopt[] = "hc:";
	struct option lopt[] = {
		{"no-config", no_argument, &no_config, 1},
		{"config", required_argument, 0, 'c'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	while ((o = getopt_long(argc, argv, sopt, lopt, &opti)) != -1) {
		switch (o) {
		case 0:
			break;
		case 'c':
			config = optarg;
			break;
		case 'h':
			printf("%s", help);
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	}

	/* If user passed initial directories in cmdline,
	 * save these paths for later
	 */
	char* init_wd[2] = { NULL, NULL };
	int init_wd_top = 0;
	while (optind < argc) {
		if (init_wd_top < 2) {
			init_wd[init_wd_top] = argv[optind];
			init_wd_top += 1;
		}
		else {
			// Only two panels, only two paths allowed
			fprintf(stderr, "invalid argument:"
					" '%s'\n", argv[optind]);
			exit(EXIT_FAILURE);
		}
		optind += 1;
	}

	struct panel fvs[2];
	memset(fvs, 0, sizeof(fvs));
	fvs[0].scending = 1;
	memcpy(fvs[0].order, default_order, FV_ORDER_SIZE);
	fvs[1].scending = 1;
	memcpy(fvs[1].order, default_order, FV_ORDER_SIZE);

	struct ui i;
	ui_init(&i, &fvs[0], &fvs[1]);

	for (int v = 0; v < 2; ++v) {
		const char* const d = (init_wd[v] ? init_wd[v] : "");
		fvs[v].wdlen = 0;
		if (getcwd(fvs[v].wd, PATH_BUF_SIZE)) {
			fvs[v].wdlen = strnlen(fvs[v].wd, PATH_MAX_LEN);
			size_t dlen = strnlen(d, PATH_MAX_LEN);
			if ((err = cd(fvs[v].wd, &fvs[v].wdlen, d, dlen))
			|| (err = panel_scan_dir(&fvs[v]))) {
				fprintf(stderr, "failed to initalize:"
					" (%d) %s\n", err, strerror(err));
				ui_end(&i);
				exit(EXIT_FAILURE);
			}
		}
		else {
			memcpy(fvs[v].wd, "/", 2);
			fvs[v].wdlen = 1;
		}
		first_entry(&fvs[v]);
	}

	struct task t;
	memset(&t, 0, sizeof(struct task));
	t.in = t.out = -1;

	struct marks m;
	memset(&m, 0, sizeof(struct marks));

	i.mt = MSG_INFO;
	xstrlcpy(i.msg, "Type ? for help and license notice.", MSG_BUFFER_SIZE);

	static const char* const config_paths[] = {
		"~/.hundrc",
		"~/.config/hund/hundrc",
		NULL
	};
	if (!no_config && !config) {
		char* p = malloc(PATH_BUF_SIZE);
		p[0] = 0;
		size_t plen = 0;
		int cp = 0;
		while (config_paths[cp]) {
			const size_t cpl = strlen(config_paths[cp]);
			cd(p, &plen, config_paths[cp], cpl);
			if (!access(p, F_OK)) {
				read_config(&i, &t, &m, p);
				break;
			}
			cp += 1;
		}
		free(p);
	}
	else if (!no_config) {
		read_config(&i, &t, &m, config);
	}

	while (i.run || t.ts != TS_CLEAN) {
		ui_draw(&i);
		if (i.run) { // TODO
			process_input(&i, &t, &m);
		}
		task_execute(&i, &t);
	}

	for (int v = 0; v < 2; ++v) {
		delete_file_list(&fvs[v]);
	}
	marks_free(&m);
	task_clean(&t);
	ui_end(&i);
	memset(fvs, 0, sizeof(fvs));
	memset(&t, 0, sizeof(struct task));
	exit(EXIT_SUCCESS);
}
