/* feat4.c - select all, duplicate, "new node" hotkey, wires that follow moved elements, autosave, remembered view.
   Included from feat.c after feat3.c. */

#define F_TOOL_SET      0x474c1c        /* (toolButton, down) */
#define MF_TOOL_NODE    0x3a0           /* "A" tool: create a named node */
#define F_CELL_SELECT   0x4cf384        /* (element, flag, x, y) */
#define F_WIRE_SEL      0x4d0a28        /* (wire, flag, part, y, x) */
#define F_WIRE_SETGEOM  0x4d0c98        /* (wire, x1, y1, [stack: x2, y2]) absolute coordinates */
#define F_WIRE_NEW      0x4d3570        /* (0, classRef) */
#define A_WIRE_CLASS    0x4ce364        /* the class reference itself is stored here */

struct FC { void *cv; HWND h; };
static BOOL CALLBACK enum_cv(HWND h, LPARAM l)
{
    struct FC *p = (struct FC *)l;
    if (is_class(h, "TFrpaint") && obj_from_hwnd(h) == p->cv) { p->h = h; return FALSE; }
    return TRUE;
}
static HWND canvas_hwnd_of(void *cv)
{
    struct FC f;
    f.cv = cv; f.h = NULL;
    if (g_main) EnumChildWindows(g_main, enum_cv, (LPARAM)&f);
    return f.h;
}

static int edit_ready(unsigned *pf, unsigned *doc, unsigned *scheme)
{
    *pf = MAINFORM();
    *doc = *pf ? (unsigned)F32(*pf, 0x314) : 0;
    *scheme = *doc ? (unsigned)F32(*doc, 0x34) : 0;
    if (!*scheme || !doc_edit_mode(*doc)) return 0;
    if (F32(*scheme, 0) && U8(F32(*scheme, 0), 0x18)) return 0;      /* a wire is being drawn */
    return 1;
}

/* ---------- Ctrl+A ---------- */
static void do_select_all(void)
{
    unsigned pf, doc, scheme, el, wl;
    int i, n;
    if (!edit_ready(&pf, &doc, &scheme)) return;
    el = (unsigned)F32(scheme, 0x10); wl = (unsigned)F32(scheme, 0x18);
    dcall((void *)F_DESELECT_ALL, scheme, 0, 0, 0, 0, 0);
    n = F32(el, 0);
    for (i = 1; i <= n; i++) dcall((void *)F_CELL_SELECT, LI(el, i), 1, 0, 1, 0, 0);
    F32(el, 0x34) = n;
    n = F32(wl, 0);
    for (i = 1; i <= n; i++) dcall((void *)F_WIRE_SEL, LI(wl, i), 1, 0, 2, 0, 0);
    F32(wl, 0x30) = n;
    dcall((void *)F_REFRESH, (unsigned)F32(pf, 0x308), 0, 0, 0, 0, 0);
}

/* ---------- Ctrl+D: copy of the selection, one cell down-right, and it becomes the selection ---------- */
static void do_duplicate(void)
{
    unsigned pf, doc, scheme, el, wl;
    int i, n, x0 = 0, y0 = 0, have = 0;
    if (!edit_ready(&pf, &doc, &scheme)) return;
    el = (unsigned)F32(scheme, 0x10); wl = (unsigned)F32(scheme, 0x18);
    for (i = 1, n = F32(el, 0); i <= n; i++) {
        unsigned e = LI(el, i);
        if (!e || !U8(e, 0x50)) continue;
        if (!have || F32(e, 0x44) < x0) x0 = F32(e, 0x44);
        if (!have || F32(e, 0x48) < y0) y0 = F32(e, 0x48);
        have = 1;
    }
    for (i = 1, n = F32(wl, 0); i <= n; i++) {
        unsigned w = LI(wl, i);
        if (!w || !U8(w, 0x18)) continue;
        if (!have || F32(w, 0x24) < x0) x0 = F32(w, 0x24);
        if (!have || F32(w, 0x28) < y0) y0 = F32(w, 0x28);
        have = 1;
    }
    if (!have) return;
    hist_note(doc);
    dcall((void *)F_COPY, scheme, 0, 0, 0, 0, 0);
    dcall((void *)F_PASTE, scheme, (unsigned)(x0 + 2), (unsigned)(y0 + 2), 0, 0, 0);
    dcall((void *)F_REFRESH, (unsigned)F32(pf, 0x308), 0, 0, 0, 0, 0);
    hist_note(doc);
}

/* ---------- V: new node at the pointer (same as the "A" tool + a click there) ---------- */
static void do_new_node(void)
{
    unsigned pf, doc, scheme, mf, btn;
    void *cv;
    HWND cvw;
    POINT pt;
    if (!edit_ready(&pf, &doc, &scheme)) return;
    mf = main_form();
    btn = mf ? (unsigned)F32(mf, MF_TOOL_NODE) : 0;
    cv = (void *)F32(pf, 0x308);
    cvw = cv ? canvas_hwnd_of(cv) : NULL;
    if (!btn || !cvw) return;
    GetCursorPos(&pt);
    ScreenToClient(cvw, &pt);
    {
        RECT rc;
        GetClientRect(cvw, &rc);
        if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom) { pt.x = rc.right / 2; pt.y = rc.bottom / 2; }
    }
    hist_note(doc);
    dcall((void *)F_TOOL_SET, btn, 1, 0, 0, 0, 0);
    PostMessageA(cvw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
    PostMessageA(cvw, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    if (g_main) PostMessageA(g_main, WM_APP + 0x4A4, 0, 0);      /* back to the selection tool (also after Cancel) */
}

/* nothing under the pointer: no element body, no line, no node */
static int empty_spot(HWND cvw, void *cv)
{
    unsigned pf, doc, scheme, el;
    POINT pt;
    VIEW v;
    int i, n, mx, my;
    if (!edit_ready(&pf, &doc, &scheme) || (unsigned)F32(pf, 0x308) != (unsigned)cv || !tool_down(MF_TOOL_SELECT)) return 0;
    GetCursorPos(&pt);
    ScreenToClient(cvw, &pt);
    view_of(cv, &v);
    if (find_wire(scheme, &v, pt.x, pt.y, g_cfg_hit)) return 0;
    mx = (int)dcall((void *)F_VIEW2MODEL_X, (unsigned)cv, (unsigned)pt.x, 0, 0, 0, 0);
    my = (int)dcall((void *)F_VIEW2MODEL_Y, (unsigned)cv, (unsigned)pt.y, 0, 0, 0, 0);
    el = (unsigned)F32(scheme, 0x10);
    for (i = 1, n = F32(el, 0); i <= n; i++) {
        unsigned e = LI(el, i), def = e ? (unsigned)F32(e, 8) : 0;
        int x0, y0, w, h;
        if (!e) continue;
        x0 = F32(e, 0x44); y0 = F32(e, 0x48);
        w = def ? F32(def, 0x21c) : 0; h = def ? F32(def, 0x220) : 0;
        if (mx >= x0 - 2 && mx <= x0 + w + 2 && my >= y0 - 1 && my <= y0 + h + 1) return 0;   /* with its pins */
    }
    return 1;
}

/* ---------- lines follow moved elements: _afdk_remastered_wires.dll next to this DLL (optional) ---------- */
typedef void (*WIRES_CAPTURE)(unsigned);
typedef void (*WIRES_APPLY)(void);
static WIRES_CAPTURE p_wires_capture;
static WIRES_APPLY p_wires_apply;
static HINSTANCE g_self;

static int wires_ready(void)
{
    static int tried;
    if (!tried) {
        char p[MAX_PATH], *s;
        HMODULE m;
        tried = 1;
        if (!g_cfg_wire_follow || !GetModuleFileNameA(g_self, p, MAX_PATH) || !(s = strrchr(p, '\\'))) return 0;
        lstrcpyA(s + 1, "_afdk_remastered_wires.dll");
        if ((m = LoadLibraryA(p)) != NULL) {
            p_wires_capture = (WIRES_CAPTURE)GetProcAddress(m, "WiresCapture");
            p_wires_apply = (WIRES_APPLY)GetProcAddress(m, "WiresApply");
        }
    }
    return p_wires_capture && p_wires_apply;
}
static void mv_capture(unsigned scheme) { if (wires_ready()) p_wires_capture(scheme); }
static void mv_apply(void) { if (wires_ready()) p_wires_apply(); }

/* ---------- MCP channel: _afdk_remastered_mcp.dll next to this DLL (optional), started on the UI thread ---------- */
static AFDK_API g_api;

static void mcp_load(void)
{
    static int tried;
    char p[MAX_PATH], *s;
    HMODULE m;
    MCP_START start;
    if (tried || !g_main) return;
    tried = 1;
    if (!GetModuleFileNameA(g_self, p, MAX_PATH) || !(s = strrchr(p, '\\'))) return;
    lstrcpyA(s + 1, "_afdk_remastered_mcp.dll");
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES || !(m = LoadLibraryA(p))) return;
    if (!(start = (MCP_START)GetProcAddress(m, "McpStart"))) return;
    g_api.version = AFDK_MCP_API_VERSION;
    g_api.main_wnd = g_main;
    g_api.active_pf = active_pf;
    g_api.doc_edit_mode = doc_edit_mode;
    g_api.hist_note = hist_note;
    g_api.hist_step = hist_step;
    g_api.select_all = do_select_all;
    g_api.duplicate = do_duplicate;
    g_api.nudge_selection = nudge_selection;
    g_api.canvas_hwnd_of = canvas_hwnd_of;
    g_api.set_scroll = set_scroll;
    g_api.main_form = main_form;
    g_api.set_dialog_file = set_dialog_file;
    start(&g_api);
}

/* ---------- periodic work: autosave, remembered zoom/scroll, notes for edits made in dialogs ---------- */
#define VIEWKEY "Software\\DCA Laboratory\\AFDKfix"
static unsigned g_view_seen[64];
static int g_view_last[64][3];
static int g_autosave_ticks;

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

static int is_project_file(const char *p)
{
    int n = (int)strlen(p);
    return n > 4 && lstrcmpiA(p + n - 4, ".afp") == 0 && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static void view_tick(unsigned pf)
{
    unsigned doc = (unsigned)F32(pf, 0x314), cv = (unsigned)F32(pf, 0x308);
    char path[MAX_PATH], val[64];
    int k, slot = -1, cur[3];
    HWND cvw;
    HKEY key;
    DWORD sz, type;
    if (!doc || !cv || !doc_path(doc, path, sizeof path) || !is_project_file(path)) return;
    cvw = canvas_hwnd_of((void *)cv);
    if (!cvw) return;
    for (k = 0; k < 64; k++) if (g_view_seen[k] == doc) slot = k;
    if (slot < 0) {                                                   /* first time we see this document: restore */
        for (k = 0; k < 64 && g_view_seen[k]; k++) ;
        if (k == 64) k = 0;
        slot = k; g_view_seen[k] = doc;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, VIEWKEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            int lv = 0, hp = 0, vp = 0;
            sz = sizeof val - 1;
            if (RegQueryValueExA(key, path, NULL, &type, (BYTE *)val, &sz) == ERROR_SUCCESS && type == REG_SZ) {
                val[sz] = 0;
                { char *q = val; lv = (int)strtol(q, &q, 10); if (*q == ',') hp = (int)strtol(q + 1, &q, 10); if (*q == ',') vp = (int)strtol(q + 1, &q, 10); }
                if (lv >= 5 && lv <= 40) {
                    if (U8((void *)cv, CV_LEVEL) != lv) dcall((void *)F_SET_ZOOM, cv, (unsigned)lv, 0, 0, 0, 0);
                    set_scroll(cvw, (void *)cv, hp, vp);
                }
            }
            RegCloseKey(key);
        }
        g_view_last[slot][0] = -1;
        return;
    }
    cur[0] = U8((void *)cv, CV_LEVEL); cur[1] = F32(F32(cv, CV_HBAR), BAR_POS); cur[2] = F32(F32(cv, CV_VBAR), BAR_POS);
    if (memcmp(cur, g_view_last[slot], sizeof cur) == 0) return;
    memcpy(g_view_last[slot], cur, sizeof cur);
    wsprintfA(val, "%d,%d,%d", cur[0], cur[1], cur[2]);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, VIEWKEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(key, path, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
        RegCloseKey(key);
    }
}

static void autosave_tick(unsigned pf)
{
    unsigned doc = (unsigned)F32(pf, 0x314);
    char path[MAX_PATH], bak[MAX_PATH + 8];
    static char backed[8][MAX_PATH];
    int k;
    if (!doc || !U8(doc, 0x18) || !doc_edit_mode(doc) || !doc_path(doc, path, sizeof path) || !is_project_file(path)) return;
    for (k = 0; k < 8 && backed[k][0] && lstrcmpiA(backed[k], path); k++) ;
    if (k < 8 && !backed[k][0]) {                                    /* keep the pre-session version once */
        lstrcpyA(backed[k], path);
        lstrcpyA(bak, path); lstrcatA(bak, ".bak");
        CopyFileA(path, bak, FALSE);
    }
    fx_log("autosave %s" NL, path);
    PostMessageA(g_main, WM_COMMAND, 4, 0);                           /* File > Save */
}

static VOID CALLBACK tick_proc(HWND h, UINT msg, UINT_PTR id, DWORD t)
{
    unsigned pf, doc, scheme;
    static int dialog_seen;
    (void)h; (void)msg; (void)id; (void)t;
    mcp_load();
    if (!main_active()) {                                            /* one of our dialogs is up */
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) dialog_seen = 1;
        return;
    }
    if (g_busy || g_ghost || g_tip_wire) return;
    tree_font_tick();
    if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || GetAsyncKeyState(VK_MBUTTON) < 0) return;
    pf = MAINFORM();
    if (!pf) return;
    doc = (unsigned)F32(pf, 0x314);
    scheme = doc ? (unsigned)F32(doc, 0x34) : 0;
    if (!scheme || (F32(scheme, 0) && U8(F32(scheme, 0), 0x18))) return;
    if (dialog_seen) { dialog_seen = 0; hist_note_full(doc); }     /* edits made in a dialog */
    else hist_note(doc);
    view_tick(pf);
    if (g_cfg_autosave && ++g_autosave_ticks >= g_cfg_autosave) { g_autosave_ticks = 0; autosave_tick(pf); }
}
