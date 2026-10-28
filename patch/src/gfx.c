/* gfx.c - look: thicker lines on the scheme, larger interface font, Windows visual styles. Included from feat.c.
   Everything is controlled by _afdk_remastered.ini (cfg.c). */

/* ---------- thicker lines: drawing calls on the scheme canvas get a wider copy of the current pen ---------- */
typedef BOOL (WINAPI *pLineTo)(HDC, int, int);
typedef BOOL (WINAPI *pPolyline)(HDC, const POINT *, int);
typedef BOOL (WINAPI *pPolyPolyline)(HDC, const POINT *, const DWORD *, DWORD);
typedef BOOL (WINAPI *pRect4)(HDC, int, int, int, int);
typedef BOOL (WINAPI *pRoundRect)(HDC, int, int, int, int, int, int);
typedef BOOL (WINAPI *pPie)(HDC, int, int, int, int, int, int, int, int);
static pLineTo o_LineTo;
static pPolyline o_Polyline;
static pPolyPolyline o_PolyPolyline;
static pRect4 o_Rectangle, o_Ellipse;
static pRoundRect o_RoundRect;
static pPie o_Pie;

#define PEN_CACHE 32
static struct { COLORREF c; int w; HPEN p; } g_pens[PEN_CACHE];
static int g_npens;

static HPEN wide_pen(COLORREF c, int w)
{
    int i;
    for (i = 0; i < g_npens; i++) if (g_pens[i].c == c && g_pens[i].w == w) return g_pens[i].p;
    if (g_npens == PEN_CACHE) { DeleteObject(g_pens[0].p); memmove(g_pens, g_pens + 1, sizeof g_pens[0] * (PEN_CACHE - 1)); g_npens--; }
    g_pens[g_npens].c = c; g_pens[g_npens].w = w; g_pens[g_npens].p = CreatePen(PS_SOLID, w, c);
    return g_pens[g_npens++].p;
}

/* pen width wanted for drawing on dc, 0 = leave alone (not the scheme canvas, dashed/XOR pens, already wide) */
static int canvas_pen_width(HDC dc)
{
    HWND w;
    void *cv;
    HPEN cur;
    LOGPEN lp;
    int level, width;
    if (g_cfg_line_width <= 1 || !(w = WindowFromDC(dc)) || !is_class(w, "TFrpaint")) return 0;
    if (GetROP2(dc) != R2_COPYPEN) return 0;
    cur = (HPEN)GetCurrentObject(dc, OBJ_PEN);
    if (!cur || GetObjectA(cur, sizeof lp, &lp) != sizeof lp || lp.lopnStyle != PS_SOLID || lp.lopnWidth.x > 1) return 0;
    cv = obj_from_hwnd(w);
    level = cv && readable((char *)cv + CV_LEVEL, 1) ? U8(cv, CV_LEVEL) : 10;
    width = (g_cfg_line_width * level + 5) / 10;
    return width > 1 ? width : 0;
}

#define WIDE_BEGIN(dc)                                                              \
    int ww_ = canvas_pen_width(dc);                                                 \
    HGDIOBJ old_ = NULL;                                                            \
    if (ww_) {                                                                      \
        LOGPEN lp_;                                                                 \
        GetObjectA(GetCurrentObject(dc, OBJ_PEN), sizeof lp_, &lp_);                \
        old_ = SelectObject(dc, wide_pen(lp_.lopnColor, ww_));                      \
    }
#define WIDE_END(dc) if (old_) SelectObject(dc, old_);

static BOOL WINAPI h_LineTo(HDC dc, int x, int y) { BOOL r; WIDE_BEGIN(dc) r = o_LineTo(dc, x, y); WIDE_END(dc) return r; }
static BOOL WINAPI h_Polyline(HDC dc, const POINT *p, int n) { BOOL r; WIDE_BEGIN(dc) r = o_Polyline(dc, p, n); WIDE_END(dc) return r; }
static BOOL WINAPI h_PolyPolyline(HDC dc, const POINT *p, const DWORD *c, DWORD n) { BOOL r; WIDE_BEGIN(dc) r = o_PolyPolyline(dc, p, c, n); WIDE_END(dc) return r; }
static BOOL WINAPI h_Rectangle(HDC dc, int a, int b, int c, int d) { BOOL r; WIDE_BEGIN(dc) r = o_Rectangle(dc, a, b, c, d); WIDE_END(dc) return r; }
static BOOL WINAPI h_Ellipse(HDC dc, int a, int b, int c, int d) { BOOL r; WIDE_BEGIN(dc) r = o_Ellipse(dc, a, b, c, d); WIDE_END(dc) return r; }
static BOOL WINAPI h_RoundRect(HDC dc, int a, int b, int c, int d, int e, int f) { BOOL r; WIDE_BEGIN(dc) r = o_RoundRect(dc, a, b, c, d, e, f); WIDE_END(dc) return r; }
static BOOL WINAPI h_Pie(HDC dc, int a, int b, int c, int d, int e, int f, int g, int k) { BOOL r; WIDE_BEGIN(dc) r = o_Pie(dc, a, b, c, d, e, f, g, k); WIDE_END(dc) return r; }

/* element outlines are drawn with FrameRect (a brush, not a pen): draw the frame several times, inset */
typedef int (WINAPI *pFrameRect)(HDC, const RECT *, HBRUSH);
static pFrameRect o_FrameRect;
static int WINAPI h_FrameRect(HDC dc, const RECT *rc, HBRUSH br)
{
    HWND w;
    int width = 1, i, r;
    if (rc && g_cfg_line_width > 1 && (w = WindowFromDC(dc)) != NULL && is_class(w, "TFrpaint")) {
        void *cv = obj_from_hwnd(w);
        int level = cv && readable((char *)cv + CV_LEVEL, 1) ? U8(cv, CV_LEVEL) : 10;
        width = (g_cfg_line_width * level + 5) / 10;
    }
    r = o_FrameRect(dc, rc, br);
    for (i = 1; i < width && rc && rc->right - rc->left > 2 * i && rc->bottom - rc->top > 2 * i; i++) {
        RECT q = { rc->left + i, rc->top + i, rc->right - i, rc->bottom - i };
        o_FrameRect(dc, &q, br);
    }
    return r;
}

/* ---------- interface font ----------
   Forms and dialogs keep their font: Delphi lays dialogs out for "MS Sans Serif" and rescales them (badly) when the text
   height differs.  The menu (system menu font) is scaled here; the library tree gets a larger font of its own. */
typedef HFONT (WINAPI *pCreateFontIndirectA)(const LOGFONTA *);
static pCreateFontIndirectA o_CreateFontIndirectA;

static HFONT WINAPI h_CreateFontIndirectA(const LOGFONTA *lf)
{
    LOGFONTA f;
    if (!lf) return o_CreateFontIndirectA(lf);
    f = *lf;
    /* text drawn on the scheme goes through TextOutA, which reads bytes in the font's charset: ask for Cyrillic */
    if (g_cp == 1251 && (f.lfCharSet == DEFAULT_CHARSET || f.lfCharSet == ANSI_CHARSET)) f.lfCharSet = RUSSIAN_CHARSET;
    if (f.lfHeight && lstrcmpiA(f.lfFaceName, "MS Sans Serif") && lstrcmpiA(f.lfFaceName, "Arial"))
        f.lfHeight = MulDiv(f.lfHeight, g_cfg_font_pct, 100);
    return o_CreateFontIndirectA(&f);
}

static HFONT g_tree_font;
static BOOL CALLBACK tree_font_cb(HWND h, LPARAM l)
{
    (void)l;
    if ((is_class(h, "TTreeView") || is_class(h, "SysTreeView32")) && (HFONT)SendMessageA(h, WM_GETFONT, 0, 0) != g_tree_font)
        SendMessageA(h, WM_SETFONT, (WPARAM)g_tree_font, TRUE);
    return TRUE;
}
/* called from the timer: (re)apply the larger font to the library tree */
static void tree_font_tick(void)
{
    if (g_cfg_font_pct == 100 || !g_main) return;
    if (!g_tree_font)
        g_tree_font = CreateFontA(-MulDiv(11, g_cfg_font_pct, 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, g_cfg_font);
    EnumChildWindows(g_main, tree_font_cb, 0);
}

/* ---------- Windows visual styles ----------
   The program initialises the common controls (InitCommonControls) before it creates any window.  At that moment we
   activate an activation context that asks for Common Controls 6 and initialise that version, so every window created
   afterwards gets the current Windows look.  Nothing is written next to the exe. */
static const char g_manifest[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>" NL
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">" NL
    "  <dependency><dependentAssembly>" NL
    "    <assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\"" NL
    "      processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>" NL
    "  </dependentAssembly></dependency>" NL
    "</assembly>" NL;

static HANDLE g_styles_ctx;
static ULONG_PTR g_styles_cookie;

/* switch the main thread to Common Controls 6; returns that DLL (0 if it failed) */
static HMODULE styles_on(void)
{
    static int done;
    static HMODULE cc6;
    if (!done) {
        char path[MAX_PATH];
        HANDLE f, ctx;
        ACTCTXA ac;
        ULONG_PTR cookie;
        done = 1;
        GetTempPathA(MAX_PATH, path);
        lstrcatA(path, "_afdk_remastered_styles.manifest");
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return NULL;
        {
            DWORD w;
            WriteFile(f, g_manifest, sizeof g_manifest - 1, &w, NULL);
            CloseHandle(f);
        }
        memset(&ac, 0, sizeof ac);
        ac.cbSize = sizeof ac;
        ac.lpSource = path;
        ctx = CreateActCtxA(&ac);
        if (ctx != INVALID_HANDLE_VALUE && ActivateActCtx(ctx, &cookie)) {       /* stays active for the main thread */
            g_styles_ctx = ctx; g_styles_cookie = cookie;
            cc6 = LoadLibraryA("comctl32.dll");
        }
        fx_log("styles: comctl32 v6 %p" NL, (void *)cc6);
    }
    return cc6;
}

/* The VCL looks up InitCommonControlsEx with GetProcAddress before it creates any window: hand it the version 6 one. */
typedef FARPROC (WINAPI *pGetProcAddress)(HMODULE, LPCSTR);
static pGetProcAddress o_GetProcAddress;
static FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR name)
{
    if ((UINT_PTR)name > 0xffff && g_cfg_themes && !lstrcmpA(name, "InitCommonControlsEx")) {
        HMODULE cc6 = styles_on();
        if (cc6) { FARPROC p = o_GetProcAddress(cc6, name); if (p) return p; }
    }
    return o_GetProcAddress(m, name);
}

/* Combo boxes, list boxes and scroll bars must stay classic: the program recreates one while a project is opened from the File menu, and
   the version 6 control refuses that ("Win32 Error 1447: The window does not have scroll bars").  The VCL builds its
   control classes from GetClassInfo("COMBOBOX" / "LISTBOX" / "SCROLLBAR"): answer those outside our activation context. */
typedef BOOL (WINAPI *pGetClassInfoA)(HINSTANCE, LPCSTR, LPWNDCLASSA);
static pGetClassInfoA o_GetClassInfoA;
static BOOL WINAPI h_GetClassInfoA(HINSTANCE inst, LPCSTR name, LPWNDCLASSA wc)
{
    BOOL r;
    if (g_styles_ctx && (UINT_PTR)name > 0xffff && (!lstrcmpiA(name, "COMBOBOX") || !lstrcmpiA(name, "LISTBOX") ||
                                                       !lstrcmpiA(name, "SCROLLBAR"))) {
        ULONG_PTR c2 = 0;
        BOOL pushed = ActivateActCtx(NULL, &c2);          /* the process default context: classic controls */
        r = o_GetClassInfoA(inst, name, wc);
        if (pushed) DeactivateActCtx(0, c2);
        return r;
    }
    return o_GetClassInfoA(inst, name, wc);
}

/* ---------- program title: "AFDK 2.0 | DEVELOPED BY FIOT STUDENT(S)" -> [View] Title from _afdk_remastered.ini ---------- */
static const char g_orig_title[] = "AFDK 2.0 | DEVELOPED BY FIOT STUDENT";

/* text with the original title at its start replaced by the configured one (or text itself) */
static LPCSTR title_fix(LPCSTR text, char *buf, int cap)
{
    int n = sizeof g_orig_title - 1;
    if (!g_cfg_title[0] || (UINT_PTR)text <= 0xffff || strncmp(text, g_orig_title, n)) return text;
    if (text[n] == 'S') n++;
    lstrcpynA(buf, g_cfg_title, cap);
    lstrcpynA(buf + lstrlenA(buf), text + n, cap - lstrlenA(buf));
    return buf;
}

typedef BOOL (WINAPI *pSetWindowTextA)(HWND, LPCSTR);
static pSetWindowTextA o_SetWindowTextA;
static BOOL WINAPI h_SetWindowTextA(HWND w, LPCSTR text)
{
    char buf[512];
    LPCSTR t = title_fix(text, buf, sizeof buf);
    if (g_cp && has_high(t)) {            /* the kernel would convert ANSI text with the system code page */
        WCHAR wt[512];
        BOOL ok = SetWindowTextW(w, a2w_cp(t, wt, 512));
        caption_fix(w);
        return ok;
    }
    return o_SetWindowTextA(w, t);
}

typedef int (WINAPI *pMessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static pMessageBoxA o_MessageBoxA;
static int WINAPI h_MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    char buf[512];
    LPCSTR c = title_fix(caption, buf, sizeof buf);
    if (g_cp && (has_high(text) || has_high(c))) {
        WCHAR wt[1024], wc[512];
        return MessageBoxW(w, text ? a2w_cp(text, wt, 1024) : NULL, c ? a2w_cp(c, wc, 512) : NULL, type);
    }
    return o_MessageBoxA(w, text, c, type);
}

typedef HWND (WINAPI *pCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static pCreateWindowExA o_CreateWindowExA;
static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    char buf[512];
    HWND r;
    LPCSTR nm = title_fix(name, buf, sizeof buf);
    if (g_cp && has_high(nm)) {          /* the kernel would convert an ANSI name with the system code page: pass Unicode */
        WCHAR wn[512], wc[256];
        LPCWSTR c = (UINT_PTR)cls > 0xffff ? a2w_cp(cls, wc, 256) : (LPCWSTR)cls;
        r = CreateWindowExW(ex, c, a2w_cp(nm, wn, 512), style, x, y, w, h, parent, menu, inst, param);
        /* the program's (ANSI) window procedure gets the name back converted with the system code page and stores
           that; put the right caption in place directly */
        if (r) {
            caption_learn(r, nm);
            caption_fix(r);
        }
    } else
        r = o_CreateWindowExA(ex, cls, nm, style, x, y, w, h, parent, menu, inst, param);
    if (!r) {
        DWORD e = GetLastError();
        err_log("CreateWindowEx failed: class %s, style %08x, error %u" NL, (UINT_PTR)cls > 0xffff ? cls : "#", (unsigned)style, (unsigned)e);
        fx_log("CreateWindowEx FAILED cls=%s style=%08x err=%u" NL, (UINT_PTR)cls > 0xffff ? cls : "#", (unsigned)style, (unsigned)e);
        SetLastError(e);
    }
    return r;
}

static void remove_old_manifest(void)          /* an earlier build wrote AFDK.exe.manifest; it is no longer used */
{
    char p[MAX_PATH + 16], buf[256] = {0};
    DWORD got = 0;
    HANDLE f;
    GetModuleFileNameA(NULL, p, MAX_PATH);
    lstrcatA(p, ".manifest");
    f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    if (strstr(buf, "afdkfix")) DeleteFileA(p);
}

static void install_gfx(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    remove_old_manifest();
    o_GetProcAddress = (pGetProcAddress)iat_hook(exe, "kernel32.dll", "GetProcAddress", (void *)h_GetProcAddress);
    if (g_cfg_themes)
    {
        o_GetClassInfoA = (pGetClassInfoA)iat_hook(exe, "user32.dll", "GetClassInfoA", (void *)h_GetClassInfoA);
    }
    if (g_cfg_line_width > 1) {
        o_LineTo = (pLineTo)iat_hook(exe, "gdi32.dll", "LineTo", (void *)h_LineTo);
        o_Polyline = (pPolyline)iat_hook(exe, "gdi32.dll", "Polyline", (void *)h_Polyline);
        o_PolyPolyline = (pPolyPolyline)iat_hook(exe, "gdi32.dll", "PolyPolyline", (void *)h_PolyPolyline);
        o_Rectangle = (pRect4)iat_hook(exe, "gdi32.dll", "Rectangle", (void *)h_Rectangle);
        o_Ellipse = (pRect4)iat_hook(exe, "gdi32.dll", "Ellipse", (void *)h_Ellipse);
        o_RoundRect = (pRoundRect)iat_hook(exe, "gdi32.dll", "RoundRect", (void *)h_RoundRect);
        o_Pie = (pPie)iat_hook(exe, "gdi32.dll", "Pie", (void *)h_Pie);
        o_FrameRect = (pFrameRect)iat_hook(exe, "user32.dll", "FrameRect", (void *)h_FrameRect);
    }
    if (g_cfg_title[0] || g_cp)
        o_SetWindowTextA = (pSetWindowTextA)iat_hook(exe, "user32.dll", "SetWindowTextA", (void *)h_SetWindowTextA);
    if (g_cfg_title[0] || g_cp)
        o_MessageBoxA = (pMessageBoxA)iat_hook(exe, "user32.dll", "MessageBoxA", (void *)h_MessageBoxA);
    if (g_cfg_title[0] || g_cfg_themes || g_cp)
        o_CreateWindowExA = (pCreateWindowExA)iat_hook(exe, "user32.dll", "CreateWindowExA", (void *)h_CreateWindowExA);
    if (g_cfg_font_pct != 100 || g_cp == 1251)
        o_CreateFontIndirectA = (pCreateFontIndirectA)iat_hook(exe, "gdi32.dll", "CreateFontIndirectA", (void *)h_CreateFontIndirectA);
}
