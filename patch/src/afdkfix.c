/* _afdk_remastered.dll (afdkfix.c) - runtime fixes for AFDK 2.0 (loaded through an import added to AFDK.exe) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdlib.h>
#include <intrin.h>
#define NL "\015\012"

/* ---------- logging (only if AFDKFIX_LOG env var is set) ---------- */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static void fx_log(const char *fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap); if (n < 0 || n > 1000) n = 0;
    va_end(ap);
    DWORD w;
    WriteFile(g_log, buf, n, &w, NULL);
}

/* ---------- IAT hooking (Borland imports have no OriginalFirstThunk, so match by resolved address) ---------- */
static void *iat_hook(HMODULE mod, const char *dll, const char *fn, void *repl)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress);
    HMODULE dm = GetModuleHandleA(dll);
    void *target = dm ? (void *)GetProcAddress(dm, fn) : NULL;
    if (!target) return NULL;
    int found = 0;
    for (; d->Name; d++) {
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; iat->u1.Function; iat++) {
            if ((void *)iat->u1.Function == target) {
                DWORD old;
                VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
                iat->u1.Function = (DWORD)repl;
                VirtualProtect(&iat->u1.Function, 4, old, &old);
                found = 1;
            }
        }
    }
    return found ? target : NULL;
}

/* ---------- registry redirection: HKLM\Software\DCA Laboratory -> HKCU ---------- */
typedef LONG (WINAPI *pRegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *pRegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
typedef LONG (WINAPI *pRegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *pRegDeleteKeyA)(HKEY, LPCSTR);

static pRegOpenKeyExA   o_RegOpenKeyExA;
static pRegCreateKeyExA o_RegCreateKeyExA;
static pRegQueryValueExA o_RegQueryValueExA;
static pRegDeleteKeyA   o_RegDeleteKeyA;

static char g_root[MAX_PATH];

static int is_dca(HKEY k, LPCSTR sub)
{
    return k == HKEY_LOCAL_MACHINE && sub && _strnicmp(sub, "Software\\DCA Laboratory", 23) == 0;
}

static LONG WINAPI h_RegOpenKeyExA(HKEY k, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    fx_log("RegOpen %p %s\r\n", k, sub ? sub : "(null)");
    if (is_dca(k, sub)) {
        LONG r = o_RegOpenKeyExA(HKEY_CURRENT_USER, sub, opt, sam & ~(REGSAM)(KEY_WOW64_64KEY), out);
        if (r == ERROR_SUCCESS) return r;
        /* fall back to a read-only look at the machine-wide key (legacy installs) */
        return o_RegOpenKeyExA(k, sub, opt, KEY_READ, out);
    }
    return o_RegOpenKeyExA(k, sub, opt, sam, out);
}

static LONG WINAPI h_RegCreateKeyExA(HKEY k, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
                                     LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
    fx_log("RegCreate %08x %s" NL, (unsigned)(UINT_PTR)k, sub ? sub : "(null)");
    if (is_dca(k, sub))
        return o_RegCreateKeyExA(HKEY_CURRENT_USER, sub, res, cls, opt, KEY_ALL_ACCESS, sa, out, disp);
    return o_RegCreateKeyExA(k, sub, res, cls, opt, sam, sa, out, disp);
}

static LONG WINAPI h_RegDeleteKeyA(HKEY k, LPCSTR sub)
{
    if (is_dca(k, sub)) return o_RegDeleteKeyA(HKEY_CURRENT_USER, sub);
    return o_RegDeleteKeyA(k, sub);
}

/* RootDir always follows the folder the exe lives in */
static LONG WINAPI h_RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    fx_log("RegQuery %p %s\r\n", k, name ? name : "(null)");
    if (name && lstrcmpiA(name, "RootDir") == 0) {
        DWORD need = (DWORD)lstrlenA(g_root) + 1;
        if (type) *type = REG_SZ;
        if (data) {
            if (!cb || *cb < need) { if (cb) *cb = need; return ERROR_MORE_DATA; }
            memcpy(data, g_root, need);
        }
        if (cb) *cb = need;
        return ERROR_SUCCESS;
    }
    return o_RegQueryValueExA(k, name, rsv, type, data, cb);
}

/* first run: make sure a library list exists, otherwise projects cannot be loaded */
static void seed_registry(void)
{
    HKEY k;
    DWORD nvals = 0;
    if (o_RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\DCA Laboratory\\AFDK 2.0\\Library", 0, NULL, 0,
                          KEY_ALL_ACCESS, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, NULL, NULL, &nvals, NULL, NULL, NULL, NULL);
    if (nvals == 0)
        RegSetValueExA(k, "Libi1", 0, REG_SZ, (const BYTE *)"Standard", 9);
    RegCloseKey(k);
}


/* ---------- file dialogs: ANSI dialogs lose characters outside the system code page ("?" in names -> I/O error 123).
   Run the Unicode dialog instead and convert the result so that it is always a valid ANSI path. ---------- */
typedef BOOL (WINAPI *pOFNA)(LPOPENFILENAMEA);
typedef BOOL (WINAPI *pOFNW)(LPOPENFILENAMEW);
static pOFNA o_GetOpenFileNameA, o_GetSaveFileNameA;

static UINT_PTR CALLBACK ofn_hook(HWND h, UINT m, WPARAM w, LPARAM l) { return 0; }  /* keeps the classic dialog layout */

static WCHAR *a2w(const char *s, int n)   /* n = length incl. terminator(s), or -1 */
{
    if (!s) return NULL;
    int len = MultiByteToWideChar(CP_ACP, 0, s, n, NULL, 0);
    WCHAR *w = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (len + 2) * sizeof(WCHAR));
    MultiByteToWideChar(CP_ACP, 0, s, n, w, len);
    return w;
}

static int multisz_len(const char *s)
{
    int i = 0;
    if (!s) return 0;
    while (s[i] || s[i + 1]) i++;
    return i + 2;
}

/* one path component (or a whole directory) -> ANSI text that can always be used as a file name.
   Characters outside the system code page are replaced by short (8.3) names when the object exists, else by '_' */
static int conv_piece(const WCHAR *dir, const WCHAR *name, int is_dir, char *out, int outsz)
{
    BOOL lossy = FALSE;
    char def = '_';
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, name, -1, out, outsz, &def, &lossy);
    if (n == 0) { out[0] = 0; return 0; }
    if (lossy) {
        WCHAR full[MAX_PATH * 2], sh[MAX_PATH];
        if (dir && *dir) { lstrcpynW(full, dir, MAX_PATH); lstrcatW(full, L"\\"); lstrcatW(full, name); }
        else lstrcpynW(full, name, MAX_PATH * 2);
        if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
            DWORD r = GetShortPathNameW(full, sh, MAX_PATH);
            if (r && r < MAX_PATH) {
                const WCHAR *src = sh;
                if (!is_dir) { const WCHAR *sl = wcsrchr(sh, L'\\'); if (sl) src = sl + 1; }
                BOOL l2 = FALSE;
                char tmp[MAX_PATH];
                if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, src, -1, tmp, MAX_PATH, &def, &l2) && !l2) {
                    lstrcpynA(out, tmp, outsz);
                    return 1;
                }
            }
        }
    }
    return 1;
}

static WCHAR g_dialog_file[MAX_PATH];                     /* see set_dialog_file (MCP) */
static void set_dialog_file(const WCHAR *p) { lstrcpynW(g_dialog_file, p ? p : L"", MAX_PATH); }

static BOOL file_dialog(LPOPENFILENAMEA a, int save, pOFNA orig)
{
    fx_log("file_dialog save=%d struct=%u flags=%x" NL, save, (unsigned)a->lStructSize, (unsigned)a->Flags);
    if (!a || a->lStructSize < sizeof(OPENFILENAMEA) - 12 || !a->lpstrFile)
        return orig(a);
    HMODULE cd = GetModuleHandleA("comdlg32.dll");
    pOFNW fn = cd ? (pOFNW)GetProcAddress(cd, save ? "GetSaveFileNameW" : "GetOpenFileNameW") : NULL;
    if (!fn) return orig(a);

    DWORD nmax = a->nMaxFile < 1 ? MAX_PATH : a->nMaxFile;
    WCHAR *wfile = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (nmax + 2) * sizeof(WCHAR));
    WCHAR *ini = a2w(a->lpstrFile, -1);
    lstrcpynW(wfile, ini, nmax);

    OPENFILENAMEW w;
    memset(&w, 0, sizeof w);
    w.lStructSize = sizeof w;
    w.hwndOwner = a->hwndOwner;
    w.hInstance = a->hInstance;
    WCHAR *filt = a->lpstrFilter ? a2w(a->lpstrFilter, multisz_len(a->lpstrFilter)) : NULL;
    w.lpstrFilter = filt;
    w.nFilterIndex = a->nFilterIndex;
    w.lpstrFile = wfile;
    w.nMaxFile = nmax;
    WCHAR *idir = a2w(a->lpstrInitialDir, -1), *ttl = a2w(a->lpstrTitle, -1), *dext = a2w(a->lpstrDefExt, -1);
    w.lpstrInitialDir = idir;
    w.lpstrTitle = ttl;
    w.lpstrDefExt = dext;
    w.Flags = (a->Flags & ~(OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) | OFN_ENABLEHOOK | OFN_EXPLORER;
    w.lpfnHook = ofn_hook;

    BOOL ok;
    if (g_dialog_file[0]) {                               /* a path given through the MCP channel: no dialog */
        memset(wfile, 0, (nmax + 2) * sizeof(WCHAR));
        lstrcpynW(wfile, g_dialog_file, nmax);
        g_dialog_file[0] = 0;
        ok = TRUE;
    } else
        ok = fn(&w);
    if (ok) {
        char *out = a->lpstrFile;
        const WCHAR *s0 = wfile;
        const WCHAR *s1 = s0 + lstrlenW(s0) + 1;
        DWORD pos = 0;
        char piece[MAX_PATH * 2];
        out[0] = 0;
        if (*s1 == 0) {                                   /* single file: full path */
            WCHAR dir[MAX_PATH * 2];
            lstrcpynW(dir, s0, MAX_PATH * 2);
            WCHAR *sl = wcsrchr(dir, L'\\');
            const WCHAR *nm = s0;
            if (sl) { nm = s0 + (sl - dir) + 1; *sl = 0; }
            if (sl) {
                conv_piece(NULL, dir, 1, piece, sizeof piece);
                lstrcpynA(out, piece, (int)nmax);
                lstrcatA(out, "\\");
                a->nFileOffset = (WORD)lstrlenA(out);
            } else a->nFileOffset = 0;
            conv_piece(sl ? dir : NULL, nm, 0, piece, sizeof piece);
            if (lstrlenA(out) + lstrlenA(piece) + 1 < (int)nmax) lstrcatA(out, piece);
            char *dot = strrchr(out + a->nFileOffset, '.');
            a->nFileExtension = dot ? (WORD)(dot - out + 1) : 0;
        } else {                                          /* several files: dir NUL name NUL name NUL NUL */
            conv_piece(NULL, s0, 1, piece, sizeof piece);
            lstrcpynA(out, piece, (int)nmax);
            pos = (DWORD)lstrlenA(out) + 1;
            a->nFileOffset = (WORD)pos;
            for (const WCHAR *p = s1; *p; p += lstrlenW(p) + 1) {
                conv_piece(s0, p, 0, piece, sizeof piece);
                DWORD l = (DWORD)lstrlenA(piece) + 1;
                if (pos + l + 1 >= nmax) break;
                memcpy(out + pos, piece, l);
                pos += l;
            }
            out[pos] = 0;
            a->nFileExtension = 0;
        }
        a->nFilterIndex = w.nFilterIndex;
    }
    HeapFree(GetProcessHeap(), 0, wfile);
    HeapFree(GetProcessHeap(), 0, ini);
    if (filt) HeapFree(GetProcessHeap(), 0, filt);
    if (idir) HeapFree(GetProcessHeap(), 0, idir);
    if (ttl) HeapFree(GetProcessHeap(), 0, ttl);
    if (dext) HeapFree(GetProcessHeap(), 0, dext);
    return ok;
}
static BOOL WINAPI h_GetOpenFileNameA(LPOPENFILENAMEA a) { return file_dialog(a, 0, o_GetOpenFileNameA); }
static BOOL WINAPI h_GetSaveFileNameA(LPOPENFILENAMEA a) { return file_dialog(a, 1, o_GetSaveFileNameA); }

/* ---------- diagnostics ---------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static pCreateFileA o_CreateFileA;
static HANDLE WINAPI h_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    HANDLE r = o_CreateFileA(p, a, s, sa, d, f, t);
    fx_log("CreateFile %s acc=%x disp=%d -> %s\r\n", p, a, d, r == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return r;
}

typedef HANDLE (WINAPI *pFindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static pFindFirstFileA o_FindFirstFileA;
static HANDLE WINAPI h_FindFirstFileA(LPCSTR p, LPWIN32_FIND_DATAA f)
{
    HANDLE r = o_FindFirstFileA(p, f);
    fx_log("FindFirst %s -> %s" NL, p, r == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return r;
}
typedef DWORD (WINAPI *pGetFileAttributesA)(LPCSTR);
static pGetFileAttributesA o_GetFileAttributesA;
static DWORD WINAPI h_GetFileAttributesA(LPCSTR p)
{
    DWORD r = o_GetFileAttributesA(p);
    fx_log("GetAttr %s -> %x" NL, p, r);
    return r;
}

static LONG CALLBACK veh_log(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == 0xC0000005 || er->ExceptionCode == 0xC0000094 || er->ExceptionCode == 0xC00000FD || er->ExceptionCode == 0x0EEDFADE)
        fx_log("EXCEPTION %08x at %p (info %p %p) eax=%p ebx=%p ecx=%p edx=%p esi=%p edi=%p ebp=%p esp=%p" NL,
               (unsigned)er->ExceptionCode, er->ExceptionAddress, (void *)er->ExceptionInformation[0],
               (void *)er->ExceptionInformation[1], (void *)ep->ContextRecord->Eax, (void *)ep->ContextRecord->Ebx,
               (void *)ep->ContextRecord->Ecx, (void *)ep->ContextRecord->Edx, (void *)ep->ContextRecord->Esi,
               (void *)ep->ContextRecord->Edi, (void *)ep->ContextRecord->Ebp, (void *)ep->ContextRecord->Esp);
    if (er->ExceptionCode == 0xC0000005 && (unsigned)er->ExceptionAddress == 0x4d6fb9) {
        unsigned *bp = (unsigned *)ep->ContextRecord->Ebp;
        unsigned o = bp[-1];
        if (o && !IsBadReadPtr((void *)o, 0x380)) {
            char nm[64]; unsigned n = *(unsigned char *)(o + 0x38); if (n > 40) n = 40;
            memcpy(nm, (char *)(o + 0x39), n); nm[n] = 0;
            fx_log("  OBJ %08x name='%s' kind=%u k250=%u p254=%08x p25c=%08x parent=%08x cnt=%d ofs368=%d ofs370=%d" NL, o, nm,
                   *(unsigned char *)(o + 0x248), *(unsigned char *)(o + 0x250), *(unsigned *)(o + 0x254),
                   *(unsigned *)(o + 0x25c), *(unsigned *)(o + 0x28), *(int *)o, *(int *)(o + 0x368), *(int *)(o + 0x370));
        }
    }
    if (er->ExceptionCode == 0xC0000005 || er->ExceptionCode == 0x0EEDFADE) {          /* walk the frame-pointer chain (Delphi code keeps EBP frames) */
        unsigned *bp = (unsigned *)ep->ContextRecord->Ebp;
        int i;
        for (i = 0; i < 12 && bp && !IsBadReadPtr(bp, 8); i++) {
            fx_log("  ret %08x" NL, bp[1]);
            bp = (unsigned *)bp[0];
        }
        fx_log("  esp dump:");
        for (i = 0; i < 24; i++) fx_log(" %08x", ((unsigned *)ep->ContextRecord->Esp)[i]);
        fx_log(NL);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------- error log: _afdk_remastered.log next to the exe (always on, kept under 1 MB) ----------
   Every error the program shows (a Delphi exception) is written there with its call chain, so a problem a user hits can
   be traced afterwards.  The registry exceptions the program raises and handles itself at start-up are skipped. */
static char g_err_path[MAX_PATH];

static void err_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    DWORD w;
    HANDLE f;
    if (!g_err_path[0]) return;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap); if (n < 0 || n > 1000) n = 0;
    va_end(ap);
    f = CreateFileA(g_err_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if (GetFileSize(f, NULL) > (1 << 20)) {                     /* too big: start over */
        CloseHandle(f);
        f = CreateFileA(g_err_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return;
    }
    WriteFile(f, buf, n, &w, NULL);
    CloseHandle(f);
}

static LONG CALLBACK veh_errors(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    BYTE *obj, *vmt;
    const char *msg = "", *cls;
    char name[64];
    unsigned *bp;
    SYSTEMTIME t;
    int i;
    if (er->ExceptionCode != 0x0EEDFADE || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    if ((unsigned)er->ExceptionInformation[0] == 0x479ad0) return EXCEPTION_CONTINUE_SEARCH;   /* registry, start-up */
    obj = (BYTE *)er->ExceptionInformation[1];
    lstrcpyA(name, "?");
    if (obj && !IsBadReadPtr(obj, 8)) {
        vmt = *(BYTE **)obj;
        if (vmt && !IsBadReadPtr(vmt - 44, 4) && (cls = *(const char **)(vmt - 44)) && !IsBadReadPtr(cls, 1)) {
            int n = (BYTE)cls[0] < 63 ? (BYTE)cls[0] : 63;
            memcpy(name, cls + 1, n); name[n] = 0;
        }
        if (*(const char **)(obj + 4) && !IsBadReadPtr(*(const char **)(obj + 4), 1)) msg = *(const char **)(obj + 4);
    }
    GetLocalTime(&t);
    err_log("%04d-%02d-%02d %02d:%02d:%02d %s: %.300s" NL "  at %08x, calls:", t.wYear, t.wMonth, t.wDay, t.wHour,
            t.wMinute, t.wSecond, name, msg, (unsigned)er->ExceptionInformation[0]);
    bp = (unsigned *)ep->ContextRecord->Ebp;
    for (i = 0; i < 16 && bp && !IsBadReadPtr(bp, 8); i++) {
        err_log(" %08x", bp[1]);
        bp = (unsigned *)bp[0];
    }
    err_log(NL);
    return EXCEPTION_CONTINUE_SEARCH;
}

#include "mcp_api.h"
#include "locale.c"
#include "feat.c"

/* ---------- entry ---------- */
__declspec(dllexport) void Init(void) {}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID rsv)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        cfg_load();
        locale_override((UINT)g_cfg_codepage);       /* Cyrillic texts under any Windows code page; must come first */
        HMODULE exe = GetModuleHandleA(NULL);
        locale_hook_menus(exe);
        locale_hook_captions(exe);
        locale_hook_gettext(exe);
        GetModuleFileNameA(exe, g_root, MAX_PATH);
        char *s = strrchr(g_root, '\\');
        if (s) *s = 0;
        lstrcpyA(g_err_path, g_root);
        lstrcatA(g_err_path, "\\_afdk_remastered.log");
        AddVectoredExceptionHandler(0, veh_errors);

        char lp[MAX_PATH];
        if (GetEnvironmentVariableA("AFDKFIX_LOG", lp, MAX_PATH)) {
            g_log = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
            AddVectoredExceptionHandler(1, veh_log);
            o_CreateFileA = iat_hook(exe, "kernel32.dll", "CreateFileA", h_CreateFileA);
            o_FindFirstFileA = iat_hook(exe, "kernel32.dll", "FindFirstFileA", h_FindFirstFileA);
            o_GetFileAttributesA = iat_hook(exe, "kernel32.dll", "GetFileAttributesA", h_GetFileAttributesA);
        }

        if (GetEnvironmentVariableA("AFDKFIX_LCID", lp, MAX_PATH))
            SetThreadLocale((LCID)strtoul(lp, NULL, 0));

        o_RegOpenKeyExA    = iat_hook(exe, "advapi32.dll", "RegOpenKeyExA",    h_RegOpenKeyExA);
        o_RegCreateKeyExA  = iat_hook(exe, "advapi32.dll", "RegCreateKeyExA",  h_RegCreateKeyExA);
        o_RegQueryValueExA = iat_hook(exe, "advapi32.dll", "RegQueryValueExA", h_RegQueryValueExA);
        o_RegDeleteKeyA    = iat_hook(exe, "advapi32.dll", "RegDeleteKeyA",    h_RegDeleteKeyA);
        o_GetOpenFileNameA = iat_hook(exe, "comdlg32.dll", "GetOpenFileNameA", h_GetOpenFileNameA);
        o_GetSaveFileNameA = iat_hook(exe, "comdlg32.dll", "GetSaveFileNameA", h_GetSaveFileNameA);
        seed_registry();
        install_features(h);
        fx_log("locale: cp=%u menu=%d dwp=%d mdi=%d frame=%d" NL, g_cp, o_InsertMenuItemA != 0, o_DefWindowProcA != 0, o_DefMDIChildProcA != 0, o_DefFrameProcA != 0);
        fx_log("hooks: open=%d create=%d query=%d delete=%d\r\n", o_RegOpenKeyExA != 0, o_RegCreateKeyExA != 0, o_RegQueryValueExA != 0, o_RegDeleteKeyA != 0);
    }
    else if (reason == DLL_PROCESS_DETACH) hist_cleanup();
    return TRUE;
}
