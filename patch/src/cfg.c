/* cfg.c - settings from _afdk_remastered.ini next to AFDK.exe (created with defaults on first start). Included from feat.c. */

static int g_cfg_line_width = 2;        /* [View] LineWidth: lines/outlines on the scheme at zoom 10, in pixels */
static int g_cfg_font_pct = 125;        /* [View] FontScale: interface font size, percent */
static char g_cfg_font[32] = "Segoe UI";/* [View] Font */
static char g_cfg_title[256];           /* [View] Title: window title instead of "AFDK 2.0 | DEVELOPED BY FIOT STUDENT" */
static int g_cfg_themes = 1;            /* [View] WindowsStyle: modern look of buttons, fields, scroll bars */
static int g_cfg_hit = 8;               /* [Edit] HitRadius: how close (pixels) a click must be to grab a line */
static int g_cfg_wire_follow = 0;       /* [Edit] WiresFollow */
static int g_cfg_autosave = 120;        /* [Edit] AutosaveSeconds: seconds, 0 = off */
static int g_cfg_codepage = 1251;       /* [System] CodePage: code page AFDK runs with, 0 = the Windows setting */

static char g_cfg_path[MAX_PATH];

static const char g_cfg_default[] =
    "; Налаштування AFDK Remastered (_afdk_remastered.dll). Зміни діють після наступного запуску програми." NL
    NL
    "[View]" NL
    "; товщина ліній і контурів елементів схеми в пікселях при звичайному масштабі (зростає разом із масштабом);" NL
    "; 1 = як в оригіналі" NL
    "LineWidth=2" NL
    "; розмір шрифту дерева бібліотеки та меню у відсотках; 100 = як в оригіналі" NL
    "FontScale=125" NL
    "; шрифт дерева бібліотеки" NL
    "Font=Segoe UI" NL
    "; назва програми в заголовку вікна, на панелі задач і у вікнах повідомлень; порожньо = як в оригіналі" NL
    "Title=" NL
    "; 1 = сучасний вигляд кнопок, полів, смуг прокрутки і діалогів (стиль Windows), 0 = як в оригіналі" NL
    "WindowsStyle=1" NL
    NL
    "[Edit]" NL
    "; на якій відстані (у пікселях) від лінії або її кінчика спрацьовує клік (видалення ПКМ, захоплення кінчика)" NL
    "HitRadius=8" NL
    "; автозбереження відкритого проєкту, секунди; 0 = вимкнено" NL
    "AutosaveSeconds=120" NL
    NL
    "[System]" NL
    "; кодування тексту програми: 1251 = кирилиця, працює навіть коли в Windows мова програм без Юнікоду англійська;" NL
    "; 0 = як налаштовано у Windows" NL
    "CodePage=1251" NL;

static int cfg_int(const char *sec, const char *key, int def, int lo, int hi)
{
    int v = (int)GetPrivateProfileIntA(sec, key, def, g_cfg_path);
    return v < lo ? lo : v > hi ? hi : v;
}

/* Title= is read by hand: the file is usually saved by Notepad as UTF-8, which the profile API would garble */
static void cfg_read_title(void)
{
    HANDLE f = CreateFileA(g_cfg_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    char *b, *p, *e;
    DWORD n, got = 0;
    WCHAR w[256];
    int k;
    g_cfg_title[0] = 0;
    if (f == INVALID_HANDLE_VALUE) return;
    n = GetFileSize(f, NULL);
    if (n == INVALID_FILE_SIZE || n > 65536 || !(b = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1))) { CloseHandle(f); return; }
    ReadFile(f, b, n, &got, NULL);
    CloseHandle(f);
    b[got] = 0;
    for (p = b; *p; p = e) {                                   /* 13 = CR, 10 = LF, 9 = TAB */
        for (e = p; *e && *e != 13 && *e != 10; e++) ;
        while (*p == ' ' || *p == 9) p++;
        if (!strncmp(p, "Title", 5)) {
            char *v = p + 5;
            while (*v == ' ' || *v == 9) v++;
            if (*v == '=') {
                v++;
                while (*v == ' ' || *v == 9) v++;
                k = (int)(e - v);
                while (k > 0 && (v[k - 1] == ' ' || v[k - 1] == 9)) k--;
                if (k > 0 && k < 255) {
                    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v, k, w, 255);
                    if (wn <= 0) wn = MultiByteToWideChar(CP_ACP, 0, v, k, w, 255);
                    if (wn > 0) WideCharToMultiByte(CP_ACP, 0, w, wn, g_cfg_title, sizeof g_cfg_title - 1, NULL, NULL);
                    g_cfg_title[sizeof g_cfg_title - 1] = 0;
                }
            }
        }
        while (*e == 13 || *e == 10) e++;
    }
    HeapFree(GetProcessHeap(), 0, b);
}

static void cfg_load(void)
{
    char *s;
    GetModuleFileNameA(NULL, g_cfg_path, MAX_PATH);
    s = strrchr(g_cfg_path, '\\');
    lstrcpyA(s ? s + 1 : g_cfg_path, "_afdk_remastered.ini");
    if (GetFileAttributesA(g_cfg_path) == INVALID_FILE_ATTRIBUTES) {      /* first start: write the defaults */
        HANDLE f = CreateFileA(g_cfg_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, g_cfg_default, sizeof g_cfg_default - 1, &w, NULL);
            CloseHandle(f);
        }
    }
    g_cfg_line_width = cfg_int("View", "LineWidth", 2, 1, 8);
    g_cfg_font_pct = cfg_int("View", "FontScale", 125, 50, 300);
    GetPrivateProfileStringA("View", "Font", "Segoe UI", g_cfg_font, sizeof g_cfg_font, g_cfg_path);
    cfg_read_title();
    g_cfg_themes = cfg_int("View", "WindowsStyle", 1, 0, 1);
    g_cfg_hit = cfg_int("Edit", "HitRadius", 8, 2, 30);
    g_cfg_wire_follow = cfg_int("Edit", "WiresFollow", 0, 0, 1);
    g_cfg_autosave = cfg_int("Edit", "AutosaveSeconds", 120, 0, 36000);
    g_cfg_codepage = cfg_int("System", "CodePage", 1251, 0, 65535);
}
