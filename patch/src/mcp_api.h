/* mcp_api.h - what _afdk_remastered.dll hands to _afdk_remastered_mcp.dll (McpStart).  Both sides include it. */
#ifndef AFDK_MCP_API_H
#define AFDK_MCP_API_H

#define AFDK_MCP_API_VERSION 1

typedef struct AFDK_API {
    int version;
    HWND main_wnd;                                  /* TMainForm */
    unsigned (*active_pf)(void);                    /* active project form, 0 = none */
    int (*doc_edit_mode)(unsigned doc);             /* 1 = editor, 0 = model (simulation) mode */
    void (*hist_note)(unsigned doc);                /* record an undo step if the scheme changed */
    void (*hist_step)(int dir);                     /* -1 undo, +1 redo */
    void (*select_all)(void);
    void (*duplicate)(void);
    int (*nudge_selection)(int dx, int dy);         /* move the selection by whole cells */
    HWND (*canvas_hwnd_of)(void *cv);
    void (*set_scroll)(HWND cvw, void *cv, int hx, int vy);
    unsigned (*main_form)(void);                    /* the object with the tool buttons */
    void (*set_dialog_file)(const WCHAR *path);     /* the next Open/Save dialog returns this path without showing */
} AFDK_API;

typedef int (*MCP_START)(const AFDK_API *api);

#endif
