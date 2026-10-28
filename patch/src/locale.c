/* locale.c - Cyrillic text under any Windows "Language for non-Unicode programs".
   AFDK keeps its texts in code page 1251.  With another system code page (English Windows: 1252) every menu, tree and
   label turns into "Ôàéë".  Like Locale Emulator, inside this process only:
   - the ntdll conversion tables (used by user32 for menus, window texts, file names...) are rebuilt from c_1251.nls;
   - GetACP, MultiByteToWideChar, WideCharToMultiByte, GetCPInfo of KernelBase (used by the common controls and the
     program itself) treat "the ANSI code page" as 1251.
   The KernelBase functions are patched through their hot-patch prologue (mov edi,edi + 5 bytes of padding).
   Included from afdkfix.c. */

typedef struct {
    USHORT CodePage, MaximumCharacterSize, DefaultChar, UniDefaultChar, TransDefaultChar, TransUniDefaultChar;
    USHORT DBCSCodePage;
    UCHAR LeadByte[12];
    PUSHORT MultiByteTable;
    PVOID WideCharTable;
    PUSHORT DBCSRanges;
    PUSHORT DBCSOffsets;
} X_CPTABLEINFO;
typedef struct { X_CPTABLEINFO Oem, Ansi; PUSHORT UpperCaseTable, LowerCaseTable; } X_NLSTABLEINFO;
typedef VOID (NTAPI *pRtlInitNlsTables)(PUSHORT, PUSHORT, PUSHORT, X_NLSTABLEINFO *);
typedef VOID (NTAPI *pRtlResetRtlTranslations)(X_NLSTABLEINFO *);

static UINT g_cp;                       /* the code page the program runs with (0 = untouched) */
static X_NLSTABLEINFO g_nls;

static void *map_nls(UINT cp)
{
    static const char *dirs[] = { "Sysnative", "System32", "SysWOW64" };   /* a 32-bit process sees System32 as SysWOW64 */
    char win[MAX_PATH], path[MAX_PATH + 64];
    int i;
    if (!GetWindowsDirectoryA(win, MAX_PATH)) return NULL;
    for (i = 0; i < 3; i++) {
        HANDLE f, m;
        void *v;
        wsprintfA(path, "%s\\%s\\c_%u.nls", win, dirs[i], cp);
        f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
        CloseHandle(f);
        if (!m) continue;
        v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(m);
        if (v) return v;
    }
    return NULL;
}

/* hot-patch fn: "jmp hook" in the 5 padding bytes before it, "jmp $-5" over its "mov edi,edi".
   The original stays callable at fn + 2.  Returns that address, or NULL if fn has no hot-patch prologue. */
static void *hotpatch(void *fn, void *hook)
{
    unsigned char *p = (unsigned char *)fn;
    DWORD old;
    int i;
    if (!p || p[0] != 0x8B || p[1] != 0xFF) return NULL;
    for (i = 1; i <= 5; i++) if (p[-i] != 0xCC && p[-i] != 0x90) return NULL;
    if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    p[-5] = 0xE9;
    *(int *)(p - 4) = (int)((unsigned char *)hook - p);
    *(volatile unsigned short *)p = 0xF9EB;
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    return p + 2;
}

static UINT cp_fix(UINT cp) { return (cp == CP_ACP || cp == CP_THREAD_ACP) ? g_cp : cp; }

typedef UINT (WINAPI *pGetACP)(void);
typedef int (WINAPI *pMBTWC)(UINT, DWORD, LPCCH, int, LPWSTR, int);
typedef int (WINAPI *pWCTMB)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
typedef BOOL (WINAPI *pGetCPInfo)(UINT, LPCPINFO);
static pMBTWC o_MBTWC;
static pWCTMB o_WCTMB;
static pGetCPInfo o_GetCPInfo;

static UINT WINAPI h_GetACP(void) { return g_cp; }
static int WINAPI h_MBTWC(UINT cp, DWORD fl, LPCCH s, int n, LPWSTR d, int dn) { return o_MBTWC(cp_fix(cp), fl, s, n, d, dn); }
static int WINAPI h_WCTMB(UINT cp, DWORD fl, LPCWCH s, int n, LPSTR d, int dn, LPCCH def, LPBOOL used)
{
    return o_WCTMB(cp_fix(cp), fl, s, n, d, dn, def, used);
}
static BOOL WINAPI h_GetCPInfo(UINT cp, LPCPINFO ci) { return o_GetCPInfo(cp_fix(cp), ci); }

static void locale_override(UINT cp)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll"), kb = GetModuleHandleA("kernelbase.dll");
    pRtlInitNlsTables init = nt ? (pRtlInitNlsTables)GetProcAddress(nt, "RtlInitNlsTables") : NULL;
    pRtlResetRtlTranslations reset = nt ? (pRtlResetRtlTranslations)GetProcAddress(nt, "RtlResetRtlTranslations") : NULL;
    unsigned char *peb;
    void *ansi;
    if (!cp || cp == GetACP() || !init || !reset || !kb) return;
    if (!(ansi = map_nls(cp))) return;
    g_cp = cp;
    /* KernelBase first: once these are in place nothing can mix the two code pages */
    o_MBTWC = (pMBTWC)hotpatch((void *)GetProcAddress(kb, "MultiByteToWideChar"), (void *)h_MBTWC);
    o_WCTMB = (pWCTMB)hotpatch((void *)GetProcAddress(kb, "WideCharToMultiByte"), (void *)h_WCTMB);
    o_GetCPInfo = (pGetCPInfo)hotpatch((void *)GetProcAddress(kb, "GetCPInfo"), (void *)h_GetCPInfo);
    hotpatch((void *)GetProcAddress(kb, "GetACP"), (void *)h_GetACP);
    /* then ntdll's tables */
    peb = (unsigned char *)__readfsdword(0x30);
    init((PUSHORT)ansi, *(PUSHORT *)(peb + 0x5c), *(PUSHORT *)(peb + 0x60), &g_nls);    /* OEM and case tables stay */
    reset(&g_nls);
    *(void **)(peb + 0x58) = ansi;                                                      /* PEB->AnsiCodePageData */
}

/* ---------- menus: user32 hands ANSI menu texts to the kernel, which converts them with the system code page ---------- */
static WCHAR *a2w_cp(LPCSTR s, WCHAR *buf, int cap)
{
    int n = MultiByteToWideChar(g_cp, 0, s, -1, buf, cap);
    if (n <= 0) buf[0] = 0;
    buf[cap - 1] = 0;
    return buf;
}
static int menu_has_text(const MENUITEMINFOA *mi)
{
    if (!mi || !mi->dwTypeData) return 0;
    if (mi->fMask & MIIM_STRING) return 1;
    return (mi->fMask & MIIM_TYPE) && !(mi->fType & (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR));
}

typedef BOOL (WINAPI *pInsertMenuItemA)(HMENU, UINT, BOOL, LPCMENUITEMINFOA);
typedef BOOL (WINAPI *pInsertMenuA)(HMENU, UINT, UINT, UINT_PTR, LPCSTR);
typedef int (WINAPI *pGetMenuStringA)(HMENU, UINT, LPSTR, int, UINT);
typedef BOOL (WINAPI *pGetMenuItemInfoA)(HMENU, UINT, BOOL, LPMENUITEMINFOA);
static pInsertMenuItemA o_InsertMenuItemA, o_SetMenuItemInfoA;
static pInsertMenuA o_InsertMenuA;
static pGetMenuStringA o_GetMenuStringA;
static pGetMenuItemInfoA o_GetMenuItemInfoA;

static BOOL WINAPI h_InsertMenuItemA(HMENU m, UINT item, BOOL bypos, LPCMENUITEMINFOA mi)
{
    MENUITEMINFOW w;
    WCHAR buf[512];
    if (!menu_has_text(mi)) return o_InsertMenuItemA(m, item, bypos, mi);
    memcpy(&w, mi, mi->cbSize < sizeof w ? mi->cbSize : sizeof w);
    w.dwTypeData = a2w_cp(mi->dwTypeData, buf, 512);
    return InsertMenuItemW(m, item, bypos, &w);
}
static BOOL WINAPI h_SetMenuItemInfoA(HMENU m, UINT item, BOOL bypos, LPCMENUITEMINFOA mi)
{
    MENUITEMINFOW w;
    WCHAR buf[512];
    if (!menu_has_text(mi)) return o_SetMenuItemInfoA(m, item, bypos, mi);
    memcpy(&w, mi, mi->cbSize < sizeof w ? mi->cbSize : sizeof w);
    w.dwTypeData = a2w_cp(mi->dwTypeData, buf, 512);
    return SetMenuItemInfoW(m, item, bypos, &w);
}
static BOOL WINAPI h_InsertMenuA(HMENU m, UINT pos, UINT fl, UINT_PTR id, LPCSTR s)
{
    WCHAR buf[512];
    if (!s || (fl & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) return o_InsertMenuA(m, pos, fl, id, s);
    return InsertMenuW(m, pos, fl, id, a2w_cp(s, buf, 512));
}
static int WINAPI h_GetMenuStringA(HMENU m, UINT item, LPSTR s, int cap, UINT fl)
{
    WCHAR buf[512];
    int n = GetMenuStringW(m, item, buf, 512, fl);
    if (!s || cap <= 0) return n;
    if (n <= 0) { s[0] = 0; return n; }
    n = WideCharToMultiByte(g_cp, 0, buf, -1, s, cap, NULL, NULL);
    s[cap - 1] = 0;
    return n > 0 ? lstrlenA(s) : 0;
}
static BOOL WINAPI h_GetMenuItemInfoA(HMENU m, UINT item, BOOL bypos, LPMENUITEMINFOA mi)
{
    MENUITEMINFOW w;
    WCHAR buf[512];
    BOOL r;
    if (!mi || !(mi->fMask & (MIIM_STRING | MIIM_TYPE)) || !mi->dwTypeData || !mi->cch) return o_GetMenuItemInfoA(m, item, bypos, mi);
    memcpy(&w, mi, mi->cbSize < sizeof w ? mi->cbSize : sizeof w);
    w.dwTypeData = buf; w.cch = 512;
    r = GetMenuItemInfoW(m, item, bypos, &w);
    if (r) {
        LPSTR out = mi->dwTypeData;
        UINT cap = mi->cch;
        memcpy(mi, &w, mi->cbSize < sizeof w ? mi->cbSize : sizeof w);
        mi->dwTypeData = out;
        if (!(w.fType & (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR))) {
            WideCharToMultiByte(g_cp, 0, buf, -1, out, cap, NULL, NULL);
            out[cap - 1] = 0;
            mi->cch = lstrlenA(out);
        } else mi->cch = cap;
    }
    return r;
}

static void locale_hook_menus(HMODULE exe)
{
    if (!g_cp) return;
    o_InsertMenuItemA = (pInsertMenuItemA)iat_hook(exe, "user32.dll", "InsertMenuItemA", (void *)h_InsertMenuItemA);
    o_SetMenuItemInfoA = (pInsertMenuItemA)iat_hook(exe, "user32.dll", "SetMenuItemInfoA", (void *)h_SetMenuItemInfoA);
    o_InsertMenuA = (pInsertMenuA)iat_hook(exe, "user32.dll", "InsertMenuA", (void *)h_InsertMenuA);
    o_GetMenuStringA = (pGetMenuStringA)iat_hook(exe, "user32.dll", "GetMenuStringA", (void *)h_GetMenuStringA);
    o_GetMenuItemInfoA = (pGetMenuItemInfoA)iat_hook(exe, "user32.dll", "GetMenuItemInfoA", (void *)h_GetMenuItemInfoA);
}

/* ---------- window captions: DefWindowProcA(WM_SETTEXT) also leaves the conversion to the kernel ---------- */
static int has_high(LPCSTR s)
{
    if (!s || (UINT_PTR)s <= 0xffff) return 0;
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return 1;
    return 0;
}

typedef LRESULT (WINAPI *pDefWindowProcA)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (WINAPI *pDefFrameProcA)(HWND, HWND, UINT, WPARAM, LPARAM);
static pDefWindowProcA o_DefWindowProcA, o_DefMDIChildProcA;
static pDefFrameProcA o_DefFrameProcA;

static LRESULT dwp_gettext(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT (WINAPI *def)(HWND, UINT, WPARAM, LPARAM));

static LRESULT WINAPI h_DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WCHAR buf[512];
    if (msg == WM_SETTEXT && has_high((LPCSTR)lp)) return DefWindowProcW(h, msg, wp, (LPARAM)a2w_cp((LPCSTR)lp, buf, 512));
    if (msg == WM_GETTEXT || msg == WM_GETTEXTLENGTH)
        return dwp_gettext(h, msg, wp, lp, DefWindowProcW);
    return o_DefWindowProcA(h, msg, wp, lp);
}
static LRESULT WINAPI h_DefMDIChildProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WCHAR buf[512];
    if (msg == WM_SETTEXT && has_high((LPCSTR)lp)) return DefMDIChildProcW(h, msg, wp, (LPARAM)a2w_cp((LPCSTR)lp, buf, 512));
    return o_DefMDIChildProcA(h, msg, wp, lp);
}
static LRESULT WINAPI h_DefFrameProcA(HWND h, HWND client, UINT msg, WPARAM wp, LPARAM lp)
{
    WCHAR buf[512];
    if (msg == WM_SETTEXT && has_high((LPCSTR)lp)) return DefFrameProcW(h, client, msg, wp, (LPARAM)a2w_cp((LPCSTR)lp, buf, 512));
    return o_DefFrameProcA(h, client, msg, wp, lp);
}

static void locale_hook_captions(HMODULE exe)
{
    if (!g_cp) return;
    o_DefWindowProcA = (pDefWindowProcA)iat_hook(exe, "user32.dll", "DefWindowProcA", (void *)h_DefWindowProcA);
    o_DefMDIChildProcA = (pDefWindowProcA)iat_hook(exe, "user32.dll", "DefMDIChildProcA", (void *)h_DefMDIChildProcA);
    o_DefFrameProcA = (pDefFrameProcA)iat_hook(exe, "user32.dll", "DefFrameProcA", (void *)h_DefFrameProcA);
}

/* ---------- window captions, the rest: whatever path the program uses, the caption Windows stores ends up converted
   with the system code page.  The program's own copy of the text (TControl.FText, a 1251 string) is right, so after
   every caption change the stored caption is replaced by that text in Unicode. */
static int g_text_off;                  /* offset of FText in VCL controls (found on the first window created) */

static void *vcl_obj(HWND h)
{
    WORD atom = *(WORD *)0x4e5756;      /* Controls.ControlAtom */
    return h ? (void *)GetPropA(h, (LPCSTR)(UINT_PTR)atom) : NULL;
}

/* the name passed to CreateWindowEx is the control's own FText pointer: find where the object keeps it */
static void caption_learn(HWND h, LPCSTR name)
{
    unsigned char *o;
    int off;
    if (g_text_off || !name || (UINT_PTR)name <= 0xffff || !(o = (unsigned char *)vcl_obj(h))) return;
    for (off = 0x10; off < 0x200; off += 4)
        if (!IsBadReadPtr(o + off, 4) && *(LPCSTR *)(o + off) == name) { g_text_off = off; break; }
}

static void caption_fix(HWND h)
{
    unsigned char *o = (unsigned char *)vcl_obj(h);
    LPCSTR t;
    WCHAR want[512], have[512];
    if (!g_cp || !g_text_off || !o || IsBadReadPtr(o + g_text_off, 4)) return;
    t = *(LPCSTR *)(o + g_text_off);
    if (!has_high(t) || IsBadStringPtrA(t, 512)) return;
    a2w_cp(t, want, 512);
    have[0] = 0;
    InternalGetWindowText(h, have, 512);
    if (lstrcmpW(want, have)) DefWindowProcW(h, WM_SETTEXT, 0, (LPARAM)want);
}

static void CALLBACK caption_event(HWINEVENTHOOK hk, DWORD ev, HWND h, LONG obj, LONG child, DWORD th, DWORD t)
{
    (void)hk; (void)th; (void)t;
    if ((ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_NAMECHANGE) && obj == OBJID_WINDOW && child == CHILDID_SELF && h &&
        !(GetWindowLongA(h, GWL_STYLE) & WS_CHILD))
        caption_fix(h);
}

static void locale_watch_captions(void)          /* called once the message loop runs */
{
    static int done;
    if (done) return;
    done = 1;
    if (g_cp)
        SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_NAMECHANGE, NULL, caption_event, GetCurrentProcessId(), 0,
                        WINEVENT_OUTOFCONTEXT);
}

/* ---------- reading captions back: the kernel converts the stored (Unicode) caption with the system code page, which
   turns Cyrillic into "?" - and the program writes that back when it recreates a window ---------- */
typedef int (WINAPI *pGetWindowTextA)(HWND, LPSTR, int);
static pGetWindowTextA o_GetWindowTextA;
static int WINAPI h_GetWindowTextA(HWND h, LPSTR s, int cap)
{
    WCHAR buf[512];
    int n;
    if (!s || cap <= 0) return o_GetWindowTextA(h, s, cap);
    n = GetWindowTextW(h, buf, 512);
    if (n <= 0) { s[0] = 0; return 0; }
    n = WideCharToMultiByte(g_cp, 0, buf, -1, s, cap, NULL, NULL);
    s[cap - 1] = 0;
    return n > 0 ? lstrlenA(s) : 0;
}

/* WM_GETTEXT handled by the default procedure: give the caption in the program's code page */
static LRESULT dwp_gettext(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT (WINAPI *def)(HWND, UINT, WPARAM, LPARAM))
{
    WCHAR buf[512];
    char tmp[1024];
    int n;
    (void)msg;
    n = (int)def(h, WM_GETTEXT, 512, (LPARAM)buf);
    if (n < 0) n = 0;
    buf[n < 511 ? n : 511] = 0;
    n = WideCharToMultiByte(g_cp, 0, buf, -1, tmp, sizeof tmp, NULL, NULL);
    if (n <= 0) tmp[0] = 0;
    if (msg == WM_GETTEXTLENGTH) return lstrlenA(tmp);
    if (!lp || !wp) return 0;
    lstrcpynA((LPSTR)lp, tmp, (int)wp);
    return lstrlenA((LPSTR)lp);
}

static void locale_hook_gettext(HMODULE exe)
{
    if (g_cp) o_GetWindowTextA = (pGetWindowTextA)iat_hook(exe, "user32.dll", "GetWindowTextA", (void *)h_GetWindowTextA);
}
