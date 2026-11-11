/* SpeedCopy window */
/* Written for Windows 10/11 64-bit + MinGW-w64 */
/* Author: Markus Thilo */
/* E-mail: markus.thilo@proton.me */
/* License: MIT */
/*
 * Shown when speedcopy is started with no arguments. The same exe is
 * started again, hidden, to do the copy. Wide controls, so paths are
 * not limited to the ANSI code page.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const wchar_t SPEEDCOPY_VERSION[];

int speedcopy_job_limit(void);
int speedcopy_optimum_jobs(void);
int speedcopy_correct_jobs(const wchar_t *s, int *out);

#define ID_SRCLIST     100
#define ID_ADD         103
#define ID_REMOVE      105
#define ID_DEST        106
#define ID_BROWSEDEST  107
#define ID_CSV         108
#define ID_BROWSECSV   109
#define ID_VERIFY_NONE 120
#define ID_VERIFY_SIZE 110
#define ID_VERIFY_MD5  117
#define ID_JOBS        118
#define ID_JOBS_SPIN   119
#define ID_START       111
#define ID_STOP        112
#define ID_HELP        113
#define ID_QUIT        114
#define ID_LOG         115
#define ID_STATUS      116

#define ID_LAB_SRC    -10
#define ID_LAB_DEST   -21
#define ID_LAB_CSV    -22
#define ID_LAB_VERIFY -23
#define ID_LAB_VMODE  -26
#define ID_LAB_JOBS   -25

#define WM_APP_LOG       (WM_APP + 1)
#define WM_APP_DONE      (WM_APP + 2)
#define WM_APP_OVERWRITE (WM_APP + 3)

#define ST_IDLE 0
#define ST_RUN  1
#define ST_OK   2
#define ST_ERR  3

#define PATH_CAP 32768

static HFONT g_font;
static HBRUSH g_br_back, g_br_ok, g_br_run, g_br_err, g_br_err2;
static HWND g_log, g_status;
static int g_state, g_blink, g_busy, g_fix_jobs;
static int g_min_w = 760, g_min_h = 680;
static CRITICAL_SECTION g_lock;
static HANDLE g_proc;
static HANDLE g_child_in;
static volatile LONG g_abort;
static volatile LONG g_rerun_force;
static volatile LONG g_overwrite_cancel;
static HDWP g_hdwp;

static HWND mkctl(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
	HWND h = CreateWindowExW(0, cls, text ? text : L"",
		WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
		parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
	SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
	return h;
}

static HWND mkedit(HWND parent, int id)
{
	HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
		0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
	SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
	return h;
}

static void place_hwnd(HWND ctl, int x, int y, int w, int h)
{
	if (!ctl)
		return;
	if (g_hdwp)
		g_hdwp = DeferWindowPos(g_hdwp, ctl, NULL, x, y, w, h,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	else
		MoveWindow(ctl, x, y, w, h, TRUE);
}

static void place(HWND parent, int id, int x, int y, int w, int h)
{
	place_hwnd(GetDlgItem(parent, id), x, y, w, h);
}

static int text_px(HDC dc, const wchar_t *s)
{
	SIZE sz;

	if (!s || !s[0])
		return 0;
	if (!GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz))
		return 0;
	return sz.cx;
}

static int ctl_text_px(HDC dc, HWND hwnd, int id)
{
	wchar_t t[80];
	HWND c = GetDlgItem(hwnd, id);

	if (!c)
		return 0;
	GetWindowTextW(c, t, 80);
	return text_px(dc, t);
}

static LRESULT status_color(HDC dc)
{
	COLORREF bg, fg = RGB(255, 255, 255);
	HBRUSH br = g_br_back;

	if (g_state == ST_OK) {
		bg = RGB(20, 140, 50);
		br = g_br_ok;
	} else if (g_state == ST_ERR) {
		bg = g_blink ? RGB(255, 30, 30) : RGB(140, 0, 0);
		br = g_blink ? g_br_err2 : g_br_err;
	} else if (g_state == ST_RUN) {
		bg = RGB(40, 80, 160);
		br = g_br_run;
	} else {
		bg = GetSysColor(COLOR_BTNFACE);
		fg = GetSysColor(COLOR_BTNTEXT);
	}
	SetBkColor(dc, bg);
	SetTextColor(dc, fg);
	return (LRESULT)br;
}

static void set_status(HWND hwnd, int st, const wchar_t *text)
{
	g_state = st;
	if (g_status)
		SetWindowTextW(g_status, text);
	if (st == ST_ERR) {
		g_blink = 1;
		SetTimer(hwnd, 1, 400, NULL);
	} else {
		KillTimer(hwnd, 1);
	}
	if (g_status)
		InvalidateRect(g_status, NULL, TRUE);
}

static int g_prog_at = -1;

static void clear_log_progress(void)
{
	int end;

	if (!g_log || g_prog_at < 0)
		return;
	end = GetWindowTextLengthW(g_log);
	if (g_prog_at > end)
		g_prog_at = end;
	SendMessageW(g_log, EM_SETSEL, (WPARAM)g_prog_at, (LPARAM)end);
	SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
	g_prog_at = -1;
}

static const wchar_t *clip_log_line(const wchar_t *line, wchar_t *buf)
{
	int n;

	n = lstrlenW(line);
	if (n <= 512)
		return line;
	memcpy(buf, line, 509 * sizeof(wchar_t));
	buf[509] = L'.';
	buf[510] = L'.';
	buf[511] = L'.';
	buf[512] = 0;
	return buf;
}

static void append_log(const wchar_t *line)
{
	wchar_t clipped[513];
	int n;

	if (!g_log || !line)
		return;
	line = clip_log_line(line, clipped);
	clear_log_progress();
	n = GetWindowTextLengthW(g_log);
	SendMessageW(g_log, EM_SETSEL, (WPARAM)n, (LPARAM)n);
	SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
	SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
	SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void log_progress(const wchar_t *line)
{
	wchar_t clipped[513];
	int end;

	if (!g_log || !line)
		return;
	line = clip_log_line(line, clipped);
	end = GetWindowTextLengthW(g_log);
	if (g_prog_at < 0 || g_prog_at > end)
		g_prog_at = end;
	SendMessageW(g_log, EM_SETSEL, (WPARAM)g_prog_at, (LPARAM)end);
	SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
	SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
	SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void post_log(HWND hwnd, const wchar_t *line)
{
	size_t n;
	wchar_t *copy;

	if (!line || !line[0])
		return;
	n = wcslen(line);
	copy = LocalAlloc(LMEM_FIXED, (n + 1) * sizeof(wchar_t));
	if (!copy)
		return;
	memcpy(copy, line, (n + 1) * sizeof(wchar_t));
	if (!PostMessageW(hwnd, WM_APP_LOG, 0, (LPARAM)copy))
		LocalFree(copy);
}

static void post_utf8(HWND hwnd, const char *line, int progress)
{
	int n;
	wchar_t *w;

	if (!line || !line[0])
		return;
	n = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
	if (n <= 1)
		return;
	w = LocalAlloc(LMEM_FIXED, (size_t)n * sizeof(wchar_t));
	if (!w)
		return;
	if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, w, n) ||
	    !PostMessageW(hwnd, WM_APP_LOG, progress ? 1 : 0, (LPARAM)w))
		LocalFree(w);
}

static int CALLBACK browse_cb(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
	(void)lp;
	if (msg == BFFM_INITIALIZED && data && ((const wchar_t *)data)[0])
		SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
	return 0;
}

static wchar_t *browse_dir(HWND owner, const wchar_t *title, const wchar_t *initial)
{
	BROWSEINFOW bi;
	LPITEMIDLIST pidl;
	wchar_t *path;

	ZeroMemory(&bi, sizeof(bi));
	bi.hwndOwner = owner;
	bi.lpszTitle = title;
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	bi.lpfn = browse_cb;
	bi.lParam = (LPARAM)initial;
	pidl = SHBrowseForFolderW(&bi);
	if (!pidl)
		return NULL;
	path = LocalAlloc(LMEM_FIXED, PATH_CAP * sizeof(wchar_t));
	if (!path || !SHGetPathFromIDListEx(pidl, path, PATH_CAP, 0)) {
		CoTaskMemFree(pidl);
		if (path)
			LocalFree(path);
		return NULL;
	}
	CoTaskMemFree(pidl);
	return path;
}

static wchar_t *edit_text(HWND hwnd, int id)
{
	HWND c = GetDlgItem(hwnd, id);
	int n = GetWindowTextLengthW(c);
	wchar_t *s;

	if (n < 0)
		n = 0;
	s = LocalAlloc(LMEM_FIXED, ((size_t)n + 1) * sizeof(wchar_t));
	if (!s)
		return NULL;
	GetWindowTextW(c, s, n + 1);
	return s;
}

static wchar_t *trimmed(wchar_t *s)
{
	wchar_t *end;

	if (!s)
		return s;
	while (*s == L' ' || *s == L'\t' || *s == L'"')
		s++;
	end = s + wcslen(s);
	while (end > s && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'"'))
		*--end = 0;
	return s;
}

static void parent_dir(const wchar_t *path, wchar_t *dir, size_t cap)
{
	const wchar_t *slash;
	size_t n;

	dir[0] = 0;
	if (!path || !path[0])
		return;
	slash = wcsrchr(path, L'\\');
	if (!slash || slash == path)
		return;
	n = (size_t)(slash - path);
	if (n >= cap)
		n = cap - 1;
	memcpy(dir, path, n * sizeof(wchar_t));
	dir[n] = 0;
}

struct srcs {
	wchar_t **p;
	int n, cap;
};

static void srcs_free(struct srcs *s)
{
	int i;

	if (!s)
		return;
	for (i = 0; i < s->n; i++)
		LocalFree(s->p[i]);
	if (s->p)
		LocalFree(s->p);
	s->p = NULL;
	s->n = s->cap = 0;
}

static int srcs_add(struct srcs *s, const wchar_t *text, size_t n)
{
	wchar_t *copy, **np;
	int cap;

	if (!n)
		return 1;
	copy = LocalAlloc(LMEM_FIXED, (n + 1) * sizeof(wchar_t));
	if (!copy)
		return 0;
	memcpy(copy, text, n * sizeof(wchar_t));
	copy[n] = 0;
	if (s->n + 1 > s->cap) {
		cap = s->cap ? s->cap * 2 : 8;
		np = LocalAlloc(LMEM_FIXED, (size_t)cap * sizeof(wchar_t *));
		if (!np) {
			LocalFree(copy);
			return 0;
		}
		if (s->p) {
			memcpy(np, s->p, (size_t)s->n * sizeof(wchar_t *));
			LocalFree(s->p);
		}
		s->p = np;
		s->cap = cap;
	}
	s->p[s->n++] = copy;
	return 1;
}

/* One path per line. A line that contains quotes is split into quoted paths,
 * so a paste of several "Copy as path" values still yields one path each. */
static int parse_source_text(const wchar_t *text, struct srcs *out)
{
	const wchar_t *p = text ? text : L"";

	out->p = NULL;
	out->n = out->cap = 0;
	while (*p) {
		const wchar_t *line = p, *end, *a;
		int quoted = 0;

		while (*p && *p != L'\n' && *p != L'\r') {
			if (*p == L'"')
				quoted = 1;
			p++;
		}
		end = p;
		if (*p == L'\r')
			p++;
		if (*p == L'\n')
			p++;
		while (line < end && (*line == L' ' || *line == L'\t'))
			line++;
		while (end > line && (end[-1] == L' ' || end[-1] == L'\t'))
			end--;
		if (line == end)
			continue;
		if (!quoted) {
			if (!srcs_add(out, line, (size_t)(end - line)))
				return 0;
			continue;
		}
		a = line;
		while (a < end) {
			while (a < end && (*a == L' ' || *a == L'\t'))
				a++;
			if (a >= end)
				break;
			if (*a == L'"') {
				wchar_t *buf;
				size_t n = 0, cap = (size_t)(end - a) + 1;

				a++;
				buf = LocalAlloc(LMEM_FIXED, cap * sizeof(wchar_t));
				if (!buf)
					return 0;
				while (a < end) {
					if (*a == L'"') {
						if (a + 1 < end && a[1] == L'"') {
							buf[n++] = L'"';
							a += 2;
							continue;
						}
						break;
					}
					buf[n++] = *a++;
				}
				if (a < end && *a == L'"')
					a++;
				buf[n] = 0;
				if (n && !srcs_add(out, buf, n)) {
					LocalFree(buf);
					return 0;
				}
				LocalFree(buf);
			} else {
				const wchar_t *b = a;
				while (b < end && *b != L' ' && *b != L'\t' && *b != L'"')
					b++;
				if (!srcs_add(out, a, (size_t)(b - a)))
					return 0;
				a = b;
			}
		}
	}
	return 1;
}

static int source_listed(const wchar_t *text, const wchar_t *path)
{
	struct srcs s;
	int i, found = 0;

	if (!parse_source_text(text, &s)) {
		srcs_free(&s);
		return 0;
	}
	for (i = 0; i < s.n; i++) {
		if (_wcsicmp(s.p[i], path) == 0)
			found = 1;
	}
	srcs_free(&s);
	return found;
}

static void append_source(HWND hwnd, const wchar_t *path)
{
	HWND ed = GetDlgItem(hwnd, ID_SRCLIST);
	wchar_t *cur;
	int n;

	if (!path || !path[0])
		return;
	cur = edit_text(hwnd, ID_SRCLIST);
	if (cur && source_listed(cur, path)) {
		LocalFree(cur);
		return;
	}
	n = cur ? (int)wcslen(cur) : 0;
	SendMessageW(ed, EM_SETSEL, (WPARAM)n, (LPARAM)n);
	if (n > 0 && cur[n - 1] != L'\n')
		SendMessageW(ed, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n");
	SendMessageW(ed, EM_REPLACESEL, TRUE, (LPARAM)path);
	SendMessageW(ed, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n");
	SendMessageW(ed, EM_SCROLLCARET, 0, 0);
	if (cur)
		LocalFree(cur);
}

static int last_source(HWND hwnd, wchar_t *dst, size_t cap)
{
	wchar_t *text = edit_text(hwnd, ID_SRCLIST);
	struct srcs s;
	int ok = 0;

	dst[0] = 0;
	if (!text)
		return 0;
	if (!parse_source_text(text, &s)) {
		srcs_free(&s);
		LocalFree(text);
		return 0;
	}
	if (s.n > 0) {
		wcsncpy(dst, s.p[s.n - 1], cap - 1);
		dst[cap - 1] = 0;
		ok = 1;
	}
	srcs_free(&s);
	LocalFree(text);
	return ok;
}

static void ui_metrics(HWND hwnd, int *row, int *lab, int *btn, int *gap, int *margin, int *ch, int *cw);

/* The common Open dialog cannot return files and folders together.
 * Explorer's own view can: several of either, in one selection. */
typedef struct pick_state {
	HWND owner;
	IExplorerBrowser *eb;
} pick_state;

static int take_selection(HWND owner, IExplorerBrowser *eb)
{
	IFolderView2 *view = NULL;
	IShellItemArray *items = NULL;
	DWORD count = 0, i;
	int got = 0;

	if (!eb || FAILED(IExplorerBrowser_GetCurrentView(eb, &IID_IFolderView2, (void **)&view)) || !view)
		return 0;
	if (FAILED(IFolderView2_GetSelection(view, FALSE, &items)) || !items) {
		IFolderView2_Release(view);
		return 0;
	}
	IFolderView2_Release(view);
	IShellItemArray_GetCount(items, &count);
	for (i = 0; i < count; i++) {
		IShellItem *item = NULL;
		LPWSTR path = NULL;

		if (FAILED(IShellItemArray_GetItemAt(items, i, &item)) || !item)
			continue;
		if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path && path[0]) {
			append_source(owner, path);
			got++;
		}
		if (path)
			CoTaskMemFree(path);
		IShellItem_Release(item);
	}
	IShellItemArray_Release(items);
	return got;
}

static void place_browser(HWND hwnd, pick_state *st)
{
	RECT rc, br;
	int row, gap, margin, lab, btn, ch, cw;

	if (!st || !st->eb)
		return;
	GetClientRect(hwnd, &rc);
	ui_metrics(hwnd, &row, &lab, &btn, &gap, &margin, &ch, &cw);
	(void)lab;
	(void)btn;
	(void)ch;
	(void)cw;
	br = rc;
	br.left += margin;
	br.top += margin;
	br.right -= margin;
	br.bottom -= margin + row + gap;
	if (br.bottom < br.top + row)
		br.bottom = br.top + row;
	IExplorerBrowser_SetRect(st->eb, NULL, br);
	place_hwnd(GetDlgItem(hwnd, IDOK), rc.right - margin - btn * 2 - gap,
		rc.bottom - margin - row, btn, row);
	place_hwnd(GetDlgItem(hwnd, IDCANCEL), rc.right - margin - btn,
		rc.bottom - margin - row, btn, row);
}

static LRESULT CALLBACK pick_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	pick_state *st = (pick_state *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

	switch (msg) {
	case WM_CREATE:
		st = ((CREATESTRUCTW *)lp)->lpCreateParams;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
		mkctl(hwnd, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
		mkctl(hwnd, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
		return 0;
	case WM_SIZE:
		place_browser(hwnd, st);
		return 0;
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK && HIWORD(wp) == BN_CLICKED) {
			if (!st || !st->eb || !take_selection(st->owner, st->eb)) {
				MessageBoxW(hwnd, L"Select one or more files or folders.",
					L"SpeedCopy", MB_OK | MB_ICONINFORMATION);
				return 0;
			}
			DestroyWindow(hwnd);
			return 0;
		}
		if (LOWORD(wp) == IDCANCEL && HIWORD(wp) == BN_CLICKED) {
			DestroyWindow(hwnd);
			return 0;
		}
		return 0;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		if (st && st->eb) {
			IExplorerBrowser_Destroy(st->eb);
			IExplorerBrowser_Release(st->eb);
			st->eb = NULL;
		}
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void add_paths(HWND hwnd)
{
	pick_state st;
	WNDCLASSW wc;
	HWND dlg, owner = hwnd;
	MSG msg;
	RECT rc, wr;
	FOLDERSETTINGS fs;
	LPITEMIDLIST pidl = NULL;
	wchar_t start[PATH_CAP];
	int x, y, w, h;
	HRESULT hr;

	ZeroMemory(&st, sizeof(st));
	st.owner = owner;
	hr = CoCreateInstance(&CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER,
		&IID_IExplorerBrowser, (void **)&st.eb);
	if (FAILED(hr) || !st.eb) {
		MessageBoxW(hwnd, L"Cannot open the file and folder list.", L"SpeedCopy",
			MB_OK | MB_ICONWARNING);
		return;
	}
	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = pick_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"SpeedCopyPick";
	RegisterClassW(&wc);
	GetWindowRect(owner, &wr);
	w = 720;
	h = 480;
	x = wr.left + ((wr.right - wr.left) - w) / 2;
	y = wr.top + ((wr.bottom - wr.top) - h) / 2;
	dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SpeedCopyPick", L"Select files and folders",
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		x, y, w, h, owner, NULL, wc.hInstance, &st);
	if (!dlg) {
		IExplorerBrowser_Release(st.eb);
		MessageBoxW(hwnd, L"Cannot open the file and folder list.", L"SpeedCopy",
			MB_OK | MB_ICONWARNING);
		return;
	}
	GetClientRect(dlg, &rc);
	fs.ViewMode = FVM_DETAILS;
	fs.fFlags = FWF_AUTOARRANGE | FWF_SHOWSELALWAYS;
	IExplorerBrowser_SetOptions(st.eb, EBO_SHOWFRAMES | EBO_ALWAYSNAVIGATE);
	if (FAILED(IExplorerBrowser_Initialize(st.eb, dlg, &rc, &fs))) {
		DestroyWindow(dlg);
		MessageBoxW(hwnd, L"Cannot open the file and folder list.", L"SpeedCopy",
			MB_OK | MB_ICONWARNING);
		return;
	}
	start[0] = 0;
	if (last_source(owner, start, PATH_CAP)) {
		wchar_t dir[PATH_CAP];
		parent_dir(start, dir, PATH_CAP);
		pidl = ILCreateFromPathW(dir[0] ? dir : start);
	}
	if (!pidl)
		SHGetFolderLocation(dlg, CSIDL_DRIVES, NULL, 0, &pidl);
	if (pidl) {
		IExplorerBrowser_BrowseToIDList(st.eb, pidl, SBSP_ABSOLUTE);
		ILFree(pidl);
	}
	place_browser(dlg, &st);
	EnableWindow(owner, FALSE);
	ShowWindow(dlg, SW_SHOWNORMAL);
	UpdateWindow(dlg);
	while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (IsDialogMessageW(dlg, &msg))
			continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (msg.message == WM_QUIT)
		PostQuitMessage((int)msg.wParam);
	EnableWindow(owner, TRUE);
	SetForegroundWindow(owner);
}

static void remove_source(HWND hwnd)
{
	HWND ed = GetDlgItem(hwnd, ID_SRCLIST);
	DWORD sel0 = 0, sel1 = 0;
	wchar_t *text;
	int n, start, end;

	SendMessageW(ed, EM_GETSEL, (WPARAM)&sel0, (LPARAM)&sel1);
	text = edit_text(hwnd, ID_SRCLIST);
	if (!text)
		return;
	n = (int)wcslen(text);
	if (!n) {
		LocalFree(text);
		return;
	}
	if ((int)sel0 > n)
		sel0 = (DWORD)n;
	if ((int)sel1 > n)
		sel1 = (DWORD)n;
	if (sel1 < sel0) {
		DWORD tmp = sel0;
		sel0 = sel1;
		sel1 = tmp;
	}
	start = (int)sel0;
	while (start > 0 && text[start - 1] != L'\n')
		start--;
	if (sel0 == sel1 && start > 0 &&
	    (sel0 == (DWORD)n || text[sel0] == L'\n' || text[sel0] == L'\r')) {
		start--;
		while (start > 0 && text[start - 1] != L'\n')
			start--;
	}
	end = (sel0 == sel1) ? start : (int)sel1;
	if (!(sel0 != sel1 && end > start && text[end - 1] == L'\n')) {
		while (end < n && text[end] != L'\n')
			end++;
		if (end < n && text[end] == L'\n')
			end++;
	}
	SendMessageW(ed, EM_SETSEL, (WPARAM)start, (LPARAM)end);
	SendMessageW(ed, EM_REPLACESEL, TRUE, (LPARAM)L"");
	LocalFree(text);
}

typedef struct wbuf {
	wchar_t *s;
	size_t n, cap;
} wbuf;

#define WIN_CMDLINE_MAX 32766u

static int g_cmd_too_long;

static int wb_add(wbuf *b, const wchar_t *text)
{
	size_t n = wcslen(text);
	wchar_t *p;
	size_t cap;

	if (n > WIN_CMDLINE_MAX || b->n > WIN_CMDLINE_MAX - n) {
		g_cmd_too_long = 1;
		return 0;
	}
	if (b->n + n + 1 <= b->cap) {
		memcpy(b->s + b->n, text, (n + 1) * sizeof(wchar_t));
		b->n += n;
		return 1;
	}
	cap = b->cap ? b->cap : 256;
	while (b->n + n + 1 > cap) {
		if (cap > (size_t)-1 / 2)
			return 0;
		cap *= 2;
	}
	p = LocalAlloc(LMEM_FIXED, cap * sizeof(wchar_t));
	if (!p)
		return 0;
	if (b->s) {
		memcpy(p, b->s, (b->n + 1) * sizeof(wchar_t));
		LocalFree(b->s);
	} else {
		p[0] = 0;
	}
	b->s = p;
	b->cap = cap;
	memcpy(b->s + b->n, text, (n + 1) * sizeof(wchar_t));
	b->n += n;
	return 1;
}

static int wb_slashes(wbuf *b, unsigned n)
{
	while (n--) {
		if (!wb_add(b, L"\\"))
			return 0;
	}
	return 1;
}

static int wb_quoted(wbuf *b, const wchar_t *text)
{
	unsigned slashes = 0;

	if (!wb_add(b, L"\""))
		return 0;
	for (; *text; text++) {
		if (*text == L'\\') {
			slashes++;
			if (!wb_add(b, L"\\"))
				return 0;
			continue;
		}
		if (*text == L'"') {
			/* A backslash escapes a quote, so those slashes are doubled. */
			if (!wb_slashes(b, slashes + 1) || !wb_add(b, L"\""))
				return 0;
			slashes = 0;
			continue;
		}
		slashes = 0;
		{
			wchar_t one[2];

			one[0] = *text;
			one[1] = 0;
			if (!wb_add(b, one))
				return 0;
		}
	}
	/* A trailing backslash would escape the closing quote. Double that run. */
	if (!wb_slashes(b, slashes))
		return 0;
	return wb_add(b, L"\"");
}

static wchar_t *exe_path(void)
{
	wchar_t *p = LocalAlloc(LMEM_FIXED, PATH_CAP * sizeof(wchar_t));
	DWORD n;

	if (!p)
		return NULL;
	n = GetModuleFileNameW(NULL, p, PATH_CAP);
	if (!n || n >= PATH_CAP) {
		LocalFree(p);
		return NULL;
	}
	return p;
}

static void set_child(HANDLE proc, HANDLE in_wr)
{
	EnterCriticalSection(&g_lock);
	g_proc = proc;
	g_child_in = in_wr;
	LeaveCriticalSection(&g_lock);
}

static void kill_child(void)
{
	HANDLE in;
	DWORD wrote = 0;

	EnterCriticalSection(&g_lock);
	in = g_child_in;
	if (in)
		WriteFile(in, "s\n", 2, &wrote, NULL);
	LeaveCriticalSection(&g_lock);
}

static char *normalize_line(char *line)
{
	char *end;

	if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB
		&& (unsigned char)line[2] == 0xBF)
		line += 3;
	while (*line == ' ' || *line == '\t')
		line++;
	end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		*--end = 0;
	return line;
}

static int ends_with(const char *line, const char *tail)
{
	size_t n = strlen(line);
	size_t tail_n = strlen(tail);

	if (n < tail_n)
		return 0;
	return memcmp(line + n - tail_n, tail, tail_n) == 0;
}

/* The count line ends with "overwritten." The next line tells a console
 * user to pass -f. Either one opens the dialog, once. */
static int is_overwrite_line(const char *line)
{
	return ends_with(line, "overwritten.");
}

static int is_force_hint(const char *line)
{
	return strcmp(line, "Use -f or --force to copy anyway.") == 0;
}

static wchar_t *with_force(const wchar_t *cmd)
{
	const wchar_t *rest;
	wchar_t *out;
	size_t head, n;

	if (!cmd || !cmd[0])
		return NULL;
	rest = cmd;
	if (*rest == L'"') {
		rest++;
		while (*rest && *rest != L'"')
			rest++;
		if (*rest == L'"')
			rest++;
	} else {
		while (*rest && *rest != L' ')
			rest++;
	}
	head = (size_t)(rest - cmd);
	n = wcslen(cmd);
	out = LocalAlloc(LMEM_FIXED, (n + 4) * sizeof(wchar_t));
	if (!out)
		return NULL;
	memcpy(out, cmd, head * sizeof(wchar_t));
	memcpy(out + head, L" -f", 3 * sizeof(wchar_t));
	memcpy(out + head + 3, rest, (n - head + 1) * sizeof(wchar_t));
	return out;
}

static int g_overwrite_asked;

static void ask_overwrite(HWND hwnd, const char *text)
{
	wchar_t msg[200];
	int n, yes;

	if (g_overwrite_asked)
		return;
	g_overwrite_asked = 1;
	n = MultiByteToWideChar(CP_UTF8, 0, text, -1, msg, 200);
	if (n <= 0)
		lstrcpyW(msg, L"Files already exist and would be overwritten.");
	msg[199] = 0;
	yes = (int)SendMessageW(hwnd, WM_APP_OVERWRITE, 0, (LPARAM)msg) == IDYES;
	if (yes)
		InterlockedExchange(&g_rerun_force, 1);
	else
		InterlockedExchange(&g_overwrite_cancel, 1);
}

static void deliver_line(HWND hwnd, HANDLE in_wr, char *line, size_t ln, int progress)
{
	char *text;

	(void)in_wr;
	if (!ln)
		return;
	line[ln] = 0;
	text = normalize_line(line);
	if (!progress && is_overwrite_line(text)) {
		ask_overwrite(hwnd, text);
		return;
	}
	if (!progress && is_force_hint(text)) {
		ask_overwrite(hwnd, "Files already exist and would be overwritten.");
		return;
	}
	if (progress && strcmp(line, "...") == 0) {
		PostMessageW(hwnd, WM_APP_LOG, 2, 0);
		return;
	}
	post_utf8(hwnd, line, progress);
}

static int run_copy(HWND hwnd, const wchar_t *exe, wchar_t *cmd)
{
	SECURITY_ATTRIBUTES sa;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	HANDLE rd = NULL, wr = NULL, in_rd = NULL, in_wr = NULL;
	char buf[4096], line[8192];
	DWORD nread, code = 1;
	size_t ln = 0;
	int drop = 0, pending_cr = 0;

	g_overwrite_asked = 0;
	if (InterlockedCompareExchange(&g_abort, 0, 0))
		return 1;
	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	if (!CreatePipe(&rd, &wr, &sa, 65536) || !CreatePipe(&in_rd, &in_wr, &sa, 0)) {
		post_log(hwnd, L"Cannot create the output pipe.");
		if (rd) CloseHandle(rd);
		if (wr) CloseHandle(wr);
		if (in_rd) CloseHandle(in_rd);
		if (in_wr) CloseHandle(in_wr);
		return 1;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = in_rd;
	si.hStdOutput = wr;
	si.hStdError = wr;
	ZeroMemory(&pi, sizeof(pi));
	if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		wchar_t msg[96];
		_snwprintf(msg, 96, L"Cannot start SpeedCopy (error %lu).", GetLastError());
		msg[95] = 0;
		post_log(hwnd, msg);
		CloseHandle(rd);
		CloseHandle(wr);
		CloseHandle(in_rd);
		CloseHandle(in_wr);
		return 1;
	}
	CloseHandle(wr);
	CloseHandle(in_rd);
	set_child(pi.hProcess, in_wr);
	if (InterlockedCompareExchange(&g_abort, 0, 0))
		kill_child();
	while (ReadFile(rd, buf, sizeof(buf), &nread, NULL) && nread) {
		DWORD i = 0;

		/* A bare CR updates the progress line. CR LF is a normal log line. */
		if (pending_cr) {
			pending_cr = 0;
			if (buf[0] == '\n') {
				deliver_line(hwnd, in_wr, line, ln, 0);
				ln = 0;
				drop = 0;
				i = 1;
			} else {
				deliver_line(hwnd, in_wr, line, ln, 1);
				ln = 0;
				drop = 0;
			}
		}
		for (; i < nread; i++) {
			if (buf[i] == '\r') {
				if (i + 1 < nread && buf[i + 1] == '\n') {
					deliver_line(hwnd, in_wr, line, ln, 0);
					ln = 0;
					drop = 0;
					i++;
				} else if (i + 1 < nread) {
					deliver_line(hwnd, in_wr, line, ln, 1);
					ln = 0;
					drop = 0;
				} else {
					pending_cr = 1;
				}
				continue;
			}
			if (buf[i] == '\n') {
				deliver_line(hwnd, in_wr, line, ln, 0);
				ln = 0;
				drop = 0;
				continue;
			}
			if (drop)
				continue;
			if (ln + 1 < sizeof(line))
				line[ln++] = buf[i];
			else
				drop = 1;
		}
	}
	if (pending_cr)
		deliver_line(hwnd, in_wr, line, ln, 1);
	else if (ln)
		deliver_line(hwnd, in_wr, line, ln, 0);
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	set_child(NULL, NULL);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(rd);
	CloseHandle(in_wr);
	if (InterlockedCompareExchange(&g_abort, 0, 0))
		return 1;
	return (int)code;
}

typedef struct job {
	HWND hwnd;
	wchar_t *exe;
	wchar_t *cmd;
} job;

static DWORD WINAPI worker_main(LPVOID arg)
{
	job *j = arg;
	int rc = run_copy(j->hwnd, j->exe, j->cmd);
	int stopped = InterlockedCompareExchange(&g_abort, 0, 0) ? 1 : 0;

	if (!stopped && InterlockedCompareExchange(&g_rerun_force, 0, 0)) {
		wchar_t *forced = with_force(j->cmd);

		InterlockedExchange(&g_rerun_force, 0);
		InterlockedExchange(&g_overwrite_cancel, 0);
		if (forced) {
			wchar_t *shown;
			size_t n = wcslen(forced);

			shown = LocalAlloc(LMEM_FIXED, (n + 10) * sizeof(wchar_t));
			if (shown) {
				lstrcpyW(shown, L"Running: ");
				lstrcatW(shown, forced);
				post_log(j->hwnd, shown);
				LocalFree(shown);
			}
			LocalFree(j->cmd);
			j->cmd = forced;
			rc = run_copy(j->hwnd, j->exe, j->cmd);
			stopped = InterlockedCompareExchange(&g_abort, 0, 0) ? 1 : 0;
		}
	} else if (!stopped && InterlockedCompareExchange(&g_overwrite_cancel, 0, 0)) {
		rc = 2;
	}
	PostMessageW(j->hwnd, WM_APP_DONE, (WPARAM)rc, (LPARAM)stopped);
	LocalFree(j->exe);
	LocalFree(j->cmd);
	LocalFree(j);
	return 0;
}

static int has_source(HWND hwnd)
{
	wchar_t *text = edit_text(hwnd, ID_SRCLIST);
	struct srcs srcs;
	int n = 0;

	if (!text)
		return 0;
	srcs.p = NULL;
	srcs.n = srcs.cap = 0;
	if (parse_source_text(text, &srcs))
		n = srcs.n;
	srcs_free(&srcs);
	LocalFree(text);
	return n > 0;
}

static int has_dest(HWND hwnd)
{
	wchar_t *text = edit_text(hwnd, ID_DEST);
	int ok;

	if (!text)
		return 0;
	ok = trimmed(text)[0] != 0;
	LocalFree(text);
	return ok;
}

static void update_copy_button(HWND hwnd)
{
	int on = !g_busy && has_source(hwnd) && has_dest(hwnd);

	EnableWindow(GetDlgItem(hwnd, ID_START), on);
}

static void set_jobs_value(HWND hwnd, int n)
{
	wchar_t buf[16];
	HWND ed = GetDlgItem(hwnd, ID_JOBS);
	HWND spin = GetDlgItem(hwnd, ID_JOBS_SPIN);

	_snwprintf(buf, 16, L"%d", n);
	g_fix_jobs = 1;
	SetWindowTextW(ed, buf);
	if (spin)
		SendMessageW(spin, UDM_SETPOS32, 0, n);
	g_fix_jobs = 0;
}

static void note_jobs_edit(HWND hwnd, int empty_to_one)
{
	wchar_t *text, shown[16];
	const wchar_t *s;
	int n, pos;
	HWND spin;

	if (g_fix_jobs)
		return;
	text = edit_text(hwnd, ID_JOBS);
	if (!text)
		return;
	s = trimmed(text);
	if (!s[0]) {
		LocalFree(text);
		if (empty_to_one)
			set_jobs_value(hwnd, 1);
		return;
	}
	if (!speedcopy_correct_jobs(s, &n)) {
		LocalFree(text);
		return;
	}
	_snwprintf(shown, 16, L"%d", n);
	spin = GetDlgItem(hwnd, ID_JOBS_SPIN);
	pos = spin ? (int)SendMessageW(spin, UDM_GETPOS32, 0, 0) : n;
	if (wcscmp(s, shown) != 0)
		set_jobs_value(hwnd, n);
	else if (spin && pos != n) {
		g_fix_jobs = 1;
		SendMessageW(spin, UDM_SETPOS32, 0, n);
		g_fix_jobs = 0;
	}
	LocalFree(text);
}

static void set_busy(HWND hwnd, int busy)
{
	static const int off[] = {
		ID_ADD, ID_REMOVE, ID_VERIFY_NONE, ID_VERIFY_SIZE, ID_VERIFY_MD5, ID_JOBS, ID_JOBS_SPIN,
		ID_DEST, ID_CSV, ID_BROWSEDEST, ID_BROWSECSV, ID_SRCLIST
	};
	int i;

	g_busy = busy;
	for (i = 0; i < (int)(sizeof(off) / sizeof(off[0])); i++)
		EnableWindow(GetDlgItem(hwnd, off[i]), !busy);
	EnableWindow(GetDlgItem(hwnd, ID_STOP), busy);
	if (busy)
		EnableWindow(GetDlgItem(hwnd, ID_START), FALSE);
	else
		update_copy_button(hwnd);
}

static DWORD gui_attrs(const wchar_t *path)
{
	wchar_t *owned = NULL;
	const wchar_t *p = path;
	DWORD attr;
	int n;

	if (!path || !path[0])
		return INVALID_FILE_ATTRIBUTES;
	n = lstrlenW(path);
	if (!(n >= 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\')) {
		if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
			&& path[1] == L':') {
			owned = LocalAlloc(LMEM_FIXED, ((size_t)n + 5) * sizeof(wchar_t));
			if (!owned)
				return INVALID_FILE_ATTRIBUTES;
			lstrcpyW(owned, L"\\\\?\\");
			lstrcatW(owned, path);
			p = owned;
		} else if (path[0] == L'\\' && path[1] == L'\\') {
			owned = LocalAlloc(LMEM_FIXED, ((size_t)n + 8) * sizeof(wchar_t));
			if (!owned)
				return INVALID_FILE_ATTRIBUTES;
			lstrcpyW(owned, L"\\\\?\\UNC\\");
			lstrcatW(owned, path + 2);
			p = owned;
		}
	}
	attr = GetFileAttributesW(p);
	if (owned)
		LocalFree(owned);
	return attr;
}

static int build_cmd(HWND hwnd, wbuf *out, wchar_t **exe_out)
{
	wchar_t *exe, *dest, *csv, *jobs = NULL, *src_text = NULL;
	const wchar_t *dest_s, *csv_s, *jobs_s;
	struct srcs srcs;
	int i, md5, none, ready = 0, ok = 0;

	srcs.p = NULL;
	srcs.n = srcs.cap = 0;
	*exe_out = NULL;
	out->s = NULL;
	out->n = 0;
	out->cap = 0;
	g_cmd_too_long = 0;
	exe = exe_path();
	dest = edit_text(hwnd, ID_DEST);
	csv = edit_text(hwnd, ID_CSV);
	jobs = edit_text(hwnd, ID_JOBS);
	src_text = edit_text(hwnd, ID_SRCLIST);
	if (!exe || !dest || !csv || !jobs || !src_text)
		goto done;
	dest_s = trimmed(dest);
	csv_s = trimmed(csv);
	if (!dest_s[0]) {
		MessageBoxW(hwnd, L"Choose a destination folder.", L"SpeedCopy",
			MB_OK | MB_ICONWARNING);
		goto done;
	}
	if (!parse_source_text(src_text, &srcs)) {
		MessageBoxW(hwnd, L"Out of memory.", L"SpeedCopy", MB_OK | MB_ICONWARNING);
		goto done;
	}
	if (srcs.n <= 0) {
		MessageBoxW(hwnd, L"Enter at least one source path.", L"SpeedCopy",
			MB_OK | MB_ICONWARNING);
		goto done;
	}
	jobs_s = trimmed(jobs);
	none = (int)SendMessageW(GetDlgItem(hwnd, ID_VERIFY_NONE), BM_GETCHECK, 0, 0) == BST_CHECKED;
	md5 = (int)SendMessageW(GetDlgItem(hwnd, ID_VERIFY_MD5), BM_GETCHECK, 0, 0) == BST_CHECKED;
	ready = 1;
	if (!wb_quoted(out, exe))
		goto done;
	if (jobs_s[0]) {
		int n = 0;
		wchar_t piece[32];

		if (!speedcopy_correct_jobs(jobs_s, &n)) {
			MessageBoxW(hwnd, L"Jobs must be a number.", L"SpeedCopy",
				MB_OK | MB_ICONWARNING);
			ready = 0;
			goto done;
		}
		set_jobs_value(hwnd, n);
		_snwprintf(piece, 32, L" -j %d", n);
		if (!wb_add(out, piece))
			goto done;
	} else {
		set_jobs_value(hwnd, 1);
		if (!wb_add(out, L" -j 1"))
			goto done;
	}
	if (none && !wb_add(out, L" -n"))
		goto done;
	if (md5 && !wb_add(out, L" -m"))
		goto done;
	if (csv_s[0]) {
		DWORD attr = gui_attrs(csv_s);

		if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
			MessageBoxW(hwnd, L"The protocol path is a folder.", L"SpeedCopy",
				MB_OK | MB_ICONWARNING);
			ready = 0;
			goto done;
		}
		if (attr != INVALID_FILE_ATTRIBUTES) {
			int yes = MessageBoxW(hwnd,
				L"The CSV file already exists and will be overwritten.\n\nOverwrite it?",
				L"SpeedCopy",
				MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;

			if (!yes) {
				ready = 0;
				goto done;
			}
		}
		if (!wb_add(out, L" -c") || !wb_add(out, L" ") || !wb_quoted(out, csv_s))
			goto done;
	}
	for (i = 0; i < srcs.n; i++) {
		if (!wb_add(out, L" -s") || !wb_add(out, L" ") || !wb_quoted(out, srcs.p[i]))
			goto done;
	}
	if (!wb_add(out, L" ") || !wb_quoted(out, dest_s))
		goto done;
	*exe_out = exe;
	exe = NULL;
	ok = 1;
done:
	srcs_free(&srcs);
	if (src_text)
		LocalFree(src_text);
	if (exe)
		LocalFree(exe);
	if (dest)
		LocalFree(dest);
	if (csv)
		LocalFree(csv);
	if (jobs)
		LocalFree(jobs);
	if (!ok && out->s) {
		LocalFree(out->s);
		out->s = NULL;
	}
	if (!ok && g_cmd_too_long)
		MessageBoxW(hwnd, L"The command line is too long.", L"SpeedCopy",
			MB_OK | MB_ICONWARNING);
	else if (!ok && ready)
		MessageBoxW(hwnd, L"Could not build the copy command.", L"SpeedCopy",
			MB_OK | MB_ICONWARNING);
	return ok;
}

static void start_copy(HWND hwnd)
{
	wbuf cmd;
	wchar_t *exe = NULL;
	job *j;
	HANDLE thread;

	if (g_busy)
		return;
	if (!build_cmd(hwnd, &cmd, &exe))
		return;
	j = LocalAlloc(LMEM_FIXED, sizeof(*j));
	if (!j) {
		LocalFree(cmd.s);
		LocalFree(exe);
		set_status(hwnd, ST_ERR, L"Out of memory");
		return;
	}
	j->hwnd = hwnd;
	j->exe = exe;
	j->cmd = cmd.s;
	InterlockedExchange(&g_abort, 0);
	InterlockedExchange(&g_rerun_force, 0);
	InterlockedExchange(&g_overwrite_cancel, 0);
	SetWindowTextW(g_log, L"");
	{
		size_t n = (size_t)lstrlenW(cmd.s);
		wchar_t *shown = LocalAlloc(LMEM_FIXED, (n + 10) * sizeof(wchar_t));

		if (shown) {
			lstrcpyW(shown, L"Running: ");
			lstrcatW(shown, cmd.s);
			append_log(shown);
			LocalFree(shown);
		} else {
			append_log(cmd.s);
		}
	}
	set_busy(hwnd, 1);
	set_status(hwnd, ST_RUN, L"Copying...");
	thread = CreateThread(NULL, 0, worker_main, j, 0, NULL);
	if (!thread) {
		LocalFree(j->exe);
		LocalFree(j->cmd);
		LocalFree(j);
		set_busy(hwnd, 0);
		set_status(hwnd, ST_ERR, L"Cannot start the copy");
		return;
	}
	CloseHandle(thread);
}

static void stop_copy(HWND hwnd)
{
	if (!g_busy)
		return;
	InterlockedExchange(&g_abort, 1);
	kill_child();
	set_status(hwnd, ST_ERR, L"Stopping...");
}

static void on_done(HWND hwnd, int rc, int stopped)
{
	set_busy(hwnd, 0);
	EnableWindow(GetDlgItem(hwnd, ID_QUIT), TRUE);
	if (stopped)
		set_status(hwnd, ST_ERR, L"Stopped");
	else if (rc == 2)
		set_status(hwnd, ST_IDLE, L"Cancelled");
	else if (rc == 0) {
		set_status(hwnd, ST_OK, L"Finished");
		SetWindowTextW(GetDlgItem(hwnd, ID_SRCLIST), L"");
		update_copy_button(hwnd);
	}
	else
		set_status(hwnd, ST_ERR, L"Error");
}

static void show_help(HWND hwnd)
{
	static const wchar_t text[] =
		L"SpeedCopy copies files and folders into one destination. "
		L"Size or MD5 checks run alongside the copy. None copies only.\n\n"
		L"Sources\n"
		L"One path per line. Type a path, or paste paths with Ctrl-V. "
		L"Add selects several files and folders at once. Remove deletes the line at the cursor. "
		L"Junctions and symbolic links are skipped. "
		L"Names that are not regular files are skipped.\n\n"
		L"Destination folder\n"
		L"One directory or volume. Each source is placed under its own name. "
		L"A drive root such as D:\\ is copied directly, so its children keep their paths. "
		L"An existing directory is written into. "
		L"The copy does not start when that directory cannot be written. "
		L"If any file would be replaced, SpeedCopy shows how many and asks before copying. "
		L"That line is the same one written to the error output: "
		L"1 file already exists and would be overwritten, "
		L"N files already exist and would be overwritten, "
		L"or >1000 files already exist and would be overwritten. "
		L"No leaves every file as it is and does not start the copy. "
		L"Stop leaves finished files and deletes a file that was only partly copied. "
		L"The copy stops when more than 10000 files could not be copied. "
		L"Times and ordinary attributes are kept.\n\n"
		L"Protocol CSV\n"
		L"Optional. One row per file. "
		L"If that file already exists, SpeedCopy asks before replacing it. "
		L"The copy does not start when the file cannot be written. "
		L"It is written after the copy. Checks still running finish before it is written. "
		L"The first line names the columns. "
		L"No verification stores the two paths, the source size, and OK or ERROR. "
		L"Size stores the two paths, the two sizes, and OK or MISSMATCH. "
		L"MD5 also stores both hashes.\n\n"
		L"Verify\n"
		L"None copies only. "
		L"Size reads each destination size on another task after that file has been copied and compares it. "
		L"MD5 hashes each source on other tasks during the copy, and hashes each destination after that file has been copied. "
		L"The copy does not wait on a hash or a comparison. "
		L"A mismatch is a warning in the log and the status shows Error. "
		L"A finished copy with no mismatches shows Finished.\n\n"
		L"Jobs\n"
		L"The maximum number of threads. Copy threads and check threads together stay within it. "
		L"The number shown at start is one thread per CPU core, "
		L"lowered when the copy buffers would use more than half of memory. "
		L"With no verification there are no check threads, and at most 4 threads copy, "
		L"because more full-speed copies slow the disks. "
		L"Each copy thread holds two 512 MiB buffers. Arrows stop at 1 and at the number of CPU cores. "
		L"A typed number below 1 becomes 1, and a number above the core count becomes that count.";

	MessageBoxW(hwnd, text, L"SpeedCopy", MB_OK | MB_ICONINFORMATION);
}

static void ui_metrics(HWND hwnd, int *row, int *lab, int *btn, int *gap, int *margin, int *ch, int *cw)
{
	HDC dc = hwnd ? GetDC(hwnd) : GetDC(NULL);
	TEXTMETRICW tm;
	HFONT old = SelectObject(dc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));
	int ave, line;

	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	if (hwnd)
		ReleaseDC(hwnd, dc);
	else
		ReleaseDC(NULL, dc);
	ave = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
	line = tm.tmHeight + tm.tmExternalLeading;
	if (line < 16)
		line = 16;
	*cw = ave;
	*ch = line;
	*margin = ave;
	if (*margin < 8)
		*margin = 8;
	*gap = ave / 2;
	if (*gap < 4)
		*gap = 4;
	*row = line + 8;
	if (*row < 22)
		*row = 22;
	*lab = ave * 18;
	if (*lab < 120)
		*lab = 120;
	*btn = ave * 12;
	if (*btn < 88)
		*btn = 88;
}

static void layout(HWND hwnd, int cx, int cy)
{
	static const int labs[] = { ID_LAB_DEST, ID_LAB_CSV, ID_LAB_VERIFY };
	static const int side[] = { ID_ADD, ID_REMOVE, ID_BROWSEDEST, ID_BROWSECSV };
	int margin, row, gap, lab, btn, ch, cw, y, x, w, field;
	int box, pref, chrome, i, best, btn_y, none_w, size_w, md5_w, job_w, job_lab, mode_w;
	HDC dc;
	HFONT old;

	ui_metrics(hwnd, &row, &lab, &btn, &gap, &margin, &ch, &cw);
	dc = GetDC(hwnd);
	old = SelectObject(dc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));
	best = lab;
	for (i = 0; i < (int)(sizeof(labs) / sizeof(labs[0])); i++) {
		int tw = ctl_text_px(dc, hwnd, labs[i]) + 12;
		if (tw > best)
			best = tw;
	}
	lab = best;
	best = btn;
	for (i = 0; i < (int)(sizeof(side) / sizeof(side[0])); i++) {
		int tw = ctl_text_px(dc, hwnd, side[i]) + 16;
		if (tw > best)
			best = tw;
	}
	btn = best;
	none_w = ctl_text_px(dc, hwnd, ID_VERIFY_NONE) + 24;
	size_w = ctl_text_px(dc, hwnd, ID_VERIFY_SIZE) + 24;
	md5_w = ctl_text_px(dc, hwnd, ID_VERIFY_MD5) + 24;
	mode_w = ctl_text_px(dc, hwnd, ID_LAB_VMODE) + 4;
	job_lab = ctl_text_px(dc, hwnd, ID_LAB_JOBS) + 4;
	job_w = cw * 3 + 8 + GetSystemMetrics(SM_CXVSCROLL);
	SelectObject(dc, old);
	ReleaseDC(hwnd, dc);

	pref = 16 * ch + 6;
	chrome = 2 * margin + 4 * (row + gap) + 2 * row + 3 * gap;
	box = pref;
	if (chrome + box * 2 > cy) {
		box = (cy - chrome) / 2;
		if (box < 4 * ch + 6)
			box = 4 * ch + 6;
	}
	x = margin;
	w = cx - 2 * margin;
	if (w < 96 * cw + GetSystemMetrics(SM_CXVSCROLL))
		w = 96 * cw + GetSystemMetrics(SM_CXVSCROLL);
	y = margin;

	g_hdwp = BeginDeferWindowPos(28);
	{
		int rx = x + w;
		int labw;
		place(hwnd, ID_REMOVE, rx - btn, y, btn, row);
		rx -= btn + gap;
		place(hwnd, ID_ADD, rx - btn, y, btn, row);
		labw = rx - btn - gap - x;
		if (labw < 40)
			labw = 40;
		place(hwnd, ID_LAB_SRC, x, y, labw, row);
	}
	y += row + gap;
	place(hwnd, ID_SRCLIST, x, y, w, box);
	y += box + gap;

	field = w - lab - gap - btn;
	if (field < 40)
		field = 40;
	place(hwnd, ID_LAB_DEST, x, y, lab, row);
	place(hwnd, ID_DEST, x + lab + gap, y, field, row);
	place(hwnd, ID_BROWSEDEST, x + w - btn, y, btn, row);
	y += row + gap;
	place(hwnd, ID_LAB_CSV, x, y, lab, row);
	place(hwnd, ID_CSV, x + lab + gap, y, field, row);
	place(hwnd, ID_BROWSECSV, x + w - btn, y, btn, row);
	y += row + gap;
	{
		int fx = x + lab + gap;
		int step = cw > gap ? cw : gap;

		place(hwnd, ID_LAB_VERIFY, x, y, lab, row);
		place(hwnd, ID_LAB_VMODE, fx, y, mode_w, row);
		fx += mode_w + step;
		place(hwnd, ID_VERIFY_NONE, fx, y, none_w, row);
		fx += none_w + step;
		place(hwnd, ID_VERIFY_SIZE, fx, y, size_w, row);
		fx += size_w + step;
		place(hwnd, ID_VERIFY_MD5, fx, y, md5_w, row);
		fx += md5_w + step * 2;
		place(hwnd, ID_LAB_JOBS, fx, y, job_lab, row);
		fx += job_lab + gap;
		place(hwnd, ID_JOBS, fx, y, job_w, row);
	}
	y += row + gap;
	place(hwnd, ID_LOG, x, y, w, box);
	y += box + gap;
	place(hwnd, ID_STATUS, x, y, w, row);
	btn_y = cy - margin - row;
	if (btn_y < y + gap)
		btn_y = y + gap;
	{
		int bw = (w - gap * 3) / 4;
		if (bw < 40)
			bw = 40;
		place(hwnd, ID_START, x, btn_y, bw, row);
		place(hwnd, ID_STOP, x + bw + gap, btn_y, bw, row);
		place(hwnd, ID_HELP, x + (bw + gap) * 2, btn_y, bw, row);
		place(hwnd, ID_QUIT, x + (bw + gap) * 3, btn_y, w - (bw + gap) * 3, row);
	}
	if (g_hdwp) {
		EndDeferWindowPos(g_hdwp);
		g_hdwp = NULL;
	}
	if (GetDlgItem(hwnd, ID_JOBS_SPIN))
		SendMessageW(GetDlgItem(hwnd, ID_JOBS_SPIN), UDM_SETBUDDY,
			(WPARAM)GetDlgItem(hwnd, ID_JOBS), 0);
}

static void create_ui(HWND hwnd)
{
	HWND sources, log;

	g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	g_br_ok = CreateSolidBrush(RGB(20, 140, 50));
	g_br_run = CreateSolidBrush(RGB(40, 80, 160));
	g_br_err = CreateSolidBrush(RGB(140, 0, 0));
	g_br_err2 = CreateSolidBrush(RGB(255, 30, 30));
	g_br_back = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));

	mkctl(hwnd, L"STATIC", L"Sources", SS_LEFT, ID_LAB_SRC);
	mkctl(hwnd, L"BUTTON", L"Add", WS_TABSTOP | BS_PUSHBUTTON, ID_ADD);
	mkctl(hwnd, L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON, ID_REMOVE);
	sources = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
		ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
		0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_SRCLIST, GetModuleHandleW(NULL), NULL);
	SendMessageW(sources, WM_SETFONT, (WPARAM)g_font, TRUE);
	SendMessageW(sources, EM_SETLIMITTEXT, 2 * 1024 * 1024, 0);

	mkctl(hwnd, L"STATIC", L"Destination folder", SS_LEFT | SS_CENTERIMAGE, ID_LAB_DEST);
	mkedit(hwnd, ID_DEST);
	mkctl(hwnd, L"BUTTON", L"Browse", WS_TABSTOP | BS_PUSHBUTTON, ID_BROWSEDEST);
	mkctl(hwnd, L"STATIC", L"Protocol CSV", SS_LEFT | SS_CENTERIMAGE, ID_LAB_CSV);
	mkedit(hwnd, ID_CSV);
	mkctl(hwnd, L"BUTTON", L"Browse", WS_TABSTOP | BS_PUSHBUTTON, ID_BROWSECSV);
	mkctl(hwnd, L"STATIC", L"Options", SS_LEFT | SS_CENTERIMAGE, ID_LAB_VERIFY);
	mkctl(hwnd, L"STATIC", L"Verify:", SS_LEFT | SS_CENTERIMAGE, ID_LAB_VMODE);
	mkctl(hwnd, L"BUTTON", L"None",
		WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, ID_VERIFY_NONE);
	mkctl(hwnd, L"BUTTON", L"Size",
		WS_TABSTOP | BS_AUTORADIOBUTTON, ID_VERIFY_SIZE);
	mkctl(hwnd, L"BUTTON", L"MD5 hash",
		WS_TABSTOP | BS_AUTORADIOBUTTON, ID_VERIFY_MD5);
	SendMessageW(GetDlgItem(hwnd, ID_VERIFY_SIZE), BM_SETCHECK, BST_CHECKED, 0);
	mkctl(hwnd, L"STATIC", L"Jobs:", SS_LEFT | SS_CENTERIMAGE, ID_LAB_JOBS);
	{
		HWND jobs, spin;
		INITCOMMONCONTROLSEX icc;

		icc.dwSize = sizeof(icc);
		icc.dwICC = ICC_UPDOWN_CLASS;
		InitCommonControlsEx(&icc);
		jobs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER,
			0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_JOBS, GetModuleHandleW(NULL), NULL);
		SendMessageW(jobs, WM_SETFONT, (WPARAM)g_font, TRUE);
		SendMessageW(jobs, EM_SETLIMITTEXT, 6, 0);
		spin = CreateWindowExW(0, UPDOWN_CLASSW, L"",
			WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT |
			UDS_ARROWKEYS | UDS_NOTHOUSANDS,
			0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_JOBS_SPIN, GetModuleHandleW(NULL), NULL);
		if (spin) {
			int n = speedcopy_optimum_jobs();

			SendMessageW(spin, UDM_SETBUDDY, (WPARAM)jobs, 0);
			SendMessageW(spin, UDM_SETRANGE32, 1, speedcopy_job_limit());
			SendMessageW(spin, UDM_SETPOS32, 0, n);
		}
	}

	mkctl(hwnd, L"BUTTON", L"Copy", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_START);
	mkctl(hwnd, L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, ID_STOP);
	mkctl(hwnd, L"BUTTON", L"Help", WS_TABSTOP | BS_PUSHBUTTON, ID_HELP);
	mkctl(hwnd, L"BUTTON", L"Quit", WS_TABSTOP | BS_PUSHBUTTON, ID_QUIT);
	EnableWindow(GetDlgItem(hwnd, ID_START), FALSE);
	EnableWindow(GetDlgItem(hwnd, ID_STOP), FALSE);
	EnableWindow(GetDlgItem(hwnd, ID_QUIT), FALSE);

	log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
		ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_LOG, GetModuleHandleW(NULL), NULL);
	SendMessageW(log, WM_SETFONT, (WPARAM)g_font, TRUE);
	SendMessageW(log, EM_SETLIMITTEXT, 2 * 1024 * 1024, 0);
	g_log = log;

	g_status = mkctl(hwnd, L"STATIC", L"Ready",
		SS_CENTER | SS_CENTERIMAGE | SS_SUNKEN, ID_STATUS);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		create_ui(hwnd);
		return 0;
	case WM_SIZE:
		if (wp != SIZE_MINIMIZED)
			layout(hwnd, LOWORD(lp), HIWORD(lp));
		return 0;
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mm = (MINMAXINFO *)lp;
		mm->ptMinTrackSize.x = g_min_w;
		mm->ptMinTrackSize.y = g_min_h;
		return 0;
	}
	case WM_COMMAND:
		if (HIWORD(wp) == EN_CHANGE) {
			int id = LOWORD(wp);

			if (id == ID_JOBS)
				note_jobs_edit(hwnd, 0);
			if (id == ID_SRCLIST || id == ID_DEST)
				update_copy_button(hwnd);
			return 0;
		}
		if (HIWORD(wp) == EN_KILLFOCUS && LOWORD(wp) == ID_JOBS) {
			note_jobs_edit(hwnd, 1);
			return 0;
		}
		if (HIWORD(wp) == BN_CLICKED) {
			int id = LOWORD(wp);
			if (id == ID_BROWSEDEST) {
				wchar_t *cur = edit_text(hwnd, ID_DEST);
				wchar_t *picked = browse_dir(hwnd, L"Destination folder",
					cur ? trimmed(cur) : L"");
				if (cur)
					LocalFree(cur);
				if (picked) {
					SetDlgItemTextW(hwnd, ID_DEST, picked);
					LocalFree(picked);
				}
			} else if (id == ID_BROWSECSV) {
				wchar_t *buf = LocalAlloc(LPTR, PATH_CAP * sizeof(wchar_t));
				wchar_t *cur, *dest, dir[PATH_CAP];
				OPENFILENAMEW of;
				if (!buf)
					return 0;
				cur = edit_text(hwnd, ID_CSV);
				if (cur) {
					wcsncpy(buf, trimmed(cur), PATH_CAP - 1);
					LocalFree(cur);
				}
				dest = edit_text(hwnd, ID_DEST);
				parent_dir(dest ? trimmed(dest) : L"", dir, PATH_CAP);
				if (dest)
					LocalFree(dest);
				ZeroMemory(&of, sizeof(of));
				of.lStructSize = sizeof(of);
				of.hwndOwner = hwnd;
				of.lpstrFile = buf;
				of.nMaxFile = PATH_CAP;
				of.lpstrFilter = L"CSV files\0*.csv\0All files\0*.*\0\0";
				of.nFilterIndex = 1;
				of.lpstrInitialDir = dir[0] ? dir : NULL;
				of.lpstrDefExt = L"csv";
				of.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
					OFN_ENABLESIZING | OFN_PATHMUSTEXIST;
				if (GetSaveFileNameW(&of))
					SetDlgItemTextW(hwnd, ID_CSV, buf);
				LocalFree(buf);
			} else if (id == ID_ADD)
				add_paths(hwnd);
			else if (id == ID_REMOVE)
				remove_source(hwnd);
			else if (id == ID_START)
				start_copy(hwnd);
			else if (id == ID_STOP)
				stop_copy(hwnd);
			else if (id == ID_HELP)
				show_help(hwnd);
			else if (id == ID_QUIT)
				SendMessageW(hwnd, WM_CLOSE, 0, 0);
		}
		return 0;
	case WM_APP_LOG:
		if (wp == 2) {
			clear_log_progress();
			return 0;
		}
		if (wp)
			log_progress((const wchar_t *)lp);
		else
			append_log((const wchar_t *)lp);
		LocalFree((void *)lp);
		return 0;
	case WM_APP_DONE:
		on_done(hwnd, (int)wp, (int)lp);
		return 0;
	case WM_APP_OVERWRITE: {
		wchar_t ask[240];
		const wchar_t *msg = (const wchar_t *)lp;

		if (!msg || !msg[0])
			msg = L"Files already exist and would be overwritten.";
		_snwprintf(ask, 240, L"%s\n\nCopy anyway?", msg);
		ask[239] = 0;
		SetForegroundWindow(hwnd);
		return MessageBoxW(hwnd, ask, L"SpeedCopy",
			MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
	}
	case WM_TIMER:
		if (g_state == ST_ERR && g_status) {
			g_blink = !g_blink;
			InvalidateRect(g_status, NULL, TRUE);
		}
		return 0;
	case WM_CTLCOLORSTATIC:
		if ((HWND)lp == g_status)
			return status_color((HDC)wp);
		break;
	case WM_CLOSE:
		if (g_busy) {
			if (MessageBoxW(hwnd, L"Stop the copy and close?", L"SpeedCopy",
				MB_YESNO | MB_ICONWARNING) != IDYES)
				return 0;
			InterlockedExchange(&g_abort, 1);
			kill_child();
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, 1);
		if (g_br_ok) DeleteObject(g_br_ok);
		if (g_br_run) DeleteObject(g_br_run);
		if (g_br_err) DeleteObject(g_br_err);
		if (g_br_err2) DeleteObject(g_br_err2);
		if (g_br_back) DeleteObject(g_br_back);
		g_br_ok = g_br_run = g_br_err = g_br_err2 = g_br_back = NULL;
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void window_geom(int *x, int *y, int *w, int *h)
{
	RECT work, frame, minr;
	int aw, ah, row, lab, btn, gap, margin, ch, cw;
	int box, client_w, client_h, min_client_h;

	ui_metrics(NULL, &row, &lab, &btn, &gap, &margin, &ch, &cw);
	(void)lab;
	(void)btn;
	box = 16 * ch + 6;
	client_w = margin * 2 + 128 * cw + GetSystemMetrics(SM_CXVSCROLL);
	client_h = 2 * margin + 4 * (row + gap) + 2 * row + 3 * gap + 2 * box;
	frame.left = 0;
	frame.top = 0;
	frame.right = client_w;
	frame.bottom = client_h;
	AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
	*w = frame.right - frame.left;
	*h = frame.bottom - frame.top;

	min_client_h = 2 * margin + 4 * (row + gap) + 2 * row + 3 * gap + 2 * (4 * ch + 6);
	minr.left = 0;
	minr.top = 0;
	minr.right = margin * 2 + 96 * cw + GetSystemMetrics(SM_CXVSCROLL);
	minr.bottom = min_client_h;
	AdjustWindowRect(&minr, WS_OVERLAPPEDWINDOW, FALSE);
	g_min_w = minr.right - minr.left;
	g_min_h = minr.bottom - minr.top;

	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	aw = work.right - work.left;
	ah = work.bottom - work.top;
	if (aw > 200 && *w > aw - 16)
		*w = aw - 16;
	if (ah > 200 && *h > ah - 16)
		*h = ah - 16;
	if (g_min_w > *w)
		g_min_w = *w;
	if (g_min_h > *h)
		g_min_h = *h;
	*x = work.left + (aw - *w) / 2;
	*y = work.top + (ah - *h) / 2;
	if (*x < work.left)
		*x = work.left;
	if (*y < work.top)
		*y = work.top;
}

int gui_main(void)
{
	WNDCLASSEXW wc;
	HWND hwnd;
	MSG msg;
	HINSTANCE inst = GetModuleHandleW(NULL);
	wchar_t title[80];
	int x, y, w, h;

	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	InitializeCriticalSection(&g_lock);
	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"SpeedCopy";
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(2));
	if (!wc.hIcon)
		wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
	wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(2), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	if (!RegisterClassExW(&wc)) {
		MessageBoxW(NULL, L"Cannot register the window class.", L"SpeedCopy",
			MB_OK | MB_ICONERROR);
		DeleteCriticalSection(&g_lock);
		CoUninitialize();
		return 1;
	}
	_snwprintf(title, 80, L"SpeedCopy %s", SPEEDCOPY_VERSION);
	title[79] = 0;
	window_geom(&x, &y, &w, &h);
	hwnd = CreateWindowExW(0, L"SpeedCopy", title,
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		x, y, w, h, NULL, NULL, inst, NULL);
	if (!hwnd) {
		MessageBoxW(NULL, L"Cannot create the window.", L"SpeedCopy",
			MB_OK | MB_ICONERROR);
		DeleteCriticalSection(&g_lock);
		CoUninitialize();
		return 1;
	}
	ShowWindow(hwnd, SW_SHOWNORMAL);
	UpdateWindow(hwnd);
	SetForegroundWindow(hwnd);
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (IsDialogMessageW(hwnd, &msg))
			continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	DeleteCriticalSection(&g_lock);
	CoUninitialize();
	return 0;
}
