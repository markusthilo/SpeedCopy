/* SpeedCopy */
/* Written for Windows 10/11 64-bit + MinGW-w64 */
/* Author: Markus Thilo */
/* E-mail: markus.thilo@proton.me */
/* License: MIT */
/*
 * Recursively copy files and directories into one destination directory
 * or volume. Files are copied in parallel. Files of 16 MiB and more use
 * overlapped unbuffered transfers (up to 512 MiB buffers). Smaller files
 * use one buffered read and write. A check never runs on a copy task,
 * and the copy does not wait for a comparison or a hash.
 *
 * The destination is a single directory. Each named source is placed
 * under its own name. A source drive root (D:\) is placed directly, so
 * its children keep their paths relative to that root.
 *
 * -n or --no-verify copies only. By default, another task reads each
 * destination size after that file has been copied and compares it.
 * -m or -md5 hashes each source on other tasks and hashes each
 * destination after that file has been copied. Mismatches are warnings.
 * -c or --csv writes one protocol row per file.
 *
 * Paths use \ between directories and files.
 * Each source is -s or --source. The last argument is the destination.
 * -j or --jobs sets the maximum number of worker threads. With no arguments,
 * a window opens: source paths, one destination directory, an optional
 * CSV protocol, no verification, size, or MD5, and the job count.
 *
 * Build: ./make-exe.sh
 *   x86_64-w64-mingw32-windres -O coff --target=pe-x86-64 -o speedcopy_res.o speedcopy.rc
 *   x86_64-w64-mingw32-gcc -O2 -s -static -mconsole -o speedcopy.exe speedcopy.c speedcopy_gui.c speedcopy_res.o -lcomdlg32 -lshell32 -lole32 -lgdi32 -lcomctl32 -luuid
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>

const wchar_t SPEEDCOPY_VERSION[] = L"0.2.0-2026-11-11";
#define VERSION SPEEDCOPY_VERSION
#include <io.h>
#include <fcntl.h>
#ifdef _MSC_VER
#include <intrin.h>
#define MD5_ROTL(x, n) _rotl((x), (n))
#pragma comment(lib, "shell32.lib")
#else
#define MD5_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#endif

#define VERIFY_NONE 0
#define VERIFY_SIZE 1
#define VERIFY_MD5  2
#define WORK_SIZE   1
#define WORK_SRC    2
#define WORK_DST    3

#define ALIGN_SIZE 4096u
#define BLOCK_SIZE (512u << 20)
#define MIN_BLOCK (1u << 20)
#define UNBUFFERED_MIN (16u << 20)
#define IO_ATTEMPTS 3
#define SHARE_ANY (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define OVERWRITE_COUNT_LIMIT 1000
#define COPY_FAIL_LIMIT 10000
/* With no verification every thread copies at full speed. More than a few
 * of those at once make the disks slower, so the copy stays under this
 * even when the job limit is higher. */
#define COPY_ONLY_THREADS 4

static CRITICAL_SECTION g_out_lock;
static volatile LONG g_out_lock_ready;
static volatile LONG g_warn;
static volatile LONG g_walk_err;
static volatile LONG g_no_unbuffered;
static volatile LONG g_phase;
static volatile LONG g_progress_drawn;
static int g_verify = VERIFY_SIZE;
static int g_jobs;
static int g_force;
static int g_overwrite_pass;
static int g_tty;
static int g_csv_err;
static int g_verify_oom;
static FILE *g_csv;
static LPWSTR g_csv_path;
static LONG64 g_queued;
static LONG64 g_total_bytes;
static LONG64 g_overwrite;
static LONG64 g_copy_done;
static LONG64 g_copy_files;
static LONG64 g_copy_fail;
static LONG64 g_copy_bytes;
static LONG64 g_copy_progress;
static LONG64 g_ver_progress;
static LONG64 g_check_total;
static LONG64 g_tail_base;
static LONG64 g_settled;
static LONG64 g_ok;
static LONG64 g_bad;
static volatile LONG g_stop;
static volatile LONG g_fail_stop;
static int g_scan_stop;
static HANDLE g_stop_ev;
static HANDLE g_prog_thread;

struct op_err {
	DWORD code;
	LPCWSTR path;
	const wchar_t *what;
};

struct job {
	LPWSTR src;
	LPWSTR dst;
	ULONGLONG copy_size;
	ULONGLONG src_size;
	ULONGLONG dst_size;
	BYTE src_md5[16];
	BYTE dst_md5[16];
	int copy_size_valid;
	int copy_ok;
	DWORD copy_err;
	int src_md5_ok;
	int dst_md5_ok;
	int src_present;
	int dst_present;
	int ok;
	int omit;
	volatile LONG parts;
	struct job *qnext;
	struct job *allnext;
};

static struct job *g_all_head;
static struct job *g_all_tail;

struct dir_stamp {
	LPWSTR src;
	LPWSTR dst;
	struct dir_stamp *next;
};

static struct dir_stamp *g_dirs;

struct job_queue {
	struct job *head;
	struct job *tail;
	CRITICAL_SECTION lock;
	HANDLE sem;
	HANDLE init_sem;
	volatile LONG ready;
};

struct pool {
	HANDLE *threads;
	int started;
	int ready;
};

struct work {
	struct job *job;
	int kind;
	struct work *next;
};

struct work_queue {
	struct work *head;
	struct work *tail;
	CRITICAL_SECTION lock;
	HANDLE sem;
	HANDLE init_sem;
	volatile LONG ready;
};

static struct work_queue *g_checks;
static volatile LONG g_check_live;

struct tick {
	LARGE_INTEGER t0;
	LARGE_INTEGER freq;
};

struct md5 {
	unsigned int s[4];
	unsigned char buf[64];
	ULONGLONG nbytes;
	unsigned int nbuf;
};

struct io_slot {
	OVERLAPPED ov;
	HANDLE ev;
	int pending;
	DWORD got;
	DWORD want;
};

static void out_lock(void)
{
	if (g_out_lock_ready)
		EnterCriticalSection(&g_out_lock);
}

static void out_unlock(void)
{
	if (g_out_lock_ready)
		LeaveCriticalSection(&g_out_lock);
}

static void warn(LPCWSTR fmt, ...)
{
	va_list ap;

	if (g_overwrite_pass)
		return;
	InterlockedIncrement(&g_warn);
	out_lock();
	if (g_progress_drawn) {
		if (g_tty)
			fwprintf(stdout, L"\r                                                                                \r");
		else
			fwprintf(stdout, L"...\r");
		fflush(stdout);
		g_progress_drawn = 0;
	}
	fwprintf(stdout, L"ERROR: ");
	va_start(ap, fmt);
	vfwprintf(stdout, fmt, ap);
	va_end(ap);
	fwprintf(stdout, L"\n");
	fflush(stdout);
	out_unlock();
}

static void caution(LPCWSTR fmt, ...)
{
	va_list ap;

	if (g_overwrite_pass)
		return;
	out_lock();
	if (g_progress_drawn) {
		if (g_tty)
			fwprintf(stdout, L"\r                                                                                \r");
		else
			fwprintf(stdout, L"...\r");
		fflush(stdout);
		g_progress_drawn = 0;
	}
	fwprintf(stdout, L"WARNING: ");
	va_start(ap, fmt);
	vfwprintf(stdout, fmt, ap);
	va_end(ap);
	fwprintf(stdout, L"\n");
	fflush(stdout);
	out_unlock();
}

static void set_err(struct op_err *oe, DWORD err, LPCWSTR path, const wchar_t *what)
{
	if (!err)
		err = ERROR_GEN_FAILURE;
	oe->code = err;
	oe->path = path;
	oe->what = what;
}

static void sysmsg(DWORD err, LPWSTR buf, int cap)
{
	DWORD n;

	if (cap <= 0)
		return;
	if (!err)
		err = ERROR_GEN_FAILURE;
	n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, err, 0, buf, cap, NULL);
	if (!n) {
		_snwprintf(buf, cap, L"error %lu", err);
		buf[cap - 1] = 0;
		return;
	}
	while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' '))
		buf[--n] = 0;
}

static int wieq(LPCWSTR a, LPCWSTR b)
{
	return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

static int wistart(LPCWSTR s, LPCWSTR prefix)
{
	int n = lstrlenW(prefix);

	if (lstrlenW(s) < n)
		return 0;
	return CompareStringOrdinal(s, n, prefix, n, TRUE) == CSTR_EQUAL;
}

static LPWSTR wcsdup_w(LPCWSTR s)
{
	int n = lstrlenW(s);
	LPWSTR out = (LPWSTR)LocalAlloc(LMEM_FIXED, (n + 1) * sizeof(WCHAR));

	if (out)
		lstrcpyW(out, s);
	return out;
}

static LPWSTR join_dir_file(LPCWSTR dir, LPCWSTR name)
{
	int dlen = lstrlenW(dir);
	int nlen = lstrlenW(name);
	int sep = (dlen == 0 || dir[dlen - 1] != L'\\');
	LPWSTR out = (LPWSTR)LocalAlloc(LMEM_FIXED,
		(dlen + sep + nlen + 1) * sizeof(WCHAR));

	if (!out)
		return NULL;
	lstrcpyW(out, dir);
	if (sep)
		lstrcatW(out, L"\\");
	lstrcatW(out, name);
	return out;
}

static LPCWSTR basename_w(LPCWSTR path)
{
	LPCWSTR name = path;

	for (; *path; path++) {
		if (*path == L'\\')
			name = path + 1;
	}
	return name;
}

static int ends_with_sep(LPCWSTR s, int n)
{
	return n > 0 && s[n - 1] == L'\\';
}

static void strip_dir_slash(LPWSTR path)
{
	int n = lstrlenW(path);

	while (n > 1 && ends_with_sep(path, n) && path[n - 2] != L':')
		path[--n] = 0;
}

static int is_windows_path(LPCWSTR path)
{
	for (; path && *path; path++) {
		if (*path == L'/')
			return 0;
	}
	return 1;
}

static LPWSTR full_path(LPCWSTR path)
{
	DWORD n;
	LPWSTR tmp, out;
	int len = lstrlenW(path);
	WCHAR drive[4];

	if (len == 2 && path[1] == L':') {
		drive[0] = path[0];
		drive[1] = L':';
		drive[2] = L'\\';
		drive[3] = 0;
		path = drive;
		len = 3;
	}
	tmp = (LPWSTR)LocalAlloc(LMEM_FIXED, (len + 1) * sizeof(WCHAR));
	if (!tmp)
		return NULL;
	lstrcpyW(tmp, path);
	n = GetFullPathNameW(tmp, 0, NULL, NULL);
	if (!n) {
		LocalFree(tmp);
		return NULL;
	}
	out = (LPWSTR)LocalAlloc(LMEM_FIXED, n * sizeof(WCHAR));
	if (!out) {
		LocalFree(tmp);
		return NULL;
	}
	if (!GetFullPathNameW(tmp, n, out, NULL)) {
		LocalFree(tmp);
		LocalFree(out);
		return NULL;
	}
	LocalFree(tmp);
	strip_dir_slash(out);
	return out;
}

static int is_same_or_child(LPCWSTR parent, LPCWSTR child)
{
	int plen = lstrlenW(parent);
	int clen = lstrlenW(child);
	int parent_root;

	if (plen > 1 && ends_with_sep(parent, plen) && !(plen == 3 && parent[1] == L':'))
		plen--;
	parent_root = ends_with_sep(parent, plen);
	if (clen < plen)
		return 0;
	if (CompareStringOrdinal(parent, plen, child, plen, TRUE) != CSTR_EQUAL)
		return 0;
	if (clen == plen || parent_root)
		return 1;
	return child[plen] == L'\\';
}

static int root_prefix_len(LPCWSTR path)
{
	int i;

	if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
		&& path[1] == L':')
		return path[2] == L'\\' ? 3 : 2;
	if (path[0] == L'\\' && path[1] == L'\\') {
		i = 2;
		while (path[i] && path[i] != L'\\')
			i++;
		if (!path[i])
			return i;
		i++;
		while (path[i] && path[i] != L'\\')
			i++;
		if (path[i] == L'\\')
			i++;
		return i;
	}
	return 0;
}

static int is_root_path(LPCWSTR path)
{
	return path[root_prefix_len(path)] == 0;
}

/* A colon is an alternate data stream, as in Thumbs.db:encryptable. */
static int bad_file_name(LPCWSTR name, int *colon)
{
	const wchar_t *p;

	if (colon)
		*colon = 0;
	if (!name || !name[0])
		return 1;
	for (p = name; *p; p++) {
		if (*p == L':') {
			if (colon)
				*colon = 1;
			return 1;
		}
		if (*p == L'<' || *p == L'>' || *p == L'"' || *p == L'|'
			|| *p == L'?' || *p == L'*' || *p < 32)
			return 1;
	}
	return 0;
}

static int path_has_bad_name(LPCWSTR path)
{
	const wchar_t *rest;

	if (!path)
		return 1;
	rest = path + root_prefix_len(path);
	if (!rest[0])
		return 0;
	return bad_file_name(rest, NULL);
}

static void skip_bad_name(LPCWSTR dir, LPCWSTR name)
{
	int colon = 0;

	bad_file_name(name, &colon);
	if (colon)
		caution(L"skipping \"%s\" in \"%s\": a colon is not a file name. Thumbs.db:encryptable is a thumbnail stream, not a file that can be copied",
			name, dir);
	else
		caution(L"skipping \"%s\" in \"%s\": the name is not valid on Windows",
			name, dir);
	InterlockedIncrement(&g_walk_err);
}

/* The \\?\ prefix keeps reserved names (CON, NUL, AUX, COM, LPT) as files
 * and keeps a trailing dot or space. Stored paths stay readable. */
static LPCWSTR as_api(LPCWSTR path, LPWSTR *owned)
{
	int n;
	LPWSTR out;

	*owned = NULL;
	if (!path || !path[0])
		return path;
	n = lstrlenW(path);
	if (n >= 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\')
		return path;
	if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
		&& path[1] == L':') {
		out = (LPWSTR)LocalAlloc(LMEM_FIXED, (n + 5) * sizeof(WCHAR));
		if (!out)
			return path;
		lstrcpyW(out, L"\\\\?\\");
		lstrcatW(out, path);
		*owned = out;
		return out;
	}
	if (path[0] == L'\\' && path[1] == L'\\') {
		out = (LPWSTR)LocalAlloc(LMEM_FIXED, (n + 8) * sizeof(WCHAR));
		if (!out)
			return path;
		lstrcpyW(out, L"\\\\?\\UNC\\");
		lstrcatW(out, path + 2);
		*owned = out;
		return out;
	}
	return path;
}

static HANDLE open_file(LPCWSTR path, DWORD access, DWORD share, DWORD disp, DWORD flags)
{
	LPWSTR owned = NULL;
	LPCWSTR p = as_api(path, &owned);
	HANDLE h = CreateFileW(p, access, share, NULL, disp, flags, NULL);

	if (h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();

		if (owned)
			LocalFree(owned);
		SetLastError(e);
		return h;
	}
	if (owned)
		LocalFree(owned);
	return h;
}

static DWORD get_attrs(LPCWSTR path)
{
	LPWSTR owned = NULL;
	LPCWSTR p = as_api(path, &owned);
	DWORD a = GetFileAttributesW(p);
	DWORD e = GetLastError();

	if (owned)
		LocalFree(owned);
	SetLastError(e);
	return a;
}

static int file_size_of(LPCWSTR path, ULONGLONG *out)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	LPWSTR owned = NULL;
	LPCWSTR p = as_api(path, &owned);
	int ok = GetFileAttributesExW(p, GetFileExInfoStandard, &a);
	DWORD e = GetLastError();

	if (owned)
		LocalFree(owned);
	if (!ok) {
		SetLastError(e);
		return 0;
	}
	if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return 0;
	*out = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
	return 1;
}

static void clear_readonly(LPCWSTR path)
{
	LPWSTR owned = NULL;
	LPCWSTR p = as_api(path, &owned);

	SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
	if (owned)
		LocalFree(owned);
}

#define KEEP_ATTR (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | \
	FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | \
	FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY)

static DWORD kept_attrs(DWORD src)
{
	DWORD a = src & KEEP_ATTR;

	if (src & FILE_ATTRIBUTE_DIRECTORY)
		a |= FILE_ATTRIBUTE_DIRECTORY;
	if (a & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY))
		a &= ~FILE_ATTRIBUTE_NORMAL;
	if (!(a & ~FILE_ATTRIBUTE_DIRECTORY))
		a |= FILE_ATTRIBUTE_NORMAL;
	if (a & FILE_ATTRIBUTE_DIRECTORY)
		a &= ~FILE_ATTRIBUTE_NORMAL;
	return a;
}

/* Times and ordinary attributes. Compression and encryption are left alone:
 * applying either rewrites the file and slows the copy. */
static void stamp_meta(LPCWSTR src, LPCWSTR dst)
{
	WIN32_FILE_ATTRIBUTE_DATA info;
	LPWSTR owned = NULL;
	LPCWSTR p;
	HANDLE h;
	DWORD attr;

	p = as_api(src, &owned);
	if (!GetFileAttributesExW(p, GetFileExInfoStandard, &info)) {
		caution(L"cannot read attributes: \"%s\"", src);
		if (owned)
			LocalFree(owned);
		return;
	}
	if (owned)
		LocalFree(owned);
	owned = NULL;

	clear_readonly(dst);
	h = open_file(dst, FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
	if (h == INVALID_HANDLE_VALUE ||
		!SetFileTime(h, &info.ftCreationTime, &info.ftLastAccessTime, &info.ftLastWriteTime))
		caution(L"cannot keep times: \"%s\"", dst);
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);

	attr = kept_attrs(info.dwFileAttributes);
	p = as_api(dst, &owned);
	if (!SetFileAttributesW(p, attr))
		caution(L"cannot keep attributes: \"%s\"", dst);
	if (owned)
		LocalFree(owned);
}

static void remember_dir(LPCWSTR src, LPCWSTR dst)
{
	struct dir_stamp *d;

	if (g_overwrite_pass)
		return;
	d = (struct dir_stamp *)LocalAlloc(LMEM_FIXED, sizeof(*d));

	if (!d) {
		warn(L"out of memory keeping attributes of \"%s\"", dst);
		return;
	}
	d->src = wcsdup_w(src);
	d->dst = wcsdup_w(dst);
	if (!d->src || !d->dst) {
		warn(L"out of memory keeping attributes of \"%s\"", dst);
		if (d->src)
			LocalFree(d->src);
		if (d->dst)
			LocalFree(d->dst);
		LocalFree(d);
		return;
	}
	d->next = g_dirs;
	g_dirs = d;
}

static void stamp_dirs(void)
{
	while (g_dirs) {
		struct dir_stamp *d = g_dirs;

		g_dirs = d->next;
		if (d->src && d->dst) {
			DWORD at = get_attrs(d->dst);

			if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
				stamp_meta(d->src, d->dst);
		}
		if (d->src)
			LocalFree(d->src);
		if (d->dst)
			LocalFree(d->dst);
		LocalFree(d);
	}
}

static int make_dir(LPCWSTR path)
{
	LPWSTR owned = NULL;
	LPCWSTR p = as_api(path, &owned);
	DWORD attr, e;
	int rc = 0;

	if (!CreateDirectoryW(p, NULL)) {
		e = GetLastError();
		if (e != ERROR_ALREADY_EXISTS) {
			rc = 1;
			SetLastError(e);
			goto done;
		}
		attr = GetFileAttributesW(p);
		if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
			rc = 2;
	}
done:
	if (owned)
		LocalFree(owned);
	return rc;
}

static int ensure_dir(LPCWSTR path)
{
	LPWSTR tmp;
	DWORD attr;
	int n, i, rc;

	if (is_root_path(path)) {
		attr = get_attrs(path);
		if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
			return 1;
		return 0;
	}
	tmp = wcsdup_w(path);
	if (!tmp)
		return 1;
	n = lstrlenW(tmp);
	i = root_prefix_len(tmp);
	for (; i < n; i++) {
		if (tmp[i] == L'\\') {
			tmp[i] = 0;
			rc = make_dir(tmp);
			tmp[i] = L'\\';
			if (rc) {
				LocalFree(tmp);
				return rc;
			}
		}
	}
	rc = make_dir(tmp);
	LocalFree(tmp);
	return rc;
}

static HANDLE find_first(LPCWSTR pattern, WIN32_FIND_DATAW *fd)
{
	LPWSTR owned = NULL;
	LPCWSTR p = as_api(pattern, &owned);
	HANDLE h = FindFirstFileExW(p, FindExInfoBasic, fd,
		FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);

	if (h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();

		if (owned)
			LocalFree(owned);
		SetLastError(e);
		return h;
	}
	if (owned)
		LocalFree(owned);
	return h;
}

static int is_dot_name(LPCWSTR name)
{
	return name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
}

/* Linux fifos and WSL specials use these tags. They are not in every header. */
#define SC_TAG_LX_FIFO 0x80000024L
#define SC_TAG_LX_CHR  0x80000025L
#define SC_TAG_LX_BLK  0x80000026L

/* Name surrogates (junctions, mount points, symbolic links) are not followed.
 * System-compressed and deduplicated files are real data and are copied. */
static const wchar_t *entity_skip_reason(DWORD attr, DWORD tag, int tag_known)
{
	if (attr & FILE_ATTRIBUTE_DEVICE)
		return L"it is a device";
	if (attr & (FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
		return L"its data is not stored on this disk";
	if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT))
		return NULL;
	if (!tag_known || tag == 0 || IsReparseTagNameSurrogate(tag))
		return L"it is a junction or symbolic link";
	if (tag == IO_REPARSE_TAG_APPEXECLINK)
		return L"it is an application execution alias";
	if (tag == IO_REPARSE_TAG_AF_UNIX)
		return L"it is a socket";
	if (tag == SC_TAG_LX_FIFO || tag == SC_TAG_LX_CHR || tag == SC_TAG_LX_BLK)
		return L"it is not a regular file";
	return NULL;
}

static int skip_special_entity(LPCWSTR path, DWORD attr, DWORD tag, int tag_known)
{
	const wchar_t *why = entity_skip_reason(attr, tag, tag_known);

	if (!why)
		return 0;
	caution(L"skipping \"%s\": %s", path, why);
	InterlockedIncrement(&g_walk_err);
	return 1;
}

static int reparse_tag_of(LPCWSTR path, DWORD *tag)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = find_first(path, &fd);

	if (h == INVALID_HANDLE_VALUE)
		return 0;
	*tag = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
	FindClose(h);
	return 1;
}

#define MD5_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD5_G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_STEP(f, a, b, c, d, w, sh, ac) do { \
	(a) += f((b), (c), (d)) + (w) + (unsigned int)(ac); \
	(a) = MD5_ROTL((a), (sh)); \
	(a) += (b); \
} while (0)

static unsigned int md5_load(const unsigned char *p)
{
	unsigned int v;

	memcpy(&v, p, 4);
	return v;
}

static void md5_block(unsigned int st[4], const unsigned char *blk)
{
	unsigned int a = st[0], b = st[1], c = st[2], d = st[3];
	unsigned int w0 = md5_load(blk), w1 = md5_load(blk + 4);
	unsigned int w2 = md5_load(blk + 8), w3 = md5_load(blk + 12);
	unsigned int w4 = md5_load(blk + 16), w5 = md5_load(blk + 20);
	unsigned int w6 = md5_load(blk + 24), w7 = md5_load(blk + 28);
	unsigned int w8 = md5_load(blk + 32), w9 = md5_load(blk + 36);
	unsigned int w10 = md5_load(blk + 40), w11 = md5_load(blk + 44);
	unsigned int w12 = md5_load(blk + 48), w13 = md5_load(blk + 52);
	unsigned int w14 = md5_load(blk + 56), w15 = md5_load(blk + 60);

	MD5_STEP(MD5_F, a, b, c, d, w0, 7, 0xd76aa478); MD5_STEP(MD5_F, d, a, b, c, w1, 12, 0xe8c7b756);
	MD5_STEP(MD5_F, c, d, a, b, w2, 17, 0x242070db); MD5_STEP(MD5_F, b, c, d, a, w3, 22, 0xc1bdceee);
	MD5_STEP(MD5_F, a, b, c, d, w4, 7, 0xf57c0faf); MD5_STEP(MD5_F, d, a, b, c, w5, 12, 0x4787c62a);
	MD5_STEP(MD5_F, c, d, a, b, w6, 17, 0xa8304613); MD5_STEP(MD5_F, b, c, d, a, w7, 22, 0xfd469501);
	MD5_STEP(MD5_F, a, b, c, d, w8, 7, 0x698098d8); MD5_STEP(MD5_F, d, a, b, c, w9, 12, 0x8b44f7af);
	MD5_STEP(MD5_F, c, d, a, b, w10, 17, 0xffff5bb1); MD5_STEP(MD5_F, b, c, d, a, w11, 22, 0x895cd7be);
	MD5_STEP(MD5_F, a, b, c, d, w12, 7, 0x6b901122); MD5_STEP(MD5_F, d, a, b, c, w13, 12, 0xfd987193);
	MD5_STEP(MD5_F, c, d, a, b, w14, 17, 0xa679438e); MD5_STEP(MD5_F, b, c, d, a, w15, 22, 0x49b40821);

	MD5_STEP(MD5_G, a, b, c, d, w1, 5, 0xf61e2562); MD5_STEP(MD5_G, d, a, b, c, w6, 9, 0xc040b340);
	MD5_STEP(MD5_G, c, d, a, b, w11, 14, 0x265e5a51); MD5_STEP(MD5_G, b, c, d, a, w0, 20, 0xe9b6c7aa);
	MD5_STEP(MD5_G, a, b, c, d, w5, 5, 0xd62f105d); MD5_STEP(MD5_G, d, a, b, c, w10, 9, 0x02441453);
	MD5_STEP(MD5_G, c, d, a, b, w15, 14, 0xd8a1e681); MD5_STEP(MD5_G, b, c, d, a, w4, 20, 0xe7d3fbc8);
	MD5_STEP(MD5_G, a, b, c, d, w9, 5, 0x21e1cde6); MD5_STEP(MD5_G, d, a, b, c, w14, 9, 0xc33707d6);
	MD5_STEP(MD5_G, c, d, a, b, w3, 14, 0xf4d50d87); MD5_STEP(MD5_G, b, c, d, a, w8, 20, 0x455a14ed);
	MD5_STEP(MD5_G, a, b, c, d, w13, 5, 0xa9e3e905); MD5_STEP(MD5_G, d, a, b, c, w2, 9, 0xfcefa3f8);
	MD5_STEP(MD5_G, c, d, a, b, w7, 14, 0x676f02d9); MD5_STEP(MD5_G, b, c, d, a, w12, 20, 0x8d2a4c8a);

	MD5_STEP(MD5_H, a, b, c, d, w5, 4, 0xfffa3942); MD5_STEP(MD5_H, d, a, b, c, w8, 11, 0x8771f681);
	MD5_STEP(MD5_H, c, d, a, b, w11, 16, 0x6d9d6122); MD5_STEP(MD5_H, b, c, d, a, w14, 23, 0xfde5380c);
	MD5_STEP(MD5_H, a, b, c, d, w1, 4, 0xa4beea44); MD5_STEP(MD5_H, d, a, b, c, w4, 11, 0x4bdecfa9);
	MD5_STEP(MD5_H, c, d, a, b, w7, 16, 0xf6bb4b60); MD5_STEP(MD5_H, b, c, d, a, w10, 23, 0xbebfbc70);
	MD5_STEP(MD5_H, a, b, c, d, w13, 4, 0x289b7ec6); MD5_STEP(MD5_H, d, a, b, c, w0, 11, 0xeaa127fa);
	MD5_STEP(MD5_H, c, d, a, b, w3, 16, 0xd4ef3085); MD5_STEP(MD5_H, b, c, d, a, w6, 23, 0x04881d05);
	MD5_STEP(MD5_H, a, b, c, d, w9, 4, 0xd9d4d039); MD5_STEP(MD5_H, d, a, b, c, w12, 11, 0xe6db99e5);
	MD5_STEP(MD5_H, c, d, a, b, w15, 16, 0x1fa27cf8); MD5_STEP(MD5_H, b, c, d, a, w2, 23, 0xc4ac5665);

	MD5_STEP(MD5_I, a, b, c, d, w0, 6, 0xf4292244); MD5_STEP(MD5_I, d, a, b, c, w7, 10, 0x432aff97);
	MD5_STEP(MD5_I, c, d, a, b, w14, 15, 0xab9423a7); MD5_STEP(MD5_I, b, c, d, a, w5, 21, 0xfc93a039);
	MD5_STEP(MD5_I, a, b, c, d, w12, 6, 0x655b59c3); MD5_STEP(MD5_I, d, a, b, c, w3, 10, 0x8f0ccc92);
	MD5_STEP(MD5_I, c, d, a, b, w10, 15, 0xffeff47d); MD5_STEP(MD5_I, b, c, d, a, w1, 21, 0x85845dd1);
	MD5_STEP(MD5_I, a, b, c, d, w8, 6, 0x6fa87e4f); MD5_STEP(MD5_I, d, a, b, c, w15, 10, 0xfe2ce6e0);
	MD5_STEP(MD5_I, c, d, a, b, w6, 15, 0xa3014314); MD5_STEP(MD5_I, b, c, d, a, w13, 21, 0x4e0811a1);
	MD5_STEP(MD5_I, a, b, c, d, w4, 6, 0xf7537e82); MD5_STEP(MD5_I, d, a, b, c, w11, 10, 0xbd3af235);
	MD5_STEP(MD5_I, c, d, a, b, w2, 15, 0x2ad7d2bb); MD5_STEP(MD5_I, b, c, d, a, w9, 21, 0xeb86d391);

	st[0] += a;
	st[1] += b;
	st[2] += c;
	st[3] += d;
}

static void md5_init(struct md5 *m)
{
	m->s[0] = 0x67452301u;
	m->s[1] = 0xefcdab89u;
	m->s[2] = 0x98badcfeu;
	m->s[3] = 0x10325476u;
	m->nbytes = 0;
	m->nbuf = 0;
}

static void md5_update(struct md5 *m, const void *data, unsigned int len)
{
	const unsigned char *p = data;

	m->nbytes += len;
	if (m->nbuf) {
		unsigned int take = 64 - m->nbuf;

		if (take > len)
			take = len;
		memcpy(m->buf + m->nbuf, p, take);
		m->nbuf += take;
		p += take;
		len -= take;
		if (m->nbuf == 64) {
			md5_block(m->s, m->buf);
			m->nbuf = 0;
		}
	}
	while (len >= 64) {
		unsigned int blocks = len >> 6;

		do {
			md5_block(m->s, p);
			p += 64;
		} while (--blocks);
		len &= 63;
	}
	if (len) {
		memcpy(m->buf + m->nbuf, p, len);
		m->nbuf += len;
	}
}

static void md5_final(struct md5 *m, unsigned char out[16])
{
	ULONGLONG bits = m->nbytes << 3;
	unsigned int n = m->nbuf, i;

	m->buf[n++] = 0x80;
	if (n > 56) {
		while (n < 64)
			m->buf[n++] = 0;
		md5_block(m->s, m->buf);
		n = 0;
	}
	while (n < 56)
		m->buf[n++] = 0;
	for (i = 0; i < 8; i++)
		m->buf[56 + i] = (unsigned char)(bits >> (8 * i));
	md5_block(m->s, m->buf);
	for (i = 0; i < 4; i++) {
		out[i * 4] = (unsigned char)m->s[i];
		out[i * 4 + 1] = (unsigned char)(m->s[i] >> 8);
		out[i * 4 + 2] = (unsigned char)(m->s[i] >> 16);
		out[i * 4 + 3] = (unsigned char)(m->s[i] >> 24);
	}
}

static void hex_md5(const BYTE m[16], int ok, LPWSTR out)
{
	static const wchar_t *hexd = L"0123456789abcdef";
	int i;

	if (!ok) {
		out[0] = L'-';
		out[1] = 0;
		return;
	}
	for (i = 0; i < 16; i++) {
		out[i * 2] = hexd[m[i] >> 4];
		out[i * 2 + 1] = hexd[m[i] & 0xf];
	}
	out[32] = 0;
}

static int io_too_big(DWORD err)
{
	return err == ERROR_INVALID_PARAMETER
		|| err == ERROR_NO_SYSTEM_RESOURCES
		|| err == ERROR_NOT_ENOUGH_MEMORY
		|| err == ERROR_WORKING_SET_QUOTA;
}

static int is_unsup(DWORD err)
{
	return err == ERROR_INVALID_PARAMETER
		|| err == ERROR_INVALID_FUNCTION
		|| err == ERROR_NOT_SUPPORTED;
}

static DWORD alloc_buffers(LPVOID buf[2])
{
	DWORD size;

	for (size = BLOCK_SIZE; size >= MIN_BLOCK; size >>= 1) {
		buf[0] = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		buf[1] = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (buf[0] && buf[1])
			return size;
		if (buf[0])
			VirtualFree(buf[0], 0, MEM_RELEASE);
		if (buf[1])
			VirtualFree(buf[1], 0, MEM_RELEASE);
		buf[0] = buf[1] = NULL;
	}
	return 0;
}

static void free_buffers(LPVOID buf[2])
{
	if (buf[0])
		VirtualFree(buf[0], 0, MEM_RELEASE);
	if (buf[1])
		VirtualFree(buf[1], 0, MEM_RELEASE);
	buf[0] = buf[1] = NULL;
}

static DWORD chunk_len(ULONGLONG total, ULONGLONG at, DWORD xfer)
{
	ULONGLONG left = total - at;

	if (xfer && left > xfer)
		return xfer;
	if (left > 0xffffffffULL)
		return 0xffffffffu;
	return (DWORD)left;
}

static int set_eof(HANDLE file, ULONGLONG n)
{
	FILE_END_OF_FILE_INFO eof;
	FILE_ALLOCATION_INFO alloc;
	LARGE_INTEGER pos;
	ULONGLONG reserve = (n + (ALIGN_SIZE - 1)) & ~(ULONGLONG)(ALIGN_SIZE - 1);

	if (reserve) {
		alloc.AllocationSize.QuadPart = (LONGLONG)reserve;
		SetFileInformationByHandle(file, FileAllocationInfo, &alloc, sizeof(alloc));
	}
	eof.EndOfFile.QuadPart = (LONGLONG)n;
	if (SetFileInformationByHandle(file, FileEndOfFileInfo, &eof, sizeof(eof)))
		return 1;
	pos.QuadPart = (LONGLONG)n;
	return SetFilePointerEx(file, pos, NULL, FILE_BEGIN) && SetEndOfFile(file);
}

static void shrink_file(LPCWSTR path, ULONGLONG n)
{
	HANDLE h = open_file(path, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);

	if (h == INVALID_HANDLE_VALUE)
		return;
	set_eof(h, n);
	CloseHandle(h);
}

static int begin_io(HANDLE file, struct io_slot *op, int write, void *buf, DWORD len, ULONGLONG offset)
{
	BOOL ok;
	DWORD n = 0;

	op->pending = 0;
	op->want = len;
	op->got = 0;
	ZeroMemory(&op->ov, sizeof(op->ov));
	op->ov.Offset = (DWORD)offset;
	op->ov.OffsetHigh = (DWORD)(offset >> 32);
	op->ov.hEvent = op->ev;
	ResetEvent(op->ev);
	if (write)
		ok = WriteFile(file, buf, len, &n, &op->ov);
	else
		ok = ReadFile(file, buf, len, &n, &op->ov);
	if (!ok) {
		if (GetLastError() != ERROR_IO_PENDING)
			return 0;
		op->pending = 1;
		return 1;
	}
	op->got = n;
	return n == len;
}

static int end_io(HANDLE file, struct io_slot *op)
{
	DWORD n = op->got;

	if (op->pending) {
		if (!GetOverlappedResult(file, &op->ov, &n, TRUE)) {
			op->pending = 0;
			return 0;
		}
		op->pending = 0;
		op->got = n;
	}
	return n == op->want;
}

static void drop_io(HANDLE file, struct io_slot *op)
{
	DWORD n;

	if (!op->pending || file == INVALID_HANDLE_VALUE)
		return;
	CancelIoEx(file, &op->ov);
	GetOverlappedResult(file, &op->ov, &n, TRUE);
	op->pending = 0;
}

static int retry_begin(HANDLE file, struct io_slot *op, int write, void *buf, DWORD len, ULONGLONG offset)
{
	int attempt;

	for (attempt = 0; attempt < IO_ATTEMPTS; attempt++) {
		if (begin_io(file, op, write, buf, len, offset))
			return 1;
	}
	return 0;
}

static int retry_end(HANDLE file, struct io_slot *op, int write, void *buf, DWORD len, ULONGLONG offset)
{
	int attempt;

	if (end_io(file, op))
		return 1;
	for (attempt = 1; attempt < IO_ATTEMPTS; attempt++) {
		drop_io(file, op);
		if (!begin_io(file, op, write, buf, len, offset))
			continue;
		if (end_io(file, op))
			return 1;
	}
	return 0;
}

static int io_at(HANDLE file, int write, void *buf, DWORD len, ULONGLONG offset)
{
	LARGE_INTEGER pos;
	int attempt;

	pos.QuadPart = (LONGLONG)offset;
	for (attempt = 0; attempt < IO_ATTEMPTS; attempt++) {
		DWORD got = 0;
		BOOL ok;

		if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN))
			continue;
		if (write)
			ok = WriteFile(file, buf, len, &got, NULL);
		else
			ok = ReadFile(file, buf, len, &got, NULL);
		if (ok && got == len)
			return 1;
	}
	return 0;
}

static DWORD create_preallocated(LPCWSTR path, ULONGLONG logical)
{
	HANDLE h;
	DWORD err;

	clear_readonly(path);
	h = open_file(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
	if (h == INVALID_HANDLE_VALUE) {
		err = GetLastError();
		return err ? err : ERROR_GEN_FAILURE;
	}
	if (logical && !set_eof(h, logical)) {
		err = GetLastError();
		CloseHandle(h);
		shrink_file(path, 0);
		return err ? err : ERROR_GEN_FAILURE;
	}
	CloseHandle(h);
	return 0;
}

static int user_stopped(void)
{
	return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
}

/* Double-buffered overlapped copy of the byte range [0, end). */
static DWORD copy_pipeline(LPCWSTR src_path, LPCWSTR dst_path, LPVOID buf[2], DWORD xfer,
	ULONGLONG end, DWORD flags, struct md5 *hash, ULONGLONG *committed,
	struct op_err *oe, int *unsup)
{
	int reading = 0, writing = 0, slot = 0, wr_slot = 0;
	HANDLE src = INVALID_HANDLE_VALUE, dst = INVALID_HANDLE_VALUE;
	struct io_slot rd, wr;
	ULONGLONG rd_at = 0, wr_at = 0, wr_off = 0, done = 0;
	DWORD rd_len = 0, wr_len = 0, err = 0;

	ZeroMemory(&rd, sizeof(rd));
	ZeroMemory(&wr, sizeof(wr));
	*committed = 0;
	*unsup = 0;
	if (!end)
		return 0;

	rd.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	wr.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!rd.ev || !wr.ev) {
		set_err(oe, GetLastError(), dst_path, L"Create event");
		err = oe->code;
		goto done;
	}
	src = open_file(src_path, GENERIC_READ, SHARE_ANY, OPEN_EXISTING, flags);
	if (src == INVALID_HANDLE_VALUE) {
		err = GetLastError();
		if ((flags & FILE_FLAG_NO_BUFFERING) && is_unsup(err))
			*unsup = 1;
		set_err(oe, err, src_path, L"Open source");
		err = oe->code;
		goto done;
	}
	dst = open_file(dst_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		OPEN_EXISTING, flags);
	if (dst == INVALID_HANDLE_VALUE) {
		err = GetLastError();
		if ((flags & FILE_FLAG_NO_BUFFERING) && is_unsup(err))
			*unsup = 1;
		set_err(oe, err, dst_path, L"Open destination");
		err = oe->code;
		goto done;
	}
	rd_len = chunk_len(end, 0, xfer);
	if (!retry_begin(src, &rd, 0, buf[0], rd_len, 0)) {
		set_err(oe, GetLastError(), src_path, L"Read");
		err = oe->code;
		goto done;
	}
	reading = 1;

	while (reading || writing) {
		if (user_stopped()) {
			set_err(oe, ERROR_CANCELLED, dst_path, L"Stop");
			err = oe->code;
			goto done;
		}
		if (reading) {
			DWORD hash_len;
			int hash_slot;

			if (!retry_end(src, &rd, 0, buf[slot], rd_len, rd_at)) {
				set_err(oe, GetLastError(), src_path, L"Read");
				err = oe->code;
				goto done;
			}
			reading = 0;
			if (writing) {
				if (!retry_end(dst, &wr, 1, buf[wr_slot], wr_len, wr_off)) {
					set_err(oe, GetLastError(), dst_path, L"Write");
					err = oe->code;
					goto done;
				}
				writing = 0;
				done += wr_len;
				InterlockedAdd64(&g_copy_progress, (LONG64)wr_len);
			}
			wr_slot = slot;
			wr_len = rd_len;
			wr_off = wr_at;
			if (!retry_begin(dst, &wr, 1, buf[slot], rd_len, wr_at)) {
				set_err(oe, GetLastError(), dst_path, L"Write");
				err = oe->code;
				goto done;
			}
			writing = 1;
			wr_at += rd_len;
			hash_slot = slot;
			hash_len = rd_len;
			rd_at += rd_len;
			if (rd_at < end) {
				slot ^= 1;
				rd_len = chunk_len(end, rd_at, xfer);
				if (!retry_begin(src, &rd, 0, buf[slot], rd_len, rd_at)) {
					set_err(oe, GetLastError(), src_path, L"Read");
					err = oe->code;
					goto done;
				}
				reading = 1;
			}
			if (hash)
				md5_update(hash, buf[hash_slot], hash_len);
		} else if (writing) {
			if (!retry_end(dst, &wr, 1, buf[wr_slot], wr_len, wr_off)) {
				set_err(oe, GetLastError(), dst_path, L"Write");
				err = oe->code;
				goto done;
			}
			writing = 0;
			done += wr_len;
			InterlockedAdd64(&g_copy_progress, (LONG64)wr_len);
		}
	}

done:
	*committed = done;
	drop_io(src, &rd);
	drop_io(dst, &wr);
	if (rd.ev)
		CloseHandle(rd.ev);
	if (wr.ev)
		CloseHandle(wr.ev);
	if (src != INVALID_HANDLE_VALUE)
		CloseHandle(src);
	if (dst != INVALID_HANDLE_VALUE)
		CloseHandle(dst);
	return err;
}

static DWORD copy_tail(LPCWSTR src_path, LPCWSTR dst_path, ULONGLONG offset, DWORD len,
	struct md5 *hash, struct op_err *oe)
{
	HANDLE src = INVALID_HANDLE_VALUE, dst = INVALID_HANDLE_VALUE;
	unsigned char tail[ALIGN_SIZE];
	DWORD err = 0;

	if (user_stopped()) {
		set_err(oe, ERROR_CANCELLED, dst_path, L"Stop");
		return oe->code;
	}
	src = open_file(src_path, GENERIC_READ, SHARE_ANY, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
	if (src == INVALID_HANDLE_VALUE) {
		set_err(oe, GetLastError(), src_path, L"Read");
		err = oe->code;
		goto done;
	}
	dst = open_file(dst_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
	if (dst == INVALID_HANDLE_VALUE) {
		set_err(oe, GetLastError(), dst_path, L"Write");
		err = oe->code;
		goto done;
	}
	if (!io_at(src, 0, tail, len, offset)) {
		set_err(oe, GetLastError(), src_path, L"Read");
		err = oe->code;
		goto done;
	}
	if (hash)
		md5_update(hash, tail, len);
	if (!io_at(dst, 1, tail, len, offset)) {
		set_err(oe, GetLastError(), dst_path, L"Write");
		err = oe->code;
		goto done;
	}
	InterlockedAdd64(&g_copy_progress, (LONG64)len);

done:
	if (src != INVALID_HANDLE_VALUE)
		CloseHandle(src);
	if (dst != INVALID_HANDLE_VALUE)
		CloseHandle(dst);
	return err;
}

static DWORD copy_sync_handle(HANDLE src, LPCWSTR src_path, LPCWSTR dst_path,
	void *buf, DWORD cap, ULONGLONG logical, struct md5 *hash, struct op_err *oe)
{
	HANDLE dst = INVALID_HANDLE_VALUE;
	ULONGLONG at = 0;
	DWORD err = 0;

	if (!buf || !cap) {
		CloseHandle(src);
		set_err(oe, ERROR_NOT_ENOUGH_MEMORY, src_path, L"Buffer");
		return oe->code;
	}
	clear_readonly(dst_path);
	dst = open_file(dst_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
	if (dst == INVALID_HANDLE_VALUE) {
		set_err(oe, GetLastError(), dst_path, L"Create");
		CloseHandle(src);
		return oe->code;
	}
	while (at < logical) {
		DWORD n = chunk_len(logical, at, cap);

		if (user_stopped()) {
			set_err(oe, ERROR_CANCELLED, dst_path, L"Stop");
			err = oe->code;
			break;
		}
		if (!io_at(src, 0, buf, n, at)) {
			set_err(oe, GetLastError(), src_path, L"Read");
			err = oe->code;
			break;
		}
		if (hash)
			md5_update(hash, buf, n);
		if (!io_at(dst, 1, buf, n, at)) {
			set_err(oe, GetLastError(), dst_path, L"Write");
			err = oe->code;
			break;
		}
		at += n;
		InterlockedAdd64(&g_copy_progress, (LONG64)n);
	}
	CloseHandle(src);
	CloseHandle(dst);
	if (err)
		shrink_file(dst_path, at);
	return err;
}

static DWORD copy_span_retry(LPCWSTR src_path, LPCWSTR dst_path, LPVOID buf[2], DWORD xfer,
	ULONGLONG logical, int unbuffered, struct md5 *hash, struct op_err *oe, int *fallback)
{
	DWORD flags = FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN;
	DWORD use = xfer, err = 0;
	ULONGLONG span = logical;

	*fallback = 0;
	if (unbuffered) {
		flags |= FILE_FLAG_NO_BUFFERING;
		span = logical & ~(ULONGLONG)(ALIGN_SIZE - 1);
	}
	err = create_preallocated(dst_path, logical);
	if (err) {
		set_err(oe, err, dst_path, L"Create");
		return err;
	}
	for (;;) {
		int last = use <= MIN_BLOCK;
		int unsup = 0;
		ULONGLONG committed = 0;

		if (user_stopped()) {
			set_err(oe, ERROR_CANCELLED, dst_path, L"Stop");
			return oe->code;
		}
		if (hash)
			md5_init(hash);
		err = 0;
		if (span) {
			err = copy_pipeline(src_path, dst_path, buf, use, span, flags,
				hash, &committed, oe, &unsup);
		}
		if (!err && unbuffered && logical > span) {
			err = copy_tail(src_path, dst_path, span, (DWORD)(logical - span), hash, oe);
			if (err)
				committed = span;
		}
		if (!err)
			return 0;
		shrink_file(dst_path, committed);
		if (unsup) {
			*fallback = 1;
			shrink_file(dst_path, 0);
			return err;
		}
		if (last || !io_too_big(err))
			return err;
		use >>= 1;
	}
}

static void delete_path(LPCWSTR path)
{
	LPWSTR owned = NULL;

	DeleteFileW(as_api(path, &owned));
	if (owned)
		LocalFree(owned);
}

static int hash_hole(struct md5 *hash, void *buf, DWORD cap, ULONGLONG n)
{
	InterlockedAdd64(&g_copy_progress, (LONG64)n);
	if (!hash || !n || !buf || !cap)
		return user_stopped() ? 0 : 1;
	memset(buf, 0, cap);
	while (n) {
		DWORD c;

		if (user_stopped())
			return 0;
		c = n > cap ? cap : (DWORD)n;
		md5_update(hash, buf, c);
		n -= c;
	}
	return 1;
}

/* Copy only the allocated ranges of a sparse file so holes stay holes.
 * Returns 1 when this path handled the file. Returns 0 to use the normal copy. */
static int copy_sparse(HANDLE src, LPCWSTR src_path, LPCWSTR dst_path, void *buf, DWORD cap,
	ULONGLONG logical, struct md5 *hash, DWORD *err_out, struct op_err *oe)
{
	FILE_ALLOCATED_RANGE_BUFFER in, ranges[64];
	HANDLE dst = INVALID_HANDLE_VALUE;
	ULONGLONG at = 0, query = 0;
	DWORD got = 0, sparse_on = 1;
	FILE_END_OF_FILE_INFO eof;
	int saw = 0;

	*err_out = 0;
	if (!buf || !cap || !logical)
		return 0;
	in.FileOffset.QuadPart = 0;
	in.Length.QuadPart = (LONGLONG)logical;
	if (!DeviceIoControl(src, FSCTL_QUERY_ALLOCATED_RANGES, &in, sizeof(in),
		ranges, sizeof(ranges), &got, NULL) && GetLastError() != ERROR_MORE_DATA)
		return 0;

	clear_readonly(dst_path);
	dst = open_file(dst_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
	if (dst == INVALID_HANDLE_VALUE) {
		set_err(oe, GetLastError(), dst_path, L"Create");
		*err_out = oe->code;
		return 1;
	}
	if (!DeviceIoControl(dst, FSCTL_SET_SPARSE, &sparse_on, sizeof(sparse_on),
		NULL, 0, &got, NULL)) {
		CloseHandle(dst);
		delete_path(dst_path);
		return 0;
	}
	eof.EndOfFile.QuadPart = (LONGLONG)logical;
	if (!SetFileInformationByHandle(dst, FileEndOfFileInfo, &eof, sizeof(eof))) {
		LARGE_INTEGER pos;

		pos.QuadPart = (LONGLONG)logical;
		if (!SetFilePointerEx(dst, pos, NULL, FILE_BEGIN) || !SetEndOfFile(dst)) {
			set_err(oe, GetLastError(), dst_path, L"Set size");
			*err_out = oe->code;
			CloseHandle(dst);
			return 1;
		}
	}
	if (hash)
		md5_init(hash);

	for (;;) {
		int n, i;
		DWORD qerr = 0;
		BOOL ok;

		if (query >= logical)
			break;
		in.FileOffset.QuadPart = (LONGLONG)query;
		in.Length.QuadPart = (LONGLONG)(logical - query);
		ok = DeviceIoControl(src, FSCTL_QUERY_ALLOCATED_RANGES, &in, sizeof(in),
			ranges, sizeof(ranges), &got, NULL);
		if (!ok)
			qerr = GetLastError();
		n = (int)(got / sizeof(ranges[0]));
		if (!ok && qerr != ERROR_MORE_DATA) {
			if (!saw) {
				CloseHandle(dst);
				delete_path(dst_path);
				return 0;
			}
			set_err(oe, qerr ? qerr : ERROR_GEN_FAILURE, src_path, L"Read");
			*err_out = oe->code;
			CloseHandle(dst);
			return 1;
		}
		for (i = 0; i < n; i++) {
			ULONGLONG off = (ULONGLONG)ranges[i].FileOffset.QuadPart;
			ULONGLONG len = (ULONGLONG)ranges[i].Length.QuadPart;
			ULONGLONG left;

			if (off < at)
				off = at;
			if (off > logical)
				off = logical;
			if (off > at && !hash_hole(hash, buf, cap, off - at)) {
				set_err(oe, ERROR_CANCELLED, dst_path, L"Stop");
				*err_out = oe->code;
				CloseHandle(dst);
				return 1;
			}
			at = off;
			if (len > logical - at)
				len = logical - at;
			left = len;
			while (left) {
				DWORD chunk = left > cap ? cap : (DWORD)left;

				if (user_stopped()) {
					set_err(oe, ERROR_CANCELLED, dst_path, L"Stop");
					*err_out = oe->code;
					CloseHandle(dst);
					return 1;
				}
				if (!io_at(src, 0, buf, chunk, at)) {
					set_err(oe, GetLastError(), src_path, L"Read");
					*err_out = oe->code;
					CloseHandle(dst);
					return 1;
				}
				if (hash)
					md5_update(hash, buf, chunk);
				if (!io_at(dst, 1, buf, chunk, at)) {
					set_err(oe, GetLastError(), dst_path, L"Write");
					*err_out = oe->code;
					CloseHandle(dst);
					return 1;
				}
				InterlockedAdd64(&g_copy_progress, (LONG64)chunk);
				at += chunk;
				left -= chunk;
			}
			saw = 1;
		}
		if (n > 0) {
			ULONGLONG end = (ULONGLONG)ranges[n - 1].FileOffset.QuadPart
				+ (ULONGLONG)ranges[n - 1].Length.QuadPart;

			if (end > query)
				query = end;
			else
				break;
		}
		if (ok || n == 0)
			break;
	}
	if (at < logical && !hash_hole(hash, buf, cap, logical - at)) {
		set_err(oe, ERROR_CANCELLED, dst_path, L"Stop");
		*err_out = oe->code;
		CloseHandle(dst);
		return 1;
	}
	CloseHandle(dst);
	return 1;
}

static DWORD copy_auto(LPCWSTR src_path, LPCWSTR dst_path, LPVOID buf[2], DWORD xfer,
	BYTE md5out[16], ULONGLONG *size_out, int *size_valid, struct op_err *oe)
{
	HANDLE src;
	LARGE_INTEGER sz;
	ULONGLONG logical;
	DWORD err;
	int fallback = 0;
	struct md5 hash;
	struct md5 *hp = NULL;

	if (size_valid)
		*size_valid = 0;
	if (size_out)
		*size_out = 0;
	src = open_file(src_path, GENERIC_READ, SHARE_ANY, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
	if (src == INVALID_HANDLE_VALUE) {
		set_err(oe, GetLastError(), src_path, L"Open source");
		return oe->code;
	}
	if (!GetFileSizeEx(src, &sz)) {
		set_err(oe, GetLastError(), src_path, L"Read size");
		CloseHandle(src);
		return oe->code;
	}
	logical = (ULONGLONG)sz.QuadPart;
	if (size_out)
		*size_out = logical;
	if (size_valid)
		*size_valid = 1;
	if (md5out)
		hp = &hash;
	{
		DWORD kind = GetFileType(src);

		if (kind == FILE_TYPE_PIPE || kind == FILE_TYPE_CHAR) {
			CloseHandle(src);
			set_err(oe, ERROR_INVALID_FUNCTION, src_path, L"Not a regular file");
			return oe->code;
		}
	}
	if (logical && (get_attrs(src_path) & FILE_ATTRIBUTE_SPARSE_FILE)) {
		int did = 0;

		err = 0;
		did = copy_sparse(src, src_path, dst_path, buf ? buf[0] : NULL, xfer,
			logical, hp, &err, oe);
		if (did) {
			CloseHandle(src);
			if (!err && hp)
				md5_final(hp, md5out);
			return err;
		}
	}

	/* An empty file is only created. There are no bytes to read or hash. */
	if (!logical) {
		CloseHandle(src);
		err = create_preallocated(dst_path, 0);
		if (err) {
			set_err(oe, err, dst_path, L"Create");
			return err;
		}
		if (hp) {
			md5_init(hp);
			md5_final(hp, md5out);
		}
		return 0;
	}

	/* Below 16 MiB the setup of an overlapped unbuffered copy costs more than it saves. */
	if (logical < UNBUFFERED_MIN) {
		if (hp)
			md5_init(hp);
		err = copy_sync_handle(src, src_path, dst_path, buf[0], xfer, logical, hp, oe);
		if (!err && hp)
			md5_final(hp, md5out);
		return err;
	}
	CloseHandle(src);

	if (!g_no_unbuffered) {
		err = copy_span_retry(src_path, dst_path, buf, xfer, logical, 1, hp, oe, &fallback);
		if (!err) {
			if (hp)
				md5_final(hp, md5out);
			return 0;
		}
		if (!fallback)
			return err;
		InterlockedExchange(&g_no_unbuffered, 1);
	}
	err = copy_span_retry(src_path, dst_path, buf, xfer, logical, 0, hp, oe, &fallback);
	if (!err && hp)
		md5_final(hp, md5out);
	return err;
}

static DWORD hash_sync(HANDLE h, void *buf, DWORD cap, ULONGLONG logical, struct md5 *hash)
{
	ULONGLONG at = 0;

	if (!buf || !cap)
		return ERROR_NOT_ENOUGH_MEMORY;
	while (at < logical) {
		DWORD n = chunk_len(logical, at, cap);

		if (user_stopped() || InterlockedCompareExchange(&g_fail_stop, 0, 0))
			return ERROR_CANCELLED;
		if (!io_at(h, 0, buf, n, at)) {
			DWORD e = GetLastError();

			return e ? e : ERROR_GEN_FAILURE;
		}
		md5_update(hash, buf, n);
		InterlockedAdd64(&g_ver_progress, (LONG64)n);
		at += n;
	}
	return 0;
}

/* Overlapped read of a large file. MD5 of one buffer runs while the next read is in flight. */
static DWORD hash_pipeline(LPCWSTR path, LPVOID buf[2], DWORD xfer, ULONGLONG logical,
	struct md5 *hash)
{
	HANDLE h = INVALID_HANDLE_VALUE;
	struct io_slot rd;
	ULONGLONG at = 0;
	DWORD len, err = 0;
	int reading = 0, slot = 0;

	ZeroMemory(&rd, sizeof(rd));
	rd.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!rd.ev) {
		err = GetLastError();
		return err ? err : ERROR_GEN_FAILURE;
	}
	h = open_file(path, GENERIC_READ, SHARE_ANY, OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN);
	if (h == INVALID_HANDLE_VALUE) {
		err = GetLastError();
		CloseHandle(rd.ev);
		return err ? err : ERROR_GEN_FAILURE;
	}
	len = chunk_len(logical, 0, xfer);
	if (!retry_begin(h, &rd, 0, buf[0], len, 0)) {
		err = GetLastError();
		if (!err)
			err = ERROR_GEN_FAILURE;
		goto done;
	}
	reading = 1;
	while (reading) {
		DWORD hash_len;
		int hash_slot;

		if (user_stopped() || InterlockedCompareExchange(&g_fail_stop, 0, 0)) {
			err = ERROR_CANCELLED;
			goto done;
		}
		if (!retry_end(h, &rd, 0, buf[slot], len, at)) {
			err = GetLastError();
			if (!err)
				err = ERROR_GEN_FAILURE;
			goto done;
		}
		reading = 0;
		hash_slot = slot;
		hash_len = len;
		at += len;
		if (at < logical) {
			slot ^= 1;
			len = chunk_len(logical, at, xfer);
			if (!retry_begin(h, &rd, 0, buf[slot], len, at)) {
				err = GetLastError();
				if (!err)
					err = ERROR_GEN_FAILURE;
				goto done;
			}
			reading = 1;
		}
		md5_update(hash, buf[hash_slot], hash_len);
		InterlockedAdd64(&g_ver_progress, (LONG64)hash_len);
	}

done:
	drop_io(h, &rd);
	if (rd.ev)
		CloseHandle(rd.ev);
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	return err;
}

static DWORD hash_file(LPCWSTR path, LPVOID buf[2], DWORD cap, BYTE out[16],
	ULONGLONG *size_out)
{
	HANDLE h;
	LARGE_INTEGER sz;
	ULONGLONG logical;
	struct md5 hash;
	DWORD err;

	if (size_out)
		*size_out = 0;
	h = open_file(path, GENERIC_READ, SHARE_ANY, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
	if (h == INVALID_HANDLE_VALUE) {
		err = GetLastError();
		return err ? err : ERROR_GEN_FAILURE;
	}
	if (!GetFileSizeEx(h, &sz)) {
		err = GetLastError();
		CloseHandle(h);
		return err ? err : ERROR_GEN_FAILURE;
	}
	logical = (ULONGLONG)sz.QuadPart;
	if (size_out)
		*size_out = logical;
	md5_init(&hash);
	if (!logical) {
		CloseHandle(h);
		md5_final(&hash, out);
		return 0;
	}
	if (logical >= MIN_BLOCK && buf && buf[0] && buf[1] && cap >= MIN_BLOCK) {
		CloseHandle(h);
		h = INVALID_HANDLE_VALUE;
		err = hash_pipeline(path, buf, cap, logical, &hash);
		if (!err) {
			md5_final(&hash, out);
			return 0;
		}
		if (hash.nbytes || err == ERROR_CANCELLED)
			return err;
		/* Overlapped reads are not available on every file system. Read it plainly. */
		md5_init(&hash);
		h = open_file(path, GENERIC_READ, SHARE_ANY, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
		if (h == INVALID_HANDLE_VALUE) {
			err = GetLastError();
			return err ? err : ERROR_GEN_FAILURE;
		}
	}
	err = hash_sync(h, buf ? buf[0] : NULL, cap, logical, &hash);
	CloseHandle(h);
	if (err)
		return err;
	md5_final(&hash, out);
	return 0;
}

static void catalog_add(struct job *j)
{
	j->allnext = NULL;
	if (g_all_tail)
		g_all_tail->allnext = j;
	else
		g_all_head = j;
	g_all_tail = j;
}

static int enqueue_job(struct job_queue *q, struct job *j)
{
	j->qnext = NULL;
	EnterCriticalSection(&q->lock);
	if (q->tail)
		q->tail->qnext = j;
	else
		q->head = j;
	q->tail = j;
	LeaveCriticalSection(&q->lock);
	ReleaseSemaphore(q->sem, 1, NULL);
	return 0;
}

static int remember_file(LPWSTR src, LPWSTR dst, ULONGLONG bytes)
{
	struct job *j;
	DWORD attr;

	/* The check pass only counts files the copy would replace. */
	if (g_overwrite_pass) {
		if (g_overwrite <= OVERWRITE_COUNT_LIMIT) {
			attr = get_attrs(dst);
			if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
				InterlockedIncrement64(&g_overwrite);
				if (g_overwrite > OVERWRITE_COUNT_LIMIT)
					g_scan_stop = 1;
			}
		}
		LocalFree(src);
		LocalFree(dst);
		return 0;
	}
	j = (struct job *)LocalAlloc(LMEM_ZEROINIT, sizeof(*j));
	if (!j)
		return 1;
	j->src = src;
	j->dst = dst;
	j->src_size = bytes;
	catalog_add(j);
	InterlockedIncrement64(&g_queued);
	InterlockedAdd64(&g_total_bytes, (LONG64)bytes);
	return 0;
}

static int enqueue_sentinel(struct job_queue *q)
{
	struct job *j = (struct job *)LocalAlloc(LMEM_ZEROINIT, sizeof(*j));

	if (!j)
		return 1;
	return enqueue_job(q, j);
}

static struct job *dequeue(struct job_queue *q)
{
	struct job *j;

	WaitForSingleObject(q->sem, INFINITE);
	EnterCriticalSection(&q->lock);
	j = q->head;
	if (j) {
		q->head = j->qnext;
		if (!q->head)
			q->tail = NULL;
	}
	LeaveCriticalSection(&q->lock);
	return j;
}

int speedcopy_job_limit(void)
{
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = NULL;
	DWORD bytes = 0, i, ninfo;
	unsigned cores = 0;

	GetLogicalProcessorInformation(NULL, &bytes);
	if (bytes) {
		info = LocalAlloc(LMEM_FIXED, bytes);
		if (info && GetLogicalProcessorInformation(info, &bytes)) {
			ninfo = bytes / sizeof(*info);
			for (i = 0; i < ninfo; i++) {
				if (info[i].Relationship == RelationProcessorCore)
					cores++;
			}
		}
		if (info)
			LocalFree(info);
	}
	if (!cores) {
		SYSTEM_INFO si;

		GetSystemInfo(&si);
		cores = si.dwNumberOfProcessors;
	}
	if (cores < 1)
		cores = 1;
	return (int)cores;
}

int speedcopy_optimum_jobs(void)
{
	MEMORYSTATUSEX mem;
	unsigned cores, cap, jobs;

	cores = (unsigned)speedcopy_job_limit();
	cap = cores;
	mem.dwLength = sizeof(mem);
	if (GlobalMemoryStatusEx(&mem) && mem.ullTotalPhys)
		cap = (unsigned)(mem.ullTotalPhys / 2 / (2ull * BLOCK_SIZE));
	if (cap < 1)
		cap = 1;
	jobs = cores < cap ? cores : cap;
	if (jobs < 1)
		jobs = 1;
	return (int)jobs;
}

int speedcopy_correct_jobs(const wchar_t *s, int *out)
{
	unsigned long n = 0;
	int maxn;

	maxn = speedcopy_job_limit();
	if (!s || !s[0]) {
		*out = 1;
		return 1;
	}
	for (; *s; s++) {
		if (*s < L'0' || *s > L'9')
			return 0;
		if (n > (unsigned long)maxn)
			continue;
		n = n * 10u + (unsigned)(*s - L'0');
	}
	if (n < 1)
		n = 1;
	if (n > (unsigned long)maxn)
		n = (unsigned long)maxn;
	*out = (int)n;
	return 1;
}

static int buffer_slots(void)
{
	MEMORYSTATUSEX mem;
	unsigned n = 1;

	mem.dwLength = sizeof(mem);
	if (GlobalMemoryStatusEx(&mem) && mem.ullTotalPhys) {
		n = (unsigned)(mem.ullTotalPhys / 2 / (2ull * BLOCK_SIZE));
		if (n < 1)
			n = 1;
	}
	return (int)n;
}

/* copy_n and check_n are the threads that will run. Their sum never
 * exceeds the job limit. A size check takes one thread and no copy
 * buffer. An MD5 check takes a buffer, so it shares that budget too. */
static void thread_plan(int *copy_n, int *check_n)
{
	int limit = g_jobs > 0 ? g_jobs : speedcopy_optimum_jobs();
	int slots = buffer_slots();

	if (limit < 1)
		limit = 1;
	*copy_n = 1;
	*check_n = 0;
	if (g_verify == VERIFY_NONE) {
		if (limit > COPY_ONLY_THREADS)
			limit = COPY_ONLY_THREADS;
		if (limit > slots)
			limit = slots;
		*copy_n = limit;
		return;
	}
	if (g_verify == VERIFY_SIZE) {
		if (limit < 2) {
			*copy_n = 1;
			return;
		}
		*check_n = 1;
		*copy_n = limit - 1;
		if (*copy_n > slots)
			*copy_n = slots;
		if (*copy_n < 1)
			*copy_n = 1;
		return;
	}
	{
		int total = limit < slots ? limit : slots;

		if (total < 2) {
			*copy_n = 1;
			return;
		}
		*check_n = total / 4;
		if (*check_n < 1)
			*check_n = 1;
		if (*check_n > total - 1)
			*check_n = total - 1;
		*copy_n = total - *check_n;
	}
}

static int pool_start(struct pool *p, struct job_queue *q, LPTHREAD_START_ROUTINE fn, int n)
{
	int t;

	if (n < 1)
		n = 1;
	if (p->threads)
		LocalFree(p->threads);
	p->threads = (HANDLE *)LocalAlloc(LMEM_ZEROINIT, (SIZE_T)n * sizeof(HANDLE));
	p->started = 0;
	p->ready = 0;
	if (!p->threads)
		return 1;
	for (t = 0; t < n; t++) {
		p->threads[t] = CreateThread(NULL, 0, fn, q, 0, NULL);
		if (!p->threads[t])
			break;
		p->started++;
	}
	if (!p->started) {
		LocalFree(p->threads);
		p->threads = NULL;
		return 1;
	}
	for (t = 0; t < p->started; t++)
		WaitForSingleObject(q->init_sem, INFINITE);
	p->ready = (int)q->ready;
	return 0;
}

static void pool_join(struct pool *p, struct job_queue *q)
{
	int t;

	for (t = 0; t < p->ready; t++)
		enqueue_sentinel(q);
	for (t = 0; t < p->started; t++) {
		WaitForSingleObject(p->threads[t], INFINITE);
		CloseHandle(p->threads[t]);
		p->threads[t] = NULL;
	}
	if (p->threads)
		LocalFree(p->threads);
	p->threads = NULL;
	p->started = 0;
	p->ready = 0;
}

static void after_copy(struct job *j);
static void check_size(struct job *j);

static DWORD WINAPI copy_task(LPVOID param)
{
	struct job_queue *q = param;
	LPVOID buf[2] = {NULL, NULL};
	DWORD xfer = alloc_buffers(buf);

	if (xfer)
		InterlockedIncrement(&q->ready);
	ReleaseSemaphore(q->init_sem, 1, NULL);
	if (!xfer)
		return 0;

	for (;;) {
		struct job *j = dequeue(q);
		struct op_err oe;
		ULONGLONG logical = 0;
		int known = 0;
		DWORD err;
		WCHAR msg[512];

		ZeroMemory(&oe, sizeof(oe));
		if (!j || !j->src) {
			if (j)
				LocalFree(j);
			break;
		}
		if (user_stopped() || InterlockedCompareExchange(&g_fail_stop, 0, 0))
			break;
		/* Hashing and comparing happen on other tasks. This task only copies. */
		err = copy_auto(j->src, j->dst, buf, xfer, NULL, &logical, &known, &oe);
		if (user_stopped()) {
			delete_path(j->dst);
			break;
		}
		j->copy_size = logical;
		j->copy_size_valid = known;
		j->copy_err = err;
		j->copy_ok = (err == 0);
		if (known) {
			j->src_size = logical;
			j->src_present = 1;
		}
		if (err && oe.what && wcscmp(oe.what, L"Not a regular file") == 0) {
			caution(L"skipping \"%s\": it is not a regular file", j->src);
			j->omit = 1;
			j->copy_ok = 1;
			j->copy_err = 0;
			if (known)
				InterlockedAdd64(&g_copy_progress, (LONG64)logical);
		} else if (err) {
			delete_path(j->dst);
			sysmsg(err, msg, 512);
			warn(L"could not copy \"%s\" to \"%s\" (%s: %s)",
				j->src, j->dst, oe.what ? oe.what : L"Copy", msg);
			if (InterlockedIncrement64(&g_copy_fail) > COPY_FAIL_LIMIT)
				InterlockedExchange(&g_fail_stop, 1);
		} else {
			stamp_meta(j->src, j->dst);
			InterlockedIncrement64(&g_copy_files);
			InterlockedAdd64(&g_copy_bytes, (LONG64)logical);
		}
		after_copy(j);
		InterlockedIncrement64(&g_copy_done);
	}
	free_buffers(buf);
	return 0;
}

static void warn_mismatch(struct job *j)
{
	WCHAR sh[40], dh[40];

	if (!j->src_present) {
		caution(L"cannot check \"%s\" because the source cannot be read", j->src);
		return;
	}
	if (!j->dst_present) {
		caution(L"\"%s\" was not copied. The destination \"%s\" is missing",
			j->src, j->dst);
		return;
	}
	if (g_verify != VERIFY_MD5 || j->src_size != j->dst_size) {
		caution(L"\"%s\" is %llu bytes and \"%s\" is %llu bytes",
			j->src, (unsigned long long)j->src_size,
			j->dst, (unsigned long long)j->dst_size);
		return;
	}
	hex_md5(j->src_md5, j->src_md5_ok, sh);
	hex_md5(j->dst_md5, j->dst_md5_ok, dh);
	caution(L"the hash of \"%s\" is %s and the hash of \"%s\" is %s",
		j->src, sh, j->dst, dh);
}

static int stopped_now(void)
{
	return user_stopped() || InterlockedCompareExchange(&g_fail_stop, 0, 0);
}

static void finish_md5(struct job *j)
{
	int size_ok, hash_ok;

	if (stopped_now()) {
		InterlockedIncrement64(&g_settled);
		return;
	}
	if (j->omit) {
		j->ok = 1;
		InterlockedIncrement64(&g_settled);
		return;
	}
	/* Both files exist and both are empty. That is a match, including MD5 of no bytes. */
	size_ok = j->src_present && j->dst_present && j->src_size == j->dst_size;
	hash_ok = j->src_md5_ok && j->dst_md5_ok
		&& memcmp(j->src_md5, j->dst_md5, 16) == 0;
	j->ok = j->copy_ok && size_ok && hash_ok;
	if (j->ok)
		InterlockedIncrement64(&g_ok);
	else
		InterlockedIncrement64(&g_bad);
	if (!j->ok && !j->copy_err)
		warn_mismatch(j);
	InterlockedIncrement64(&g_settled);
}

/* The caller that brings the count to 3 publishes the result. Copy, source hash, and destination hash each count once. */
static void md5_part_done(struct job *j)
{
	if (InterlockedIncrement(&j->parts) == 3)
		finish_md5(j);
}

static int work_enqueue_node(struct work_queue *q, struct work *w)
{
	w->next = NULL;
	EnterCriticalSection(&q->lock);
	if (q->tail)
		q->tail->next = w;
	else
		q->head = w;
	q->tail = w;
	LeaveCriticalSection(&q->lock);
	ReleaseSemaphore(q->sem, 1, NULL);
	return 0;
}

static int enqueue_work(struct work_queue *q, struct job *j, int kind)
{
	struct work *w;

	w = (struct work *)LocalAlloc(LMEM_ZEROINIT, sizeof(*w));
	if (!w) {
		warn(L"out of memory checking \"%s\"", j->src);
		if (kind == WORK_SRC || kind == WORK_DST)
			md5_part_done(j);
		else {
			j->ok = 0;
			InterlockedIncrement64(&g_bad);
			InterlockedIncrement64(&g_settled);
		}
		return 1;
	}
	w->job = j;
	w->kind = kind;
	return work_enqueue_node(q, w);
}

static int enqueue_work_sentinel(struct work_queue *q)
{
	struct work *w = (struct work *)LocalAlloc(LMEM_ZEROINIT, sizeof(*w));

	if (!w)
		return 1;
	return work_enqueue_node(q, w);
}

static struct work *dequeue_work(struct work_queue *q)
{
	struct work *w;

	WaitForSingleObject(q->sem, INFINITE);
	EnterCriticalSection(&q->lock);
	w = q->head;
	if (w) {
		q->head = w->next;
		if (!q->head)
			q->tail = NULL;
	}
	LeaveCriticalSection(&q->lock);
	return w;
}

static void after_copy(struct job *j)
{
	if (g_verify == VERIFY_NONE) {
		j->ok = j->copy_ok;
		return;
	}
	if (j->omit) {
		j->ok = 1;
		if (g_verify == VERIFY_MD5 && g_check_live) {
			md5_part_done(j);
			md5_part_done(j);
			return;
		}
		InterlockedIncrement64(&g_settled);
		return;
	}
	/* No check thread. A size compare is one metadata call on this thread.
	 * An MD5 hash is not done here: it would hold the copy up. */
	if (!g_check_live) {
		if (g_verify == VERIFY_SIZE) {
			check_size(j);
			return;
		}
		j->ok = 0;
		return;
	}
	if (!j->copy_ok) {
		j->ok = 0;
		j->dst_present = 0;
		j->dst_size = 0;
		if (g_verify == VERIFY_MD5) {
			md5_part_done(j);
			md5_part_done(j);
			return;
		}
		InterlockedIncrement64(&g_bad);
		InterlockedIncrement64(&g_settled);
		return;
	}
	if (g_verify == VERIFY_SIZE) {
		if (!enqueue_work(g_checks, j, WORK_SIZE))
			InterlockedAdd64(&g_check_total, (LONG64)j->copy_size);
		return;
	}
	md5_part_done(j);
	if (!enqueue_work(g_checks, j, WORK_DST))
		InterlockedAdd64(&g_check_total, (LONG64)j->copy_size);
}

static void check_size(struct job *j)
{
	ULONGLONG ds = 0;
	int dp, size_ok;

	if (stopped_now()) {
		InterlockedIncrement64(&g_settled);
		return;
	}
	dp = file_size_of(j->dst, &ds);
	j->dst_present = dp;
	j->dst_size = dp ? ds : 0;
	size_ok = j->src_present && dp && j->src_size == j->dst_size;
	j->ok = j->copy_ok && size_ok;
	if (j->ok)
		InterlockedIncrement64(&g_ok);
	else
		InterlockedIncrement64(&g_bad);
	if (!j->ok && !j->copy_err)
		warn_mismatch(j);
	InterlockedAdd64(&g_ver_progress, (LONG64)j->src_size);
	InterlockedIncrement64(&g_settled);
}

static void check_src_md5(struct job *j, LPVOID buf[2], DWORD cap)
{
	HANDLE h;
	DWORD kind;
	ULONGLONG sz = 0;

	/* Do not read a pipe or a device. That would consume bytes the copy still needs. */
	if (!j->omit && !stopped_now()) {
		h = open_file(j->src, GENERIC_READ, SHARE_ANY, OPEN_EXISTING,
			FILE_FLAG_SEQUENTIAL_SCAN);
		if (h != INVALID_HANDLE_VALUE) {
			kind = GetFileType(h);
			CloseHandle(h);
			if (kind == FILE_TYPE_DISK
				&& !hash_file(j->src, buf, cap, j->src_md5, &sz))
				j->src_md5_ok = 1;
		}
	}
	md5_part_done(j);
}

static void check_dst_md5(struct job *j, LPVOID buf[2], DWORD cap)
{
	ULONGLONG sz = 0;

	if (j->omit || !j->copy_ok || stopped_now()) {
		md5_part_done(j);
		return;
	}
	if (!hash_file(j->dst, buf, cap, j->dst_md5, &sz)) {
		j->dst_md5_ok = 1;
		j->dst_size = sz;
		j->dst_present = 1;
	} else if (!stopped_now()) {
		j->dst_present = file_size_of(j->dst, &j->dst_size);
	}
	md5_part_done(j);
}

static void begin_background(int *background)
{
	*background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	if (!*background)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

static void end_background(int background)
{
	if (background)
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

static DWORD WINAPI check_task(LPVOID param)
{
	struct work_queue *q = param;
	LPVOID buf[2] = {NULL, NULL};
	DWORD xfer = 0;
	int background = 0;

	/* Take the read buffer before dropping to background priority. */
	if (g_verify == VERIFY_MD5) {
		xfer = alloc_buffers(buf);
		if (xfer)
			InterlockedIncrement(&q->ready);
	} else {
		InterlockedIncrement(&q->ready);
	}
	ReleaseSemaphore(q->init_sem, 1, NULL);
	if (g_verify == VERIFY_MD5 && !xfer)
		return 0;
	/* Stay behind the copy so a hash or a size read does not take its disk time. */
	begin_background(&background);

	for (;;) {
		struct work *w = dequeue_work(q);
		struct job *j;

		if (!w || !w->job) {
			if (w)
				LocalFree(w);
			break;
		}
		j = w->job;
		if (stopped_now()) {
			LocalFree(w);
			break;
		}
		if (w->kind == WORK_SIZE)
			check_size(j);
		else if (w->kind == WORK_SRC)
			check_src_md5(j, buf, xfer);
		else if (w->kind == WORK_DST)
			check_dst_md5(j, buf, xfer);
		LocalFree(w);
	}
	free_buffers(buf);
	end_background(background);
	return 0;
}

static void enqueue_src_hashes(struct work_queue *q)
{
	struct job *j;

	for (j = g_all_head; j; j = j->allnext) {
		if (!enqueue_work(q, j, WORK_SRC))
			InterlockedAdd64(&g_check_total, (LONG64)j->src_size);
	}
}

static int create_planned_dirs(void)
{
	struct dir_stamp *d;
	int bad = 0;

	for (d = g_dirs; d; d = d->next) {
		if (make_dir(d->dst)) {
			WCHAR msg[512];

			sysmsg(GetLastError(), msg, 512);
			caution(L"cannot create the folder \"%s\": %s", d->dst, msg);
			InterlockedIncrement(&g_walk_err);
			bad = 1;
		}
	}
	return bad;
}

static void report_overwrites(void)
{
	if (g_overwrite > OVERWRITE_COUNT_LIMIT)
		fwprintf(stderr, L">1000 files already exist and would be overwritten.\n");
	else if (g_overwrite == 1)
		fwprintf(stderr, L"1 file already exists and would be overwritten.\n");
	else
		fwprintf(stderr, L"%llu files already exist and would be overwritten.\n",
			(unsigned long long)g_overwrite);
	fwprintf(stderr, L"Use -f or --force to copy anyway.\n");
	fflush(stderr);
}

static DWORD WINAPI stop_watcher(LPVOID unused)
{
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	char buf[8];
	DWORD n = 0;

	(void)unused;
	if (in == NULL || in == INVALID_HANDLE_VALUE)
		return 0;
	if (ReadFile(in, buf, sizeof(buf), &n, NULL) && n > 0)
		InterlockedExchange(&g_stop, 1);
	return 0;
}

/* The window writes one byte to the pipe when Stop is pressed. */
static void start_stop_watcher(void)
{
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE thread;

	if (in == NULL || in == INVALID_HANDLE_VALUE)
		return;
	if (GetFileType(in) != FILE_TYPE_PIPE)
		return;
	thread = CreateThread(NULL, 0, stop_watcher, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
}

static void enqueue_catalog(struct job_queue *q)
{
	struct job *j;

	for (j = g_all_head; j; j = j->allnext)
		enqueue_job(q, j);
}

static void add_tree(LPCWSTR src, LPCWSTR dst, struct job_queue *q);

static void add_file(struct job_queue *q, LPCWSTR src, LPCWSTR dst)
{
	LPWSTR owned, target;
	ULONGLONG bytes = 0;

	(void)q;

	if (is_same_or_child(src, dst)) {
		warn(L"skipping \"%s\": destination \"%s\" is inside the source", src, dst);
		InterlockedIncrement(&g_walk_err);
		return;
	}
	file_size_of(src, &bytes);
	owned = wcsdup_w(src);
	target = wcsdup_w(dst);
	if (!owned || !target || remember_file(owned, target, bytes)) {
		warn(L"out of memory queueing \"%s\"", src);
		InterlockedIncrement(&g_walk_err);
		if (owned)
			LocalFree(owned);
		if (target)
			LocalFree(target);
	}
}

static void add_tree(LPCWSTR src, LPCWSTR dst, struct job_queue *q)
{
	WIN32_FIND_DATAW fd;
	HANDLE find = INVALID_HANDLE_VALUE;
	LPWSTR pattern = NULL;
	DWORD err;
	int stopped = 0;

	(void)q;
	pattern = join_dir_file(src, L"*");
	if (!pattern) {
		warn(L"out of memory listing \"%s\"", src);
		InterlockedIncrement(&g_walk_err);
		return;
	}
	find = find_first(pattern, &fd);
	if (find == INVALID_HANDLE_VALUE) {
		err = GetLastError();
		LocalFree(pattern);
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES)
			return;
		{
			WCHAR msg[512];

			sysmsg(err ? err : ERROR_ACCESS_DENIED, msg, 512);
			caution(L"cannot read the folder \"%s\": %s", src, msg);
		}
		InterlockedIncrement(&g_walk_err);
		return;
	}

	do {
		LPWSTR child_src = NULL;
		LPWSTR child_dst = NULL;

		if (g_scan_stop) {
			stopped = 1;
			break;
		}
		if (is_dot_name(fd.cFileName))
			continue;
		if (bad_file_name(fd.cFileName, NULL)) {
			skip_bad_name(src, fd.cFileName);
			continue;
		}
		child_src = join_dir_file(src, fd.cFileName);
		child_dst = join_dir_file(dst, fd.cFileName);
		if (!child_src || !child_dst) {
			warn(L"out of memory listing \"%s\"", src);
			InterlockedIncrement(&g_walk_err);
			stopped = 1;
			goto next;
		}
		if (is_same_or_child(child_src, child_dst)) {
			caution(L"skipping \"%s\": destination \"%s\" is inside the source",
				child_src, child_dst);
			InterlockedIncrement(&g_walk_err);
			goto next;
		}
		if (skip_special_entity(child_src, fd.dwFileAttributes, fd.dwReserved0, 1))
			goto next;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			DWORD at = get_attrs(child_dst);

			if (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY)) {
				caution(L"skipping \"%s\": a file is already there, so the folder cannot be created",
					child_dst);
				InterlockedIncrement(&g_walk_err);
				goto next;
			}
			add_tree(child_src, child_dst, q);
			remember_dir(child_src, child_dst);
		} else if (remember_file(child_src, child_dst,
			((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow)) {
			warn(L"out of memory queueing \"%s\"", child_src);
			InterlockedIncrement(&g_walk_err);
			stopped = 1;
		} else {
			child_src = NULL;
			child_dst = NULL;
		}
	next:
		if (child_src)
			LocalFree(child_src);
		if (child_dst)
			LocalFree(child_dst);
		if (stopped)
			break;
	} while (FindNextFileW(find, &fd));

	if (!stopped) {
		err = GetLastError();
		if (err != ERROR_NO_MORE_FILES) {
			WCHAR msg[512];

			sysmsg(err ? err : ERROR_ACCESS_DENIED, msg, 512);
			caution(L"cannot read the folder \"%s\": %s", src, msg);
			InterlockedIncrement(&g_walk_err);
		}
	}
	FindClose(find);
	LocalFree(pattern);
}

static void add_source(struct job_queue *q, LPCWSTR src, LPCWSTR dest)
{
	DWORD attr = get_attrs(src);
	LPCWSTR name;
	LPWSTR target = NULL;

	if (attr == INVALID_FILE_ATTRIBUTES) {
		warn(L"source not found: \"%s\"", src);
		InterlockedIncrement(&g_walk_err);
		return;
	}
	if (path_has_bad_name(src)) {
		int colon = 0;

		bad_file_name(src + root_prefix_len(src), &colon);
		if (colon)
			caution(L"skipping \"%s\": a colon is not a file name. Thumbs.db:encryptable is a thumbnail stream, not a file that can be copied",
				src);
		else
			caution(L"skipping \"%s\": the name is not valid on Windows", src);
		InterlockedIncrement(&g_walk_err);
		return;
	}
	if (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE |
		FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
		DWORD tag = 0;
		int known = !(attr & FILE_ATTRIBUTE_REPARSE_POINT)
			|| reparse_tag_of(src, &tag);

		if (skip_special_entity(src, attr, tag, known))
			return;
	}
	if (attr & FILE_ATTRIBUTE_DIRECTORY) {
		name = basename_w(src);
		if (!name[0]) {
			add_tree(src, dest, q);
			return;
		}
		target = join_dir_file(dest, name);
		if (!target) {
			warn(L"out of memory preparing \"%s\"", src);
			InterlockedIncrement(&g_walk_err);
			return;
		}
		if (is_same_or_child(src, target)) {
			warn(L"skipping \"%s\": destination \"%s\" is inside the source", src, target);
			InterlockedIncrement(&g_walk_err);
			LocalFree(target);
			return;
		}
		{
			DWORD at = get_attrs(target);

			if (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY)) {
				warn(L"cannot create directory \"%s\"", target);
				InterlockedIncrement(&g_walk_err);
				LocalFree(target);
				return;
			}
		}
		add_tree(src, target, q);
		remember_dir(src, target);
		LocalFree(target);
		return;
	}
	name = basename_w(src);
	if (!name[0]) {
		warn(L"source path has no file name: \"%s\"", src);
		InterlockedIncrement(&g_walk_err);
		return;
	}
	target = join_dir_file(dest, name);
	if (!target) {
		warn(L"out of memory preparing \"%s\"", src);
		InterlockedIncrement(&g_walk_err);
		return;
	}
	add_file(q, src, target);
	LocalFree(target);
}

static int csv_write_path(FILE *fp, LPCWSTR path)
{
	int bytes, i;
	char *u;

	bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
	if (bytes <= 0) {
		fputs("\"\"", fp);
		return ferror(fp) ? 1 : 0;
	}
	u = (char *)LocalAlloc(LMEM_FIXED, bytes);
	if (!u)
		return 1;
	WideCharToMultiByte(CP_UTF8, 0, path, -1, u, bytes, NULL, NULL);
	if (fputc('"', fp) == EOF) {
		LocalFree(u);
		return 1;
	}
	for (i = 0; u[i]; i++) {
		if (u[i] == '"' && fputc('"', fp) == EOF) {
			LocalFree(u);
			return 1;
		}
		if (fputc((unsigned char)u[i], fp) == EOF) {
			LocalFree(u);
			return 1;
		}
	}
	LocalFree(u);
	return fputc('"', fp) == EOF;
}

static int csv_row(struct job *j)
{
	int i;

	if (g_verify == VERIFY_NONE) {
		if (csv_write_path(g_csv, j->src))
			return 1;
		if (fprintf(g_csv, ", %llu, ", (unsigned long long)j->src_size) < 0)
			return 1;
		if (csv_write_path(g_csv, j->dst))
			return 1;
		if (fprintf(g_csv, ", \"%s\"\n", j->ok ? "OK" : "ERROR") < 0)
			return 1;
		return 0;
	}
	if (csv_write_path(g_csv, j->src))
		return 1;
	if (fprintf(g_csv, ", %llu", (unsigned long long)j->src_size) < 0)
		return 1;
	if (g_verify == VERIFY_MD5) {
		if (fprintf(g_csv, ", \"") < 0)
			return 1;
		if (j->src_md5_ok) {
			for (i = 0; i < 16; i++) {
				if (fprintf(g_csv, "%02x", j->src_md5[i]) < 0)
					return 1;
			}
		}
		if (fprintf(g_csv, "\"") < 0)
			return 1;
	}
	if (fprintf(g_csv, ", ") < 0)
		return 1;
	if (csv_write_path(g_csv, j->dst))
		return 1;
	if (fprintf(g_csv, ", %llu", (unsigned long long)j->dst_size) < 0)
		return 1;
	if (g_verify == VERIFY_MD5) {
		if (fprintf(g_csv, ", \"") < 0)
			return 1;
		if (j->dst_md5_ok) {
			for (i = 0; i < 16; i++) {
				if (fprintf(g_csv, "%02x", j->dst_md5[i]) < 0)
					return 1;
			}
		}
		if (fprintf(g_csv, "\"") < 0)
			return 1;
	}
	if (fprintf(g_csv, ", \"%s\"\n", j->ok ? "OK" : "MISSMATCH") < 0)
		return 1;
	return 0;
}

static int open_csv(void);

static void write_csv(void)
{
	struct job *j;
	const char *head;

	if (!g_csv_path || g_csv_err)
		return;
	if (!open_csv())
		return;
	if (g_verify == VERIFY_NONE)
		head = "\"src_path\", \"src_bytes\", \"dst_path\", \"result\"\n";
	else if (g_verify == VERIFY_MD5)
		head = "\"src_path\", \"src_bytes\", \"src_md5\", \"dst_path\", \"dst_bytes\", \"dst_md5\", \"result\"\n";
	else
		head = "\"src_path\", \"src_bytes\", \"dst_path\", \"dst_bytes\", \"result\"\n";
	if (fputs(head, g_csv) == EOF) {
		warn(L"cannot write protocol \"%s\"", g_csv_path);
		g_csv_err = 1;
		return;
	}
	for (j = g_all_head; j; j = j->allnext) {
		if (j->omit)
			continue;
		if (csv_row(j)) {
			warn(L"cannot write protocol \"%s\"", g_csv_path);
			g_csv_err = 1;
			return;
		}
	}
	if (fflush(g_csv) != 0) {
		warn(L"cannot write protocol \"%s\"", g_csv_path);
		g_csv_err = 1;
	}
}

static void human_bytes(LPWSTR dst, int cap, ULONGLONG n)
{
	static const wchar_t *u[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};
	double v = (double)n;
	int i = 0;

	if (cap <= 0)
		return;
	while (v >= 1024.0 && i < 5) {
		v /= 1024.0;
		i++;
	}
	if (i == 0)
		_snwprintf(dst, cap, L"%llu B", (unsigned long long)n);
	else
		_snwprintf(dst, cap, L"%.1f %s", v, u[i]);
	dst[cap - 1] = 0;
}

/* Decimal units: 1 MB = 1000 kB = 1 000 000 bytes. */
static void human_rate(LPWSTR dst, int cap, ULONGLONG n)
{
	static const wchar_t *u[] = {
		L"B/sec", L"kB/sec", L"MB/sec", L"GB/sec", L"TB/sec", L"PB/sec"
	};
	double v = (double)n;
	int i = 0;

	if (cap <= 0)
		return;
	while (v >= 1000.0 && i < 5) {
		v /= 1000.0;
		i++;
	}
	if (i == 0)
		_snwprintf(dst, cap, L"%llu B/sec", (unsigned long long)n);
	else
		_snwprintf(dst, cap, L"%.1f %s", v, u[i]);
	dst[cap - 1] = 0;
}

static void format_span(ULONGLONG ms, LPWSTR buf, int cap)
{
	ULONGLONG hours = ms / 3600000ULL;
	ULONGLONG mins = (ms / 60000ULL) % 60ULL;
	ULONGLONG secs = (ms / 1000ULL) % 60ULL;
	ULONGLONG millis = ms % 1000ULL;

	if (cap <= 0)
		return;
	_snwprintf(buf, cap, L"%02llu:%02llu:%02llu.%03llu",
		(unsigned long long)hours,
		(unsigned long long)mins,
		(unsigned long long)secs,
		(unsigned long long)millis);
	buf[cap - 1] = 0;
}

static void tick_start(struct tick *t)
{
	QueryPerformanceFrequency(&t->freq);
	if (!t->freq.QuadPart)
		t->freq.QuadPart = 1;
	QueryPerformanceCounter(&t->t0);
}

static ULONGLONG tick_ms(const struct tick *t)
{
	LARGE_INTEGER t1;

	QueryPerformanceCounter(&t1);
	return (ULONGLONG)((t1.QuadPart - t->t0.QuadPart) * 1000 / t->freq.QuadPart);
}

static struct tick g_phase_tick;
static int g_spin_i;

static void print_progress(void)
{
	static const wchar_t spin[] = L"-\\|/";
	WCHAR done_h[40], total_h[40], rate_h[40], eta[16];
	const wchar_t *phase;
	LONG64 done, total;
	ULONGLONG ms, bps, remain, eta_s;
	double pct;

	if (g_phase == 0) {
		phase = L"copied";
		done = g_copy_progress;
		total = g_total_bytes;
	} else {
		phase = L"checked";
		done = g_ver_progress - g_tail_base;
		total = g_check_total - g_tail_base;
	}
	if (done < 0)
		done = 0;
	if (total < 0)
		total = 0;
	if (done > total && total > 0)
		done = total;
	pct = total > 0 ? (100.0 * (double)done / (double)total) : 100.0;
	if (pct < 0.0)
		pct = 0.0;
	if (pct > 100.0)
		pct = 100.0;
	human_bytes(done_h, 40, (ULONGLONG)done);
	human_bytes(total_h, 40, (ULONGLONG)total);
	ms = tick_ms(&g_phase_tick);
	bps = ms > 0 ? (ULONGLONG)done * 1000ULL / ms : 0;
	human_rate(rate_h, 40, bps);
	if (bps > 0 && total > done) {
		remain = (ULONGLONG)(total - done);
		eta_s = remain / bps;
		_snwprintf(eta, 16, L"%02llu:%02llu:%02llu",
			(unsigned long long)(eta_s / 3600ULL),
			(unsigned long long)((eta_s / 60ULL) % 60ULL),
			(unsigned long long)(eta_s % 60ULL));
	} else if (total > 0 && done >= total) {
		wcscpy(eta, L"00:00:00");
	} else {
		wcscpy(eta, L"--:--:--");
	}
	eta[15] = 0;
	out_lock();
	fwprintf(stdout, L"... %-8s %3d%%  %10s / %10s  %14s  ETA %s %c\r",
		phase, (int)(pct + 0.5), done_h, total_h, rate_h, eta, spin[g_spin_i & 3]);
	fflush(stdout);
	g_progress_drawn = 1;
	g_spin_i++;
	out_unlock();
}

static DWORD WINAPI progress_stdout(LPVOID param)
{
	HANDLE stop = param;

	for (;;) {
		if (WaitForSingleObject(stop, 1000) == WAIT_OBJECT_0)
			break;
		print_progress();
	}
	return 0;
}

static void progress_start(void)
{
	g_spin_i = 0;
	g_stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_stop_ev)
		return;
	print_progress();
	g_prog_thread = CreateThread(NULL, 0, progress_stdout, g_stop_ev, 0, NULL);
	if (!g_prog_thread) {
		CloseHandle(g_stop_ev);
		g_stop_ev = NULL;
	}
}

static void progress_stop(void)
{
	if (g_stop_ev)
		SetEvent(g_stop_ev);
	if (g_prog_thread) {
		WaitForSingleObject(g_prog_thread, INFINITE);
		CloseHandle(g_prog_thread);
		g_prog_thread = NULL;
	}
	if (g_stop_ev) {
		CloseHandle(g_stop_ev);
		g_stop_ev = NULL;
	}
	if (g_progress_drawn) {
		if (g_tty)
			fwprintf(stdout, L"\r                                                                                \r");
		else
			fwprintf(stdout, L"...\r");
		fflush(stdout);
		g_progress_drawn = 0;
	}
}

static void print_copy_summary(void)
{
	if (g_copy_fail)
		fwprintf(stdout, L"Copy finished, %llu failed\n",
			(unsigned long long)g_copy_fail);
	else
		fwprintf(stdout, L"Copy finished\n");
	fflush(stdout);
}

static void print_protocol(void)
{
	if (g_csv_path && !g_csv_err && !g_verify_oom)
		fwprintf(stdout, L"Protocol \"%s\"\n", g_csv_path);
}

static void print_check_summary(void)
{
	fwprintf(stdout, L"Verification finished, %llu ok, %llu %s\n",
		(unsigned long long)g_ok,
		(unsigned long long)g_bad,
		g_bad == 1 ? L"mismatch" : L"mismatches");
	print_protocol();
	fflush(stdout);
}

static void print_run_summary(ULONGLONG ms)
{
	WCHAR span[40], hb[40], rate[48];
	ULONGLONG bps = ms > 0 ? (ULONGLONG)g_copy_bytes * 1000ULL / ms : 0;

	format_span(ms, span, 40);
	human_bytes(hb, 40, (ULONGLONG)g_copy_bytes);
	human_rate(rate, 48, bps);
	if (g_copy_files == 1)
		fwprintf(stdout, L"Copied 1 file, %s, in %s, %s\n", hb, span, rate);
	else
		fwprintf(stdout, L"Copied %llu files, %s, in %s, %s\n",
			(unsigned long long)g_copy_files, hb, span, rate);
	fflush(stdout);
}

static int queue_init(struct job_queue *q)
{
	ZeroMemory(q, sizeof(*q));
	InitializeCriticalSection(&q->lock);
	q->sem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
	q->init_sem = CreateSemaphoreW(NULL, 0, speedcopy_job_limit(), NULL);
	if (!q->sem || !q->init_sem) {
		if (q->sem)
			CloseHandle(q->sem);
		if (q->init_sem)
			CloseHandle(q->init_sem);
		DeleteCriticalSection(&q->lock);
		return 1;
	}
	return 0;
}

static void queue_free(struct job_queue *q)
{
	if (q->sem)
		CloseHandle(q->sem);
	if (q->init_sem)
		CloseHandle(q->init_sem);
	DeleteCriticalSection(&q->lock);
	q->sem = q->init_sem = NULL;
}

static int work_init(struct work_queue *q)
{
	ZeroMemory(q, sizeof(*q));
	InitializeCriticalSection(&q->lock);
	q->sem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
	q->init_sem = CreateSemaphoreW(NULL, 0, speedcopy_job_limit(), NULL);
	if (!q->sem || !q->init_sem) {
		if (q->sem)
			CloseHandle(q->sem);
		if (q->init_sem)
			CloseHandle(q->init_sem);
		DeleteCriticalSection(&q->lock);
		return 1;
	}
	return 0;
}

static void work_free(struct work_queue *q)
{
	if (q->sem)
		CloseHandle(q->sem);
	if (q->init_sem)
		CloseHandle(q->init_sem);
	DeleteCriticalSection(&q->lock);
	q->sem = q->init_sem = NULL;
}

static int pool_start_work(struct pool *p, struct work_queue *q, LPTHREAD_START_ROUTINE fn, int n)
{
	int t;

	if (n < 1)
		n = 1;
	if (p->threads)
		LocalFree(p->threads);
	p->threads = (HANDLE *)LocalAlloc(LMEM_ZEROINIT, (SIZE_T)n * sizeof(HANDLE));
	p->started = 0;
	p->ready = 0;
	if (!p->threads)
		return 1;
	for (t = 0; t < n; t++) {
		p->threads[t] = CreateThread(NULL, 0, fn, q, 0, NULL);
		if (!p->threads[t])
			break;
		p->started++;
	}
	if (!p->started) {
		LocalFree(p->threads);
		p->threads = NULL;
		return 1;
	}
	for (t = 0; t < p->started; t++)
		WaitForSingleObject(q->init_sem, INFINITE);
	p->ready = (int)q->ready;
	return 0;
}

static void pool_join_work(struct pool *p, struct work_queue *q)
{
	int t;

	for (t = 0; t < p->ready; t++)
		enqueue_work_sentinel(q);
	for (t = 0; t < p->started; t++) {
		WaitForSingleObject(p->threads[t], INFINITE);
		CloseHandle(p->threads[t]);
		p->threads[t] = NULL;
	}
	if (p->threads)
		LocalFree(p->threads);
	p->threads = NULL;
	p->started = 0;
	p->ready = 0;
}

static void free_catalog(void)
{
	struct job *j = g_all_head;

	while (j) {
		struct job *next = j->allnext;

		if (j->src)
			LocalFree(j->src);
		if (j->dst)
			LocalFree(j->dst);
		LocalFree(j);
		j = next;
	}
	g_all_head = g_all_tail = NULL;
}

static int is_help_flag(LPCWSTR s)
{
	return wieq(s, L"/?") || wieq(s, L"/help") || wieq(s, L"--help")
		|| wieq(s, L"-h") || wieq(s, L"-help") || wieq(s, L"-?");
}

static int is_md5_flag(LPCWSTR s)
{
	return wieq(s, L"-m") || wieq(s, L"-md5") || wieq(s, L"--md5")
		|| wieq(s, L"/md5");
}

static int is_none_flag(LPCWSTR s)
{
	return wieq(s, L"-n") || wieq(s, L"--no-verify") || wieq(s, L"--none");
}

static int is_csv_flag(LPCWSTR s)
{
	return wieq(s, L"-c") || wieq(s, L"--csv") || wieq(s, L"/csv")
		|| wieq(s, L"-csv");
}

static int is_source_flag(LPCWSTR s)
{
	return wieq(s, L"-s") || wieq(s, L"--source");
}

static int is_job_flag(LPCWSTR s)
{
	return wieq(s, L"-j") || wieq(s, L"--job") || wieq(s, L"--jobs");
}

static int is_force_flag(LPCWSTR s)
{
	return wieq(s, L"-f") || wieq(s, L"--force");
}

static LPCWSTR job_attached(LPCWSTR s)
{
	if (wistart(s, L"--jobs="))
		return s + 7;
	if (wistart(s, L"--job="))
		return s + 6;
	if (s[0] == L'-' && s[1] == L'j' && s[2] >= L'0' && s[2] <= L'9')
		return s + 2;
	return NULL;
}

static LPCWSTR csv_attached(LPCWSTR s)
{
	if (wistart(s, L"--csv="))
		return s + 6;
	if (wistart(s, L"/csv:"))
		return s + 5;
	if (wistart(s, L"-csv:"))
		return s + 5;
	return NULL;
}

static LPCWSTR source_attached(LPCWSTR s)
{
	if (wistart(s, L"--source="))
		return s + 9;
	return NULL;
}

static void usage(void)
{
	fwprintf(stderr, L"Usage: speedcopy.exe [-f|--force] [-n|--no-verify] [-m|-md5] [-j|--jobs N] [-c|--csv FILE] [-s|--source PATH]... <destination>\n");
	fwprintf(stderr, L"       speedcopy.exe -h|--help\n");
}

static void help(void)
{
	fwprintf(stdout,
		L"________                 _________________\n"
		L"__  ___/_______________________  /_  ____/__________________  __\n"
		L"_____ \\___  __ \\  _ \\  _ \\  __  /_  /    _  __ \\__  __ \\_  / / /\n"
		L"____/ /__  /_/ /  __/  __/ /_/ / / /___  / /_/ /_  /_/ /  /_/ /\n"
		L"/____/ _  .___/\\___/\\___/\\__,_/  \\____/  \\____/_  .___/_\\__, /\n"
		L"       /_/                                     /_/     /____/\n"
		L"\n"
		L"Version %s\n"
		L"Copy files and directories into one destination.\n"
		L"Size and MD5 checks run on other tasks during the copy.\n"
		L"\n"
		L"Usage: speedcopy.exe [-f|--force] [-n|--no-verify] [-m|-md5] [-j|--jobs N] [-c|--csv FILE] [-s|--source PATH]... <destination>\n"
		L"       speedcopy.exe -h|--help\n"
		L"\n"
		L"With no arguments, SpeedCopy opens a window.\n"
		L"The command line never asks a question. It stops when the copy cannot start.\n"
		L"Otherwise it runs until the end. A file or folder that cannot be read is\n"
		L"reported, and the other files are still copied. A file that was only\n"
		L"partly copied is deleted. The copy stops after more than 10000 files\n"
		L"could not be copied.\n"
		L"\n"
		L"-h, --help\n"
		L"        Show this help.\n"
		L"\n"
		L"-f, --force\n"
		L"        Overwrite existing files. Without this, a check runs before the\n"
		L"        copy. When any existing file would be replaced, the copy does not\n"
		L"        start. The check counts up to 1000 files and writes one line to\n"
		L"        the error output.\n"
		L"        The window shows that same line and asks before it copies.\n"
		L"\n"
		L"-n, --no-verify\n"
		L"        Copy only. No size or MD5 check runs. The protocol row is:\n"
		L"        \"<src_path>\", <src_bytes>, \"<dst_path>\", \"OK\"/\"ERROR\"\n"
		L"\n"
		L"-m, -md5\n"
		L"        Hash each source file on other tasks while the copy runs, and\n"
		L"        hash each destination file after that file has been copied.\n"
		L"        The copy does not wait for a hash or a comparison. Without -n\n"
		L"        and without -m, another task reads each destination size after\n"
		L"        that file has been copied and compares it with the source size.\n"
		L"        -n and -m together are rejected.\n"
		L"\n"
		L"-c, --csv FILE\n"
		L"        Write a UTF-8 protocol, one row per file, after the copy. Checks\n"
		L"        that are still running finish before the file is written. There\n"
		L"        is no separate verification pass. An existing file is replaced\n"
		L"        without asking. The copy does not start when the file cannot be\n"
		L"        written. The first line names the columns. With -n:\n"
		L"        \"<src_path>\", <src_bytes>, \"<dst_path>\", \"OK\"/\"ERROR\"\n"
		L"        Compared by size:\n"
		L"        \"<src_path>\", <src_bytes>, \"<dst_path>\", <dst_bytes>, \"OK\"/\"MISSMATCH\"\n"
		L"        With -m or -md5 the row also stores both hashes:\n"
		L"        \"<src_path>\", <src_bytes>, \"<src_md5>\", \"<dst_path>\", <dst_bytes>, \"<dst_md5>\", \"OK\"/\"MISSMATCH\"\n"
		L"\n"
		L"-j, --jobs N\n"
		L"        Use at most N worker threads, from 1 to the number of CPU cores.\n"
		L"        A smaller number is raised to 1 and a larger number is lowered\n"
		L"        to that count. Copy threads and check threads together stay\n"
		L"        within N. --job is the same option. With -n or --no-verify there\n"
		L"        are no check threads, and at most 4 threads copy, because more\n"
		L"        full-speed copies slow the disks. Each copy thread holds two\n"
		L"        512 MiB buffers. Without this option the limit is one thread per\n"
		L"        core, lowered when those buffers would use more than half of\n"
		L"        memory. A size check uses one thread and no copy buffer. MD5\n"
		L"        uses part of the same limit and does not run on a copy thread.\n"
		L"\n"
		L"-s, --source PATH\n"
		L"        A file or directory to copy. Repeat this for every source.\n"
		L"        A path written before the destination is a source as well.\n"
		L"\n"
		L"The last argument is the destination, one directory or volume.\n"
		L"Each source is placed there under its own name. A source drive root\n"
		L"is placed directly:\n"
		L"\n"
		L"    speedcopy.exe -s D:\\photos -s D:\\notes.txt E:\\backup\n"
		L"    D:\\photos      ->  E:\\backup\\photos\\...\n"
		L"    D:\\notes.txt   ->  E:\\backup\\notes.txt\n"
		L"    D:\\            ->  E:\\backup\\<children of D>\n"
		L"\n"
		L"Paths use \\ between directories and files. A trailing \\ is ignored, except on a drive root.\n"
		L"Junctions, symbolic links, and other names that point elsewhere are skipped.\n"
		L"A file whose data is not on this disk is skipped. Reserved names such as CON,\n"
		L"and names with a trailing dot or space, are copied as files.\n"
		L"The sources and the destination are read first. Then the files are copied.\n"
		L"Size and MD5 checks run on other tasks during that copy. The copy does\n"
		L"not wait on them. A size check reads the destination size after that\n"
		L"file has been copied. An MD5 check hashes the source on its own and\n"
		L"hashes the destination after the copy of that file. Those tasks run\n"
		L"at background priority, so they do not take the copy's disk time.\n"
		L"The run waits for them only after every copy has finished, so the\n"
		L"protocol and the mismatch count are complete.\n"
		L"An existing directory is written into. The copy does not start when that\n"
		L"directory cannot be written. Without -f or --force, the check above\n"
		L"stops the copy when an existing file would be replaced. The window\n"
		L"shows that line and asks before it starts the copy again with -f.\n"
		L"Creation, access, and write times are kept, with the archive, hidden,\n"
		L"system, read-only, temporary, and not-content-indexed attributes.\n"
		L"Large files use overlapped unbuffered transfers.\n"
		L"A mismatch is a warning and the exit code is 1. The exit code is also 1\n"
		L"when a file could not be copied or the copy cannot start. A skipped name\n"
		L"is a warning and does not by itself fail the copy.\n",
		VERSION);
}

static int csv_can_write(LPCWSTR path, DWORD *err_out)
{
	DWORD attr;
	HANDLE h;
	LPWSTR owned = NULL;
	LPCWSTR p;
	int created = 0;

	*err_out = 0;
	attr = get_attrs(path);
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
		*err_out = ERROR_ACCESS_DENIED;
		return 0;
	}
	p = as_api(path, &owned);
	if (attr == INVALID_FILE_ATTRIBUTES) {
		h = CreateFileW(p, GENERIC_WRITE, SHARE_ANY,
			NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		created = h != INVALID_HANDLE_VALUE;
	} else {
		h = CreateFileW(p, GENERIC_WRITE, SHARE_ANY,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	}
	if (h == INVALID_HANDLE_VALUE)
		*err_out = GetLastError();
	else
		CloseHandle(h);
	if (created)
		DeleteFileW(p);
	if (owned)
		LocalFree(owned);
	if (h == INVALID_HANDLE_VALUE) {
		if (!*err_out)
			*err_out = ERROR_ACCESS_DENIED;
		return 0;
	}
	return 1;
}

/* Replace an existing protocol. Called after the copy, once checks have finished. */
static int open_csv(void)
{
	LPWSTR owned = NULL;
	LPCWSTR p;
	HANDLE h;
	int fd;

	if (!g_csv_path)
		return 1;
	p = as_api(g_csv_path, &owned);
	h = CreateFileW(p, GENERIC_WRITE, SHARE_ANY, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (owned)
		LocalFree(owned);
	if (h == INVALID_HANDLE_VALUE) {
		warn(L"cannot write protocol \"%s\"", g_csv_path);
		g_csv_err = 1;
		return 0;
	}
	fd = _open_osfhandle((intptr_t)h, _O_BINARY);
	if (fd < 0) {
		CloseHandle(h);
		warn(L"cannot write protocol \"%s\"", g_csv_path);
		g_csv_err = 1;
		return 0;
	}
	g_csv = _fdopen(fd, "wb");
	if (!g_csv) {
		_close(fd);
		warn(L"cannot write protocol \"%s\"", g_csv_path);
		g_csv_err = 1;
		return 0;
	}
	return 1;
}

/* Prove a file can be created in the destination, then remove it. */
static int dir_can_write(LPCWSTR dir, DWORD *err_out)
{
	WCHAR leaf[64];
	LPWSTR path, owned = NULL;
	LPCWSTR p;
	HANDLE h;

	*err_out = 0;
	_snwprintf(leaf, 64, L"speedcopy-%lu-%lu.tmp",
		GetCurrentProcessId(), GetTickCount());
	leaf[63] = 0;
	path = join_dir_file(dir, leaf);
	if (!path) {
		*err_out = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	p = as_api(path, &owned);
	h = CreateFileW(p, GENERIC_WRITE, SHARE_ANY,
		NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		*err_out = GetLastError();
		if (!*err_out)
			*err_out = ERROR_ACCESS_DENIED;
		if (owned)
			LocalFree(owned);
		LocalFree(path);
		return 0;
	}
	CloseHandle(h);
	if (!DeleteFileW(p))
		DeleteFileW(p);
	if (owned)
		LocalFree(owned);
	LocalFree(path);
	return 1;
}

static void set_csv_path(LPCWSTR path)
{
	LPWSTR copy;

	if (!path || !path[0])
		return;
	copy = wcsdup_w(path);
	if (!copy)
		return;
	if (g_csv_path)
		LocalFree(g_csv_path);
	g_csv_path = copy;
}

int gui_main(void);

int main(void)
{
	int argc, rc = 1, i, nitems = 0, nsrc = 0, s, any = 0;
	int argc_probe = 0, saw_none = 0, saw_md5 = 0;
	LPWSTR *probe;
	LPWSTR *argv = NULL;
	LPCWSTR *texts = NULL;
	int *is_pos = NULL;
	LPWSTR *sources = NULL;
	LPWSTR dest = NULL;
	LPCWSTR attached;
	struct job_queue queue;
	struct work_queue checks;
	struct pool workers;
	struct pool checkers;
	struct tick run_clock;
	DWORD attr;
	int queue_ready = 0;
	int checks_ready = 0;
	int copy_running = 0;
	int check_running = 0;
	int checks_later = 0;
	int copy_n = 1;
	int check_n = 0;

	probe = CommandLineToArgvW(GetCommandLineW(), &argc_probe);
	if (probe && argc_probe < 2) {
		LocalFree(probe);
		FreeConsole();
		return gui_main();
	}
	if (probe)
		LocalFree(probe);

	_setmode(_fileno(stdout), _O_U8TEXT);
	_setmode(_fileno(stderr), _O_U8TEXT);
	g_tty = _isatty(_fileno(stdout));
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
	InitializeCriticalSection(&g_out_lock);
	g_out_lock_ready = 1;
	ZeroMemory(&queue, sizeof(queue));
	ZeroMemory(&checks, sizeof(checks));
	ZeroMemory(&workers, sizeof(workers));
	ZeroMemory(&checkers, sizeof(checkers));

	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) {
		usage();
		goto done;
	}
	texts = (LPCWSTR *)LocalAlloc(LMEM_FIXED, (argc > 0 ? argc : 1) * sizeof(LPCWSTR));
	is_pos = (int *)LocalAlloc(LMEM_FIXED, (argc > 0 ? argc : 1) * sizeof(int));
	if (!texts || !is_pos) {
		warn(L"the copy did not start because there is not enough memory");
		goto done;
	}
	for (i = 1; i < argc; i++) {
		LPCWSTR a = argv[i];

		if (!a[0])
			continue;
		if (is_help_flag(a)) {
			help();
			rc = 0;
			goto done;
		}
		if (is_none_flag(a)) {
			if (saw_md5) {
				warn(L"the copy did not start. Choose either -n or -m, not both");
				goto done;
			}
			saw_none = 1;
			g_verify = VERIFY_NONE;
			continue;
		}
		if (is_md5_flag(a)) {
			if (saw_none) {
				warn(L"the copy did not start. Choose either -n or -m, not both");
				goto done;
			}
			saw_md5 = 1;
			g_verify = VERIFY_MD5;
			continue;
		}
		if (is_force_flag(a)) {
			g_force = 1;
			continue;
		}
		attached = job_attached(a);
		if (attached || is_job_flag(a)) {
			int n = 0;
			LPCWSTR num = attached;

			if (!num) {
				if (i + 1 >= argc) {
					usage();
					goto done;
				}
				num = argv[++i];
			}
			if (!speedcopy_correct_jobs(num, &n)) {
				warn(L"the copy did not start because the job count is not a number");
				goto done;
			}
			g_jobs = n;
			continue;
		}
		attached = csv_attached(a);
		if (attached) {
			if (!attached[0]) {
				usage();
				goto done;
			}
			if (!is_windows_path(attached)) {
				warn(L"the copy did not start. Use \\ between directories, not /: \"%s\"", attached);
				goto done;
			}
			set_csv_path(attached);
			if (!g_csv_path) {
				warn(L"the copy did not start because there is not enough memory");
				goto done;
			}
			continue;
		}
		if (is_csv_flag(a)) {
			if (i + 1 >= argc || !argv[i + 1][0]) {
				usage();
				goto done;
			}
			if (!is_windows_path(argv[i + 1])) {
				warn(L"the copy did not start. Use \\ between directories, not /: \"%s\"", argv[i + 1]);
				goto done;
			}
			set_csv_path(argv[++i]);
			if (!g_csv_path) {
				warn(L"the copy did not start because there is not enough memory");
				goto done;
			}
			continue;
		}
		attached = source_attached(a);
		if (attached) {
			if (!attached[0]) {
				usage();
				goto done;
			}
			if (!is_windows_path(attached)) {
				warn(L"the copy did not start. Use \\ between directories, not /: \"%s\"", attached);
				goto done;
			}
			texts[nitems] = attached;
			is_pos[nitems] = 0;
			nitems++;
			continue;
		}
		if (is_source_flag(a)) {
			if (i + 1 >= argc || !argv[i + 1][0]) {
				usage();
				goto done;
			}
			if (!is_windows_path(argv[i + 1])) {
				warn(L"the copy did not start. Use \\ between directories, not /: \"%s\"", argv[i + 1]);
				goto done;
			}
			texts[nitems] = argv[++i];
			is_pos[nitems] = 0;
			nitems++;
			continue;
		}
		if (a[0] == L'-') {
			warn(L"the copy did not start because \"%s\" is not a known option", a);
			usage();
			goto done;
		}
		if (!is_windows_path(a)) {
			warn(L"the copy did not start. Use \\ between directories, not /: \"%s\"", a);
			goto done;
		}
		texts[nitems] = a;
		is_pos[nitems] = 1;
		nitems++;
	}
	{
		int last = -1, count = 0;

		for (i = 0; i < nitems; i++) {
			if (is_pos[i])
				last = i;
		}
		if (last < 0) {
			usage();
			goto done;
		}
		for (i = 0; i < nitems; i++) {
			if (i != last)
				count++;
		}
		if (count < 1) {
			usage();
			goto done;
		}
		sources = (LPWSTR *)LocalAlloc(LMEM_ZEROINIT, count * sizeof(LPWSTR));
		if (!sources) {
			warn(L"the copy did not start because there is not enough memory");
			goto done;
		}
		for (i = 0; i < nitems; i++) {
			if (i == last)
				continue;
			sources[nsrc] = full_path(texts[i]);
			if (!sources[nsrc]) {
				warn(L"the copy did not start. Cannot resolve \"%s\"", texts[i]);
				goto done;
			}
			nsrc++;
		}
		dest = full_path(texts[last]);
		if (!dest) {
			warn(L"the copy did not start. Cannot resolve \"%s\"", texts[last]);
			goto done;
		}
	}
	LocalFree(argv);
	argv = NULL;

	for (s = 0; s < nsrc; s++) {
		if (path_has_bad_name(sources[s])) {
			int colon = 0;

			bad_file_name(sources[s] + root_prefix_len(sources[s]), &colon);
			if (colon)
				caution(L"skipping \"%s\": a colon is not a file name. Thumbs.db:encryptable is a thumbnail stream, not a file that can be copied",
					sources[s]);
			else
				caution(L"skipping \"%s\": the name is not a valid Windows file name",
					sources[s]);
			InterlockedIncrement(&g_walk_err);
			continue;
		}
		attr = get_attrs(sources[s]);
		if (attr == INVALID_FILE_ATTRIBUTES) {
			caution(L"source was not found: \"%s\"", sources[s]);
			InterlockedIncrement(&g_walk_err);
			continue;
		}
		any = 1;
	}
	if (!any) {
		warn(L"the copy did not start because no source could be copied");
		goto done;
	}

	if (g_csv_path) {
		DWORD csv_err = 0;

		if (!csv_can_write(g_csv_path, &csv_err)) {
			WCHAR msg[512];

			sysmsg(csv_err, msg, 512);
			warn(L"the copy did not start. Cannot write the protocol file \"%s\": %s",
				g_csv_path, msg);
			g_csv_err = 1;
			goto done;
		}
	}

	attr = get_attrs(dest);
	if (attr == INVALID_FILE_ATTRIBUTES) {
		if (ensure_dir(dest)) {
			WCHAR msg[512];

			sysmsg(GetLastError(), msg, 512);
			warn(L"the copy did not start. Cannot create the destination \"%s\": %s", dest, msg);
			goto done;
		}
	} else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		warn(L"the copy did not start because the destination is not a directory: \"%s\"", dest);
		goto done;
	}
	{
		DWORD werr = 0;

		if (!dir_can_write(dest, &werr)) {
			WCHAR msg[512];

			sysmsg(werr, msg, 512);
			warn(L"the copy did not start. Cannot write to the destination \"%s\": %s",
				dest, msg);
			goto done;
		}
	}

	if (queue_init(&queue)) {
		warn(L"the copy did not start. Cannot create the copy queue");
		goto done;
	}
	queue_ready = 1;

	if (!g_force) {
		g_overwrite_pass = 1;
		for (s = 0; s < nsrc; s++) {
			if (g_scan_stop)
				break;
			attr = get_attrs(sources[s]);
			if (attr == INVALID_FILE_ATTRIBUTES)
				continue;
			add_source(&queue, sources[s], dest);
		}
		g_overwrite_pass = 0;
		if (g_overwrite > 0) {
			report_overwrites();
			rc = 1;
			goto done;
		}
		g_scan_stop = 0;
		g_walk_err = 0;
		g_warn = 0;
	}
	thread_plan(&copy_n, &check_n);
	if (check_n > 0)
		fwprintf(stdout, L"SpeedCopy %s, %d copy threads, %d check threads\n",
			VERSION, copy_n, check_n);
	else
		fwprintf(stdout, L"SpeedCopy %s, %d copy threads\n", VERSION, copy_n);
	fflush(stdout);
	tick_start(&run_clock);
	for (s = 0; s < nsrc; s++) {
		attr = get_attrs(sources[s]);
		if (attr == INVALID_FILE_ATTRIBUTES)
			continue;
		add_source(&queue, sources[s], dest);
	}
	start_stop_watcher();
	if (g_queued) {
		const wchar_t *note = L"";

		create_planned_dirs();
		if (g_verify == VERIFY_MD5 || (g_verify == VERIFY_SIZE && check_n > 0)) {
			if (work_init(&checks)) {
				warn(L"the copy did not start. Cannot create the check queue");
				goto done;
			}
			checks_ready = 1;
			g_checks = &checks;
		}
		/* Copy buffers are taken first. Check tasks use what is left and never run on a copy thread. */
		if (pool_start(&workers, &queue, copy_task, copy_n) || workers.ready == 0) {
			if (workers.started)
				pool_join(&workers, &queue);
			warn(L"the copy did not start. Not enough memory for a copy buffer. Each job needs two buffers of at least 1 MiB");
			goto done;
		}
		copy_running = 1;
		if (checks_ready && check_n > 0) {
			if (pool_start_work(&checkers, &checks, check_task, check_n) || checkers.ready == 0) {
				if (checkers.started)
					pool_join_work(&checkers, &checks);
				if (g_verify == VERIFY_MD5) {
					warn(L"verification did not run because there is not enough memory for an MD5 buffer");
					g_verify_oom = 1;
				} else {
					warn(L"the copy did not start. Cannot start a task to check sizes");
					goto done;
				}
			} else {
				InterlockedExchange(&g_check_live, 1);
				check_running = 1;
			}
		} else if (checks_ready) {
			/* One thread slot. Hash after the copy so the limit still holds. */
			InterlockedExchange(&g_check_live, 1);
			checks_later = 1;
		}
		g_phase = 0;
		tick_start(&g_phase_tick);
		if (g_verify == VERIFY_SIZE)
			note = L", checking sizes";
		else if (g_verify == VERIFY_MD5 && (check_running || checks_later))
			note = L", checking MD5";
		{
			WCHAR hb[40];

			human_bytes(hb, 40, (ULONGLONG)g_total_bytes);
			if (g_queued == 1)
				fwprintf(stdout, L"Starting copy: 1 file, %s%s\n", hb, note);
			else
				fwprintf(stdout, L"Starting copy: %llu files, %s%s\n",
					(unsigned long long)g_queued, hb, note);
			fflush(stdout);
		}
		progress_start();
		enqueue_catalog(&queue);
		if (g_verify == VERIFY_MD5 && (check_running || checks_later))
			enqueue_src_hashes(&checks);
		pool_join(&workers, &queue);
		copy_running = 0;
		stamp_dirs();
		progress_stop();
		if (user_stopped()) {
			fwprintf(stdout, L"Copy stopped\n");
			fflush(stdout);
			goto done;
		}
		print_copy_summary();
		if (InterlockedCompareExchange(&g_fail_stop, 0, 0)) {
			warn(L"the copy was stopped. More than 10000 files could not be copied");
			goto done;
		}
		if (checks_later) {
			checks_later = 0;
			if (pool_start_work(&checkers, &checks, check_task, 1) || checkers.ready == 0) {
				if (checkers.started)
					pool_join_work(&checkers, &checks);
				warn(L"verification did not run because there is not enough memory for an MD5 buffer");
				g_verify_oom = 1;
			} else {
				check_running = 1;
			}
		}
		if (check_running) {
			if (g_settled < g_queued) {
				g_tail_base = g_ver_progress;
				g_phase = 1;
				tick_start(&g_phase_tick);
				progress_start();
			}
			pool_join_work(&checkers, &checks);
			check_running = 0;
			progress_stop();
			write_csv();
			print_check_summary();
		} else if (!g_verify_oom) {
			write_csv();
			print_protocol();
			fflush(stdout);
		}
		print_run_summary(tick_ms(&run_clock));
	} else if (g_warn || g_walk_err) {
		warn(L"the copy did not start because no file could be queued");
		goto done;
	} else {
		print_copy_summary();
		write_csv();
		if (g_verify == VERIFY_NONE) {
			print_protocol();
			fflush(stdout);
		} else {
			print_check_summary();
		}
		print_run_summary(tick_ms(&run_clock));
	}

	rc = (g_warn || g_copy_fail || g_bad || g_csv_err || g_verify_oom) ? 1 : 0;

done:
	progress_stop();
	if (copy_running) {
		pool_join(&workers, &queue);
		stamp_dirs();
	}
	if (check_running) {
		InterlockedExchange(&g_stop, 1);
		pool_join_work(&checkers, &checks);
		check_running = 0;
	}
	g_checks = NULL;
	InterlockedExchange(&g_check_live, 0);
	if (checks_ready)
		work_free(&checks);
	while (g_dirs) {
		struct dir_stamp *d = g_dirs;

		g_dirs = d->next;
		if (d->src)
			LocalFree(d->src);
		if (d->dst)
			LocalFree(d->dst);
		LocalFree(d);
	}
	if (g_csv) {
		fclose(g_csv);
		g_csv = NULL;
	}
	if (queue_ready)
		queue_free(&queue);
	free_catalog();
	if (sources) {
		for (s = 0; s < nsrc; s++) {
			if (sources[s])
				LocalFree(sources[s]);
		}
		LocalFree(sources);
	}
	if (dest)
		LocalFree(dest);
	if (g_csv_path)
		LocalFree(g_csv_path);
	if (texts)
		LocalFree(texts);
	if (is_pos)
		LocalFree(is_pos);
	if (argv)
		LocalFree(argv);
	if (g_out_lock_ready) {
		DeleteCriticalSection(&g_out_lock);
		g_out_lock_ready = 0;
	}
	SetThreadExecutionState(ES_CONTINUOUS);
	return rc;
}
