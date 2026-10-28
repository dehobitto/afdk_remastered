/* feat.c - editor features added on top of the original code (included from afdkfix.c).
   The original program is a Delphi (register calling convention) binary; internal functions are called by address.
   Addresses / field offsets were taken from the decompiled code (see patch\re\NOTES.md). */

#define NL "\015\012"

#include "common.h"
#include "cfg.c"

/* ---------- addresses inside AFDK.exe (image base 0x400000) ---------- */
#define A_MAINFORM_PP   0x4e4cac        /* **(TMainForm***) */
#define F_VIEW2MODEL_X  0x47ddcc        /* (canvas, xView) -> xModel  = round((hbar.pos + x) / level) */
#define F_VIEW2MODEL_Y  0x47de14
#define F_SET_ZOOM      0x47d67c        /* (canvas, level) keeps the view centre */
#define F_REFRESH       0x47de5c        /* (canvas) */
#define F_BAR_SETPOS    0x42c05c        /* TScrollBar.SetPosition(bar, pos) */
#define F_PASTE         0x4d5be8        /* (scheme, x, y) paste internal clipboard with its origin at model (x,y) */
#define F_COPY          0x4d5a84        /* (scheme) copy selection into the internal clipboard */

/* TFrpaint (canvas) object fields */
#define CV_HBAR   0x208
#define CV_VBAR   0x20c
#define CV_LEVEL  0x22c                 /* byte: zoom level, 5..40 */
#define CV_NOTIFY 0x240                 /* byte: 0 while the scroll bars are being changed */
#define BAR_POS   0x1f4

#define MAINFORM()   active_pf()

/* the active project form / document, or 0 while no project window exists */
static unsigned active_pf(void)
{
    unsigned *var = (unsigned *)*(unsigned *)A_MAINFORM_PP;
    unsigned pf = readable(var, 4) ? *var : 0;
    return pf && readable((void *)(pf + 0x318), 4) ? pf : 0;
}
static unsigned active_doc(void)
{
    unsigned pf = active_pf();
    return pf ? (unsigned)F32(pf, 0x314) : 0;
}

static HHOOK g_hook;

static int is_class(HWND h, const char *cls)
{
    char b[64];
    if (!h || !GetClassNameA(h, b, sizeof b)) return 0;
    return lstrcmpA(b, cls) == 0;
}

/* VCL keeps the owning control in a window property (Controls.FindControl, GetProp with the ControlAtom) */
static void *obj_from_hwnd(HWND h)
{
    WORD atom = *(WORD *)0x4e5756;
    return h ? (void *)GetPropA(h, (LPCSTR)(UINT_PTR)atom) : NULL;
}

/* the canvas ("TFrpaint") under the cursor; scroll bars / frame of it count as the canvas */
static HWND canvas_at(POINT pt)
{
    HWND h = WindowFromPoint(pt);
    while (h) {
        if (is_class(h, "TFrpaint")) return h;
        if (is_class(h, "TMainForm") || is_class(h, "TApplication")) return NULL;
        h = GetParent(h);
    }
    return NULL;
}

/* scroll bars (Win32 handles) of a canvas */
static void find_bars(HWND canvas, HWND *v, HWND *hh)
{
    HWND c = GetWindow(canvas, GW_CHILD);
    *v = *hh = NULL;
    for (; c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!is_class(c, "TScrollBar") || !IsWindowVisible(c)) continue;
        if (GetWindowLongA(c, GWL_STYLE) & SBS_VERT) *v = c; else *hh = c;
    }
}

static int clamp_pos(HWND bar, int pos)
{
    SCROLLINFO si;
    int mx;
    si.cbSize = sizeof si; si.fMask = SIF_ALL;
    if (!bar || !GetScrollInfo(bar, SB_CTL, &si)) return pos < 0 ? 0 : pos;
    mx = si.nMax - (int)si.nPage + 1;
    if (mx < si.nMin) mx = si.nMin;
    if (pos > mx) pos = mx;
    if (pos < si.nMin) pos = si.nMin;
    return pos;
}

/* move both scroll positions of a canvas (absolute, pixels); refreshes once like the original pan code */
static void set_scroll(HWND cvw, void *cv, int hx, int vy)
{
    HWND vb, hb;
    void *hbo = (void *)F32(cv, CV_HBAR), *vbo = (void *)F32(cv, CV_VBAR);
    find_bars(cvw, &vb, &hb);
    hx = clamp_pos(hb, hx);
    vy = clamp_pos(vb, vy);
    if (hx == F32(hbo, BAR_POS) && vy == F32(vbo, BAR_POS)) return;
    U8(cv, CV_NOTIFY) = 0;
    dcall((void *)F_BAR_SETPOS, (unsigned)hbo, (unsigned)hx, 0, 0, 0, 0);
    dcall((void *)F_BAR_SETPOS, (unsigned)vbo, (unsigned)vy, 0, 0, 0, 0);
    U8(cv, CV_NOTIFY) = 1;
    dcall((void *)F_REFRESH, (unsigned)cv, 0, 0, 0, 0, 0);
}

static void scroll_by(HWND cvw, void *cv, int dx, int dy)
{
    set_scroll(cvw, cv, F32(F32(cv, CV_HBAR), BAR_POS) + dx, F32(F32(cv, CV_VBAR), BAR_POS) + dy);
}

static int round_i(double v) { return (int)(v + (v >= 0 ? 0.5 : -0.5)); }

/* zoom one level in/out keeping the model point under (cx, cy) (canvas client pixels) fixed */
static void zoom_at(HWND cvw, void *cv, int dir, int cx, int cy)
{
    int l1 = U8(cv, CV_LEVEL), l2 = l1 + (dir > 0 ? 1 : -1);
    double mx, my;
    int hp = F32(F32(cv, CV_HBAR), BAR_POS), vp = F32(F32(cv, CV_VBAR), BAR_POS);
    if (l2 < 5 || l2 > 40 || l1 <= 0) return;
    mx = (hp + cx) / (double)l1;
    my = (vp + cy) / (double)l1;
    {   /* the scroll range covers only the drawing's extent (+0x224/+0x228, model units): widen it so the point under
           the pointer can stay there, otherwise the view snaps towards the top-left corner */
        RECT rc;
        int needw, needh;
        GetClientRect(cvw, &rc);
        needw = (int)mx + (rc.right - cx) / l2 + 2;
        needh = (int)my + (rc.bottom - cy) / l2 + 2;
        if (F32(cv, 0x224) < needw) F32(cv, 0x224) = needw;
        if (F32(cv, 0x228) < needh) F32(cv, 0x228) = needh;
    }
    {   /* integers only from here: the program's code does not preserve the FPU registers across the call */
        int tx = round_i(mx * l2 - cx), ty = round_i(my * l2 - cy);
        dcall((void *)F_SET_ZOOM, (unsigned)cv, (unsigned)l2, 0, 0, 0, 0);     /* also recomputes the scroll ranges */
        set_scroll(cvw, cv, tx, ty);
    }
    dcall((void *)F_REFRESH, (unsigned)cv, 0, 0, 0, 0, 0);
}

#include "feat2.c"
#include "gfx.c"
#include "filekey.c"
#include "feat3.c"
#include "feat4.c"

#define WM_APP_SNAP (WM_APP + 0x4A1)
#define WM_APP_MVFIX (WM_APP + 0x4A2)
#define WM_APP_MVCAP (WM_APP + 0x4A3)
static int g_snap_pending;
static int g_swallow_up;

static HWND canvas_at_cursor(void)
{
    POINT p;
    GetCursorPos(&p);
    return canvas_at(p);
}

static int in_canvas(HWND h)
{
    for (; h; h = GetParent(h)) {
        if (is_class(h, "TFrpaint")) return 1;
        if (is_class(h, "TMainForm")) return 0;
    }
    return 0;
}

/* ---------- middle button panning ---------- */
static HWND g_pan_cvw;
static POINT g_pan_last;     /* screen coordinates */

/* ---------- WH_GETMESSAGE hook ---------- */
static LRESULT CALLBACK getmsg_proc(int code, WPARAM wp, LPARAM lp)
{
    MSG *m = (MSG *)lp;
    locale_watch_captions();
    if (code == HC_ACTION && wp == PM_REMOVE && m) {
        switch (m->message) {
        case WM_MOUSEWHEEL: {
            POINT pt = { (short)LOWORD(m->lParam), (short)HIWORD(m->lParam) };
            HWND cvw = canvas_at(pt);
            void *cv = cvw ? obj_from_hwnd(cvw) : NULL;
            int steps;
            if (!cv) break;
            steps = (short)HIWORD(m->wParam) / 120;
            if (!steps) steps = (short)HIWORD(m->wParam) > 0 ? 1 : -1;
            {
                POINT c = pt; int i, n = steps < 0 ? -steps : steps;
                ScreenToClient(cvw, &c);
                for (i = 0; i < n; i++) zoom_at(cvw, cv, steps > 0 ? 1 : -1, c.x, c.y);
            }
            m->message = WM_NULL;
            break;
        }
        case WM_MBUTTONDOWN: {
            POINT pt = { (short)LOWORD(m->lParam), (short)HIWORD(m->lParam) };
            ClientToScreen(m->hwnd, &pt);
            {
                HWND cvw = canvas_at(pt);
                if (cvw && obj_from_hwnd(cvw)) {
                    g_pan_cvw = cvw; g_pan_last = pt;
                    SetCapture(m->hwnd);
                    SetCursor(LoadCursorA(NULL, IDC_SIZEALL));
                    m->message = WM_NULL;
                }
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (g_ghost && main_active() && canvas_at_cursor()) ghost_place();
            if (g_tip_wire) {
                void *cv = obj_from_hwnd(g_tip_cvw);
                if (cv && (m->wParam & MK_LBUTTON)) {
                    POINT c; GetCursorPos(&c); ScreenToClient(g_tip_cvw, &c);
                    tip_move(cv, c.x, c.y);
                    m->message = WM_NULL;
                    break;
                }
                if (cv) tip_end(cv); else g_tip_wire = 0;
                ReleaseCapture();
            }
            if (g_pan_cvw) {
                if (m->wParam & MK_MBUTTON) {
                    POINT pt = { (short)LOWORD(m->lParam), (short)HIWORD(m->lParam) };
                    void *cv = obj_from_hwnd(g_pan_cvw);
                    ClientToScreen(m->hwnd, &pt);
                    if (cv) scroll_by(g_pan_cvw, cv, g_pan_last.x - pt.x, g_pan_last.y - pt.y);
                    g_pan_last = pt;
                    m->message = WM_NULL;
                } else { g_pan_cvw = NULL; ReleaseCapture(); }
            }
            break;
        case WM_LBUTTONDOWN: {
            HWND cvw = in_canvas(m->hwnd) && main_active() ? canvas_at_cursor() : NULL;
            void *cv = cvw ? obj_from_hwnd(cvw) : NULL;
            if (g_ghost) {
                if (cv) { ghost_place(); ghost_end(1); g_swallow_up |= 1; m->message = WM_NULL; break; }
                ghost_end(1);
            }
            if (cv && !(m->wParam & (MK_CONTROL | MK_SHIFT))) {
                POINT c; GetCursorPos(&c); ScreenToClient(cvw, &c);
                if (tip_begin(cvw, cv, c.x, c.y)) { SetCapture(cvw); m->message = WM_NULL; break; }
            }
            if (cv) hist_note(active_doc());
            if (cv && g_main && tool_down(MF_TOOL_SELECT)) PostMessageA(g_main, WM_APP_MVCAP, 0, 0);
            break;
        }
        case WM_RBUTTONDOWN: {
            HWND cvw = in_canvas(m->hwnd) && main_active() ? canvas_at_cursor() : NULL;
            void *cv = cvw ? obj_from_hwnd(cvw) : NULL;
            if (g_ghost) { ghost_end(0); g_swallow_up |= 2; m->message = WM_NULL; break; }
            if (cv) {
                POINT c; GetCursorPos(&c); ScreenToClient(cvw, &c);
                if (wire_delete_at(cvw, cv, c.x, c.y)) { m->message = WM_NULL; break; }
                hist_note(active_doc());
            }
            break;
        }
        case WM_RBUTTONUP:
            if (g_swallow_up & 2) { g_swallow_up &= ~2; m->message = WM_NULL; break; }
            if (g_main && !g_snap_pending && main_active()) { g_snap_pending = 1; PostMessageA(g_main, WM_APP_SNAP, 0, 0); }
            break;
        case WM_LBUTTONDBLCLK:
            if (in_canvas(m->hwnd) && main_active()) {
                HWND cvw = canvas_at_cursor();
                void *cv = cvw ? obj_from_hwnd(cvw) : NULL;
                if (cv && !g_ghost && empty_spot(cvw, cv)) {         /* double click on empty space: new node there */
                    g_swallow_up |= 1;
                    m->message = WM_NULL;
                    do_new_node();
                    break;
                }
                hist_note(active_doc());
            }
            break;
        case WM_LBUTTONUP:
            if (g_swallow_up & 1) { g_swallow_up &= ~1; m->message = WM_NULL; break; }
            if (g_tip_wire) {
                void *cv = obj_from_hwnd(g_tip_cvw);
                ReleaseCapture();
                if (cv) tip_end(cv); else g_tip_wire = 0;
                m->message = WM_NULL;
                break;
            }
            if (g_main && p_wires_apply && main_active()) PostMessageA(g_main, WM_APP_MVFIX, 0, 0);
            /* fall through */
        case WM_KEYUP:
            if (g_main && !g_snap_pending && main_active()) {
                g_snap_pending = 1;
                PostMessageA(g_main, WM_APP_SNAP, 0, 0);
            }
            break;
        case WM_APP_MVCAP: {
            unsigned pf, doc, scheme;
            if (edit_ready(&pf, &doc, &scheme)) mv_capture(scheme);
            m->message = WM_NULL;
            break;
        }
        case WM_APP + 0x4A4: {                                     /* after the "new node" click: selection tool */
            unsigned mf = main_form(), b = mf ? (unsigned)F32(mf, MF_TOOL_SELECT) : 0;
            if (b) dcall((void *)F_TOOL_SET, b, 1, 0, 0, 0, 0);
            m->message = WM_NULL;
            break;
        }
        case WM_APP_MVFIX:
            mv_apply();
            m->message = WM_NULL;
            break;
        case WM_APP_SNAP:
            g_snap_pending = 0;
            if (main_active()) hist_note(active_doc());
            m->message = WM_NULL;
            break;
        case WM_KEYDOWN:
            if (g_ghost && m->wParam != VK_CONTROL && m->wParam != VK_SHIFT && m->wParam != VK_MENU) {
                if (m->wParam == VK_ESCAPE) { ghost_end(0); m->message = WM_NULL; break; }
                if (!(m->wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))) ghost_end(1);
            }
            if ((m->wParam == VK_LEFT || m->wParam == VK_RIGHT || m->wParam == VK_UP || m->wParam == VK_DOWN) &&
                !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) && main_active() &&
                !focus_is_text() && !is_class(GetFocus(), "TTreeView") && !is_class(GetFocus(), "SysTreeView32")) {
                int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 5 : 1;
                int dx = m->wParam == VK_LEFT ? -step : m->wParam == VK_RIGHT ? step : 0;
                int dy = m->wParam == VK_UP ? -step : m->wParam == VK_DOWN ? step : 0;
                if (nudge_selection(dx, dy)) { m->message = WM_NULL; break; }
            }
            if (m->wParam == 'V' && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) &&
                !(GetKeyState(VK_SHIFT) & 0x8000) && main_active() && !focus_is_text() &&
                !is_class(GetFocus(), "TTreeView") && !is_class(GetFocus(), "SysTreeView32")) {
                do_new_node(); m->message = WM_NULL; break;
            }
            if (main_active() && m->wParam != VK_CONTROL && m->wParam != VK_SHIFT && m->wParam != VK_MENU &&
                !((GetKeyState(VK_CONTROL) & 0x8000) && (m->wParam == 'Z' || m->wParam == 'Y')))
                hist_note(active_doc());
            if ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) && main_active() &&
                !focus_is_text()) {
                int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                switch (m->wParam) {
                case 'C': do_copy(); m->message = WM_NULL; break;
                case 'V': do_paste_at_cursor(); m->message = WM_NULL; break;
                case 'A': do_select_all(); m->message = WM_NULL; break;
                case 'D': do_duplicate(); m->message = WM_NULL; break;
                case 'Z': hist_step(shift ? +1 : -1); m->message = WM_NULL; break;
                case 'Y': hist_step(+1); m->message = WM_NULL; break;
                }
            }
            break;
        case WM_MBUTTONUP:
            if (g_pan_cvw) { g_pan_cvw = NULL; ReleaseCapture(); m->message = WM_NULL; }
            break;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void install_features(HINSTANCE inst)
{
    g_self = inst;
    cfg_load();
    install_gfx();
    fix_file_keys();
    hist_sweep_stale();
    hook_paste_command();
    SetTimer(NULL, 0, 1000, tick_proc);
    g_hook = SetWindowsHookExA(WH_GETMESSAGE, getmsg_proc, inst, GetCurrentThreadId());
}
