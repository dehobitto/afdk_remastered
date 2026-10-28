/* feat2.c - undo/redo (project snapshots) and clipboard helpers. Included from feat.c. */

#define F_SAVE_STREAM   0x4893f4        /* (doc, FileRec*) write the project (what File > Save does) */
#define F_LOAD_STREAM   0x489388        /* (doc, FileRec*, 0) load a project into an existing document */
#define F_ASSIGN        0x405dfe        /* System.AssignFile(FileRec, string) */
#define F_RESET         0x406179        /* System.Reset(FileRec, recsize) */
#define F_REWRITE       0x406190        /* System.Rewrite(FileRec, recsize) */
#define F_CLOSE         0x405f68        /* System.CloseFile(FileRec) */
#define F_IORESULT      0x4027e4        /* raises EInOutError when InOutRes <> 0 */
#define F_IS_MODEL_MODE 0x484498        /* (doc) -> 1 while the simulation mode is on */

#define MAXHIST 100
#define MAXDOCS 16
/* history entries are project files in the temp folder: library objects load their bodies lazily from the file
   they were read from, so the files of live snapshots must stay on disk */
typedef struct { unsigned doc; int n, cur; unsigned char stamp[8]; unsigned ghash; char *img; DWORD imglen; char path[MAXHIST][MAX_PATH]; } DOCHIST;
static DOCHIST g_hist[MAXDOCS];
static HWND g_main;
static void hist_gc(void);
static unsigned char g_frec[512] __attribute__((aligned(8)));
static int g_busy;            /* inside snapshot/restore */
static unsigned g_snap_serial;

static DOCHIST *hist_for(unsigned doc, int create)
{
    int i;
    for (i = 0; i < MAXDOCS; i++) if (g_hist[i].doc == doc) return &g_hist[i];
    if (!create) return NULL;
    for (i = 0; i < MAXDOCS; i++) if (!g_hist[i].doc) { g_hist[i].doc = doc; return &g_hist[i]; }
    return NULL;
}

/* Delphi long string: [refcount=-1][length][chars][0] */
static char *mk_dstr(const char *s)
{
    DWORD n = (DWORD)lstrlenA(s);
    DWORD *b = (DWORD *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 8 + n + 1);
    b[0] = 0xFFFFFFFF; b[1] = n;
    memcpy(b + 2, s, n + 1);
    return (char *)(b + 2);
}
static void free_dstr(char *p) { HeapFree(GetProcessHeap(), 0, (DWORD *)p - 2); }

static void new_snap_path(char *out)
{
    GetTempPathA(MAX_PATH, out);
    wsprintfA(out + lstrlenA(out), "afdk_undo_%u_%u.tmp", (unsigned)GetCurrentProcessId(), ++g_snap_serial);
}

/* Every real edit stamps the project object with the modification time (FUN_004d7584 writes a double at +0x140),
   selection changes and scrolling do not - so the stamp tells us whether a new snapshot is needed. */
static int doc_stamp(unsigned doc, unsigned char *out8)
{
    unsigned obj = (unsigned)F32(doc, 0x14);
    if (!readable((void *)(obj + 0x140), 8)) return 0;
    memcpy(out8, (void *)(obj + 0x140), 8);
    return 1;
}

/* Selecting an item also bumps the modification stamp, so the stamp alone would record "undo" steps that change
   nothing.  A hash of the geometry (elements, wires, names) decides whether a step is real. */
static unsigned fnv(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
static unsigned scheme_hash(unsigned doc)
{
    unsigned sch = (unsigned)F32(doc, 0x34), el, wl, h = 2166136261u, e, w;
    int i, n, k;
    if (!sch) return 0;
    el = (unsigned)F32(sch, 0x10); wl = (unsigned)F32(sch, 0x18);
    n = el ? F32(el, 0) : 0;
    h = fnv(h, (unsigned)n);
    for (i = 1; i <= n; i++) {
        e = dcall((void *)0x45235c, el, (unsigned)i, 0, 0, 0, 0);
        if (!e) continue;
        h = fnv(h, (unsigned)F32(e, 0x44)); h = fnv(h, (unsigned)F32(e, 0x48));
        h = fnv(h, (unsigned)F32(e, 0x4c)); h = fnv(h, (unsigned)F32(e, 0xc));
    }
    n = wl ? F32(wl, 0) : 0;
    h = fnv(h, (unsigned)n);
    for (i = 1; i <= n; i++) {
        unsigned char *nm;
        w = dcall((void *)0x45235c, wl, (unsigned)i, 0, 0, 0, 0);
        if (!w) continue;
        h = fnv(h, (unsigned)F32(w, 0x24)); h = fnv(h, (unsigned)F32(w, 0x28));
        h = fnv(h, (unsigned)F32(w, 0x2c)); h = fnv(h, (unsigned)F32(w, 0x30));
        h = fnv(h, (unsigned)F32(w, 0x1c)); h = fnv(h, (unsigned)F32(w, 0x20));
        nm = (unsigned char *)(F32(w, 0x14) + 1);
        if (readable(nm, 1) && readable(nm, nm[0] + 1u)) for (k = 0; k <= nm[0]; k++) h = fnv(h, nm[k]);
    }
    return h;
}

static int doc_edit_mode(unsigned doc) { return (dcall((void *)F_IS_MODEL_MODE, doc, 0, 0, 0, 0, 0) & 0xff) == 0; }

/* Writing a project updates the "position inside the file" fields (+0x368, +0x370) of every library object; they
   must keep pointing into the file the object was really loaded from, so save and restore them around a snapshot. */
typedef struct { unsigned obj; int a, b; } LAZY;
#define MAXLAZY 4096
static LAZY g_lazy[MAXLAZY];
static int g_nlazy;

static void lazy_collect(unsigned obj, int depth)
{
    int i, cnt;
    if (!obj || depth > 16 || g_nlazy >= MAXLAZY || !readable((void *)obj, 0x380)) return;
    g_lazy[g_nlazy].obj = obj; g_lazy[g_nlazy].a = F32(obj, 0x368); g_lazy[g_nlazy].b = F32(obj, 0x370); g_nlazy++;
    cnt = F32(obj, 0);
    if (cnt < 0 || cnt > 5000) return;
    for (i = 1; i <= cnt; i++)
        lazy_collect(dcall((void *)0x45235c, obj, (unsigned)i, 0, 0, 0, 0), depth + 1);
}
static void lazy_restore(void)
{
    int i;
    for (i = 0; i < g_nlazy; i++) { F32(g_lazy[i].obj, 0x368) = g_lazy[i].a; F32(g_lazy[i].obj, 0x370) = g_lazy[i].b; }
    g_nlazy = 0;
}

/* serialise a document the same way File > Save does into 'path' */
static int snap_take(unsigned doc, const char *path)
{
    char *ds;
    unsigned char dirty = U8(doc, 0x18);      /* saving clears the "modified" flag - keep it */
    g_nlazy = 0;
    lazy_collect((unsigned)F32(doc, 0x14), 0);
    ds = mk_dstr(path);
    memset(g_frec, 0, sizeof g_frec);
    dcall((void *)F_ASSIGN, (unsigned)g_frec, (unsigned)ds, 0, 0, 0, 0);
    dcall((void *)F_REWRITE, (unsigned)g_frec, 1, 0, 0, 0, 0);
    dcall((void *)F_IORESULT, 0, 0, 0, 0, 0, 0);
    dcall((void *)F_SAVE_STREAM, doc, (unsigned)g_frec, 0, 0, 0, 0);
    U8(doc, 0x18) = dirty;
    dcall((void *)F_CLOSE, (unsigned)g_frec, 0, 0, 0, 0, 0);
    dcall((void *)F_IORESULT, 0, 0, 0, 0, 0, 0);
    free_dstr(ds);
    lazy_restore();
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* Library objects read from a file keep their algorithm bodies on disk and load them on demand from the file named by
   the container object (+0x261, ShortString).  Load every body now while that name points at the snapshot file, so the
   restored project no longer depends on it. */
static void pin_bodies(unsigned obj, int depth)
{
    int i, cnt;
    if (!obj || depth > 16 || !readable((void *)obj, 0x380)) return;
    if (depth > 0 && U8(obj, 0x248) == 4 && !F32(obj, 0x254)) dcall((void *)0x4d7048, obj, 0, 0, 0, 0, 0);   /* not the project itself: its body is the document's live netlist */
    cnt = F32(obj, 0);
    if (cnt < 0 || cnt > 5000) return;
    for (i = 1; i <= cnt; i++) pin_bodies(dcall((void *)0x45235c, obj, (unsigned)i, 0, 0, 0, 0), depth + 1);
}

static int snap_restore(unsigned doc, const char *path)
{
    char *ds;
    int old_model;
    unsigned old_lib, cont = (unsigned)F32(doc, 0x40);
    unsigned char keep[256];
    int have_cont = cont && readable((void *)(cont + 0x261), 256);
    ds = mk_dstr(path);
    memset(g_frec, 0, sizeof g_frec);
    dcall((void *)F_ASSIGN, (unsigned)g_frec, (unsigned)ds, 0, 0, 0, 0);
    dcall((void *)F_RESET, (unsigned)g_frec, 1, 0, 0, 0, 0);
    dcall((void *)F_IORESULT, 0, 0, 0, 0, 0, 0);
    if (have_cont) {                 /* lazy bodies must be read from the snapshot while we pin them */
        int n = lstrlenA(path); if (n > 255) n = 255;
        memcpy(keep, (void *)(cont + 0x261), 256);
        U8(cont, 0x261) = (unsigned char)n; memcpy((void *)(cont + 0x262), path, n);
    }
    /* The loader registers the reloaded project library in the document's container and then binds the document's model
       to whatever library a key lookup in that container finds first.  Unregister the current one beforehand, otherwise
       the model would stay bound to the old library and File > Save would write a stale netlist. */
    old_lib = (unsigned)F32(doc, 0x14);
    /* tear the old model down first, while its library can still be looked up (the loader would do it later) */
    if (F32(doc, 8)) dcall((void *)0x4d5024, (unsigned)F32(doc, 8), 0, 0, 0, 0, 0);
    if (cont && old_lib && readable((void *)cont, 0x40) && F32(cont, 0x18)) {
        int ci, cn = F32(cont, 0);
        for (ci = 1; ci <= cn; ci++)
            if (dcall((void *)0x45235c, cont, (unsigned)ci, 0, 0, 0, 0) == old_lib) {
                dcall((void *)F32(F32(cont, 0x18), 0x24), cont, 0, 0, 0, 0, 0);       /* delete the item under the cursor */
                break;
            }
    }
    old_model = F32(doc, 0xc);       /* a document that was just created has no model object yet - the loader builds one */
    F32(doc, 0xc) = 0;
    dcall((void *)F_LOAD_STREAM, doc, (unsigned)g_frec, 0, 0, 0, 0);
    dcall((void *)F_CLOSE, (unsigned)g_frec, 0, 0, 0, 0, 0);
    dcall((void *)F_IORESULT, 0, 0, 0, 0, 0, 0);
    pin_bodies((unsigned)F32(doc, 0x14), 0);
    if (have_cont) memcpy((void *)(cont + 0x261), keep, 256);
    free_dstr(ds);
    fx_log("restore %s: old model %x new model %x" NL, path, old_model, F32(doc, 0xc));
    return 1;
}

static void hist_free_from(DOCHIST *h, int from)
{
    int i;
    for (i = from; i < h->n; i++) { DeleteFileA(h->path[i]); h->path[i][0] = 0; }
    if (from < h->n) h->n = from;
}

static void hist_keep_image(DOCHIST *h, unsigned doc);

/* make the snapshot file at 'path' the newest history entry of doc */
static void hist_push(DOCHIST *h, unsigned doc, const char *path)
{
    unsigned char st[8];
    doc_stamp(doc, st);                                 /* the snapshot may normalise the scheme: re-read both */
    memcpy(h->stamp, st, 8);
    h->ghash = scheme_hash(doc);
    hist_free_from(h, h->n ? h->cur + 1 : 0);          /* a new action drops the redo branch */
    if (h->n == MAXHIST) {                              /* forget the oldest */
        DeleteFileA(h->path[0]);
        memmove(&h->path[0], &h->path[1], (MAXHIST - 1) * sizeof h->path[0]);
        h->n--;
    }
    lstrcpyA(h->path[h->n], path); h->cur = h->n; h->n++;
    hist_keep_image(h, doc);
    fx_log("hist: doc=%x n=%d cur=%d %s" NL, doc, h->n, h->cur, path);
}

/* record the current state of a document if it was edited since the last record */
static int g_ghost;                                      /* a pasted block is following the pointer */

static void hist_note(unsigned doc)
{
    DOCHIST *h;
    unsigned char st[8];
    unsigned gh;
    char path[MAX_PATH];
    if (!doc || g_ghost || g_busy || !doc_edit_mode(doc) || !doc_stamp(doc, st)) return;
    {   /* do not snapshot while a wire is being drawn */
        unsigned sch = (unsigned)F32(doc, 0x34), builder = sch ? (unsigned)F32(sch, 0) : 0;
        if (builder && U8(builder, 0x18)) return;
    }
    h = hist_for(doc, 0);
    if (h && h->n && memcmp(h->stamp, st, 8) == 0) return;        /* nothing was edited */
    if (!h) hist_gc();
    gh = scheme_hash(doc);
    fx_log("note: gh=%08x prev=%08x n=%d" NL, gh, h ? h->ghash : 0, h ? h->n : 0);
    if (h && h->n && h->ghash == gh) { memcpy(h->stamp, st, 8); return; }   /* only selection etc. changed */
    if (h && h->n && !doc_stamp(doc, st)) return;
    new_snap_path(path);
    g_busy = 1;
    if (!snap_take(doc, path)) { g_busy = 0; return; }
    g_busy = 0;
    if (!h) h = hist_for(doc, 1);
    if (!h) { DeleteFileA(path); return; }
    hist_push(h, doc, path);
}

/* The elements and lines of a document exactly as the program writes them to a project file (FUN_004d3c3c), with
   their parameters.  Unlike a whole snapshot this contains no addresses, so equal schemes give equal bytes. */
static char *scheme_image(unsigned doc, DWORD *len)
{
    char path[MAX_PATH], *ds, *b = NULL;
    unsigned sch = (unsigned)F32(doc, 0x34);
    HANDLE f;
    DWORD n, got = 0;
    *len = 0;
    if (!sch) return NULL;
    GetTempPathA(MAX_PATH, path);
    wsprintfA(path + lstrlenA(path), "afdk_scheme_%u.tmp", (unsigned)GetCurrentProcessId());
    ds = mk_dstr(path);
    memset(g_frec, 0, sizeof g_frec);
    dcall((void *)F_ASSIGN, (unsigned)g_frec, (unsigned)ds, 0, 0, 0, 0);
    dcall((void *)F_REWRITE, (unsigned)g_frec, 1, 0, 0, 0, 0);
    dcall((void *)F_IORESULT, 0, 0, 0, 0, 0, 0);
    dcall((void *)0x4d3c3c, sch, (unsigned)g_frec, 0, 0, 0, 0);
    dcall((void *)F_CLOSE, (unsigned)g_frec, 0, 0, 0, 0, 0);
    dcall((void *)F_IORESULT, 0, 0, 0, 0, 0, 0);
    free_dstr(ds);
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        n = GetFileSize(f, NULL);
        if (n != INVALID_FILE_SIZE && (b = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1)) != NULL &&
            (!ReadFile(f, b, n, &got, NULL) || got != n)) { HeapFree(GetProcessHeap(), 0, b); b = NULL; }
        CloseHandle(f);
        if (b) *len = n;
    }
    DeleteFileA(path);
    return b;
}

/* remember the scheme image of the current history entry */
static void hist_keep_image(DOCHIST *h, unsigned doc)
{
    int busy = g_busy;
    if (h->img) HeapFree(GetProcessHeap(), 0, h->img);
    g_busy = 1;
    h->img = scheme_image(doc, &h->imglen);
    g_busy = busy;
}

/* After a dialog (element parameters etc.) the geometry may be unchanged although the scheme is not: compare the
   scheme image with the one of the current history entry and record a step if they differ. */
static void hist_note_full(unsigned doc)
{
    DOCHIST *h;
    char path[MAX_PATH], *img;
    DWORD len;
    int same, ok;
    hist_note(doc);
    h = hist_for(doc, 0);
    if (!h || !h->n || g_busy || g_ghost || !doc_edit_mode(doc)) return;
    g_busy = 1;
    img = scheme_image(doc, &len);
    g_busy = 0;
    same = img && h->img && len == h->imglen && memcmp(img, h->img, len) == 0;
    if (img) HeapFree(GetProcessHeap(), 0, img);
    if (same || !img) return;
    new_snap_path(path);
    g_busy = 1;
    ok = snap_take(doc, path);
    g_busy = 0;
    if (ok) hist_push(h, doc, path);
}

static void hist_step(int dir)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0;
    DOCHIST *h;
    int to;
    if (!doc || g_busy || !doc_edit_mode(doc)) return;
    hist_note(doc);                                     /* make sure the latest edit is on the stack */
    h = hist_for(doc, 0);
    if (!h || h->n < 2) return;
    to = h->cur + dir;
    if (to < 0 || to >= h->n) return;
    g_busy = 1;
    if (snap_restore(doc, h->path[to])) {
        h->cur = to;
        doc_stamp(doc, h->stamp);
        h->ghash = scheme_hash(doc);
        hist_keep_image(h, doc);
        U8(doc, 0x18) = 1;
        dcall((void *)0x48b4f8, pf, 0, 0, 0, 0, 0);     /* what the original does after loading a project: view extents */
        dcall((void *)F_REFRESH, (unsigned)F32(pf, 0x308), 0, 0, 0, 0, 0);
    }
    g_busy = 0;
    fx_log("hist: step %d -> cur=%d/%d" NL, dir, h->cur, h->n);
}

/* forget histories of documents that were closed (their memory address may be reused by a new document) */
static void hist_gc(void)
{
    unsigned live[MAXDOCS], pf;
    int i, n = 0;
    HWND mdi = NULL, h;
    if (g_main) mdi = FindWindowExA(g_main, NULL, "MDIClient", NULL);
    for (h = mdi ? GetWindow(mdi, GW_CHILD) : NULL; h && n < MAXDOCS; h = GetWindow(h, GW_HWNDNEXT)) {
        pf = (unsigned)obj_from_hwnd(h);
        if (pf && readable((void *)(pf + 0x314), 4) && F32(pf, 0x314)) live[n++] = (unsigned)F32(pf, 0x314);
    }
    if (!mdi) return;
    for (i = 0; i < MAXDOCS; i++) {
        int k, found = 0;
        if (!g_hist[i].doc) continue;
        for (k = 0; k < n; k++) if (live[k] == g_hist[i].doc) found = 1;
        if (!found) {
            hist_free_from(&g_hist[i], 0); g_hist[i].doc = 0;
            if (g_hist[i].img) { HeapFree(GetProcessHeap(), 0, g_hist[i].img); g_hist[i].img = NULL; }
        }
    }
}

/* snapshot files left behind by instances that were killed or crashed */
static void hist_sweep_stale(void)
{
    char dir[MAX_PATH], pat[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    GetTempPathA(MAX_PATH, dir);
    wsprintfA(pat, "%safdk_undo_*.tmp", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        unsigned pid = 0;
        const char *p = fd.cFileName + 10;                 /* after "afdk_undo_" */
        HANDLE ph;
        while (*p >= '0' && *p <= '9') pid = pid * 10 + (unsigned)(*p++ - '0');
        if (!pid || pid == (unsigned)GetCurrentProcessId()) continue;
        ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (ph) { CloseHandle(ph); continue; }             /* that instance is still running */
        wsprintfA(full, "%s%s", dir, fd.cFileName);
        DeleteFileA(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void hist_cleanup(void)
{
    int i;
    for (i = 0; i < MAXDOCS; i++) if (g_hist[i].doc) hist_free_from(&g_hist[i], 0);
}

/* ===================== clipboard ===================== */

static int main_active(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg || !is_class(fg, "TMainForm")) return 0;
    g_main = fg;
    return GetActiveWindow() == fg;
}

static int menu_cmd_enabled(int id)
{
    HMENU m = g_main ? GetMenu(g_main) : NULL;
    UINT st;
    if (!m) return 0;
    st = GetMenuState(m, (UINT)id, MF_BYCOMMAND);
    return st != (UINT)-1 && !(st & (MF_GRAYED | MF_DISABLED));
}

static int focus_is_text(void)
{
    HWND f = GetFocus();
    return f && (is_class(f, "Edit") || is_class(f, "TEdit") || is_class(f, "TMemo") || is_class(f, "RICHEDIT") ||
                 is_class(f, "TRichEdit") || is_class(f, "TComboBox") || is_class(f, "ComboBox"));
}

static void do_copy(void)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0;
    if (doc && menu_cmd_enabled(11)) dcall((void *)F_COPY, (unsigned)F32(doc, 0x34), 0, 0, 0, 0, 0);
}

/* Paste: the block appears with its corner at the pointer and follows it (the pasted copy is the live preview,
   so it looks exactly like the final result) until a click drops it; Esc / right button cancels. */
#ifndef F_DELETE_SEL
#define F_DELETE_SEL     0x4d1bcc
#define F_REBUILD        0x4d22d4
#define F_AFTER_EDIT     0x4d2bb0
#endif
static unsigned g_ghost_doc;
static int g_gx = 0x7fffffff, g_gy;
static int g_gox, g_goy, g_goff_ok;                      /* pasted block's centre relative to its origin corner */


/* bounding box of the selected elements/wires (model units); 0 if nothing is selected */
static int sel_bbox(unsigned scheme, int *x0, int *y0, int *x1, int *y1)
{
    unsigned el = (unsigned)F32(scheme, 0x10), wl = (unsigned)F32(scheme, 0x18);
    int i, n, have = 0;
#define ACC(x, y) do { if (!have) { *x0 = *x1 = (x); *y0 = *y1 = (y); have = 1; } else {         if ((x) < *x0) *x0 = (x); if ((x) > *x1) *x1 = (x); if ((y) < *y0) *y0 = (y); if ((y) > *y1) *y1 = (y); } } while (0)
    for (i = 1, n = el ? F32(el, 0) : 0; i <= n; i++) {
        unsigned e = LI(el, i), def;
        if (!e || !U8(e, 0x50)) continue;
        def = (unsigned)F32(e, 8);
        ACC(F32(e, 0x44), F32(e, 0x48));
        if (def) ACC(F32(e, 0x44) + F32(def, 0x21c), F32(e, 0x48) + F32(def, 0x220));
    }
    for (i = 1, n = wl ? F32(wl, 0) : 0; i <= n; i++) {
        unsigned w = LI(wl, i);
        if (!w || !U8(w, 0x18)) continue;
        ACC(F32(w, 0x24), F32(w, 0x28));
        ACC(F32(w, 0x24) + F32(w, 0x2c), F32(w, 0x28) + F32(w, 0x30));
    }
#undef ACC
    return have;
}

static int ghost_target(unsigned pf, int *mx, int *my)
{
    unsigned cv = (unsigned)F32(pf, 0x308);
    POINT pt;
    HWND cvw;
    int cx, cy;
    if (!cv) return 0;
    GetCursorPos(&pt);
    cvw = canvas_at(pt);
    if (cvw && obj_from_hwnd(cvw) == (void *)cv) {
        ScreenToClient(cvw, &pt);
        cx = pt.x; cy = pt.y;
    } else {                                           /* pointer is elsewhere: window centre until it comes over */
        cx = F32(F32(cv, 0x220), 0x38) / 2;
        cy = F32(F32(cv, 0x220), 0x3c) / 2;
    }
    *mx = (int)dcall((void *)F_VIEW2MODEL_X, cv, (unsigned)cx, 0, 0, 0, 0);
    *my = (int)dcall((void *)F_VIEW2MODEL_Y, cv, (unsigned)cy, 0, 0, 0, 0);
    return 1;
}

static void ghost_place(void)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0, scheme;
    int mx, my, bx0, by0, bx1, by1;
    if (!doc || doc != g_ghost_doc || !doc_edit_mode(doc)) { g_ghost = 0; return; }
    scheme = (unsigned)F32(doc, 0x34);
    if (!scheme || !ghost_target(pf, &mx, &my) || (mx == g_gx && my == g_gy)) return;
    if (g_gx != 0x7fffffff) dcall((void *)F_DELETE_SEL, scheme, 0, 0, 0, 0, 0);   /* the previous copy is the only selection */
    dcall((void *)F_PASTE, scheme, (unsigned)(mx - g_gox), (unsigned)(my - g_goy), 0, 0, 0);
    if (!g_goff_ok) {                                  /* first copy: measure it, then centre it on the pointer */
        g_goff_ok = 1;
        if (sel_bbox(scheme, &bx0, &by0, &bx1, &by1)) {
            g_gox = (bx0 + bx1) / 2 - mx; g_goy = (by0 + by1) / 2 - my;
            dcall((void *)F_DELETE_SEL, scheme, 0, 0, 0, 0, 0);
            dcall((void *)F_PASTE, scheme, (unsigned)(mx - g_gox), (unsigned)(my - g_goy), 0, 0, 0);
        }
    }
    g_gx = mx; g_gy = my;
}

static void ghost_end(int keep)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0, scheme;
    if (!g_ghost) return;
    if (!keep && doc && doc == g_ghost_doc && (scheme = (unsigned)F32(doc, 0x34)) != 0) {
        dcall((void *)F_DELETE_SEL, scheme, 0, 0, 0, 0, 0);
        dcall((void *)F_REBUILD, scheme, 0, 0, 0, 0, 0);
        dcall((void *)F_AFTER_EDIT, scheme, 0, 0, 0, 0, 0);
    }
    g_ghost = 0;
    if (keep && doc == g_ghost_doc) hist_note(doc);
}

static void do_paste_at_cursor(void)
{
    unsigned pf = MAINFORM(), doc;
    if (!pf || !menu_cmd_enabled(12)) return;
    ghost_end(1);
    doc = (unsigned)F32(pf, 0x314);
    if (!doc || !F32(doc, 0x34) || !F32(pf, 0x308)) return;
    hist_note(doc);
    g_ghost_doc = doc;
    g_gx = 0x7fffffff; g_gox = g_goy = 0; g_goff_ok = 0;
    g_ghost = 1;
    ghost_place();
}

/* replaces the original Edit>Paste handler (toolbar button / menu), which pasted at the window centre */
static void paste_command(void)
{
    do_paste_at_cursor();
}

static void hook_paste_command(void)
{
    unsigned char *p = (unsigned char *)0x4da558;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0xE9;
    *(int *)(p + 1) = (int)((unsigned char *)paste_command - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}
