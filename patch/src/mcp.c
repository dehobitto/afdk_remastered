/* mcp.c - _afdk_remastered_mcp.dll: a local command channel into AFDK for the MCP server (patch\mcp\afdk_mcp.py).

   _afdk_remastered.dll loads this DLL if it lies next to it and calls McpStart on the program's UI thread.  We open the
   named pipe \\.\pipe\afdk_remastered (this user only, no remote clients).  A client sends one request per message:
   UTF-8 text, the command name and its arguments separated by spaces (an argument with spaces goes in double quotes).
   The answer is one message with UTF-8 JSON: {"ok":true,...} or {"ok":false,"error":"..."}.  Commands run on the UI
   thread (through a hidden window), so they see the program exactly as a click would. */
#include <windows.h>
#include <commctrl.h>
#include <sddl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include "common.h"
#include "mcp_api.h"

#define NL "\015\012"

static const AFDK_API *A;
static HWND g_wnd;                          /* hidden window on the UI thread */
#define WM_MCP_RUN    (WM_APP + 0x5A0)
#define WM_MCP_DIALOG (WM_APP + 0x5A1)       /* fill the node-name dialog once it shows */

/* ---------- program addresses (see CLAUDE.md / feat*.c) ---------- */
#define F_REFRESH       0x47de5c        /* (canvas) */
#define F_SET_ZOOM      0x47d67c        /* (canvas, level) */
#define F_DESELECT_ALL  0x4d1c20        /* (scheme) */
#define F_TOOL_SET      0x474c1c        /* (toolButton, down) */
#define F_SEL_COUNT     0x4d1cc0        /* (scheme) -> number of selected items */
#define F_DELETE_SEL    0x4d1bcc        /* (scheme) delete the selected items */
#define F_REBUILD       0x4d22d4        /* (scheme) rebuild junctions after an edit */
#define F_AFTER_EDIT    0x4d2bb0        /* (scheme) */
#define F_ACT_MODEL     0x4d9b24        /* TMainForm.AModelExecute(form, sender): simulation mode */
#define F_ACT_EDIT      0x4db02c        /* TMainForm.AEditExecute(form, sender): editor mode */
#define MF_TOOL_SELECT  0x374
#define MF_TOOL_LINE    0x37c
#define MF_TOOL_NODE    0x3a0
#define CV_HBAR   0x208
#define CV_VBAR   0x20c
#define CV_LEVEL  0x22c
#define BAR_POS   0x1f4

/* ---------- growing output buffer + JSON helpers ---------- */
typedef struct { char *p; int n, cap; } BUF;

static void b_put(BUF *b, const char *s, int n)
{
    if (n < 0) n = lstrlenA(s);
    if (b->n + n + 1 > b->cap) {
        int c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        b->p = b->p ? (char *)HeapReAlloc(GetProcessHeap(), 0, b->p, c) : (char *)HeapAlloc(GetProcessHeap(), 0, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void b_fmt(BUF *b, const char *fmt, ...)
{
    char t[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = wvsprintfA(t, fmt, ap);
    va_end(ap);
    if (n > 0) b_put(b, t, n);
}
static void b_jstr_utf8(BUF *b, const char *s)            /* JSON string from UTF-8 */
{
    b_put(b, "\"", 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; b_put(b, e, 2); }
        else if (c < 0x20) b_fmt(b, "\\u%04x", c);
        else b_put(b, (const char *)&c, 1);
    }
    b_put(b, "\"", 1);
}
static void b_jstr_w(BUF *b, const WCHAR *w, int n)      /* JSON string from UTF-16 */
{
    char t[2048];
    int k = WideCharToMultiByte(CP_UTF8, 0, w, n, t, sizeof t - 1, NULL, NULL);
    t[k > 0 ? k : 0] = 0;
    b_jstr_utf8(b, t);
}
static void b_jstr_cp(BUF *b, const char *s, int n)      /* JSON string from the program's own (1251) text */
{
    WCHAR w[1024];
    int k;
    if (n < 0) n = lstrlenA(s);
    k = MultiByteToWideChar(1251, 0, s, n, w, 1023);
    b_jstr_w(b, w, k > 0 ? k : 0);
}
static void b_short(BUF *b, unsigned obj, int off)        /* Delphi ShortString field */
{
    unsigned char n;
    if (!readable((void *)(obj + off), 1)) { b_put(b, "\"\"", 2); return; }
    n = U8(obj, off);
    if (n > 250 || !readable((void *)(obj + off + 1), n)) { b_put(b, "\"\"", 2); return; }
    b_jstr_cp(b, (const char *)(obj + off + 1), n);
}
static void b_err(BUF *b, const char *msg) { b->n = 0; b_put(b, "{\"ok\":false,\"error\":", -1); b_jstr_utf8(b, msg); b_put(b, "}", 1); }

/* ---------- request parsing ---------- */
#define MAXARG 8
typedef struct { int argc; char *argv[MAXARG]; } ARGS;

static void parse(char *s, ARGS *a)
{
    a->argc = 0;
    while (*s && a->argc < MAXARG) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s) break;
        if (*s == '"') {
            char *d;
            a->argv[a->argc++] = d = ++s;
            /* \" and \\ are escapes; any other backslash is literal, so plain Windows paths work too */
            while (*s && *s != '"') { if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++; *d++ = *s++; }
            if (*s) s++;
            *d = 0;
        } else {
            a->argv[a->argc++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
            if (*s) *s++ = 0;
        }
    }
}
static int iarg(const ARGS *a, int i, int def) { return i < a->argc ? (int)strtol(a->argv[i], NULL, 0) : def; }

static int utf8_to_cp(const char *u, char *out, int cap)  /* -> the program's text; 0 if a character has no 1251 form */
{
    WCHAR w[512];
    BOOL lossy = FALSE;
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, w, 512);
    if (n <= 0) return 0;
    n = WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, w, -1, out, cap, NULL, &lossy);
    return n > 0 && !lossy;
}

/* ---------- program state helpers ---------- */
typedef struct { unsigned pf, doc, scheme, cv; } CTX;

static int ctx(CTX *c, BUF *b, int need_edit)
{
    c->pf = A->active_pf();
    c->doc = c->pf ? (unsigned)F32(c->pf, 0x314) : 0;
    c->scheme = c->doc ? (unsigned)F32(c->doc, 0x34) : 0;
    c->cv = c->pf ? (unsigned)F32(c->pf, 0x308) : 0;
    if (!c->scheme) { if (b) b_err(b, "no project is open"); return 0; }
    if (need_edit && !A->doc_edit_mode(c->doc)) { if (b) b_err(b, "the project is in Model (simulation) mode; switch to Editor first"); return 0; }
    if (need_edit && F32(c->scheme, 0) && U8(F32(c->scheme, 0), 0x18)) { if (b) b_err(b, "a line is being drawn in the program"); return 0; }
    return 1;
}

static int doc_path(unsigned doc, char *out, int cap)
{
    unsigned cont = (unsigned)F32(doc, 0x40);
    int n;
    out[0] = 0;
    if (!cont || !readable((void *)(cont + 0x261), 256)) return 0;
    n = U8(cont, 0x261);
    if (n <= 0 || n >= cap) return 0;
    memcpy(out, (void *)(cont + 0x262), n);
    out[n] = 0;
    return n;
}

static void refresh(CTX *c) { if (c->cv) dcall((void *)F_REFRESH, c->cv, 0, 0, 0, 0, 0); }

static int tool(int field)
{
    unsigned mf = A->main_form(), btn = mf ? (unsigned)F32(mf, field) : 0;
    if (!btn) return 0;
    dcall((void *)F_TOOL_SET, btn, 1, 0, 0, 0, 0);
    return 1;
}

/* ---------- commands ---------- */
static void c_state(BUF *b, int full)
{
    CTX c;
    unsigned el, wl;
    int i, n, k2;
    char path[300];
    if (!ctx(&c, NULL, 0)) { b_put(b, "{\"ok\":true,\"project\":null}", -1); return; }
    doc_path(c.doc, path, sizeof path);
    b_put(b, "{\"ok\":true,\"project\":{\"path\":", -1);
    b_jstr_cp(b, path, -1);
    b_fmt(b, ",\"modified\":%s,\"mode\":\"%s\",\"zoom\":%d}", U8(c.doc, 0x18) ? "true" : "false",
          A->doc_edit_mode(c.doc) ? "editor" : "model", c.cv ? U8(c.cv, CV_LEVEL) : 0);
    el = (unsigned)F32(c.scheme, 0x10);
    wl = (unsigned)F32(c.scheme, 0x18);
    /* element: +0x44/+0x48 cell, +0x50 selected, +8 definition: +0x21c/+0x220 size, +0x224 label (ShortString),
       +0x214/+0x218 input/output pin arrays (Delphi dynamic arrays: length at -4).  Pins sit one cell outside the body:
       inputs at (x-1, y+k), outputs at (x+w+1, y+k), k = 1.. */
    b_put(b, ",\"elements\":[", -1);
    for (i = 1, n = el ? F32(el, 0) : 0; i <= n; i++) {
        unsigned e = LI(el, i), def = e ? (unsigned)F32(e, 8) : 0;
        int x, y, w = 0, h = 0, nin = 0, nout = 0, k;
        if (!e) continue;
        x = F32(e, 0x44); y = F32(e, 0x48);
        if (def) {
            unsigned li = (unsigned)F32(def, 0x214), lo = (unsigned)F32(def, 0x218);
            w = F32(def, 0x21c); h = F32(def, 0x220);
            nin = li && readable((void *)(li - 4), 4) ? F32(li, -4) : 0;
            nout = lo && readable((void *)(lo - 4), 4) ? F32(lo, -4) : 0;
        }
        b_fmt(b, "%s{\"id\":%d,\"label\":", i > 1 ? "," : "", i);
        if (def) b_short(b, def, 0x224); else b_put(b, "\"\"", 2);
        b_fmt(b, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"selected\":%s,\"inputs\":[", x, y, w, h,
              U8(e, 0x50) ? "true" : "false");
        for (k = 1; k <= nin && k < 64; k++) b_fmt(b, "%s[%d,%d]", k > 1 ? "," : "", x - 1, y + k);
        b_put(b, "],\"outputs\":[", -1);
        for (k = 1; k <= nout && k < 64; k++) b_fmt(b, "%s[%d,%d]", k > 1 ? "," : "", x + w + 1, y + k);
        b_put(b, "]", 1);
        if (full) b_fmt(b, ",\"obj\":\"%08x\",\"def\":\"%08x\"", e, def);
        b_put(b, "}", 1);
    }
    b_put(b, "]", 1);
    /* wires: +0x24/+0x28 start, +0x2c/+0x30 end offset, +0x18 selected, +0x14 -> {flag, ShortString name}.
       A named zero-length wire is a node (variable). */
    for (k2 = 0; k2 < 2; k2++) {
        int first = 1;
        b_put(b, k2 ? ",\"wires\":[" : ",\"nodes\":[", -1);
        for (i = 1, n = wl ? F32(wl, 0) : 0; i <= n; i++) {
            unsigned w = LI(wl, i), nm;
            int x, y, dx, dy, named;
            if (!w) continue;
            x = F32(w, 0x24); y = F32(w, 0x28); dx = F32(w, 0x2c); dy = F32(w, 0x30);
            nm = (unsigned)F32(w, 0x14);
            named = nm && readable((void *)nm, 2) && U8(nm, 1) > 0;
            if ((!dx && !dy && named) != !k2) continue;
            b_fmt(b, "%s{\"id\":%d,", first ? "" : ",", i);
            first = 0;
            if (named) { b_put(b, "\"name\":", -1); b_short(b, nm, 1); b_put(b, ",", 1); }
            if (k2) b_fmt(b, "\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d", x, y, x + dx, y + dy);
            else b_fmt(b, "\"x\":%d,\"y\":%d", x, y);
            b_fmt(b, ",\"selected\":%s", U8(w, 0x18) ? "true" : "false");
            if (full) b_fmt(b, ",\"obj\":\"%08x\"", w);
            b_put(b, "}", 1);
        }
        b_put(b, "]", 1);
    }
    b_put(b, "}", 1);
}

static void c_dump(BUF *b, const ARGS *a)
{
    unsigned p = (unsigned)strtoul(a->argc > 1 ? a->argv[1] : "0", NULL, 16);
    int n = iarg(a, 2, 64), i;
    if (n > 4096) n = 4096;
    if (!readable((void *)p, n)) { b_err(b, "not readable"); return; }
    b_put(b, "{\"ok\":true,\"hex\":\"", -1);
    for (i = 0; i < n; i++) b_fmt(b, "%02x", ((unsigned char *)p)[i]);
    b_put(b, "\"}", 2);
}

static void c_command(BUF *b, int id)
{
    DWORD_PTR r;
    if (!SendMessageTimeoutA(A->main_wnd, WM_COMMAND, (WPARAM)id, 0, SMTO_NORMAL, 20000, &r)) { b_err(b, "the program did not finish the command (a dialog may be waiting)"); return; }
    b_put(b, "{\"ok\":true}", -1);
}

static void c_file(BUF *b, const ARGS *a, int id)            /* open / save as: the dialog is answered with the path */
{
    WCHAR w[MAX_PATH];
    if (a->argc < 2 || MultiByteToWideChar(CP_UTF8, 0, a->argv[1], -1, w, MAX_PATH) <= 0) { b_err(b, "path expected"); return; }
    A->set_dialog_file(w);
    PostMessageA(A->main_wnd, WM_COMMAND, (WPARAM)id, 0);
    b_put(b, "{\"ok\":true,\"note\":\"started; call state to see the result\"}", -1);
}

/* mouse messages to the scheme window at model cell (x,y): the same path a real click takes */
static HWND canvas(CTX *c) { return c->cv ? A->canvas_hwnd_of((void *)c->cv) : NULL; }
static LPARAM vpt(CTX *c, int mx, int my)
{
    int lv = U8(c->cv, CV_LEVEL);
    int x = mx * lv - F32(F32(c->cv, CV_HBAR), BAR_POS), y = my * lv - F32(F32(c->cv, CV_VBAR), BAR_POS);
    return MAKELPARAM((short)x, (short)y);
}
static void show_cell(CTX *c, int mx, int my)         /* scroll so that the cell is inside the window */
{
    HWND h = canvas(c);
    RECT r;
    int lv = U8(c->cv, CV_LEVEL), x, y;
    if (!h) return;
    GetClientRect(h, &r);
    x = mx * lv - F32(F32(c->cv, CV_HBAR), BAR_POS);
    y = my * lv - F32(F32(c->cv, CV_VBAR), BAR_POS);
    if (x >= 20 && y >= 20 && x < r.right - 20 && y < r.bottom - 20) return;
    A->set_scroll(h, (void *)c->cv, mx * lv - r.right / 2, my * lv - r.bottom / 2);
}
static void click(CTX *c, int mx, int my, int right)
{
    HWND h = canvas(c);
    LPARAM p;
    if (!h) return;
    show_cell(c, mx, my);
    p = vpt(c, mx, my);
    SendMessageA(h, WM_MOUSEMOVE, 0, p);
    SendMessageA(h, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON, p);
    SendMessageA(h, right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, p);
}
static void drag(CTX *c, int x1, int y1, int x2, int y2)
{
    HWND h = canvas(c);
    int i;
    if (!h) return;
    show_cell(c, x1, y1);
    SendMessageA(h, WM_MOUSEMOVE, 0, vpt(c, x1, y1));
    SendMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, vpt(c, x1, y1));
    for (i = 1; i <= 8; i++)
        SendMessageA(h, WM_MOUSEMOVE, MK_LBUTTON, vpt(c, x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8));
    SendMessageA(h, WM_LBUTTONUP, 0, vpt(c, x2, y2));
}

static void c_mouse(BUF *b, const ARGS *a)                  /* raw: mouse <select|line|node|keep> x1 y1 [x2 y2] */
{
    CTX c;
    const char *t = a->argc > 1 ? a->argv[1] : "keep";
    int x1 = iarg(a, 2, 0), y1 = iarg(a, 3, 0);
    if (!ctx(&c, b, 1)) return;
    if (!lstrcmpA(t, "select")) tool(MF_TOOL_SELECT);
    else if (!lstrcmpA(t, "line")) tool(MF_TOOL_LINE);
    else if (!lstrcmpA(t, "node")) tool(MF_TOOL_NODE);
    A->hist_note(c.doc);
    if (a->argc > 5) drag(&c, x1, y1, iarg(a, 4, x1), iarg(a, 5, y1));
    else click(&c, x1, y1, 0);
    refresh(&c);
    A->hist_note(c.doc);
    b_put(b, "{\"ok\":true}", -1);
}

static void c_select(BUF *b, const ARGS *a)                 /* select x1 y1 x2 y2 : rubber band (model cells) */
{
    CTX c;
    if (!ctx(&c, b, 1)) return;
    tool(MF_TOOL_SELECT);
    dcall((void *)F_DESELECT_ALL, c.scheme, 0, 0, 0, 0, 0);
    drag(&c, iarg(a, 1, 0), iarg(a, 2, 0), iarg(a, 3, 0), iarg(a, 4, 0));
    refresh(&c);
    b_fmt(b, "{\"ok\":true,\"selected\":%d}", (int)dcall((void *)F_SEL_COUNT, c.scheme, 0, 0, 0, 0, 0));
}

static void c_move(BUF *b, const ARGS *a)
{
    CTX c;
    if (!ctx(&c, b, 1)) return;
    tool(MF_TOOL_SELECT);
    if (!A->nudge_selection(iarg(a, 1, 0), iarg(a, 2, 0))) { b_err(b, "nothing selected"); return; }
    b_put(b, "{\"ok\":true}", -1);
}

/* ---------- screenshots: 24-bit BMP of a window, base64 ---------- */
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b_base64(BUF *b, const unsigned char *d, int n)
{
    int i;
    char o[4];
    for (i = 0; i + 2 < n; i += 3) {
        o[0] = b64[d[i] >> 2]; o[1] = b64[((d[i] & 3) << 4) | (d[i + 1] >> 4)];
        o[2] = b64[((d[i + 1] & 15) << 2) | (d[i + 2] >> 6)]; o[3] = b64[d[i + 2] & 63];
        b_put(b, o, 4);
    }
    if (n - i == 1) { o[0] = b64[d[i] >> 2]; o[1] = b64[(d[i] & 3) << 4]; o[2] = o[3] = '='; b_put(b, o, 4); }
    else if (n - i == 2) {
        o[0] = b64[d[i] >> 2]; o[1] = b64[((d[i] & 3) << 4) | (d[i + 1] >> 4)]; o[2] = b64[(d[i + 1] & 15) << 2]; o[3] = '=';
        b_put(b, o, 4);
    }
}

static void c_screenshot(BUF *b, const ARGS *a)
{
    CTX c;
    HWND h = A->main_wnd;
    RECT r;
    HDC sdc, mdc;
    HBITMAP bm;
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER bf;
    int w, hh, stride, size;
    unsigned char *pix;
    const char *what = a->argc > 1 ? a->argv[1] : "window";
    if (!lstrcmpA(what, "canvas")) {
        if (!ctx(&c, b, 0) || !(h = A->canvas_hwnd_of((void *)c.cv))) { if (!b->n) b_err(b, "no canvas"); return; }
    } else if (lstrcmpA(what, "window") && lstrcmpA(what, "active")) {    /* a window class, e.g. TDiagram, TTable */
        HWND f = FindWindowA(what, NULL);
        DWORD pid = 0;
        if (f) GetWindowThreadProcessId(f, &pid);
        if (!f || pid != GetCurrentProcessId() || !IsWindowVisible(f)) { b_err(b, "no such visible window"); return; }
        h = f;
    } else if (!lstrcmpA(what, "active")) {
        HWND f = GetForegroundWindow();
        DWORD pid = 0;
        if (f) GetWindowThreadProcessId(f, &pid);
        if (pid == GetCurrentProcessId()) h = f;
    }
    GetWindowRect(h, &r);
    w = r.right - r.left; hh = r.bottom - r.top;
    if (w <= 0 || hh <= 0 || w > 8000 || hh > 8000) { b_err(b, "window has no size"); return; }
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    bm = CreateCompatibleBitmap(sdc, w, hh);
    SelectObject(mdc, bm);
    if (!PrintWindow(h, mdc, 2 /* PW_RENDERFULLCONTENT */)) BitBlt(mdc, 0, 0, w, hh, sdc, r.left, r.top, SRCCOPY);
    stride = (w * 3 + 3) & ~3;
    size = stride * hh;
    pix = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, sizeof bf + sizeof bi + size);
    memset(&bi, 0, sizeof bi);
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = hh; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    GetDIBits(mdc, bm, 0, hh, pix + sizeof bf + sizeof bi, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    memset(&bf, 0, sizeof bf);
    bf.bfType = 0x4d42; bf.bfSize = sizeof bf + sizeof bi + size; bf.bfOffBits = sizeof bf + sizeof bi;
    memcpy(pix, &bf, sizeof bf);
    memcpy(pix + sizeof bf, &bi, sizeof bi);
    b_fmt(b, "{\"ok\":true,\"width\":%d,\"height\":%d,\"bmp\":\"", w, hh);
    b_base64(b, pix, sizeof bf + sizeof bi + size);
    b_put(b, "\"}", 2);
    HeapFree(GetProcessHeap(), 0, pix);
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

/* ---------- windows and dialogs of the program ---------- */
typedef struct { BUF *b; int first; } WENUM;
static BOOL CALLBACK enum_child_text(HWND h, LPARAM l)
{
    WENUM *e = (WENUM *)l;
    WCHAR t[512], cls[64];
    int n = GetWindowTextW(h, t, 512);
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameW(h, cls, 64);
    b_put(e->b, e->first ? "{\"class\":" : ",{\"class\":", -1);
    e->first = 0;
    b_jstr_w(e->b, cls, -1);
    b_put(e->b, ",\"text\":", -1);
    b_jstr_w(e->b, t, n);
    b_put(e->b, "}", 1);
    return TRUE;
}
static BOOL CALLBACK enum_top(HWND h, LPARAM l)
{
    WENUM *e = (WENUM *)l, sub;
    DWORD pid = 0;
    WCHAR t[512], cls[64];
    int n;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || h == g_wnd) return TRUE;
    GetClassNameW(h, cls, 64);
    if (!lstrcmpW(cls, L"TApplication")) return TRUE;
    n = GetWindowTextW(h, t, 512);
    b_put(e->b, e->first ? "{\"class\":" : ",{\"class\":", -1);
    e->first = 0;
    b_jstr_w(e->b, cls, -1);
    b_put(e->b, ",\"title\":", -1);
    b_jstr_w(e->b, t, n);
    b_fmt(e->b, ",\"enabled\":%s", IsWindowEnabled(h) ? "true" : "false");
    if (h != A->main_wnd) {
        b_put(e->b, ",\"controls\":[", -1);
        sub.b = e->b; sub.first = 1;
        EnumChildWindows(h, enum_child_text, (LPARAM)&sub);
        b_put(e->b, "]", 1);
    }
    b_put(e->b, "}", 1);
    return TRUE;
}
static void c_windows(BUF *b)
{
    WENUM e;
    e.b = b; e.first = 1;
    b_put(b, "{\"ok\":true,\"windows\":[", -1);
    EnumWindows(enum_top, (LPARAM)&e);
    b_put(b, "]}", 2);
}

typedef struct { const WCHAR *want; HWND found; } FINDBTN;
static BOOL CALLBACK enum_btn(HWND h, LPARAM l)
{
    FINDBTN *f = (FINDBTN *)l;
    WCHAR t[256], *s, *d;
    if (!IsWindowVisible(h) || !IsWindowEnabled(h)) return TRUE;
    GetWindowTextW(h, t, 256);
    for (s = d = t; *s; s++) if (*s != L'&') *d++ = *s;        /* "&Yes" -> "Yes" */
    *d = 0;
    if (!lstrcmpiW(t, f->want)) { f->found = h; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK enum_top_btn(HWND h, LPARAM l)
{
    FINDBTN *f = (FINDBTN *)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || h == A->main_wnd || h == g_wnd) return TRUE;
    EnumChildWindows(h, enum_btn, l);
    return f->found == NULL;
}
static void c_press(BUF *b, const ARGS *a)                  /* press a button in an open dialog, by its caption */
{
    WCHAR w[256];
    FINDBTN f;
    if (a->argc < 2 || MultiByteToWideChar(CP_UTF8, 0, a->argv[1], -1, w, 256) <= 0) { b_err(b, "button text expected"); return; }
    f.want = w; f.found = NULL;
    EnumWindows(enum_top_btn, (LPARAM)&f);
    if (!f.found) { b_err(b, "no such button in an open dialog"); return; }
    PostMessageA(f.found, BM_CLICK, 0, 0);
    b_put(b, "{\"ok\":true}", -1);
}

/* ---------- library tree ---------- */
static int is_tree(HWND h);
static BOOL CALLBACK enum_tree(HWND h, LPARAM l)
{
    if (is_tree(h)) { *(HWND *)l = h; return FALSE; }
    return TRUE;
}
static int is_tree(HWND h)
{
    char c[64];
    return GetClassNameA(h, c, sizeof c) && (!lstrcmpA(c, "TTreeView") || !lstrcmpA(c, "SysTreeView32"));
}
static HWND lib_tree(void)
{
    HWND t = NULL;
    EnumChildWindows(A->main_wnd, enum_tree, (LPARAM)&t);
    return t;
}
static int tree_text(HWND t, HTREEITEM it, WCHAR *out, int cap)
{
    TVITEMW tv;
    memset(&tv, 0, sizeof tv);
    tv.mask = TVIF_TEXT | TVIF_HANDLE;
    tv.hItem = it;
    tv.pszText = out;
    tv.cchTextMax = cap;
    out[0] = 0;
    return (int)SendMessageW(t, TVM_GETITEMW, 0, (LPARAM)&tv);
}
static void tree_expand_all(HWND t, HTREEITEM it)
{
    for (; it; it = (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)it)) {
        SendMessageW(t, TVM_EXPAND, TVE_EXPAND, (LPARAM)it);
        tree_expand_all(t, (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)it));
    }
}
static void tree_list(BUF *b, HWND t, HTREEITEM it, const WCHAR *prefix, int *first)
{
    WCHAR txt[256], path[1024];
    for (; it; it = (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)it)) {
        HTREEITEM ch = (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)it);
        tree_text(t, it, txt, 256);
        lstrcpynW(path, prefix, 1024);
        if (path[0]) lstrcatW(path, L"/");
        lstrcatW(path, txt);
        if (!ch) {
            b_put(b, *first ? "" : ",", -1);
            *first = 0;
            b_jstr_w(b, path, -1);
        } else tree_list(b, t, ch, path, first);
    }
}
static HTREEITEM tree_find(HWND t, HTREEITEM it, const WCHAR *path)
{
    WCHAR txt[256];
    const WCHAR *slash = wcschr(path, L'/');
    int n = slash ? (int)(slash - path) : lstrlenW(path);
    for (; it; it = (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)it)) {
        tree_text(t, it, txt, 256);
        if (lstrlenW(txt) == n && CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, txt, n, path, n) == CSTR_EQUAL) {
            if (!slash) return it;
            return tree_find(t, (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)it), slash + 1);
        }
    }
    return NULL;
}
static void c_library(BUF *b)
{
    HWND t = lib_tree();
    int first = 1;
    HTREEITEM root;
    if (!t) { b_err(b, "library tree not found"); return; }
    root = (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    tree_expand_all(t, root);
    b_put(b, "{\"ok\":true,\"items\":[", -1);
    tree_list(b, t, (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_ROOT, 0), L"", &first);
    b_put(b, "]}", 2);
}

/* add <library path> x y : choose the element in the library tree, then click the scheme at model cell (x,y) */
static void c_add(BUF *b, const ARGS *a)
{
    CTX c;
    HWND t = lib_tree();
    WCHAR w[512];
    HTREEITEM it;
    unsigned el;
    int before;
    if (a->argc < 4 || MultiByteToWideChar(CP_UTF8, 0, a->argv[1], -1, w, 512) <= 0) { b_err(b, "add <library path> <x> <y>"); return; }
    if (!ctx(&c, b, 1)) return;
    if (!t) { b_err(b, "library tree not found"); return; }
    tree_expand_all(t, (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    if (!(it = tree_find(t, (HTREEITEM)SendMessageW(t, TVM_GETNEXTITEM, TVGN_ROOT, 0), w))) { b_err(b, "no such library item (see library)"); return; }
    SendMessageW(t, TVM_SELECTITEM, TVGN_CARET, (LPARAM)it);
    el = (unsigned)F32(c.scheme, 0x10);
    before = el ? F32(el, 0) : 0;
    A->hist_note(c.doc);
    click(&c, iarg(a, 2, 0), iarg(a, 3, 0), 0);
    tool(MF_TOOL_SELECT);
    refresh(&c);
    A->hist_note(c.doc);
    el = (unsigned)F32(c.scheme, 0x10);
    if ((el ? F32(el, 0) : 0) <= before) { b_err(b, "the program did not place the element"); return; }
    b_fmt(b, "{\"ok\":true,\"id\":%d}", F32(el, 0));
}

/* wire x1 y1 x2 y2 [x3 y3 ...] : a line through the points (each segment horizontal or vertical) */
static void c_wire(BUF *b, const ARGS *a)
{
    CTX c;
    unsigned wl;
    int before, i;
    if (a->argc < 5 || (a->argc - 1) % 2) { b_err(b, "wire x1 y1 x2 y2 [x3 y3 ...]"); return; }
    if (!ctx(&c, b, 1)) return;
    wl = (unsigned)F32(c.scheme, 0x18);
    before = wl ? F32(wl, 0) : 0;
    A->hist_note(c.doc);
    tool(MF_TOOL_LINE);
    for (i = 1; i + 3 < a->argc; i += 2)                                    /* one drag per segment */
        drag(&c, iarg(a, i, 0), iarg(a, i + 1, 0), iarg(a, i + 2, 0), iarg(a, i + 3, 0));
    tool(MF_TOOL_SELECT);
    refresh(&c);
    A->hist_note(c.doc);
    wl = (unsigned)F32(c.scheme, 0x18);
    b_fmt(b, "{\"ok\":true,\"new_wires\":%d}", (wl ? F32(wl, 0) : 0) - before);
}

/* node <name> x y : a named node (variable).  The program asks for the name in a dialog: we fill it in. */
static char g_node_name[256];
static int g_node_tries;
static void c_node(BUF *b, const ARGS *a)
{
    CTX c;
    unsigned wl;
    int before;
    if (a->argc < 4) { b_err(b, "node <name> <x> <y>"); return; }
    if (!utf8_to_cp(a->argv[1], g_node_name, sizeof g_node_name)) { b_err(b, "the name has characters the program cannot store"); return; }
    if (!ctx(&c, b, 1)) return;
    wl = (unsigned)F32(c.scheme, 0x18);
    before = wl ? F32(wl, 0) : 0;
    A->hist_note(c.doc);
    tool(MF_TOOL_NODE);
    g_node_tries = 0;
    PostMessageA(g_wnd, WM_MCP_DIALOG, 0, 0);                  /* runs inside the dialog's message loop */
    click(&c, iarg(a, 2, 0), iarg(a, 3, 0), 0);
    g_node_name[0] = 0;
    tool(MF_TOOL_SELECT);
    refresh(&c);
    A->hist_note(c.doc);
    wl = (unsigned)F32(c.scheme, 0x18);
    if ((wl ? F32(wl, 0) : 0) <= before) { b_err(b, "the program did not create the node"); return; }
    b_put(b, "{\"ok\":true}", -1);
}

static BOOL CALLBACK enum_edit(HWND h, LPARAM l)
{
    char c[64];
    if (GetClassNameA(h, c, sizeof c) && (!lstrcmpA(c, "TEdit") || !lstrcmpA(c, "Edit"))) { *(HWND *)l = h; return FALSE; }
    return TRUE;
}
static void fill_node_dialog(void)
{
    HWND d = GetActiveWindow(), e = NULL;
    FINDBTN f;
    if (!g_node_name[0]) return;
    if (!d || d == A->main_wnd) {
        if (++g_node_tries < 200) { Sleep(5); PostMessageA(g_wnd, WM_MCP_DIALOG, 0, 0); }
        return;
    }
    EnumChildWindows(d, enum_edit, (LPARAM)&e);
    if (e) SetWindowTextA(e, g_node_name);
    f.want = L"OK"; f.found = NULL;
    EnumChildWindows(d, enum_btn, (LPARAM)&f);
    if (f.found) PostMessageA(f.found, BM_CLICK, 0, 0);
}

static void c_delete(BUF *b)
{
    CTX c;
    int n;
    if (!ctx(&c, b, 1)) return;
    n = (int)dcall((void *)F_SEL_COUNT, c.scheme, 0, 0, 0, 0, 0);
    if (n <= 0) { b_err(b, "nothing selected"); return; }
    A->hist_note(c.doc);
    dcall((void *)F_DELETE_SEL, c.scheme, 0, 0, 0, 0, 0);
    dcall((void *)F_REBUILD, c.scheme, 0, 0, 0, 0, 0);
    dcall((void *)F_AFTER_EDIT, c.scheme, 0, 0, 0, 0, 0);
    refresh(&c);
    A->hist_note(c.doc);
    b_fmt(b, "{\"ok\":true,\"deleted\":%d}", n);
}

static void c_mode(BUF *b, const ARGS *a)
{
    CTX c;
    int model = a->argc > 1 && !lstrcmpA(a->argv[1], "model");
    unsigned mf = A->main_form();
    if (!ctx(&c, b, 0)) return;
    if (a->argc < 2 || (!model && lstrcmpA(a->argv[1], "editor"))) { b_err(b, "mode editor|model"); return; }
    if (!mf) { b_err(b, "main form not found"); return; }
    if (model != !A->doc_edit_mode(c.doc)) dcall((void *)(model ? F_ACT_MODEL : F_ACT_EDIT), mf, mf, 0, 0, 0, 0);
    b_fmt(b, "{\"ok\":true,\"mode\":\"%s\"}", A->doc_edit_mode(c.doc) ? "editor" : "model");
}

static void c_click(BUF *b, const ARGS *a)                  /* click x y [right]: any mode, the current tool */
{
    CTX c;
    if (a->argc < 3) { b_err(b, "click x y [right]"); return; }
    if (!ctx(&c, b, 0)) return;
    click(&c, iarg(a, 1, 0), iarg(a, 2, 0), a->argc > 3 && !lstrcmpA(a->argv[3], "right"));
    refresh(&c);
    b_put(b, "{\"ok\":true}", -1);
}

static void c_zoom(BUF *b, const ARGS *a)
{
    CTX c;
    int lv = iarg(a, 1, 10);
    if (!ctx(&c, b, 0)) return;
    if (lv < 5) lv = 5;
    if (lv > 40) lv = 40;
    dcall((void *)F_SET_ZOOM, c.cv, (unsigned)lv, 0, 0, 0, 0);
    refresh(&c);
    b_fmt(b, "{\"ok\":true,\"zoom\":%d}", U8(c.cv, CV_LEVEL));
}

static void run(char *req, BUF *b)
{
    ARGS a;
    CTX c;
    parse(req, &a);
    if (!a.argc) { b_err(b, "empty request"); return; }
#define IS(s) (!lstrcmpA(a.argv[0], s))
    if (IS("ping")) b_fmt(b, "{\"ok\":true,\"api\":%d}", A->version);
    else if (IS("state")) c_state(b, 0);
    else if (IS("state_raw")) c_state(b, 1);
    else if (IS("dump")) c_dump(b, &a);
    else if (IS("command")) c_command(b, iarg(&a, 1, 0));
    else if (IS("new")) c_command(b, 2);
    else if (IS("save")) c_command(b, 4);
    else if (IS("close")) { PostMessageA(A->main_wnd, WM_COMMAND, 6, 0); b_put(b, "{\"ok\":true}", -1); }
    else if (IS("open")) c_file(b, &a, 3);
    else if (IS("save_as")) c_file(b, &a, 5);
    else if (IS("library")) c_library(b);
    else if (IS("add")) c_add(b, &a);
    else if (IS("wire")) c_wire(b, &a);
    else if (IS("node")) c_node(b, &a);
    else if (IS("select")) c_select(b, &a);
    else if (IS("select_all")) { if (ctx(&c, b, 1)) { A->select_all(); b_put(b, "{\"ok\":true}", -1); } }
    else if (IS("deselect")) { if (ctx(&c, b, 1)) { dcall((void *)F_DESELECT_ALL, c.scheme, 0, 0, 0, 0, 0); refresh(&c); b_put(b, "{\"ok\":true}", -1); } }
    else if (IS("delete")) c_delete(b);
    else if (IS("mode")) c_mode(b, &a);
    else if (IS("click")) c_click(b, &a);
    else if (IS("duplicate")) { if (ctx(&c, b, 1)) { A->duplicate(); b_put(b, "{\"ok\":true}", -1); } }
    else if (IS("move")) c_move(b, &a);
    else if (IS("mouse")) c_mouse(b, &a);
    else if (IS("undo")) { A->hist_step(-1); b_put(b, "{\"ok\":true}", -1); }
    else if (IS("redo")) { A->hist_step(+1); b_put(b, "{\"ok\":true}", -1); }
    else if (IS("zoom")) c_zoom(b, &a);
    else if (IS("screenshot")) c_screenshot(b, &a);
    else if (IS("windows")) c_windows(b);
    else if (IS("press")) c_press(b, &a);
    else b_err(b, "unknown command");
#undef IS
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_MCP_RUN) { run((char *)wp, (BUF *)lp); return 1; }
    if (m == WM_MCP_DIALOG) { fill_node_dialog(); return 0; }
    return DefWindowProcA(h, m, wp, lp);
}

/* ---------- the pipe ---------- */
static DWORD WINAPI pipe_thread(LPVOID p)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    SECURITY_ATTRIBUTES sa;
    (void)p;
    /* owner (this user) only */
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &sd, NULL);
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = sd; sa.bInheritHandle = FALSE;
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\afdk_remastered", PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 1 << 20, 1 << 16, 0, sd ? &sa : NULL);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            static char req[65536];
            for (;;) {
                DWORD got = 0, put;
                BUF b = { 0 };
                DWORD_PTR r;
                if (!ReadFile(pipe, req, sizeof req - 1, &got, NULL)) break;
                req[got] = 0;
                if (!SendMessageTimeoutA(g_wnd, WM_MCP_RUN, (WPARAM)req, (LPARAM)&b, SMTO_NORMAL, 60000, &r))
                    b_err(&b, "the program is busy (a modal dialog or a long operation); try again");
                else if (!b.n) b_err(&b, "no answer");
                if (!WriteFile(pipe, b.p, (DWORD)b.n, &put, NULL)) { HeapFree(GetProcessHeap(), 0, b.p); break; }
                HeapFree(GetProcessHeap(), 0, b.p);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

__declspec(dllexport) int McpStart(const AFDK_API *api)
{
    WNDCLASSA wc;
    if (!api || api->version != AFDK_MCP_API_VERSION || g_wnd) return 0;
    A = api;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA("_afdk_remastered_mcp.dll");
    wc.lpszClassName = "AfdkRemasteredMcp";
    RegisterClassA(&wc);
    g_wnd = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 0;
    CloseHandle(CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL));
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)reason; (void)r; return TRUE; }
