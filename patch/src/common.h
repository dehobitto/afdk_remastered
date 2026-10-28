/* common.h - helpers shared by _afdk_remastered.dll and _afdk_remastered_wires.dll for working inside AFDK.exe (Delphi 5, image base
   0x400000).  Addresses and field offsets come from the decompiled code (patch\re\all.c). */
#ifndef AFDK_COMMON_H
#define AFDK_COMMON_H

#define F32(o, off)  (*(int *)((unsigned)(o) + (off)))
#define U8(o, off)   (*(unsigned char *)((unsigned)(o) + (off)))

static int readable(const void *p, unsigned n) { return p && !IsBadReadPtr(p, n); }

/* ---------- calling Delphi 'register' functions: eax, edx, ecx, then stack args left-to-right (callee pops) ---------- */
static unsigned dcall(void *fn, unsigned a, unsigned d, unsigned c, int nst, unsigned s1, unsigned s2)
{
    unsigned r, dd = d, cc = c;
    if (nst == 0)
        __asm__ volatile("call *%[f]" : "=a"(r), "+d"(dd), "+c"(cc) : "0"(a), [f] "S"(fn) : "memory", "cc");
    else if (nst == 1)
        __asm__ volatile("push %[s1]\n\tcall *%[f]" : "=a"(r), "+d"(dd), "+c"(cc)
                         : "0"(a), [f] "S"(fn), [s1] "b"(s1) : "memory", "cc");
    else
        __asm__ volatile("push %[s1]\n\tpush %[s2]\n\tcall *%[f]" : "=a"(r), "+d"(dd), "+c"(cc)
                         : "0"(a), [f] "S"(fn), [s1] "b"(s1), [s2] "D"(s2) : "memory", "cc");
    return r;
}

/* scheme object: +0x10 elements, +0x14 junctions, +0x18 wires (lists: first dword = count, items 1-based) */
#define F_LIST_ITEM      0x45235c       /* (list, index) -> item */
#define F_REBUILD        0x4d22d4       /* (scheme) rebuild junctions after an edit (also marks the document modified) */
#define F_AFTER_EDIT     0x4d2bb0       /* (scheme) */
#define F_DESELECT_ALL   0x4d1c20       /* (scheme) */
#define F_DELETE_SEL     0x4d1bcc       /* (scheme) delete the selected items */
#define F_WIRE_NEW       0x4d3570       /* (0, classRef) */
#define A_WIRE_CLASS     0x4ce364       /* the wire class reference is stored here */

static unsigned LI(unsigned list, int i) { return dcall((void *)F_LIST_ITEM, list, (unsigned)i, 0, 0, 0, 0); }

/* a new line with the style/name of 'like', appended to the scheme's wire list (geometry is set by the caller) */
static unsigned new_wire_like(unsigned scheme, unsigned like)
{
    unsigned wl = (unsigned)F32(scheme, 0x18), w, tbl;
    w = dcall((void *)F_WIRE_NEW, 0, *(unsigned *)A_WIRE_CLASS, 0, 0, 0, 0);
    if (!w) return 0;
    dcall((void *)0x4d34e4, like, w, 0, 0, 0, 0);                                  /* copy geometry + name from 'like' */
    tbl = (unsigned)F32(wl, 0x18);
    dcall((void *)F32(tbl, 0x18), wl, w, 0, 0, 0, 0);                              /* append to the wire list */
    F32(w, 0x34) = 0; F32(w, 0x38) = 0; U8(w, 0x18) = 0;
    return w;
}

#endif
