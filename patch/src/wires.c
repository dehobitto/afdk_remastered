/* wires.c - _afdk_remastered_wires.dll: lines follow the elements they are attached to when those elements are moved.
   Loaded on demand by _afdk_remastered.dll; without this DLL a move leaves the lines where they were (original behaviour).
   Before a move afdkfix calls WiresCapture (pins of the selected elements), after it WiresApply. */
#include <windows.h>
#include "common.h"

#define fx_log(...) ((void)0)

#define MAXATT 256
static struct { int on; unsigned scheme, e0; int ex, ey, n; POINT p[MAXATT]; } g_mv;

static int pin_owned_by_selected(unsigned J)
{
    unsigned pl = (unsigned)F32(J, 8), el;
    int k, n, i, m;
    if (!pl || !readable((void *)pl, 4)) return 0;
    n = F32(pl, 0);
    el = (unsigned)F32(g_mv.scheme, 0x10);
    m = F32(el, 0);
    for (k = 1; k <= n; k++) {
        unsigned rec = LI(pl, k), owner = rec;
        if (!rec) continue;
        for (i = 0; i < 2; i++) {
            int j;
            for (j = 1; j <= m; j++) {
                unsigned e = LI(el, j);
                if (e == owner && U8(e, 0x50)) return 1;
            }
            owner = readable((void *)rec, 4) ? (unsigned)F32(rec, 0) : 0;     /* record whose first field is the element */
            if (!owner) break;
        }
    }
    return 0;
}

/* note the pin positions of the selected elements that have lines attached (positions rather than line objects: the
   original move code may merge or split lines on the way) */
static void mv_capture(unsigned scheme)
{
    unsigned el = (unsigned)F32(scheme, 0x10), jl = (unsigned)F32(scheme, 0x14), e0 = 0;
    int i, n;
    g_mv.on = 0; g_mv.n = 0; g_mv.scheme = scheme;
    for (i = 1, n = F32(el, 0); i <= n; i++) {
        unsigned e = LI(el, i);
        if (e && U8(e, 0x50)) { e0 = e; break; }
    }
    if (!e0) return;
    g_mv.e0 = e0; g_mv.ex = F32(e0, 0x44); g_mv.ey = F32(e0, 0x48);
    for (i = 1, n = F32(jl, 0); i <= n && g_mv.n < MAXATT; i++) {
        unsigned J = LI(jl, i), wl;
        if (!J || !pin_owned_by_selected(J)) continue;
        wl = (unsigned)F32(J, 4);
        if (!wl || F32(wl, 0) <= 0) continue;
        g_mv.p[g_mv.n].x = F32(J, 0x14); g_mv.p[g_mv.n].y = F32(J, 0x18); g_mv.n++;
    }
    g_mv.on = g_mv.n > 0;
}

static int mv_hit(int x, int y)
{
    int i;
    for (i = 0; i < g_mv.n; i++) if (g_mv.p[i].x == x && g_mv.p[i].y == y) return 1;
    return 0;
}

/* ---- helpers ---- */
static int pins_at(unsigned scheme, int x, int y)
{
    unsigned jl = (unsigned)F32(scheme, 0x14);
    int i, n = jl ? F32(jl, 0) : 0;
    for (i = 1; i <= n; i++) {
        unsigned J = LI(jl, i), pl;
        if (J && F32(J, 0x14) == x && F32(J, 0x18) == y && (pl = (unsigned)F32(J, 8)) != 0) return F32(pl, 0);
    }
    return 0;
}

/* lines touching (x,y): *ends = lines with an end there (zero-length ones = nodes, counted in *nodes), *through = lines
   passing through it; *other = a line other than 'self' with an end there */
static void touch_at(unsigned wl, unsigned self, int x, int y, int *ends, int *nodes, int *through, unsigned *other)
{
    int i, n = F32(wl, 0);
    *ends = *nodes = *through = 0; *other = 0;
    for (i = 1; i <= n; i++) {
        unsigned w = LI(wl, i);
        int x1, y1, x2, y2, lo, hi;
        if (!w) continue;
        x1 = F32(w, 0x24); y1 = F32(w, 0x28); x2 = x1 + F32(w, 0x2c); y2 = y1 + F32(w, 0x30);
        if (x1 == x2 && y1 == y2) { if (x1 == x && y1 == y) (*nodes)++; continue; }
        if ((x1 == x && y1 == y) || (x2 == x && y2 == y)) { (*ends)++; if (w != self && !*other) *other = w; continue; }
        if (y1 == y2 && y1 == y) { lo = x1 < x2 ? x1 : x2; hi = x1 < x2 ? x2 : x1; if (x > lo && x < hi) (*through)++; }
        if (x1 == x2 && x1 == x) { lo = y1 < y2 ? y1 : y2; hi = y1 < y2 ? y2 : y1; if (y > lo && y < hi) (*through)++; }
    }
}

/* delete the given lines without disturbing the current selection */
static void delete_wires_keep_sel(unsigned scheme, unsigned *junk, int nj)
{
    unsigned el = (unsigned)F32(scheme, 0x10), wl = (unsigned)F32(scheme, 0x18);
    static unsigned sel_e[1024], sel_w[2048];
    int ne = 0, nw = 0, i, n, k, ce = F32(el, 0x34);
    if (!nj) return;
    for (i = 1, n = F32(el, 0); i <= n; i++) {
        unsigned e = LI(el, i);
        if (!e) continue;
        if (U8(e, 0x50) && ne < 1024) sel_e[ne++] = e;
        U8(e, 0x50) = 0;
    }
    for (i = 1, n = F32(wl, 0); i <= n; i++) {
        unsigned w = LI(wl, i);
        if (!w) continue;
        if (U8(w, 0x18) && nw < 2048) sel_w[nw++] = w;
        U8(w, 0x18) = 0;
    }
    for (k = 0; k < nj; k++) U8(junk[k], 0x18) = 1;
    F32(el, 0x34) = 0; F32(wl, 0x30) = nj;
    dcall((void *)F_DELETE_SEL, scheme, 0, 0, 0, 0, 0);
    for (k = 0; k < ne; k++) U8(sel_e[k], 0x50) = 1;
    for (k = 0; k < nw; k++) U8(sel_w[k], 0x18) = 1;
    F32(el, 0x34) = ce; F32(wl, 0x30) = nw;
}

static int wire_named(unsigned w)
{
    unsigned p = (unsigned)F32(w, 0x14);
    return p && readable((void *)p, 1) && U8(p, 0) != 0;
}

/* move one end of a straight line, keeping the other end */
static void wire_move_end(unsigned w, int at_start, int nx, int ny)
{
    int x1 = F32(w, 0x24), y1 = F32(w, 0x28), x2 = x1 + F32(w, 0x2c), y2 = y1 + F32(w, 0x30);
    if (at_start) { x1 = nx; y1 = ny; } else { x2 = nx; y2 = ny; }
    F32(w, 0x24) = x1; F32(w, 0x28) = y1; F32(w, 0x2c) = x2 - x1; F32(w, 0x30) = y2 - y1;
}

/* ---- route checking: a new or changed segment must not touch anything it is not meant to connect to ---- */
#define MAXPINPTS 1024
static POINT g_pins[MAXPINPTS];
static int g_npins;
static unsigned g_excl[8];
static int g_nexcl;

static int on_seg(int px, int py, int x1, int y1, int x2, int y2)
{
    if (x1 == x2) return px == x1 && py >= (y1 < y2 ? y1 : y2) && py <= (y1 < y2 ? y2 : y1);
    if (y1 == y2) return py == y1 && px >= (x1 < x2 ? x1 : x2) && px <= (x1 < x2 ? x2 : x1);
    return 0;
}

/* segment (x1,y1)-(x2,y2) may touch other things only at (ax,ay) and (bx,by) */
static int seg_ok(unsigned wl, int x1, int y1, int x2, int y2, int ax, int ay, int bx, int by)
{
    int i, n = F32(wl, 0), k;
#define ALLOWED(px, py) (((px) == ax && (py) == ay) || ((px) == bx && (py) == by) || mv_hit((px), (py)))   /* old pin spots are being vacated */
    if (x1 == x2 && y1 == y2) return 1;
    for (k = 0; k < g_npins; k++)
        if (on_seg(g_pins[k].x, g_pins[k].y, x1, y1, x2, y2) && !ALLOWED(g_pins[k].x, g_pins[k].y)) return 0;
    for (i = 1; i <= n; i++) {
        unsigned o = LI(wl, i);
        int ox1, oy1, ox2, oy2;
        if (!o) continue;
        for (k = 0; k < g_nexcl && g_excl[k] != o; k++) ;
        if (k < g_nexcl) continue;
        ox1 = F32(o, 0x24); oy1 = F32(o, 0x28); ox2 = ox1 + F32(o, 0x2c); oy2 = oy1 + F32(o, 0x30);
        if (on_seg(ox1, oy1, x1, y1, x2, y2) && !ALLOWED(ox1, oy1)) return 0;
        if (on_seg(ox2, oy2, x1, y1, x2, y2) && !ALLOWED(ox2, oy2)) return 0;
        if (ox1 == ox2 && oy1 == oy2) continue;
        if (on_seg(x1, y1, ox1, oy1, ox2, oy2) && !ALLOWED(x1, y1)) return 0;
        if (on_seg(x2, y2, ox1, oy1, ox2, oy2) && !ALLOWED(x2, y2)) return 0;
    }
#undef ALLOWED
    return 1;
}

/* a route F -> ... -> Q of up to 3 straight segments given by its corner points */
static int route_ok(unsigned wl, const POINT *pt, int np)
{
    int k;
    for (k = 0; k + 1 < np; k++) {
        if (pt[k].x != pt[k + 1].x && pt[k].y != pt[k + 1].y) return 0;
        if (!seg_ok(wl, pt[k].x, pt[k].y, pt[k + 1].x, pt[k + 1].y, pt[0].x, pt[0].y, pt[np - 1].x, pt[np - 1].y)) return 0;
    }
    return 1;
}

/* After a move, every unselected line ending on a moved pin is brought to the pin's new place:
   - both ends on moved pins: it shifts with them;
   - the move is along the line: it stretches;
   - its far end is a plain corner (exactly one other line, nothing else there): it slides sideways with the pin and the
     neighbouring line stretches - no new segments, so repeated moves do not pile up;
   - otherwise the first of a few short orthogonal routes (jog half way, jog at the far end, jog at the pin) that touches
     nothing else is used.
   A route that would touch another pin or line is never taken (it would silently connect things); such a line is left
   where it was.  Lines squeezed to nothing are removed. */
static void mv_apply(void)
{
    unsigned scheme = g_mv.scheme, e0 = g_mv.e0, el, wl, jl;
    static unsigned done[512], touched[512], junk[256];
    int ndone = 0, ntouched = 0, njunk = 0;
    int dx, dy, i, n, k, j, found = 0, changed = 0;
    if (!g_mv.on) return;
    g_mv.on = 0;
    el = (unsigned)F32(scheme, 0x10); wl = (unsigned)F32(scheme, 0x18); jl = (unsigned)F32(scheme, 0x14);
    for (i = 1, n = F32(el, 0); i <= n; i++) if (LI(el, i) == e0) found = 1;
    if (!found) return;
    dx = F32(e0, 0x44) - g_mv.ex; dy = F32(e0, 0x48) - g_mv.ey;
    if (!dx && !dy) return;
    /* pins that routes must not touch: other elements' pins (junction table is from before the move) + moved pins */
    g_npins = 0;
    for (i = 1, n = jl ? F32(jl, 0) : 0; i <= n && g_npins < MAXPINPTS; i++) {
        unsigned J = LI(jl, i), pl;
        if (J && (pl = (unsigned)F32(J, 8)) != 0 && F32(pl, 0) > 0 && !pin_owned_by_selected(J)) {
            g_pins[g_npins].x = F32(J, 0x14); g_pins[g_npins].y = F32(J, 0x18); g_npins++;
        }
    }
    for (k = 0; k < g_mv.n && g_npins < MAXPINPTS; k++) { g_pins[g_npins].x = g_mv.p[k].x + dx; g_pins[g_npins].y = g_mv.p[k].y + dy; g_npins++; }
    /* a line running through a moved pin (rather than ending on it) is split there first */
    for (k = 0; k < g_mv.n; k++) {
        int px = g_mv.p[k].x, py = g_mv.p[k].y;
        for (i = 1, n = F32(wl, 0); i <= n; i++) {
            unsigned w = LI(wl, i), nw;
            int x1, y1, x2, y2;
            if (!w || U8(w, 0x18)) continue;
            x1 = F32(w, 0x24); y1 = F32(w, 0x28); x2 = x1 + F32(w, 0x2c); y2 = y1 + F32(w, 0x30);
            if ((x1 == px && y1 == py) || (x2 == px && y2 == py) || !on_seg(px, py, x1, y1, x2, y2)) continue;
            if ((nw = new_wire_like(scheme, w)) == 0) continue;
            F32(w, 0x2c) = px - x1; F32(w, 0x30) = py - y1;
            F32(nw, 0x24) = px; F32(nw, 0x28) = py; F32(nw, 0x2c) = x2 - px; F32(nw, 0x30) = y2 - py;
            changed = 1;
        }
    }
#define MARK(w) do { if (ndone < 512) done[ndone++] = (w); if (ntouched < 512) touched[ntouched++] = (w); } while (0)
    for (k = 0; k < g_mv.n; k++) {
        int px = g_mv.p[k].x, py = g_mv.p[k].y, qx = px + dx, qy = py + dy;
        n = F32(wl, 0);
        for (i = 1; i <= n; i++) {
            unsigned w = LI(wl, i), nb;
            int x1, y1, x2, y2, at_start, fx, fy, horiz, perp, ends, nodes, thr;
            POINT r[4];
            if (!w || U8(w, 0x18)) continue;
            for (j = 0; j < ndone && done[j] != w; j++) ;
            if (j < ndone) continue;
            x1 = F32(w, 0x24); y1 = F32(w, 0x28); x2 = x1 + F32(w, 0x2c); y2 = y1 + F32(w, 0x30);
            if (x1 == x2 && y1 == y2) {                                 /* a node on the pin travels with it */
                if (x1 == px && y1 == py) { F32(w, 0x24) = qx; F32(w, 0x28) = qy; changed = 1; MARK(w); }
                continue;
            }
            if (!((x1 == px && y1 == py) || (x2 == px && y2 == py)) || (x1 != x2 && y1 != y2)) continue;
            MARK(w);
            at_start = x1 == px && y1 == py;
            fx = at_start ? x2 : x1; fy = at_start ? y2 : y1;
            g_excl[0] = w; g_nexcl = 1;
            if (mv_hit(fx, fy)) {                                        /* both ends on moved pins: shift */
                r[0].x = fx + dx; r[0].y = fy + dy; r[1].x = qx; r[1].y = qy;
                if (route_ok(wl, r, 2)) { F32(w, 0x24) = x1 + dx; F32(w, 0x28) = y1 + dy; changed = 1; }
                continue;
            }
            horiz = y1 == y2;
            perp = horiz ? dy : dx;
            r[0].x = fx; r[0].y = fy;
            if (!perp) {                                                 /* along the line: stretch */
                r[1].x = qx; r[1].y = qy;
                if (route_ok(wl, r, 2)) { wire_move_end(w, at_start, qx, qy); changed = 1; }
                continue;
            }
            touch_at(wl, w, fx, fy, &ends, &nodes, &thr, &nb);
            if (ends == 2 && !nodes && !thr && nb && !U8(nb, 0x18) && !pins_at(scheme, fx, fy)) {
                int bx1 = F32(nb, 0x24), by1 = F32(nb, 0x28), bx2 = bx1 + F32(nb, 0x2c), by2 = by1 + F32(nb, 0x30);
                int nb_horiz = by1 == by2 && bx1 != bx2, nfx = horiz ? fx : fx + dx, nfy = horiz ? fy + dy : fy;
                int nb_start = bx1 == fx && by1 == fy, gx = nb_start ? bx2 : bx1, gy = nb_start ? by2 : by1;
                for (j = 0; j < ndone && done[j] != nb; j++) ;
                if (j == ndone && nb_horiz != horiz) {                      /* plain corner: slide sideways */
                    POINT s3[3];
                    s3[0].x = gx; s3[0].y = gy; s3[1].x = nfx; s3[1].y = nfy; s3[2].x = qx; s3[2].y = qy;
                    g_excl[1] = nb; g_nexcl = 2;
                    if (route_ok(wl, s3, 3)) {
                        wire_move_end(nb, nb_start, nfx, nfy);
                        F32(w, 0x24) = nfx; F32(w, 0x28) = nfy; F32(w, 0x2c) = qx - nfx; F32(w, 0x30) = qy - nfy;
                        MARK(nb); changed = 1;
                        continue;
                    }
                    g_nexcl = 1;
                }
            }
            {   /* candidate routes from the far end F to the new pin position Q */
                POINT c[4][4];
                int cn[4], ci, mx = (fx + qx) / 2, my = (fy + qy) / 2, got = -1;
                /* half-way jog across the line's direction */
                c[0][0] = r[0];
                if (horiz) { c[0][1].x = mx; c[0][1].y = fy; c[0][2].x = mx; c[0][2].y = qy; }
                else       { c[0][1].x = fx; c[0][1].y = my; c[0][2].x = qx; c[0][2].y = my; }
                c[0][3].x = qx; c[0][3].y = qy; cn[0] = 4;
                /* jog at the far end */
                c[1][0] = r[0];
                if (horiz) { c[1][1].x = fx; c[1][1].y = qy; } else { c[1][1].x = qx; c[1][1].y = fy; }
                c[1][2].x = qx; c[1][2].y = qy; cn[1] = 3;
                /* jog at the pin */
                c[2][0] = r[0];
                if (horiz) { c[2][1].x = qx; c[2][1].y = fy; } else { c[2][1].x = fx; c[2][1].y = qy; }
                c[2][2].x = qx; c[2][2].y = qy; cn[2] = 3;
                for (ci = 0; ci < 3 && got < 0; ci++) {
                    POINT q2[4];
                    int m = 0, t;
                    for (t = 0; t < cn[ci]; t++)                             /* drop repeated points */
                        if (!m || c[ci][t].x != q2[m - 1].x || c[ci][t].y != q2[m - 1].y) q2[m++] = c[ci][t];
                    if (m >= 2 && route_ok(wl, q2, m)) {
                        got = ci;
                        F32(w, 0x24) = q2[0].x; F32(w, 0x28) = q2[0].y;
                        F32(w, 0x2c) = q2[1].x - q2[0].x; F32(w, 0x30) = q2[1].y - q2[0].y;
                        for (t = 1; t + 1 < m; t++) {
                            unsigned nw = new_wire_like(scheme, w);
                            if (!nw) break;
                            F32(nw, 0x24) = q2[t].x; F32(nw, 0x28) = q2[t].y;
                            F32(nw, 0x2c) = q2[t + 1].x - q2[t].x; F32(nw, 0x30) = q2[t + 1].y - q2[t].y;
                            MARK(nw);
                        }
                        changed = 1;
                    }
                }
                fx_log("mv: line %x at %d,%d -> route %d" NL, w, px, py, got);
            }
        }
    }
#undef MARK
    for (j = 0; j < ntouched; j++)
        if (!F32(touched[j], 0x2c) && !F32(touched[j], 0x30) && !wire_named(touched[j]) && njunk < 256) junk[njunk++] = touched[j];
    delete_wires_keep_sel(scheme, junk, njunk);
    if (changed) {
        dcall((void *)F_REBUILD, scheme, 0, 0, 0, 0, 0);
        dcall((void *)F_AFTER_EDIT, scheme, 0, 0, 0, 0, 0);
    }
}

__declspec(dllexport) void WiresCapture(unsigned scheme) { mv_capture(scheme); }
__declspec(dllexport) void WiresApply(void) { mv_apply(); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)reason; (void)r; return TRUE; }
