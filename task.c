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

#include "task.h"

void task_new(struct task* const t, const enum task_type tp,
		const enum task_flags tf,
		char* const src, char* const dst,
		const struct string_list* const sources,
		const struct string_list* const renamed) {
	t->t = tp;
	t->ts = TS_ESTIMATE;
	t->tf = tf;
	t->src = src;
	t->dst = dst;
	t->sources = *sources;
	t->renamed = *renamed;
	t->in = t->out = -1;
	t->current_source = 0;
	t->err = 0;
	t->conflicts = t->symlinks = t->specials = 0;
	t->size_total = t->size_done = 0;
	t->files_total = t->files_done = t->dirs_total = t->dirs_done = 0;
	t->chp = t->chm = 0;
	t->cho = -1;
	t->chg = -1;
	memset(&t->tw, 0, sizeof(struct tree_walk));
}

void task_action_chmod(struct task* const t, int* const c) {
	switch (t->tw.tws) {
	case AT_LINK:
	case AT_FILE:
		t->files_done += 1;
		break;
	case AT_DIR:
		t->dirs_done += 1;
		break;
	default:
		break;
	}
	if ((t->chp != 0 || t->chm != 0)
	    && (t->err = relative_chmod(t->tw.cpath, t->chp, t->chm))) {
		return;
	}
	if (chown(t->tw.cpath, t->cho, t->chg) ? (t->err = errno) : 0) return;
	if ((t->err = tree_walk_step(&t->tw))) return;

	if (!(t->tf & TF_RECURSIVE_CHMOD)) { // TODO find a better way to chmod once
		t->tw.tws = AT_EXIT;
	}
	*c -= 1;
}

void task_action_estimate(struct task* const t, int* const c) {
	switch (t->tw.tws) {
	case AT_LINK:
		t->symlinks += 1;
	case AT_FILE:
		t->files_total += 1;
		break;
	case AT_DIR:
		t->dirs_total += 1;
		break;
	default:
		t->specials += 1;
		break;
	}
	const bool Q = t->tw.tws & (AT_DIR | AT_LINK | AT_FILE);
	if ((t->t == TASK_COPY || t->t == TASK_MOVE) && Q) {
		char new_path[PATH_BUF_SIZE];
		build_new_path(t->tw.cpath, t->src, t->dst,
			t->sources.arr[t->current_source]->str,
			(t->renamed.len ?
				t->renamed.arr[t->current_source]->str : NULL),
			new_path);
		if (!access(new_path, F_OK)) {
			t->conflicts += 1;
		}
	}
	t->size_total += t->tw.cs.st_size; // TODO
	if ((t->err = tree_walk_step(&t->tw))) {
		t->ts = TS_FAILED;
	}
	*c -= 1;
}

void task_do(struct task* const t, int c, task_action ta,
		const enum task_state onend) {
	if (t->tw.tws == AT_NOWHERE) {
		const char* const source = t->sources.arr[t->current_source]->str;
		const size_t src_len = strnlen(t->src, PATH_MAX_LEN);
		const size_t source_len = strnlen(source, NAME_MAX_LEN);
		const size_t path_len = src_len+1+source_len;
		char* path = malloc(path_len+1);
		memcpy(path, t->src, path_len+1);
		append_dir(path, source);

		t->err = tree_walk_start(&t->tw, path, t->tf & TF_DEREF_LINKS);
		free(path);
		if (t->err) {
			t->tw.tws = AT_NOWHERE;
			return;
		}
	}
	while (t->tw.tws != AT_EXIT && !t->err && c > 0) {
		ta(t, &c);
	}
	if (t->err) {
		t->ts = TS_FAILED;
	}
	else if (t->tw.tws == AT_EXIT) {
		t->current_source += 1;
		t->tw.tws = AT_NOWHERE;
		if (t->current_source == t->sources.len) {
			t->ts = onend;
			tree_walk_end(&t->tw);
			t->current_source = 0;
		}
	}
}

static int _close_files(struct task* const t) {
	// TODO maybe too much?
	int e = 0;
	if (t->in != -1 && close(t->in)) e = errno;
	if (t->out != -1 && close(t->out)) e = errno;
	t->in = t->out = -1;
	return e;
}

void task_clean(struct task* const t) {
	free_list(&t->sources);
	free_list(&t->renamed);
	t->src = t->dst = NULL;
	t->t = TASK_NONE;
	t->tf = TF_NONE;
	t->ts = TS_CLEAN;
	t->conflicts = t->specials = t->err = 0;
	_close_files(t);
}

static bool _files_opened(const struct task* const t) {
	return t->out != -1 && t->in != -1;
}

static int _open_files(struct task* const t,
		const char* const dst, const char* const src) {
	errno = 0;
	if (!access(dst, F_OK)) {
		if (t->tf & TF_SKIP_CONFLICTS) {
			t->files_done += 1;
			return 0;
		}
		else {
			if (unlink(dst)) return errno;
		}
	}
	t->out = open(src, O_RDONLY);
	if (t->out == -1) {
		return errno; // TODO TODO IMPORTANT
	}
	t->in = creat(dst, t->tw.cs.st_mode);
	if (t->in == -1) {
		close(t->out);
		t->out = -1;
		return errno;
	}
	struct stat outs;
	if (fstat(t->out, &outs)) return errno;
#if HAS_FALLOCATE
	if (outs.st_size > 0) {
		int e = posix_fallocate(t->in, 0, outs.st_size);
		// TODO detect earlier if fallocate is supported
		if (e != EOPNOTSUPP && e != ENOSYS) return e;
	}
#endif
	return 0;
}

static int _copy_some(struct task* const t,
		const char* const src, const char* const dst,
		void* const buf, const size_t bufsize, int* const c) {
	// TODO if it fails at any point it should seek back
	// to enable retrying
	int e = 0;
	if (!_files_opened(t) && (e = _open_files(t, dst, src))) {
		return e;
	}
	ssize_t wb = -1, rb = -1;
	while (*c > 0 && _files_opened(t)) {
		rb = read(t->out, buf, bufsize);
		if (!rb) { // done copying
			t->files_done += 1;
			return _close_files(t);
		}
		if (rb == -1) {
			e = errno;
			_close_files(t);
			return e;
		}
		wb = write(t->in, buf, rb);
		if (wb == -1) {
			e = errno;
			_close_files(t);
			return e;
		}
		t->size_done += wb;
		*c -= 1;
	}
	return 0;
}

/*
 * P = Full path of the source
 * swd = Source Working Directory
 * dwd = Destination Working Directory
 * S = Source Directory
 * D = Destination Directory
 * R = Result
 *
 * TODO errors
 */
int build_new_path(const char* const P, const char* const swd,
		const char* const dwd, const char* const S,
		const char* const D, char* R) {
	size_t D_len;
	size_t old_len = strnlen(swd, PATH_MAX_LEN);
	if (S && D) {
		D_len = strnlen(D, NAME_MAX_LEN);
		old_len += 1+strnlen(S, NAME_MAX_LEN);
	}
	else {
		D_len = 0;
	}
	const size_t dwd_len = strnlen(dwd, PATH_MAX_LEN);
	//if (dwd_len+D_len > PATH_MAX_LEN) return ENAMETOOLONG;
	memcpy(R, dwd, dwd_len);
	R += dwd_len;
	if (*(R-1) != '/') {
		*R = '/';
		R += 1;
	}
	memcpy(R, D, D_len);
	R += D_len;
	if (*(R-1) != '/') {
		*R = '/';
		R += 1;
	}
	const char* lp = P+old_len;
	if (*lp == '/') {
		lp += 1;
		old_len -= 1;
	}
	const size_t P_len = strnlen(P, PATH_MAX_LEN);
	memcpy(R, lp, P_len-old_len+1);
	return 0;
}

/*
 * No effects on failure
 */
static int _stat_file(struct tree_walk* const tw) {
	// lstat/stat can errno:
	// ENOENT, EACCES, ELOOP, ENAMETOOLONG, ENOMEM, ENOTDIR, EOVERFLOW
	struct stat old_cs;
	memcpy(&old_cs, &tw->cs, sizeof(struct stat));
	const enum tree_walk_state old_tws = tw->tws;
	if (lstat(tw->cpath, &tw->cs)) {
		memcpy(&tw->cs, &old_cs, sizeof(struct stat));
		return errno;
	}
	if (S_ISDIR(tw->cs.st_mode)) {
		tw->tws = AT_DIR;
	}
	else if (S_ISREG(tw->cs.st_mode)) {
		tw->tws = AT_FILE;
	}
	else if (S_ISLNK(tw->cs.st_mode)) {
		if (!tw->tl) {
			tw->tws = AT_LINK;
		}
		else if (stat(tw->cpath, &tw->cs)) {
			int err = errno;
			if (err == ENOENT) {
				tw->tws = AT_LINK;
			}
			else {
				memcpy(&tw->cs, &old_cs, sizeof(struct stat));
				tw->tws = old_tws;
				return err;
			}
		}
		else {
			if (S_ISDIR(tw->cs.st_mode)) {
				tw->tws = AT_DIR;
			}
			else tw->tws = AT_FILE;
		}
	}
	else tw->tws = AT_SPECIAL;
	return 0;
}

int tree_walk_start(struct tree_walk* const tw,
		const char* const path, const bool tl) {
	if (tw->cpath) free(tw->cpath);
	tw->cpath = malloc(PATH_BUF_SIZE);
	strncpy(tw->cpath, path, PATH_BUF_SIZE);
	tw->tl = tl;
	if (tw->dt) free(tw->dt);
	tw->dt = calloc(1, sizeof(struct dirtree));
	return _stat_file(tw);
}

void tree_walk_end(struct tree_walk* const tw) {
	struct dirtree* DT = tw->dt;
	while (DT) {
		struct dirtree* UP = DT->up;
		free(DT);
		DT = UP;
	}
	free(tw->cpath);
	memset(tw, 0, sizeof(struct tree_walk));
}

//TODO: int tree_walk_skip()
int tree_walk_step(struct tree_walk* const tw) {
	struct dirtree *new_dt, *up;
	switch (tw->tws)  {
	case AT_LINK:
	case AT_FILE:
		if (!tw->dt->cd) {
			tw->tws = AT_EXIT;
			return 0;
		}
		up_dir(tw->cpath);
		break;
	case AT_DIR:
		/* Go deeper */
		new_dt = calloc(1, sizeof(struct dirtree));
		new_dt->up = tw->dt;
		tw->dt = new_dt;
		if (!(new_dt->cd = opendir(tw->cpath))) {
			tw->dt = new_dt->up;
			free(new_dt);
			return errno;
		}
		break;
	case AT_DIR_END:
		/* Go back */
		if (tw->dt->cd) closedir(tw->dt->cd);
		up = tw->dt->up;
		free(tw->dt);
		tw->dt = up;
		up_dir(tw->cpath);
		break;
	default:
		break;
	}
	if (!tw->dt || !tw->dt->up) { // last dir
		free(tw->dt);
		tw->dt = NULL;
		tw->tws = AT_EXIT;
		return 0;
	}

	struct dirent* ce;
	do {
		errno = 0;
		ce = readdir(tw->dt->cd);
	} while (ce && DOTDOT(ce->d_name));

	// TODO errno
	if (!ce) { // directory is empty or reached end of directory
		tw->tws = AT_DIR_END;
		return errno;
	}
	append_dir(tw->cpath, ce->d_name);
	return _stat_file(tw);
}

/*
 * npath is a buffer to be used by build_new_path()
 * buf is a buffer to be used by _copy_some()
 * TODO simplify
 */
static int _at_step(struct task* const t, int* const c,
		char* const new_path, char* const buf, const size_t bufsize) {
	const bool copy = t->t & (TASK_COPY | TASK_MOVE);
	const bool remove = t->t & (TASK_MOVE | TASK_REMOVE);
	const char* const tfn = t->sources.arr[t->current_source]->str;
	const char* rfn = NULL;
	const bool sc = t->tf & TF_SKIP_CONFLICTS;
	const bool ov = t->tf & TF_OVERWRITE_CONFLICTS;
	if (copy && !ov) {
		rfn = (t->renamed.len ?
			t->renamed.arr[t->current_source]->str : NULL);
	}
	if (copy && remove && same_fs(t->src, t->dst)) {
		build_new_path(t->tw.cpath, t->src,
				t->dst, tfn, rfn, new_path);
		if (rename(t->tw.cpath, new_path)) return errno;
		t->tw.tws = AT_EXIT;
		t->size_done = t->size_total;
		t->files_done = t->files_total;
		t->dirs_done = t->dirs_total;
		return 0;
	}
	int err;
	switch (t->tw.tws) {
	case AT_LINK:
		if (t->tf & TF_SKIP_LINKS) break;
		if (copy) {
			build_new_path(t->tw.cpath, t->src,
					t->dst, tfn, rfn, new_path);
			if (t->tf & TF_RAW_LINKS) {
				err = link_copy_raw(t->tw.cpath, new_path);
			}
			else {
				err = link_copy(t->src, t->tw.cpath, new_path);
			}
			if ((!sc && err) || (sc && err != EEXIST)) {
				return err;
			}
		}
		if (remove) {
			if (unlink(t->tw.cpath)) return errno;
			if (!copy) {
				t->size_done += t->tw.cs.st_size;
				t->files_done += 1;
				*c -= 1;
			}
		}
		break;
	case AT_FILE:
		if (copy) {
			build_new_path(t->tw.cpath, t->src,
					t->dst, tfn, rfn, new_path);
			err = _copy_some(t, t->tw.cpath, new_path, buf, bufsize, c);
			if ((t->tf & TF_SKIP_CONFLICTS) && err != EEXIST) {
				return err;
			}
			if (_files_opened(t)) return 0;
		}
		if (remove) {
			if (unlink(t->tw.cpath)) return errno;
			if (!copy) {
				t->size_done += t->tw.cs.st_size;
				t->files_done += 1;
				*c -= 1;
			}
		}
		break;
	case AT_DIR:
		if (copy) {
			build_new_path(t->tw.cpath, t->src,
					t->dst, tfn, rfn, new_path);
			err = (mkdir(new_path, t->tw.cs.st_mode) ? errno : 0);
			if (!err || (ov && err == EEXIST)) {
				t->size_done += t->tw.cs.st_size;
				t->dirs_done += 1;
				*c -= 1;
			}
			else if (sc && err == EEXIST) {
				t->dirs_done += 1;
				*c -= 1;
			}
			else {
				return err;
			}

		}
		break;
	case AT_DIR_END:
		if (remove) {
			if (rmdir(t->tw.cpath)) return errno;
			t->dirs_done += 1;
			*c -= 1;
		}
		break;
	default: break;
	}
	return 0;
}

void task_action_copyremove(struct task* const t, int* const c) {
	char new_path[PATH_BUF_SIZE];
	char buf[BUFSIZ];
	if ((t->err = _at_step(t, c, new_path, buf, sizeof(buf)))
	|| (t->in != -1 || t->out != -1)
	|| (t->err = tree_walk_step(&t->tw))) {
		//if (t->err == EACCES) t->err = 0;
		return;
	}
}
