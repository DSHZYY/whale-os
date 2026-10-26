// ============================================================================
//  OS.cpp -- 鲸鱼OS：单文件假 Windows 10 desktop (single file, Win32 + GDI+)
//
//  Compile (MSVC):
//    cl /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE OS.cpp ^
//       /link user32.lib gdiplus.lib gdi32.lib shell32.lib shlwapi.lib
//
//  Run : OS.exe           (full screen, topmost layered window)
//  Exit: Ctrl + Alt + Q
//
//  v2 highlights:
//    * Win10 style taskbar + liquid glass (3 materials)
//    * Win10 style Start menu
//    * internal multi-window manager (Baidu / Notepad / Recycle Bin / Paint / Text Doc)
//    * Baidu is an in-process mock browser: the real native taskbar never appears
//    * desktop Recycle Bin icon + right click "New Text Document"
//    * no more 80px desktop clock, glass tiles instead of white squares
//    * cached HICON -> Bitmap (no per-frame Bitmap construction, no icon leak)
//    * no MessageBox anywhere (in-UI toast), pre-computed stars
// ============================================================================

#include <windows.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <math.h>
#include <string>
#include <vector>
#include <thread>
#include <utility>
#include <cwchar>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// constants / enums
// ---------------------------------------------------------------------------
static const int TASKBAR_H = 40;
static const int TILE_X[4] = { 100, 150, 200, 250 };
static const int FONT_SIZE[8] = { 12, 14, 16, 20, 28, 40, 64, 72 };

enum
{
    ST_BOOT_BLACK = 0,
    ST_BOOT_ANIM,
    ST_LOCK,
    ST_DESKTOP,
    ST_STEAM,
    ST_BSOD
};

enum
{
    APP_BAIDU = 0,
    APP_NOTEPAD = 1,
    APP_RECYCLE = 2,
    APP_PAINT = 3,
    APP_TEXTDOC = 4,
    APP_STEAM = 5,
    APP_SETTINGS = 10
};

enum { CTX_NONE = 0, CTX_MAIN = 1, CTX_NEW = 2, CTX_WALL = 3 };

static const int PINNED_KIND[4] = { APP_BAIDU, APP_NOTEPAD, APP_PAINT, APP_RECYCLE };
static const int DESKTOP_KIND[6] = { APP_RECYCLE, APP_BAIDU, APP_NOTEPAD, APP_PAINT, APP_STEAM, APP_SETTINGS };

// ---------------------------------------------------------------------------
// small structs
// ---------------------------------------------------------------------------
struct Star { int x; int y; int r; float phase; };
struct RealApp { std::wstring name; std::wstring path; HICON hIcon; Bitmap* bmp; };
struct InkStroke { int color; std::vector<Point> pts; };

struct PosWin
{
    int kind;
    int x;
    int y;
    int w;
    int h;
    bool minimized;
    std::wstring title;
    std::wstring text;
    std::wstring url;
    std::wstring query;
    int scroll;
    int paintColor;
    std::vector<InkStroke> strokes;
};

struct CtxLayout
{
    int x;
    int y;
    int w;
    int itemH;
    int count;
};

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
static HWND g_hwnd = 0;
static HDC g_dcScr = 0;
static HDC g_dcMem = 0;
static HBITMAP g_dib = 0;
static void* g_bits = 0;
static int g_W = 0;
static int g_H = 0;
static ULONG_PTR g_gdiToken = 0;

static int g_state = ST_BOOT_BLACK;
static ULONGLONG g_stateStart = 0;
static ULONGLONG g_bsodAt = 0;

static int g_wallpaperType = 0;
static int g_materialType = 0;
static bool g_islandOn = true;

static bool g_menuOpen = false;
static float g_menuAnim = 0.0f;
static bool g_drawerOpen = false;
static bool g_bootToastDone = false;   // 开机 3 秒后的那条提醒只弹一次
static float g_drawerAnim = 0.0f;

static bool g_settingsOpen = false;
static int g_setX = 0;
static int g_setY = 0;
static bool g_calendarOpen = false;
static int g_calX = 0;
static int g_calY = 0;
static int g_calOffset = 0;

static int g_ctxOpen = CTX_NONE;
static int g_ctxX = 0;
static int g_ctxY = 0;
static int g_ctxHover = -1;

static std::vector<PosWin> g_wins;
static int g_active = -1;
static int g_paintWin = -1;
static bool g_urlFocus = false;
static int g_movingWin = -1;
static int g_dragPanel = 0;

static int g_lastX = 0;
static int g_lastY = 0;
static int g_mx = 0;
static int g_my = 0;
static int g_hoverWin = -1;
static int g_hoverBtn = -1;

static std::wstring g_toastText;
static ULONGLONG g_toastUntil = 0;

static int g_steamPct = 0;
static std::thread g_steamThread;
static bool g_steamThreadLive = false;
static bool g_quitRequested = false;
static ULONGLONG g_shutdownAt = 0;
static int g_recycleCount = 3;

static Font* g_font[2][8];
static std::vector<Star> g_stars;
static std::vector<RealApp> g_realApps;

// ---------------------------------------------------------------------------
// tiny utilities
// ---------------------------------------------------------------------------
static bool PtInRectI(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int ClampI(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static std::wstring NowTimeStr()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%02d:%02d", (int)st.wHour, (int)st.wMinute);
    return std::wstring(buf);
}

static std::wstring NowDateStr()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[24];
    std::swprintf(buf, 24, L"%04d-%02d-%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay);
    return std::wstring(buf);
}

static std::wstring TruncChars(const std::wstring& s, int maxChars)
{
    if ((int)s.size() <= maxChars) return s;
    if (maxChars <= 1) return s.substr(0, (size_t)maxChars);
    return s.substr(0, (size_t)(maxChars - 1)) + L"...";
}

static void SetToast(const std::wstring& s, int ms)
{
    g_toastText = s;
    g_toastUntil = GetTickCount64() + (ULONGLONG)ms;
    g_drawerOpen = true;          // 来消息：灵动岛向上弹出
}

static void ToastNow(const std::wstring& s)
{
    SetToast(s, 4000);
}

// ---------------------------------------------------------------------------
// GDI+ helpers
// ---------------------------------------------------------------------------
static void RRect(Graphics& g, REAL x, REAL y, REAL w, REAL h, REAL r,
                  const Brush* fill, const Pen* pen)
{
    if (w <= 0.0f || h <= 0.0f) return;
    REAL rr = r;
    if (rr * 2.0f > w) rr = w / 2.0f;
    if (rr * 2.0f > h) rr = h / 2.0f;
    GraphicsPath p;
    p.AddArc(x, y, rr * 2.0f, rr * 2.0f, 180.0f, 90.0f);
    p.AddArc(x + w - rr * 2.0f, y, rr * 2.0f, rr * 2.0f, 270.0f, 90.0f);
    p.AddArc(x + w - rr * 2.0f, y + h - rr * 2.0f, rr * 2.0f, rr * 2.0f, 0.0f, 90.0f);
    p.AddArc(x, y + h - rr * 2.0f, rr * 2.0f, rr * 2.0f, 90.0f, 90.0f);
    p.CloseFigure();
    if (fill != 0) g.FillPath(fill, &p);
    if (pen != 0) g.DrawPath(pen, &p);
}

static void TextC(Graphics& g, int fi, bool bold, const std::wstring& s, int cx, int cy, Color col)
{
    if (s.empty()) return;
    Font* f = g_font[bold ? 1 : 0][fi];
    if (f == 0) return;
    RectF box((REAL)(cx - 600), (REAL)(cy - 300), 1200.0f, 600.0f);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(col);
    g.DrawString(s.c_str(), -1, f, box, &sf, &b);
}

static void TextL(Graphics& g, int fi, bool bold, const std::wstring& s, int x, int y, Color col)
{
    if (s.empty()) return;
    Font* f = g_font[bold ? 1 : 0][fi];
    if (f == 0) return;
    RectF box((REAL)x, (REAL)y, 2400.0f, 400.0f);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentNear);
    SolidBrush b(col);
    g.DrawString(s.c_str(), -1, f, box, &sf, &b);
}

static void TextR(Graphics& g, int fi, bool bold, const std::wstring& s, int xRight, int y, Color col)
{
    if (s.empty()) return;
    Font* f = g_font[bold ? 1 : 0][fi];
    if (f == 0) return;
    RectF box((REAL)(xRight - 1200), (REAL)y, 1200.0f, 400.0f);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentFar);
    sf.SetLineAlignment(StringAlignmentNear);
    SolidBrush b(col);
    g.DrawString(s.c_str(), -1, f, box, &sf, &b);
}

static void TextW(Graphics& g, int fi, bool bold, const std::wstring& s, int& outW, int& outH)
{
    outW = 0;
    outH = 0;
    Font* f = g_font[bold ? 1 : 0][fi];
    if (f == 0 || s.empty()) return;
    RectF layout(0.0f, 0.0f, 3000.0f, 400.0f);
    RectF box;
    g.MeasureString(s.c_str(), -1, f, layout, &box);
    outW = (int)box.Width;
    outH = (int)box.Height;
}

// ---------------------------------------------------------------------------
// layout helpers -- single source of truth, drawing and hit testing share them
// ---------------------------------------------------------------------------
static int TaskbarY() { return g_H - TASKBAR_H; }

static void TileRect(int i, int& x, int& y, int& s)
{
    x = 336 + i * 40;
    y = TaskbarY();
    s = 40;
}

static void RunBtnRect(int i, int& x, int& y, int& w, int& h)
{
    // Win10 风格：宽按钮，图标 + 标题
    x = 452 + i * 186;
    y = TaskbarY();
    w = 178;
    h = 40;
}

static bool IsPinnedKind(int)
{
    // 不再有"固定磁贴"：所有窗口都按运行中按钮画在任务栏上
    return false;
}

static int FindWinByKind(int kind)
{
    int i;
    for (i = (int)g_wins.size() - 1; i >= 0; i--)
    {
        if (g_wins[i].kind == kind) return i;
    }
    return -1;
}

static int RunBtnCount()
{
    int i;
    int c = 0;
    for (i = 0; i < (int)g_wins.size(); i++)
    {
        if (!IsPinnedKind(g_wins[i].kind)) c++;
    }
    return c;
}

static int RunBtnWindow(int j)
{
    int i;
    int c = 0;
    for (i = 0; i < (int)g_wins.size(); i++)
    {
        if (IsPinnedKind(g_wins[i].kind)) continue;
        if (c == j) return i;
        c++;
    }
    return -1;
}

static void StartMenuRect(int& x, int& y, int& w, int& h)
{
    w = 640;
    h = 560;
    x = 6;
    y = TaskbarY() - h + (int)((1.0f - g_menuAnim) * 24.0f);
    if (y < 4) y = 4;
}

static void StartTileRect(int i, int& x, int& y, int& s)
{
    int mx, my, mw, mh;
    StartMenuRect(mx, my, mw, mh);
    s = 84;
    x = mx + 24 + (i % 3) * 100;
    y = my + 150 + (i / 3) * 116;
}

static void StartPowerRect(int idx, int& x, int& y, int& w, int& h)
{
    int mx, my, mw, mh;
    StartMenuRect(mx, my, mw, mh);
    w = 130;
    h = 44;
    x = mx + 24 + idx * 146;
    y = my + mh - 64;
}

static void CtxMainRect(CtxLayout& L, int px, int py)
{
    L.w = 240;
    L.itemH = 34;
    L.count = 8;
    L.x = ClampI(px, 4, g_W - L.w - 4);
    L.y = ClampI(py, 4, TaskbarY() - L.count * L.itemH - 12);
}

static void CtxSubRect(const CtxLayout& L, int anchorIndex, int subCount,
                       int& sx, int& sy, int& sw, int& sh)
{
    sw = 160;
    sh = subCount * 32 + 8;
    sx = L.x + L.w + 2;
    if (sx + sw > g_W - 4) sx = L.x - sw - 2;
    sy = L.y + anchorIndex * L.itemH;
    if (sy + sh > TaskbarY() - 4) sy = TaskbarY() - 4 - sh;
}

// ---------------------------------------------------------------------------
// small vector glyphs (no image files, no font dependency)
// ---------------------------------------------------------------------------
static void DrawWinLogo(Graphics& g, int x, int y, int s, Color col)
{
    int gap = s / 6;
    if (gap < 2) gap = 2;
    int half = (s - gap) / 2;
    SolidBrush b(col);
    g.FillRectangle(&b, (REAL)x, (REAL)y, (REAL)half, (REAL)half);
    g.FillRectangle(&b, (REAL)(x + half + gap), (REAL)y, (REAL)half, (REAL)half);
    g.FillRectangle(&b, (REAL)x, (REAL)(y + half + gap), (REAL)half, (REAL)half);
    g.FillRectangle(&b, (REAL)(x + half + gap), (REAL)(y + half + gap), (REAL)half, (REAL)half);
}

static void DrawGearGlyph(Graphics& g, int cx, int cy, int r, Color col)
{
    Pen p(col, 2.0f);
    g.DrawEllipse(&p, (REAL)(cx - r), (REAL)(cy - r), (REAL)(r * 2), (REAL)(r * 2));
    SolidBrush b(col);
    int i;
    for (i = 0; i < 8; i++)
    {
        float a = (float)i * 3.14159265f / 4.0f;
        int tx = cx + (int)(cosf(a) * (float)(r + 3));
        int ty = cy + (int)(sinf(a) * (float)(r + 3));
        g.FillRectangle(&b, (REAL)(tx - 2), (REAL)(ty - 2), 4.0f, 4.0f);
    }
    g.FillEllipse(&b, (REAL)(cx - 2), (REAL)(cy - 2), 4.0f, 4.0f);
}

static void DrawEnvelopeGlyph(Graphics& g, int x, int y, Color col)
{
    Pen p(col, 1.5f);
    g.DrawRectangle(&p, (REAL)x, (REAL)y, 18.0f, 12.0f);
    g.DrawLine(&p, (REAL)x, (REAL)y, (REAL)(x + 9), (REAL)(y + 7));
    g.DrawLine(&p, (REAL)(x + 18), (REAL)y, (REAL)(x + 9), (REAL)(y + 7));
}

static void DrawTrashGlyph(Graphics& g, int cx, int cy, int s, Color col)
{
    SolidBrush b(col);
    int bw = s * 34 / 100;
    int bh = s * 40 / 100;
    int bx = cx - bw / 2;
    int by = cy - bh / 2 + s * 6 / 100;
    g.FillRectangle(&b, (REAL)(cx - s * 21 / 100), (REAL)(by - s * 6 / 100), (REAL)(s * 42 / 100), (REAL)(s * 6 / 100));
    RRect(g, (REAL)bx, (REAL)by, (REAL)bw, (REAL)bh, 3.0f, &b, 0);
    SolidBrush w(Color(255, 255, 255, 255));
    int i;
    for (i = 1; i <= 2; i++)
    {
        int lx = bx + bw * i / 3;
        g.FillRectangle(&w, (REAL)lx, (REAL)(by + 6), 2.0f, (REAL)(bh - 12));
    }
}

static void DrawPageGlyph(Graphics& g, int cx, int cy, int s, Color col)
{
    GraphicsPath p;
    int w = s * 36 / 100;
    int h = s * 46 / 100;
    int x = cx - w / 2;
    int y = cy - h / 2;
    int fold = s * 10 / 100;
    p.AddLine((REAL)x, (REAL)y, (REAL)(x + w - fold), (REAL)y);
    p.AddLine((REAL)(x + w - fold), (REAL)y, (REAL)(x + w), (REAL)(y + fold));
    p.AddLine((REAL)(x + w), (REAL)(y + fold), (REAL)(x + w), (REAL)(y + h));
    p.AddLine((REAL)(x + w), (REAL)(y + h), (REAL)x, (REAL)(y + h));
    p.CloseFigure();
    SolidBrush b(col);
    g.FillPath(&b, &p);
    Pen ln(Color(255, 120, 132, 152), 1.5f);
    g.DrawLine(&ln, (REAL)(x + 4), (REAL)(y + h / 2), (REAL)(x + w - 5), (REAL)(y + h / 2));
    g.DrawLine(&ln, (REAL)(x + 4), (REAL)(y + h / 2 + 6), (REAL)(x + w - 5), (REAL)(y + h / 2 + 6));
    g.DrawLine(&ln, (REAL)(x + 4), (REAL)(y + h / 2 + 12), (REAL)(x + w - 9), (REAL)(y + h / 2 + 12));
}

static void DrawGlyph(Graphics& g, int kind, int cx, int cy, int s)
{
    if (kind == APP_BAIDU)
    {
        TextC(g, 5, true, L"B", cx, cy, Color(255, 40, 82, 232));
    }
    else if (kind == APP_NOTEPAD)
    {
        DrawPageGlyph(g, cx, cy, s, Color(245, 250, 252, 255));
    }
    else if (kind == APP_RECYCLE)
    {
        DrawTrashGlyph(g, cx, cy, s, Color(255, 226, 232, 242));
    }
    else if (kind == APP_PAINT)
    {
        SolidBrush b(Color(255, 244, 247, 252));
        g.FillEllipse(&b, (REAL)(cx - s * 42 / 100), (REAL)(cy - s * 42 / 100), (REAL)(s * 84 / 100), (REAL)(s * 84 / 100));
        SolidBrush d1(Color(255, 228, 70, 70));
        SolidBrush d2(Color(255, 70, 150, 240));
        SolidBrush d3(Color(255, 90, 200, 110));
        SolidBrush d4(Color(255, 250, 190, 60));
        g.FillEllipse(&d1, (REAL)(cx - s * 20 / 100), (REAL)(cy - s * 17 / 100), (REAL)(s * 11 / 100), (REAL)(s * 11 / 100));
        g.FillEllipse(&d2, (REAL)(cx + s * 4 / 100), (REAL)(cy - s * 19 / 100), (REAL)(s * 11 / 100), (REAL)(s * 11 / 100));
        g.FillEllipse(&d3, (REAL)(cx - s * 8 / 100), (REAL)(cy + s * 5 / 100), (REAL)(s * 11 / 100), (REAL)(s * 11 / 100));
        g.FillEllipse(&d4, (REAL)(cx + s * 10 / 100), (REAL)(cy + s * 8 / 100), (REAL)(s * 11 / 100), (REAL)(s * 11 / 100));
    }
    else if (kind == APP_TEXTDOC)
    {
        DrawPageGlyph(g, cx, cy, s, Color(245, 250, 252, 255));
    }
    else if (kind == APP_STEAM)
    {
        TextC(g, 5, true, L"S", cx, cy, Color(255, 108, 168, 240));
    }
    else if (kind == APP_SETTINGS)
    {
        DrawGearGlyph(g, cx, cy, s * 20 / 100, Color(255, 226, 232, 242));
    }
}

static void DrawGlassTile(Graphics& g, int x, int y, int s, bool sel, bool hot)
{
    int r = s / 5;
    if (r < 4) r = 4;

    // 3D feel: soft drop shadow under the tile
    SolidBrush sh(Color(60, 0, 0, 0));
    RRect(g, (REAL)x, (REAL)(y + 3), (REAL)s, (REAL)s, (REAL)r, &sh, 0);

    Color topC(190, 255, 255, 255);
    Color botC(120, 226, 236, 250);
    if (g_materialType == 1)
    {
        topC = Color(255, 246, 248, 252);
        botC = Color(255, 226, 232, 242);
    }
    else if (g_materialType == 2)
    {
        topC = Color(255, 72, 78, 90);
        botC = Color(255, 50, 54, 62);
    }
    if (hot)
    {
        topC = Color(235, 255, 255, 255);
        botC = Color(180, 236, 244, 255);
    }
    LinearGradientBrush lb(PointF((REAL)x, (REAL)y), PointF((REAL)x, (REAL)(y + s)), topC, botC);
    Pen bd(sel ? Color(255, 90, 170, 255) : Color(120, 255, 255, 255), sel ? 2.0f : 1.0f);
    RRect(g, (REAL)x, (REAL)y, (REAL)s, (REAL)s, (REAL)r, &lb, &bd);

    // inner top highlight
    Pen hl(Color(150, 255, 255, 255), 1.0f);
    g.DrawLine(&hl, (REAL)(x + r), (REAL)(y + 1), (REAL)(x + s - r), (REAL)(y + 1));
}

// ---------------------------------------------------------------------------
// names / small layout helpers used by drawing and hit testing
// ---------------------------------------------------------------------------
static std::wstring DesktopKindName(int kind)
{
    if (kind == APP_BAIDU) return L"百度";
    if (kind == APP_NOTEPAD) return L"记事本";
    if (kind == APP_RECYCLE) return L"回收站";
    if (kind == APP_PAINT) return L"画图";
    if (kind == APP_TEXTDOC) return L"文本文档";
    if (kind == APP_STEAM) return L"蒸汽";
    if (kind == APP_SETTINGS) return L"设置";
    return L"应用";
}

static std::wstring WinShortName(int kind)
{
    if (kind == APP_BAIDU) return L"网页";
    if (kind == APP_NOTEPAD) return L"记事";
    if (kind == APP_RECYCLE) return L"回收";
    if (kind == APP_PAINT) return L"画图";
    if (kind == APP_TEXTDOC) return L"文档";
    if (kind == APP_STEAM) return L"蒸汽";
    if (kind == APP_SETTINGS) return L"设置";
    return L"窗口";
}

static std::wstring MonthName(int mon)
{
    static const wchar_t* names[12] = {
        L"一月", L"二月", L"三月", L"四月", L"五月", L"六月",
        L"七月", L"八月", L"九月", L"十月", L"十一月", L"十二月"
    };
    if (mon < 0) mon = 0;
    if (mon > 11) mon = 11;
    return std::wstring(names[mon]);
}

static int DaysInMonth(int year, int mon0)
{
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon0 < 0) mon0 = 0;
    if (mon0 > 11) mon0 = 11;
    int d = dm[mon0];
    if (mon0 == 1)
    {
        bool leap = ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0));
        if (leap) d = 29;
    }
    return d;
}

static void DesktopIconRect(int i, int& x, int& y, int& s)
{
    s = 44;
    x = 36 + (i % 3) * 112;
    y = 64 + (i / 3) * 104;
}

// 竖版灵动岛：贴在开始按钮上方；来消息向上长高，点一下已读收起
static const int ISLE_W = 132;
static const int ISLE_BASE_H = 214;
static const int ISLE_MSG_H = 122;

static int IslandMsgHeight()
{
    int h = (int)(g_drawerAnim * (float)ISLE_MSG_H);
    if (h < 0) h = 0;
    if (h > ISLE_MSG_H) h = ISLE_MSG_H;
    return h;
}

static void IslandCapsule(int& x, int& y, int& w, int& h)
{
    w = ISLE_W;
    h = ISLE_BASE_H + IslandMsgHeight();
    x = 14;
    y = TaskbarY() - h - 12;
}

static void IslandBtnRect(int i, int& x, int& y, int& s)
{
    int cx, cy, cw, ch;
    IslandCapsule(cx, cy, cw, ch);
    s = 28;
    int block = 4 * s + 3 * 6;
    int top = cy + ch - 12 - block;
    x = cx + (cw - s) / 2;
    y = top + i * (s + 6);
}

static void CalGridCell(int r, int c, int& x, int& y)
{
    x = g_calX + 10 + c * 40;
    y = g_calY + 92 + r * 38;
}

// ---------------------------------------------------------------------------
// wallpaper
// ---------------------------------------------------------------------------
static void RenderWallpaper(Graphics& g, ULONGLONG tick)
{
    float t = (float)(tick % 3600000ULL) / 1000.0f;

    if (g_wallpaperType == 0)
    {
        // 流光（蓝紫）：深蓝底 + 斜向漂移的青蓝光带
        SolidBrush bg(Color(255, 7, 10, 24));
        g.FillRectangle(&bg, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);

        int i;
        for (i = 0; i < 4; i++)
        {
            float ph = t * (0.33f + 0.11f * (float)i) + (float)i * 1.7f;
            int bx = (int)((sinf(ph) * 0.5f + 0.5f) * (float)(g_W + 700)) - 350;
            int by = g_H / 8 + i * (g_H / 6);
            PointF pts[4];
            pts[0].X = (REAL)bx;         pts[0].Y = (REAL)by;
            pts[1].X = (REAL)(bx + 620); pts[1].Y = (REAL)(by - 150);
            pts[2].X = (REAL)(bx + 665); pts[2].Y = (REAL)(by - 66);
            pts[3].X = (REAL)(bx + 45);  pts[3].Y = (REAL)(by + 84);
            SolidBrush band(Color(22 + i * 8, 96 + i * 12, 150 + i * 10, 246));
            g.FillPolygon(&band, pts, 4);
        }
        SolidBrush glow(Color(34, 92, 140, 240));
        g.FillEllipse(&glow, (REAL)(g_W / 3), (REAL)(g_H / 3), 720.0f, 380.0f);
    }
    else if (g_wallpaperType == 1)
    {
        // 夜空 v3：渐变天光 + 月亮柔光 + 星点 + 四层雾化山脊 + 底部压暗
        int horizonY = TaskbarY();

        LinearGradientBrush sky(PointF(0.0f, 0.0f), PointF(0.0f, (REAL)horizonY),
            Color(255, 11, 20, 44), Color(255, 88, 114, 148));
        g.FillRectangle(&sky, 0.0f, 0.0f, (REAL)g_W, (REAL)horizonY);

        // 星星：固定伪随机位置，只做轻微闪烁
        int x;
        for (x = 0; x < 120; x++)
        {
            float sx = (float)((x * 97 + (x * x % 61) * 7) % (g_W > 1 ? g_W : 1));
            float sy = (float)((x * 53 + (x * x % 37) * 11) % (int)((float)horizonY * 0.62f));
            int a = 70 + (int)(70.0f * sinf(t * 1.7f + (float)x));
            if (a < 30) a = 30;
            if (a > 170) a = 170;
            SolidBrush st(Color((BYTE)a, 255, 255, 255));
            g.FillEllipse(&st, sx, sy, 2.0f, 2.0f);
        }

        // 月亮 + 柔光（多层低透明度圆叠加，避免出现同心圆硬边）
        float mx = (float)g_W * 0.76f;
        float my = (float)g_H * 0.18f;
        // 柔光用径向渐变一次画完，避免一圈圈的硬边
        GraphicsPath haloPath;
        haloPath.AddEllipse(mx - 150.0f, my - 150.0f, 300.0f, 300.0f);
        PathGradientBrush haloBrush(&haloPath);
        haloBrush.SetCenterPoint(PointF(mx, my));
        haloBrush.SetCenterColor(Color(160, 206, 224, 250));
        Color haloEdge(0, 206, 224, 250);
        INT haloCount = 1;
        haloBrush.SetSurroundColors(&haloEdge, &haloCount);
        g.FillPath(&haloBrush, &haloPath);
        SolidBrush core(Color(246, 250, 252, 255));
        g.FillEllipse(&core, mx - 13.0f, my - 13.0f, 26.0f, 26.0f);

        // 四层山脊：远亮近暗
        const int NLAYER = 4;
        const float peakF[NLAYER] = { 0.44f, 0.56f, 0.68f, 0.80f };
        const int colR[NLAYER] = { 88, 70, 38, 15 };
        const int colG[NLAYER] = { 114, 96, 58, 25 };
        const int colB[NLAYER] = { 148, 130, 88, 42 };
        const float phs[NLAYER] = { 0.0f, 1.6f, 3.1f, 4.4f };
        int li, pi;
        for (li = 0; li < NLAYER; li++)
        {
            float peakY = (float)g_H * peakF[li];
            float ph = phs[li];
            int n = 44;
            std::vector<PointF> ridge;
            ridge.reserve((size_t)(n + 3));
            for (pi = 0; pi <= n; pi++)
            {
                float fx = (float)pi / (float)n;
                float u = 0.5f
                        + 0.34f * sinf(fx * 3.1f + ph)
                        + 0.15f * sinf(fx * 7.3f + ph * 1.6f)
                        + 0.10f * sinf(fx * 16.7f + ph * 2.7f);
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                ridge.push_back(PointF(fx * (float)g_W, peakY + ((float)horizonY - peakY) * u));
            }

            std::vector<PointF> poly(ridge);
            poly.push_back(PointF((REAL)g_W, (REAL)g_H));
            poly.push_back(PointF(0.0f, (REAL)g_H));
            SolidBrush mtn(Color(255, (BYTE)colR[li], (BYTE)colG[li], (BYTE)colB[li]));
            g.FillPolygon(&mtn, &poly[0], (INT)poly.size());

            // 层间薄雾：贴着地平的横向雾带
            SolidBrush mist(Color((BYTE)(16 + li * 5), 196, 214, 238));
            g.FillRectangle(&mist, 0.0f, (REAL)(horizonY - 44 + li * 10), (REAL)g_W, 7.0f);
        }

        // 底部压暗，让任务栏和桌面图标浮起来
        LinearGradientBrush bottom(PointF(0.0f, (REAL)(horizonY - 200)), PointF(0.0f, (REAL)horizonY),
            Color(0, 4, 8, 16), Color(120, 3, 6, 12));
        g.FillRectangle(&bottom, 0.0f, (REAL)(horizonY - 200), (REAL)g_W, 200.0f);
    }
    else
    {
        // aurora + stars
        SolidBrush bg(Color(255, 6, 8, 18));
        g.FillRectangle(&bg, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);

        // milky way band
        SolidBrush mw(Color(18, 120, 90, 200));
        g.FillEllipse(&mw, (REAL)(-g_W / 8), (REAL)(g_H / 5), (REAL)(g_W * 5 / 4), (REAL)(g_H / 3));

        // moon
        SolidBrush moon(Color(235, 236, 242, 252));
        g.FillEllipse(&moon, (REAL)(g_W - 190), (REAL)(g_H / 5), 70.0f, 70.0f);

        // pre-computed stars, only twinkle here (no srand in the hot path)
        int i;
        for (i = 0; i < (int)g_stars.size(); i++)
        {
            int a = 100 + (int)(80.0f * sinf(t * 2.0f + g_stars[i].phase));
            if (a < 30) a = 30;
            if (a > 235) a = 235;
            SolidBrush sb(Color((BYTE)a, 255, 255, 255));
            g.FillEllipse(&sb, (REAL)g_stars[i].x, (REAL)g_stars[i].y, (REAL)g_stars[i].r, (REAL)g_stars[i].r);
        }
    }
}

// ---------------------------------------------------------------------------
// desktop icons
// ---------------------------------------------------------------------------
static void RenderDesktopIcons(Graphics& g)
{
    int i;
    for (i = 0; i < 6; i++)
    {
        int x, y, s;
        DesktopIconRect(i, x, y, s);
        bool hot = PtInRectI(g_mx, g_my, x, y, s, s + 18);
        if (hot)
        {
            SolidBrush hb(Color(40, 255, 255, 255));
            RRect(g, (REAL)(x - 2), (REAL)(y - 2), (REAL)(s + 4), (REAL)(s + 24), 4.0f, &hb, 0);
        }
        DrawGlyph(g, DESKTOP_KIND[i], x + s / 2, y + s / 2, s);
        TextC(g, 0, false, DesktopKindName(DESKTOP_KIND[i]), x + s / 2, y + s + 10, Color(235, 255, 255, 255));
    }

    for (i = 0; i < (int)g_realApps.size(); i++)
    {
        int k = 6 + i;
        int x, y, s;
        DesktopIconRect(k, x, y, s);
        if (y > TaskbarY() - 90) break;
        bool hot = PtInRectI(g_mx, g_my, x, y, s, s + 18);
        if (hot)
        {
            SolidBrush hb(Color(40, 255, 255, 255));
            RRect(g, (REAL)(x - 2), (REAL)(y - 2), (REAL)(s + 4), (REAL)(s + 24), 4.0f, &hb, 0);
        }
        if (g_realApps[i].bmp != 0)
        {
            g.DrawImage(g_realApps[i].bmp, (REAL)(x + 6), (REAL)(y + 6), 32.0f, 32.0f);
        }
        else
        {
            TextC(g, 1, true, L"?", x + s / 2, y + s / 2, Color(255, 255, 255, 255));
        }
        TextC(g, 0, false, TruncChars(g_realApps[i].name, 10), x + s / 2, y + s + 10, Color(235, 255, 255, 255));
    }
}

// ---------------------------------------------------------------------------
// taskbar (Win10 + liquid glass)
// ---------------------------------------------------------------------------
static void RenderTaskbar(Graphics& g)
{
    int ty = TaskbarY();
    Color colBg;
    Color colFg;
    Color colSub;
    if (g_materialType == 2)
    {
        colBg = Color(255, 32, 32, 32);
        colFg = Color(255, 245, 245, 245);
        colSub = Color(200, 200, 200, 200);
    }
    else if (g_materialType == 1)
    {
        colBg = Color(255, 243, 243, 243);
        colFg = Color(255, 30, 30, 30);
        colSub = Color(190, 90, 90, 90);
    }
    else
    {
        colBg = Color(232, 243, 244, 246);
        colFg = Color(255, 30, 30, 30);
        colSub = Color(190, 90, 90, 90);
    }

    SolidBrush bg(colBg);
    g.FillRectangle(&bg, 0.0f, (REAL)ty, (REAL)g_W, (REAL)TASKBAR_H);
    Pen line(Color(255, 218, 218, 218), 1.0f);
    g.DrawLine(&line, 0.0f, (REAL)(ty + TASKBAR_H - 1), (REAL)g_W, (REAL)(ty + TASKBAR_H - 1));

    // 开始按钮
    if (PtInRectI(g_mx, g_my, 2, ty, 40, TASKBAR_H) || g_menuOpen)
    {
        SolidBrush hb(Color(40, 0, 0, 0));
        g.FillRectangle(&hb, (REAL)2, (REAL)ty, 40.0f, (REAL)TASKBAR_H);
    }
    DrawWinLogo(g, 11, ty + 11, 18, colFg);

    // 搜索框
    {
        int sx = 46;
        int sy = ty + 6;
        int sw = 236;
        int sh = 28;
        if (g_materialType == 2)
        {
            SolidBrush d2(Color(255, 54, 54, 54));
            RRect(g, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, 4.0f, &d2, 0);
        }
        else
        {
            SolidBrush sb(Color(255, 255, 255, 255));
            RRect(g, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, 4.0f, &sb, 0);
        }
        Pen sp(colSub, 1.0f);
        g.DrawEllipse(&sp, (REAL)(sx + 10), (REAL)(sy + 8), 10.0f, 10.0f);
        g.DrawLine(&sp, (REAL)(sx + 19), (REAL)(sy + 17), (REAL)(sx + 23), (REAL)(sy + 21));
        TextL(g, 0, false, L"在此输入你要搜索的内容", sx + 32, sy + 6, colSub);
    }

    // 任务视图
    {
        if (PtInRectI(g_mx, g_my, 290, ty, 40, TASKBAR_H))
        {
            SolidBrush hb(Color(40, 0, 0, 0));
            g.FillRectangle(&hb, (REAL)290, (REAL)ty, 40.0f, (REAL)TASKBAR_H);
        }
        Pen tp(colFg, 1.4f);
        g.DrawRectangle(&tp, (REAL)299, (REAL)(ty + 11), 15.0f, 11.0f);
        g.DrawRectangle(&tp, (REAL)305, (REAL)(ty + 17), 15.0f, 11.0f);
    }

    // 非固定窗口按钮
    int i;
    int rc = RunBtnCount();
    for (i = 0; i < rc; i++)
    {
        int wi = RunBtnWindow(i);
        if (wi < 0) break;
        int x, y, w, h;
        RunBtnRect(i, x, y, w, h);
        bool act = (wi == g_active && !g_wins[wi].minimized);
        bool hot = PtInRectI(g_mx, g_my, x, y, w, h);
        if (hot || act)
        {
            SolidBrush hb(Color(34, 0, 0, 0));
            g.FillRectangle(&hb, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
        }
        if (act)
        {
            SolidBrush ab(Color(46, 255, 255, 255));
            g.FillRectangle(&ab, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
        }
        DrawGlyph(g, g_wins[wi].kind, x + 20, y + h / 2 - 1, 22);
        TextL(g, 0, false, TruncChars(WinShortName(g_wins[wi].kind), 8), x + 40, y + 12,
              act ? colFg : colSub);
        if (act)
        {
            SolidBrush acc(Color(255, 0, 120, 215));
            g.FillRectangle(&acc, (REAL)(x + 8), (REAL)(y + h - 3), (REAL)(w - 16), 2.5f);
        }
    }

    // 托盘：时间/日期
    int rx = g_W - 12;
    {
        std::wstring ts = NowTimeStr();
        std::wstring ds = NowDateStr();
        int tw = 0;
        int th = 0;
        int dw = 0;
        int dh = 0;
        TextW(g, 0, false, ts, tw, th);
        TextW(g, 0, false, ds, dw, dh);
        TextR(g, 0, false, ts, rx, ty + 4, colFg);
        TextR(g, 0, false, ds, rx, ty + 21, colFg);
        rx -= ((tw > dw) ? tw : dw) + 14;
    }
    // 输入法
    TextR(g, 0, false, L"中", rx, ty + 12, colSub);
    rx -= 26;
    // 音量
    {
        Pen vp(colSub, 1.4f);
        g.DrawRectangle(&vp, (REAL)(rx - 16), (REAL)(ty + 16), 6.0f, 8.0f);
        g.DrawLine(&vp, (REAL)(rx - 10), (REAL)(ty + 16), (REAL)(rx - 4), (REAL)(ty + 11));
        g.DrawLine(&vp, (REAL)(rx - 4), (REAL)(ty + 11), (REAL)(rx - 4), (REAL)(ty + 29));
        g.DrawLine(&vp, (REAL)(rx - 4), (REAL)(ty + 29), (REAL)(rx - 10), (REAL)(ty + 24));
        rx -= 28;
    }
    // 网络
    {
        Pen np(colSub, 1.4f);
        int cx = rx - 10;
        int cy = ty + 26;
        g.DrawArc(&np, (REAL)(cx - 11), (REAL)(cy - 15), 22.0f, 22.0f, 205.0f, 130.0f);
        g.DrawArc(&np, (REAL)(cx - 6), (REAL)(cy - 10), 12.0f, 12.0f, 205.0f, 130.0f);
        SolidBrush nb(colSub);
        g.FillEllipse(&nb, (REAL)(cx - 2), (REAL)(cy - 3), 4.0f, 4.0f);
        rx -= 30;
    }
    // 信封
    DrawEnvelopeGlyph(g, rx - 18, ty + 14, colSub);
    rx -= 30;
    // 展开箭头
    {
        Pen ap(colSub, 1.4f);
        g.DrawLine(&ap, (REAL)(rx - 15), (REAL)(ty + 16), (REAL)(rx - 9), (REAL)(ty + 22));
        g.DrawLine(&ap, (REAL)(rx - 9), (REAL)(ty + 22), (REAL)(rx - 3), (REAL)(ty + 16));
    }
}

// ---------------------------------------------------------------------------
// start menu (Win10 style)
// ---------------------------------------------------------------------------
static void DrawMenuTile(Graphics& g, int x, int y, int s, int kind, bool hot)
{
    Color c(255, 0, 120, 215);
    if (kind == APP_BAIDU) c = Color(255, 40, 82, 232);
    else if (kind == APP_NOTEPAD) c = Color(255, 240, 188, 58);
    else if (kind == APP_RECYCLE) c = Color(255, 78, 168, 110);
    else if (kind == APP_PAINT) c = Color(255, 232, 104, 62);
    else if (kind == APP_TEXTDOC) c = Color(255, 120, 132, 150);
    else if (kind == APP_STEAM) c = Color(255, 26, 36, 58);
    else if (kind == APP_SETTINGS) c = Color(255, 92, 98, 112);
    SolidBrush b(c);
    RRect(g, (REAL)x, (REAL)y, (REAL)s, (REAL)s, 4.0f, &b, 0);
    if (hot)
    {
        Pen hp(Color(220, 255, 255, 255), 2.0f);
        RRect(g, (REAL)x, (REAL)y, (REAL)s, (REAL)s, 4.0f, 0, &hp);
    }
    DrawGlyph(g, kind, x + s / 2, y + s / 2 - 4, s);
}
static void RenderStartMenu(Graphics& g)
{
    if (!g_menuOpen && g_menuAnim < 0.01f) return;
    int mx, my, mw, mh;
    StartMenuRect(mx, my, mw, mh);
    int a = (int)(255.0f * g_menuAnim);
    if (a < 4) return;

    SolidBrush sh(Color(70, 0, 0, 0));
    RRect(g, (REAL)(mx + 4), (REAL)(my + 8), (REAL)mw, (REAL)mh, 8.0f, &sh, 0);

    Color topC;
    Color botC;
    if (g_materialType == 0)
    {
        topC = Color(240, 252, 254, 255);
        botC = Color(228, 232, 240, 252);
    }
    else if (g_materialType == 1)
    {
        topC = Color(255, 248, 250, 254);
        botC = Color(255, 236, 240, 248);
    }
    else
    {
        topC = Color(255, 44, 48, 58);
        botC = Color(255, 34, 37, 44);
    }
    LinearGradientBrush lb(PointF((REAL)mx, (REAL)my), PointF((REAL)mx, (REAL)(my + mh)), topC, botC);
    Pen bd(Color(130, 255, 255, 255), 1.0f);
    RRect(g, (REAL)mx, (REAL)my, (REAL)mw, (REAL)mh, 8.0f, &lb, &bd);

    Color fg;
    Color fg2;
    if (g_materialType == 2)
    {
        fg = Color(255, 226, 232, 242);
        fg2 = Color(190, 160, 175, 198);
    }
    else
    {
        fg = Color(255, 34, 44, 62);
        fg2 = Color(190, 80, 90, 110);
    }

    // user area
    SolidBrush av(Color(255, 74, 120, 200));
    g.FillEllipse(&av, (REAL)(mx + 24), (REAL)(my + 24), 64.0f, 64.0f);
    TextC(g, 2, true, L"鲸鱼OS", mx + 56, my + 56, Color(255, 255, 255, 255));
    TextL(g, 2, true, L"本机用户", mx + 102, my + 34, fg);
    TextL(g, 0, false, L"假桌面 v2", mx + 102, my + 58, fg2);
    TextL(g, 1, false, L"已固定", mx + 24, my + 110, fg2);

    int i;
    for (i = 0; i < 6; i++)
    {
        int tx, ty, ts;
        StartTileRect(i, tx, ty, ts);
        bool hot = PtInRectI(g_mx, g_my, tx, ty, ts, ts);
        DrawMenuTile(g, tx, ty, ts, DESKTOP_KIND[i], hot);
        TextC(g, 0, false, DesktopKindName(DESKTOP_KIND[i]), tx + ts / 2, ty + ts - 14, fg);
    }

    // quick access list
    TextL(g, 1, false, L"常用", mx + 360, my + 110, fg2);
    static const wchar_t* listLabel[4] = {
        L"打开回收站", L"新建文本文档", L"更换壁纸", L"关于鲸鱼OS"
    };
    for (i = 0; i < 4; i++)
    {
        int lx = mx + 360;
        int ly = my + 138 + i * 44;
        if (PtInRectI(g_mx, g_my, lx, ly, 256, 40))
        {
            SolidBrush hb(Color(60, 255, 255, 255));
            RRect(g, (REAL)lx, (REAL)ly, 256.0f, 40.0f, 6.0f, &hb, 0);
        }
        TextL(g, 1, false, listLabel[i], lx + 14, ly + 10, fg);
    }

    // power row
    for (i = 0; i < 2; i++)
    {
        int px, py, pw, ph;
        StartPowerRect(i, px, py, pw, ph);
        bool hot = PtInRectI(g_mx, g_my, px, py, pw, ph);
        SolidBrush pb(Color(hot ? 90 : 50, 255, 255, 255));
        Pen pbd(Color(120, 255, 255, 255), 1.0f);
        RRect(g, (REAL)px, (REAL)py, (REAL)pw, (REAL)ph, 6.0f, &pb, &pbd);
        TextC(g, 1, false, (i == 0) ? L"重启" : L"关机", px + pw / 2, py + ph / 2, fg);
    }
}

// ---------------------------------------------------------------------------
// dynamic island (bottom left) -- horizontal drawer, never a dropdown
// ---------------------------------------------------------------------------
static void RenderIsland(Graphics& g)
{
    if (!g_islandOn) return;
    if (g_state != ST_DESKTOP && g_state != ST_STEAM) return;

    int cx, cy, cw, ch;
    IslandCapsule(cx, cy, cw, ch);

    SolidBrush sh(Color(56, 0, 0, 0));
    RRect(g, (REAL)(cx + 3), (REAL)(cy + 5), (REAL)cw, (REAL)ch, 26.0f, &sh, 0);

    LinearGradientBrush lb(PointF((REAL)cx, (REAL)cy), PointF((REAL)cx, (REAL)(cy + ch)),
        Color(232, 22, 28, 44), Color(240, 7, 9, 18));
    Pen bd(Color(92, 255, 255, 255), 1.0f);
    RRect(g, (REAL)cx, (REAL)cy, (REAL)cw, (REAL)ch, 26.0f, &lb, &bd);

    int msgH = IslandMsgHeight();
    if (msgH > 8)
    {
        std::wstring msg = g_toastText;
        if (msg.empty()) msg = L"暂无新消息";
        TextC(g, 0, false, TruncChars(msg, 9), cx + cw / 2, cy + 12 + msgH / 2 - 6,
              Color(235, 236, 244, 255));
        Pen sep(Color(60, 255, 255, 255), 1.0f);
        g.DrawLine(&sep, (REAL)(cx + 16), (REAL)(cy + msgH + 8),
                   (REAL)(cx + cw - 16), (REAL)(cy + msgH + 8));
    }

    int btnTop = cy + ch - 12 - (4 * 28 + 3 * 6);
    int gapTop = cy + msgH + 10;
    int timeY = gapTop + (btnTop - gapTop) / 2;
    TextC(g, 1, true, NowTimeStr(), cx + cw / 2, timeY, Color(255, 240, 246, 255));
    TextC(g, 0, false, NowDateStr(), cx + cw / 2, timeY + 20, Color(190, 208, 220, 245));

    int i;
    for (i = 0; i < 4; i++)
    {
        int bx, by, bs;
        IslandBtnRect(i, bx, by, bs);
        bool hot = PtInRectI(g_mx, g_my, bx, by, bs, bs);
        SolidBrush bb(Color(hot ? 96 : 42, 255, 255, 255));
        Pen bbp(Color(hot ? 130 : 80, 255, 255, 255), 1.0f);
        RRect(g, (REAL)bx, (REAL)by, (REAL)bs, (REAL)bs, 10.0f, &bb, &bbp);
        if (i == 0) DrawWinLogo(g, bx + 5, by + 5, 18, Color(255, 236, 242, 252));
        else if (i == 1) DrawGearGlyph(g, bx + bs / 2, by + bs / 2, 8, Color(255, 236, 242, 252));
        else if (i == 2) DrawTrashGlyph(g, bx + bs / 2, by + bs / 2, 22, Color(255, 236, 242, 252));
        else
        {
            Pen cp(Color(255, 236, 242, 252), 1.4f);
            g.DrawRectangle(&cp, (REAL)(bx + 8), (REAL)(by + 6), 12.0f, 12.0f);
            g.DrawLine(&cp, (REAL)(bx + 8), (REAL)(by + 10), (REAL)(bx + 20), (REAL)(by + 10));
        }
    }
}

// ---------------------------------------------------------------------------
// window geometry helpers (all drawing + hit testing go through these)
// ---------------------------------------------------------------------------
static void WinTitleRect(const PosWin& w, int& x, int& y, int& ww, int& hh)
{
    x = w.x;
    y = w.y;
    ww = w.w;
    hh = 34;
}

static void WinCloseRect(const PosWin& w, int& x, int& y, int& ww, int& hh)
{
    ww = 44;
    hh = 34;
    x = w.x + w.w - ww;
    y = w.y;
}

static void WinMinRect(const PosWin& w, int& x, int& y, int& ww, int& hh)
{
    ww = 44;
    hh = 34;
    x = w.x + w.w - 88;
    y = w.y;
}

static void WinBodyRect(const PosWin& w, int& x, int& y, int& ww, int& hh)
{
    x = w.x + 12;
    y = w.y + 34;
    ww = w.w - 24;
    hh = w.h - 46;
}

// topmost window under the cursor; btn: -1 body, 0 min, 1 close
static bool HitTopWindow(int mx, int my, int& winIdx, int& btn)
{
    winIdx = -1;
    btn = -1;
    int i;
    for (i = (int)g_wins.size() - 1; i >= 0; i--)
    {
        if (g_wins[i].minimized) continue;
        int x = g_wins[i].x;
        int y = g_wins[i].y;
        int w = g_wins[i].w;
        int h = g_wins[i].h;
        if (!PtInRectI(mx, my, x, y, w, h)) continue;
        if (my < y + 34)
        {
            if (mx >= x + w - 44) btn = 1;
            else if (mx >= x + w - 88) btn = 0;
        }
        winIdx = i;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// toast
// ---------------------------------------------------------------------------
static void RenderToast(Graphics& g, ULONGLONG tick)
{
    if (g_toastText.empty()) return;
    if (tick >= g_toastUntil) return;
    int tw = 0;
    int th = 0;
    TextW(g, 1, false, g_toastText, tw, th);
    int boxW = tw + 48;
    if (boxW < 240) boxW = 240;
    if (boxW > g_W - 40) boxW = g_W - 40;
    int tx = (g_W - boxW) / 2;
    int ty = 24;
    ULONGLONG remain = g_toastUntil - tick;
    int a = 225;
    if (remain < 400) a = (int)(225.0f * (float)remain / 400.0f);
    if (a < 0) a = 0;
    SolidBrush b(Color((BYTE)a, 22, 26, 36));
    Pen bd(Color((BYTE)(a / 2), 255, 255, 255), 1.0f);
    RRect(g, (REAL)tx, (REAL)ty, (REAL)boxW, 44.0f, 12.0f, &b, &bd);
    TextC(g, 1, false, g_toastText, tx + boxW / 2, ty + 22, Color((BYTE)a, 255, 255, 255));
}

// ---------------------------------------------------------------------------
// settings panel
// ---------------------------------------------------------------------------
static void RenderChip(Graphics& g, int x, int y, int w, int h, const std::wstring& label, bool sel)
{
    bool hot = PtInRectI(g_mx, g_my, x, y, w, h);
    if (g_materialType == 2)
    {
        SolidBrush b(sel ? Color(255, 58, 122, 226) : Color(hot ? 90 : 55, 255, 255, 255));
        Pen p(Color(120, 255, 255, 255), 1.0f);
        RRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 6.0f, &b, &p);
        TextC(g, 1, false, label, x + w / 2, y + h / 2, Color(255, 232, 238, 250));
    }
    else
    {
        SolidBrush b(sel ? Color(255, 60, 140, 245) : Color(hot ? 235 : 205, 255, 255, 255));
        Pen p(sel ? Color(255, 60, 140, 245) : Color(150, 190, 200, 220), 1.0f);
        RRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 6.0f, &b, &p);
        TextC(g, 1, false, label, x + w / 2, y + h / 2, sel ? Color(255, 255, 255, 255) : Color(255, 40, 50, 70));
    }
}

static void RenderCard(Graphics& g, int x, int y, int w, int h, const std::wstring& title)
{
    SolidBrush cb(Color(255, 255, 255, 255));
    Pen bdp(Color(255, 216, 216, 216), 1.0f);
    RRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 4.0f, &cb, &bdp);
    TextL(g, 1, true, title, x + 16, y + 8, Color(255, 26, 26, 26));
}

static void RenderChip2(Graphics& g, int x, int y, int w, int h, const std::wstring& label, bool sel)
{
    bool hot = PtInRectI(g_mx, g_my, x, y, w, h);
    Color fill;
    if (sel) fill = Color(255, 224, 238, 255);
    else if (hot) fill = Color(255, 246, 246, 246);
    else fill = Color(255, 250, 250, 250);
    Color border = sel ? Color(255, 0, 99, 177) : Color(255, 214, 214, 214);
    Color txt = sel ? Color(255, 0, 99, 177) : Color(255, 40, 40, 40);
    SolidBrush b(fill);
    Pen p(border, sel ? 1.8f : 1.0f);
    RRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 3.0f, &b, &p);
    TextC(g, 1, false, label, x + w / 2, y + h / 2, txt);
}

static void RenderSettings(Graphics& g)
{
    if (!g_settingsOpen) return;
    int x = g_setX;
    int y = g_setY;
    int w = 520;
    int h = 560;

    SolidBrush sh(Color(80, 0, 0, 0));
    RRect(g, (REAL)(x + 6), (REAL)(y + 8), (REAL)w, (REAL)h, 8.0f, &sh, 0);

    SolidBrush bg(Color(255, 242, 242, 242));
    Pen bd(Color(255, 205, 205, 205), 1.0f);
    RRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 8.0f, &bg, &bd);

    Color fg(255, 26, 26, 26);
    Color fg2(255, 100, 100, 100);

    TextL(g, 3, false, L"设置", x + 24, y + 8, fg);
    TextL(g, 0, false, L"鲸鱼OS", x + w - 96, y + 20, fg2);

    // 搜索框（Win10 设置顶部那个）
    {
        SolidBrush sb(Color(255, 255, 255, 255));
        Pen sp(Color(255, 205, 205, 205), 1.0f);
        RRect(g, (REAL)(x + 24), (REAL)(y + 58), (REAL)(w - 48), 32.0f, 4.0f, &sb, &sp);
        Pen mg(fg2, 1.2f);
        g.DrawEllipse(&mg, (REAL)(x + 36), (REAL)(y + 66), 9.0f, 9.0f);
        g.DrawLine(&mg, (REAL)(x + 44), (REAL)(y + 74), (REAL)(x + 48), (REAL)(y + 78));
        TextL(g, 0, false, L"查找设置", x + 58, y + 68, fg2);
    }

    // 卡片：背景
    RenderCard(g, x + 24, y + 100, w - 48, 116, L"背景");
    RenderChip2(g, x + 40, y + 140, 140, 40, L"流光", g_wallpaperType == 0);
    RenderChip2(g, x + 190, y + 140, 140, 40, L"雪山", g_wallpaperType == 1);
    RenderChip2(g, x + 340, y + 140, 140, 40, L"星空", g_wallpaperType == 2);

    // 卡片：材质
    RenderCard(g, x + 24, y + 228, w - 48, 116, L"材质");
    RenderChip2(g, x + 40, y + 268, 140, 40, L"液态玻璃", g_materialType == 0);
    RenderChip2(g, x + 190, y + 268, 140, 40, L"塑料", g_materialType == 1);
    RenderChip2(g, x + 340, y + 268, 140, 40, L"原生", g_materialType == 2);

    // 卡片：灵动岛
    RenderCard(g, x + 24, y + 356, w - 48, 100, L"灵动岛");
    RenderChip2(g, x + 40, y + 392, 140, 40, L"显示", g_islandOn);
    RenderChip2(g, x + 190, y + 392, 140, 40, L"隐藏", !g_islandOn);

    // 卡片：关于
    RenderCard(g, x + 24, y + 468, w - 48, 76, L"关于");
    TextL(g, 0, false, L"鲸鱼OS · 单文件假桌面 · Win32 + GDI+", x + 40, y + 500, fg2);
    TextL(g, 0, false, L"灵动岛在左下角：来消息向上弹出，点一下已读收起。", x + 40, y + 520, fg2);

    // 关闭按钮
    if (PtInRectI(g_mx, g_my, x + w - 44, y + 6, 34, 30))
    {
        SolidBrush hb(Color(255, 226, 226, 226));
        RRect(g, (REAL)(x + w - 44), (REAL)(y + 6), 34.0f, 30.0f, 4.0f, &hb, 0);
    }
    Pen xp(fg, 1.6f);
    g.DrawLine(&xp, (REAL)(x + w - 36), (REAL)(y + 14), (REAL)(x + w - 22), (REAL)(y + 28));
    g.DrawLine(&xp, (REAL)(x + w - 22), (REAL)(y + 14), (REAL)(x + w - 36), (REAL)(y + 28));
}

// ---------------------------------------------------------------------------
// calendar panel
// ---------------------------------------------------------------------------
static void RenderCalendar(Graphics& g)
{
    if (!g_calendarOpen) return;
    int x = g_calX;
    int y = g_calY;
    int w = 300;
    int h = 364;

    SolidBrush sh(Color(85, 0, 0, 0));
    RRect(g, (REAL)(x + 6), (REAL)(y + 8), (REAL)w, (REAL)h, 12.0f, &sh, 0);

    Color topC;
    Color botC;
    if (g_materialType == 2)
    {
        topC = Color(255, 46, 50, 60);
        botC = Color(255, 34, 37, 44);
    }
    else
    {
        topC = Color(250, 252, 254, 255);
        botC = Color(238, 236, 242, 252);
    }
    LinearGradientBrush lb(PointF((REAL)x, (REAL)y), PointF((REAL)x, (REAL)(y + h)), topC, botC);
    Pen bd(Color(140, 255, 255, 255), 1.0f);
    RRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 12.0f, &lb, &bd);

    Color fg;
    Color fg2;
    if (g_materialType == 2)
    {
        fg = Color(255, 228, 234, 246);
        fg2 = Color(175, 150, 165, 190);
    }
    else
    {
        fg = Color(255, 34, 44, 62);
        fg2 = Color(175, 90, 100, 120);
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    long ym = (long)st.wYear * 12L + (long)(st.wMonth - 1) + (long)g_calOffset;
    int year = (int)(ym / 12L);
    int mon0 = (int)(ym % 12L);
    if (mon0 < 0)
    {
        mon0 += 12;
        year -= 1;
    }

    wchar_t hdr[48];
    std::swprintf(hdr, 48, L"%s %d", MonthName(mon0).c_str(), year);
    TextC(g, 1, true, hdr, x + w / 2, y + 22, fg);

    // prev / next
    if (PtInRectI(g_mx, g_my, x + 8, y + 8, 32, 28))
    {
        SolidBrush hb(Color(60, 255, 255, 255));
        RRect(g, (REAL)(x + 8), (REAL)(y + 8), 32.0f, 28.0f, 6.0f, &hb, 0);
    }
    TextC(g, 1, true, L"<", x + 24, y + 22, fg2);
    if (PtInRectI(g_mx, g_my, x + w - 40, y + 8, 32, 28))
    {
        SolidBrush hb(Color(60, 255, 255, 255));
        RRect(g, (REAL)(x + w - 40), (REAL)(y + 8), 32.0f, 28.0f, 6.0f, &hb, 0);
    }
    TextC(g, 1, true, L">", x + w - 24, y + 22, fg2);
    // close
    if (PtInRectI(g_mx, g_my, x + w - 44, y + 44, 40, 30))
    {
        SolidBrush hb(Color(255, 232, 17, 35));
        RRect(g, (REAL)(x + w - 44), (REAL)(y + 44), 40.0f, 30.0f, 6.0f, &hb, 0);
    }
    Pen xp(fg, 1.5f);
    g.DrawLine(&xp, (REAL)(x + w - 32), (REAL)(y + 52), (REAL)(x + w - 20), (REAL)(y + 64));
    g.DrawLine(&xp, (REAL)(x + w - 20), (REAL)(y + 52), (REAL)(x + w - 32), (REAL)(y + 64));

    // weekday header (Monday first)
    static const wchar_t* wd[7] = { L"M", L"T", L"W", L"T", L"F", L"S", L"S" };
    int c;
    for (c = 0; c < 7; c++)
    {
        TextC(g, 0, true, wd[c], x + 10 + c * 40 + 20, y + 74, fg2);
    }

    // day grid
    int dim = DaysInMonth(year, mon0);
    int firstMon = 0;
    {
        // weekday of the first day, Monday based
        int todayMon = ((int)st.wDayOfWeek + 6) % 7;
        firstMon = (todayMon - ((int)st.wDay - 1)) % 7;
        if (firstMon < 0) firstMon += 7;
    }
    int day;
    for (day = 1; day <= dim; day++)
    {
        int idx = firstMon + day - 1;
        int r = idx / 7;
        int col = idx % 7;
        int cx = x + 10 + col * 40;
        int cy = y + 92 + r * 38;
        bool today = (g_calOffset == 0 && day == (int)st.wDay);
        if (today)
        {
            SolidBrush b(Color(255, 60, 140, 245));
            g.FillEllipse(&b, (REAL)(cx + 8), (REAL)(cy + 2), 24.0f, 24.0f);
        }
        wchar_t num[8];
        std::swprintf(num, 8, L"%d", day);
        TextC(g, 0, false, num, cx + 20, cy + 14, today ? Color(255, 255, 255, 255) : fg);
    }
}

// ---------------------------------------------------------------------------
// windows
// ---------------------------------------------------------------------------
static void RenderWinBody(Graphics& g, PosWin& w)
{
    int bx, by, bw, bh;
    WinBodyRect(w, bx, by, bw, bh);

    if (w.kind == APP_BAIDU)
    {
        SolidBrush white(Color(255, 250, 251, 253));
        RRect(g, (REAL)bx, (REAL)by, (REAL)bw, (REAL)bh, 6.0f, &white, 0);

        int ux = bx + 10;
        int uy = by + 8;
        int uw = bw - 20;
        int uh = 34;
        SolidBrush ub(Color(255, 242, 245, 250));
        Pen ubd(Color(255, 205, 212, 224), 1.0f);
        RRect(g, (REAL)ux, (REAL)uy, (REAL)uw, (REAL)uh, 17.0f, &ub, &ubd);
        TextL(g, 1, true, L"<", ux + 14, uy + 8, Color(255, 90, 100, 120));
        if (w.url.empty())
        {
            TextL(g, 1, false, L"输入网址后按回车", ux + 40, uy + 8, Color(150, 130, 138, 158));
        }
        else
        {
            TextL(g, 1, false, TruncChars(w.url, 60), ux + 40, uy + 8, Color(255, 40, 50, 70));
        }
        // caret in the URL field
        if (g_urlFocus && g_active >= 0 && g_wins[g_active].kind == APP_BAIDU)
        {
            if (((GetTickCount64() / 500ULL) % 2ULL) == 0ULL)
            {
                int caretX = ux + 40;
                if (!w.url.empty())
                {
                    int tw = 0;
                    int th = 0;
                    TextW(g, 1, false, w.url, tw, th);
                    caretX += tw;
                }
                SolidBrush cb(Color(255, 40, 90, 220));
                g.FillRectangle(&cb, (REAL)caretX, (REAL)(uy + 9), 2.0f, 16.0f);
            }
        }
        // search button
        int swz = 66;
        int sx = ux + uw - swz - 4;
        int sy = uy + 4;
        int shz = uh - 8;
        SolidBrush sb2(Color(255, 228, 74, 42));
        RRect(g, (REAL)sx, (REAL)sy, (REAL)swz, (REAL)shz, 13.0f, &sb2, 0);
        TextC(g, 0, true, L"搜索", sx + swz / 2, sy + shz / 2, Color(255, 255, 255, 255));

        int cy = uy + uh + 12;
        if (w.query.empty())
        {
            TextC(g, 6, true, L"百度", bx + bw / 2, cy + 54, Color(255, 40, 82, 232));
            int bx2 = bx + bw / 2 - 180;
            int by2 = cy + 104;
            SolidBrush wb(Color(255, 255, 255, 255));
            Pen wbd(Color(255, 60, 140, 245), 1.5f);
            RRect(g, (REAL)bx2, (REAL)by2, 360.0f, 42.0f, 21.0f, &wb, &wbd);
            TextL(g, 1, false, L"输入关键词后按回车", bx2 + 16, by2 + 12, Color(150, 130, 138, 158));

            static const wchar_t* hot[6] = {
                L"深度求索", L"液态玻璃", L"手搓系统", L"假桌面", L"单文件", L"绘图"
            };
            int i;
            int colW = (bw - 120) / 3;
            if (colW < 40) colW = 40;
            for (i = 0; i < 6; i++)
            {
                int hx = bx + 60 + (i % 3) * colW;
                int hy = by2 + 66 + (i / 3) * 26;
                TextC(g, 0, false, hot[i], hx + colW / 2 - 40, hy, Color(255, 90, 110, 150));
            }
        }
        else
        {
            TextL(g, 0, false, L"约 320 条结果（演示）", bx + 44, cy, Color(150, 130, 140, 160));
            int i;
            for (i = 0; i < 3; i++)
            {
                int ry = cy + 26 + i * 64;
                std::wstring title = w.query + L" · 演示结果 " + std::to_wstring(i + 1);
                TextL(g, 2, false, TruncChars(title, 40), bx + 44, ry, Color(255, 42, 82, 232));
                TextL(g, 0, false, L"baidu.com/演示/" + std::to_wstring(i + 1), bx + 44, ry + 27, Color(255, 60, 150, 90));
                TextL(g, 0, false, L"本地渲染的演示结果，不联网。", bx + 44, ry + 43, Color(255, 120, 130, 150));
            }
        }
        return;
    }

    if (w.kind == APP_NOTEPAD || w.kind == APP_TEXTDOC)
    {
        SolidBrush white(Color(255, 252, 253, 255));
        RRect(g, (REAL)bx, (REAL)by, (REAL)bw, (REAL)bh, 6.0f, &white, 0);

        std::vector<std::wstring> lines;
        std::wstring cur;
        size_t k;
        for (k = 0; k < w.text.size(); k++)
        {
            wchar_t ch = w.text[k];
            if (ch == L'\n')
            {
                lines.push_back(cur);
                cur.clear();
            }
            else
            {
                cur += ch;
            }
        }
        lines.push_back(cur);

        int lineH = 20;
        int maxLines = (bh - 16) / lineH;
        if (maxLines < 1) maxLines = 1;
        int first = (int)lines.size() - maxLines;
        if (first < 0) first = 0;

        int i;
        for (i = 0; i < maxLines; i++)
        {
            int idx = first + i;
            if (idx >= (int)lines.size()) break;
            TextL(g, 1, false, TruncChars(lines[idx], 52), bx + 10, by + 8 + i * lineH, Color(255, 40, 50, 70));
        }

        // caret at the end of the last line
        if (g_active >= 0 && g_wins[g_active].kind == w.kind)
        {
            if (((GetTickCount64() / 500ULL) % 2ULL) == 0ULL)
            {
                int lastIdx = (int)lines.size() - 1 - first;
                int tw2 = 0;
                int th2 = 0;
                if (lastIdx >= 0 && lastIdx < (int)lines.size())
                {
                    TextW(g, 1, false, lines[(size_t)(first + lastIdx)], tw2, th2);
                }
                int caretY = by + 8 + lastIdx * lineH;
                SolidBrush cb(Color(255, 40, 90, 220));
                g.FillRectangle(&cb, (REAL)(bx + 10 + tw2), (REAL)caretY, 2.0f, 16.0f);
            }
        }
        return;
    }

    if (w.kind == APP_RECYCLE)
    {
        SolidBrush bgc(Color(255, 247, 249, 252));
        RRect(g, (REAL)bx, (REAL)by, (REAL)bw, (REAL)bh, 6.0f, &bgc, 0);

        static const wchar_t* names[3] = { L"旧文档.txt", L"截图_20241024.png", L"鲸鱼OS_旧版备份.exe" };
        static const wchar_t* sizes[3] = { L"12 KB", L"1.2 MB", L"8.6 MB" };
        wchar_t hdr[64];
        std::swprintf(hdr, 64, L"回收站（%d 个项目）", g_recycleCount);
        TextL(g, 1, true, hdr, bx + 12, by + 10, Color(255, 40, 50, 70));

        int i;
        for (i = 0; i < g_recycleCount && i < 3; i++)
        {
            int ry = by + 40 + i * 44;
            if (ry + 40 > by + bh - 20) break;
            DrawPageGlyph(g, bx + 28, ry + 18, 40, Color(255, 200, 210, 226));
            TextL(g, 1, false, names[i], bx + 52, ry + 8, Color(255, 45, 55, 75));
            TextR(g, 0, false, sizes[i], bx + bw - 12, ry + 12, Color(255, 130, 140, 158));
        }

        int ebx = bx + bw / 2 - 80;
        int eby = by + bh - 48;
        bool hot = PtInRectI(g_mx, g_my, ebx, eby, 160, 36);
        SolidBrush eb(Color(hot ? 255 : 235, 255, 255, 255));
        Pen ebp(Color(200, 210, 80, 90), 1.2f);
        RRect(g, (REAL)ebx, (REAL)eby, 160.0f, 36.0f, 6.0f, &eb, &ebp);
        TextC(g, 1, false, L"清空回收站", ebx + 80, eby + 18, Color(255, 150, 50, 60));
        return;
    }

    if (w.kind == APP_PAINT)
    {
        SolidBrush bgc(Color(255, 246, 248, 252));
        RRect(g, (REAL)bx, (REAL)by, (REAL)bw, (REAL)bh, 6.0f, &bgc, 0);

        static const Color swatch[5] = {
            Color(255, 20, 22, 28),
            Color(255, 216, 60, 60),
            Color(255, 50, 130, 235),
            Color(255, 60, 180, 90),
            Color(255, 240, 160, 40)
        };
        int i;
        for (i = 0; i < 5; i++)
        {
            int sx = bx + 8 + i * 32;
            int sy = by + 8;
            SolidBrush b(swatch[i]);
            RRect(g, (REAL)sx, (REAL)sy, 24.0f, 24.0f, 6.0f, &b, 0);
            if (w.paintColor == i)
            {
                Pen sel(Color(255, 40, 90, 220), 2.0f);
                RRect(g, (REAL)(sx - 2), (REAL)(sy - 2), 28.0f, 28.0f, 7.0f, 0, &sel);
            }
        }
        int cbx = bx + bw - 80;
        int cby = by + 6;
        bool chot = PtInRectI(g_mx, g_my, cbx, cby, 72, 28);
        SolidBrush cbb(Color(chot ? 235 : 210, 255, 255, 255));
        Pen cbp(Color(150, 180, 190, 210), 1.0f);
        RRect(g, (REAL)cbx, (REAL)cby, 72.0f, 28.0f, 6.0f, &cbb, &cbp);
        TextC(g, 0, false, L"清空", cbx + 36, cby + 14, Color(255, 60, 70, 92));

        int cvx = bx;
        int cvy = by + 40;
        int cvw = bw;
        int cvh = bh - 40;
        if (cvh < 20) cvh = 20;
        SolidBrush canvas(Color(255, 255, 255, 255));
        Pen cvp(Color(255, 200, 208, 222), 1.0f);
        RRect(g, (REAL)cvx, (REAL)cvy, (REAL)cvw, (REAL)cvh, 6.0f, &canvas, &cvp);

        Rect clipR((INT)cvx, (INT)cvy + 2, (INT)cvw, (INT)cvh - 4);
        g.SetClip(clipR);

        size_t s2;
        for (s2 = 0; s2 < w.strokes.size(); s2++)
        {
            const InkStroke& st = w.strokes[s2];
            if (st.pts.size() < 2) continue;
            int cidx = st.color;
            if (cidx < 0) cidx = 0;
            if (cidx > 4) cidx = 4;
            Pen pen(swatch[cidx], 3.0f);
            size_t k2;
            for (k2 = 1; k2 < st.pts.size(); k2++)
            {
                g.DrawLine(&pen, (REAL)st.pts[k2 - 1].X, (REAL)st.pts[k2 - 1].Y,
                    (REAL)st.pts[k2].X, (REAL)st.pts[k2].Y);
            }
        }
        g.ResetClip();
        return;
    }
}

static void RenderWindow(Graphics& g, int idx)
{
    PosWin& w = g_wins[idx];
    if (w.minimized) return;
    bool act = (idx == g_active);

    // layered shadow (3 passes for a soft edge)
    int k;
    for (k = 3; k >= 1; k--)
    {
        SolidBrush sh(Color(16, 0, 0, 0));
        RRect(g, (REAL)(w.x - k * 2), (REAL)(w.y - k * 2), (REAL)(w.w + k * 4), (REAL)(w.h + k * 4), 12.0f, &sh, 0);
    }

    Color bodyC;
    if (g_materialType == 0) bodyC = Color(246, 249, 251, 255);
    else if (g_materialType == 1) bodyC = Color(255, 250, 251, 254);
    else bodyC = Color(255, 42, 45, 52);

    SolidBrush body(bodyC);
    Pen border(act ? Color(255, 80, 150, 240) : Color(190, 200, 210, 224), act ? 2.0f : 1.0f);
    RRect(g, (REAL)w.x, (REAL)w.y, (REAL)w.w, (REAL)w.h, 10.0f, &body, &border);

    // title bar
    int tbTop = w.y;
    int tbH = 34;
    Color tbTopC;
    Color tbBotC;
    if (g_materialType == 2)
    {
        tbTopC = Color(255, 56, 60, 70);
        tbBotC = Color(255, 44, 47, 56);
    }
    else
    {
        tbTopC = Color(238, 250, 252, 255);
        tbBotC = Color(214, 234, 240, 250);
    }
    LinearGradientBrush tb(PointF((REAL)w.x, (REAL)tbTop), PointF((REAL)w.x, (REAL)(tbTop + tbH)), tbTopC, tbBotC);
    GraphicsPath tbPath;
    tbPath.AddArc((REAL)(w.x), (REAL)tbTop, 20.0f, 20.0f, 180.0f, 90.0f);
    tbPath.AddArc((REAL)(w.x + w.w - 20), (REAL)tbTop, 20.0f, 20.0f, 270.0f, 90.0f);
    tbPath.AddLine((REAL)(w.x + w.w), (REAL)(tbTop + tbH), (REAL)w.x, (REAL)(tbTop + tbH));
    tbPath.CloseFigure();
    g.FillPath(&tb, &tbPath);

    TextL(g, 1, true, TruncChars(w.title, 34), w.x + 12, w.y + 9, (g_materialType == 2) ? Color(255, 234, 238, 250) : Color(255, 34, 44, 62));

    // minimize button
    {
        int bx, by, bw2, bh2;
        WinMinRect(w, bx, by, bw2, bh2);
        if (g_hoverWin == idx && g_hoverBtn == 0)
        {
            SolidBrush hb(Color(70, 255, 255, 255));
            RRect(g, (REAL)bx, (REAL)by, (REAL)bw2, (REAL)bh2, 4.0f, &hb, 0);
        }
        Pen p((g_materialType == 2) ? Color(255, 226, 232, 244) : Color(255, 60, 70, 92), 2.0f);
        g.DrawLine(&p, (REAL)(bx + 15), (REAL)(by + 20), (REAL)(bx + 29), (REAL)(by + 20));
    }

    // close button
    {
        int bx, by, bw2, bh2;
        WinCloseRect(w, bx, by, bw2, bh2);
        bool hb2 = (g_hoverWin == idx && g_hoverBtn == 1);
        if (hb2)
        {
            SolidBrush hb(Color(255, 232, 17, 35));
            RRect(g, (REAL)bx, (REAL)by, (REAL)bw2, (REAL)bh2, 4.0f, &hb, 0);
        }
        Pen p(hb2 ? Color(255, 255, 255, 255) : ((g_materialType == 2) ? Color(255, 226, 232, 244) : Color(255, 60, 70, 92)), 2.0f);
        g.DrawLine(&p, (REAL)(bx + 16), (REAL)(by + 11), (REAL)(bx + 28), (REAL)(by + 23));
        g.DrawLine(&p, (REAL)(bx + 28), (REAL)(by + 11), (REAL)(bx + 16), (REAL)(by + 23));
    }

    RenderWinBody(g, w);
}

static void RenderWindows(Graphics& g)
{
    int i;
    for (i = 0; i < (int)g_wins.size(); i++)
    {
        RenderWindow(g, i);
    }
}

// ---------------------------------------------------------------------------
// right click context menu (drawing and hit testing share CtxMainRect/CtxSubRect)
// ---------------------------------------------------------------------------
static void RenderCtxMenu(Graphics& g)
{
    if (g_ctxOpen == CTX_NONE) return;
    CtxLayout L;
    CtxMainRect(L, g_ctxX, g_ctxY);
    int h = L.count * L.itemH + 8;

    SolidBrush sh(Color(90, 0, 0, 0));
    RRect(g, (REAL)(L.x + 4), (REAL)(L.y + 6), (REAL)L.w, (REAL)h, 8.0f, &sh, 0);
    LinearGradientBrush lb(PointF((REAL)L.x, (REAL)L.y), PointF((REAL)L.x, (REAL)(L.y + h)),
        Color(240, 38, 42, 54), Color(245, 26, 29, 38));
    Pen bd(Color(120, 255, 255, 255), 1.0f);
    RRect(g, (REAL)L.x, (REAL)L.y, (REAL)L.w, (REAL)h, 8.0f, &lb, &bd);

    static const wchar_t* items[8] = {
        L"查看", L"排序方式", L"刷新", L"新建", L"打开回收站",
        L"更换壁纸", L"打开系统设置", L"关于鲸鱼OS"
    };
    int i;
    for (i = 0; i < L.count; i++)
    {
        int iy = L.y + 4 + i * L.itemH;
        if (g_ctxHover == i)
        {
            SolidBrush hb(Color(70, 255, 255, 255));
            RRect(g, (REAL)(L.x + 4), (REAL)iy, (REAL)(L.w - 8), (REAL)(L.itemH - 2), 5.0f, &hb, 0);
        }
        TextL(g, 1, false, items[i], L.x + 14, iy + 9, Color(255, 234, 238, 248));
        if (i == 3 || i == 5)
        {
            TextR(g, 1, false, L">", L.x + L.w - 12, iy + 9, Color(180, 210, 218, 235));
        }
        if (i == 2 || i == 5)
        {
            Pen sep(Color(70, 255, 255, 255), 1.0f);
            g.DrawLine(&sep, (REAL)(L.x + 10), (REAL)(iy + L.itemH - 1), (REAL)(L.x + L.w - 10), (REAL)(iy + L.itemH - 1));
        }
    }

    if (g_ctxOpen == CTX_NEW || g_ctxOpen == CTX_WALL)
    {
        int anchor = (g_ctxOpen == CTX_NEW) ? 3 : 5;
        int subCount = (g_ctxOpen == CTX_NEW) ? 2 : 3;
        int sx, sy, sw, sh2;
        CtxSubRect(L, anchor, subCount, sx, sy, sw, sh2);
        SolidBrush shb(Color(90, 0, 0, 0));
        RRect(g, (REAL)(sx + 4), (REAL)(sy + 6), (REAL)sw, (REAL)sh2, 8.0f, &shb, 0);
        RRect(g, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh2, 8.0f, &lb, &bd);

        int k;
        for (k = 0; k < subCount; k++)
        {
            int iy = sy + 4 + k * 32;
            if (g_ctxHover == 100 + k)
            {
                SolidBrush hb(Color(70, 255, 255, 255));
                RRect(g, (REAL)(sx + 4), (REAL)iy, (REAL)(sw - 8), 30.0f, 5.0f, &hb, 0);
            }
            std::wstring label;
            if (g_ctxOpen == CTX_NEW)
            {
                label = (k == 0) ? L"文本文档" : L"画图画布";
            }
            else
            {
                if (k == 0) label = L"经典蓝";
                else if (k == 1) label = L"雪山";
                else label = L"星空";
            }
            TextL(g, 1, false, label, sx + 14, iy + 7, Color(255, 234, 238, 248));
        }
    }
}

// ---------------------------------------------------------------------------
// steam easter egg overlay
// ---------------------------------------------------------------------------
static void RenderSteam(Graphics& g)
{
    // 96% 之后：满屏红色错误弹窗（多张叠加，带轻微抖动）
    if (g_steamPct >= 96)
    {
        SolidBrush red(Color(190, 122, 12, 12));
        g.FillRectangle(&red, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);

        const wchar_t* msgs[6] = {
            L"无法写入文件：C:\\Windows\\System32\\whale.dll",
            L"安装失败：错误代码 0xC0000005",
            L"运行库组件缺失，安装已中止",
            L"磁盘空间不足，写入被拒绝",
            L"访问被拒绝：请以管理员身份重试",
            L"安装程序已停止工作"
        };
        int i;
        for (i = 0; i < 6; i++)
        {
            int dw = 430 + (i % 3) * 40;
            int dh = 150;
            int dx = ((i * 197) % (g_W - dw));
            int dy = ((i * 131) % (g_H - dh - 20)) + (i % 2) * 10;
            SolidBrush sh(Color(120, 0, 0, 0));
            RRect(g, (REAL)(dx + 6), (REAL)(dy + 8), (REAL)dw, (REAL)dh, 6.0f, &sh, 0);
            SolidBrush body(Color(255, 250, 246, 244));
            Pen edge(Color(255, 190, 30, 30), 2.0f);
            RRect(g, (REAL)dx, (REAL)dy, (REAL)dw, (REAL)dh, 6.0f, &body, &edge);
            SolidBrush tbar(Color(255, 196, 43, 28));
            RRect(g, (REAL)dx, (REAL)dy, (REAL)dw, 34.0f, 6.0f, &tbar, 0);
            TextL(g, 1, true, L"鲸鱼OS - 致命错误", dx + 12, dy + 8, Color(255, 255, 255, 255));
            // 红色叉号
            Pen xp(Color(255, 200, 30, 30), 4.0f);
            g.DrawEllipse(&xp, (REAL)(dx + 20), (REAL)(dy + 56), 34.0f, 34.0f);
            g.DrawLine(&xp, (REAL)(dx + 28), (REAL)(dy + 64), (REAL)(dx + 46), (REAL)(dy + 82));
            g.DrawLine(&xp, (REAL)(dx + 46), (REAL)(dy + 64), (REAL)(dx + 28), (REAL)(dy + 82));
            TextL(g, 1, false, msgs[i % 6], dx + 68, dy + 62, Color(255, 30, 30, 30));
            TextL(g, 0, false, L"点击「确定」继续（并不会有任何用）", dx + 68, dy + 90, Color(255, 110, 110, 110));
            SolidBrush btn(Color(255, 228, 228, 228));
            Pen bd(Color(255, 160, 160, 160), 1.0f);
            RRect(g, (REAL)(dx + dw - 100), (REAL)(dy + dh - 44), 84.0f, 30.0f, 4.0f, &btn, &bd);
            TextC(g, 0, false, L"确定", dx + dw - 58, dy + dh - 29, Color(255, 30, 30, 30));
        }
        TextC(g, 2, true, L"安装已中止 · 正在终止进程…", g_W / 2, g_H - 46, Color(255, 255, 255, 255));
        return;
    }

    SolidBrush dim(Color(130, 0, 0, 0));
    g.FillRectangle(&dim, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);

    int w = 460;
    int h = 240;
    int x = (g_W - w) / 2;
    int y = (g_H - h) / 2;

    LinearGradientBrush lb(PointF((REAL)x, (REAL)y), PointF((REAL)x, (REAL)(y + h)),
        Color(255, 40, 46, 60), Color(255, 22, 25, 34));
    Pen bd(Color(140, 255, 255, 255), 1.0f);
    RRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 10.0f, &lb, &bd);

    TextL(g, 2, true, L"蒸汽 安装程序", x + 24, y + 16, Color(255, 236, 240, 250));
    TextL(g, 1, false, L"正在下载并安装鲸鱼OS 运行库…", x + 24, y + 56, Color(220, 200, 214, 235));
    TextL(g, 0, false, L"请勿关闭计算机。这只是演示。", x + 24, y + 82, Color(170, 170, 186, 210));

    int barX = x + 40;
    int barY = y + 150;
    int barW = w - 80;
    int barH = 18;
    SolidBrush track(Color(255, 36, 44, 62));
    RRect(g, (REAL)barX, (REAL)barY, (REAL)barW, (REAL)barH, 9.0f, &track, 0);
    float frac = (float)g_steamPct / 100.0001f;
    int fw = (int)((float)barW * frac);
    if (fw > 4)
    {
        SolidBrush fill(Color(255, 108, 168, 240));
        RRect(g, (REAL)barX, (REAL)barY, (REAL)fw, (REAL)barH, 9.0f, &fill, 0);
    }
    wchar_t pct[16];
    std::swprintf(pct, 16, L"%d%%", g_steamPct);
    TextC(g, 1, true, pct, x + w / 2, barY + 46, Color(255, 222, 230, 246));
}

// ---------------------------------------------------------------------------
// boot / lock / bsod
// ---------------------------------------------------------------------------
static void RenderBoot(Graphics& g, ULONGLONG tick)
{
    SolidBrush black(Color(255, 0, 0, 0));
    g.FillRectangle(&black, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);

    ULONGLONG el = (tick >= g_stateStart) ? (tick - g_stateStart) : 0ULL;
    float p = (float)el / 1600.0001f;
    if (p > 1.0f) p = 1.0f;

    int a = (int)(255.0f * p);
    if (a < 20) a = 20;
    {
        int ax = g_W / 2;
        int ay = g_H / 2 - 200;
        SolidBrush ring(Color((BYTE)a, 255, 255, 255));
        g.FillEllipse(&ring, (REAL)(ax - 46), (REAL)(ay - 46), 92.0f, 92.0f);
        SolidBrush inner(Color((BYTE)a, 20, 28, 50));
        g.FillEllipse(&inner, (REAL)(ax - 42), (REAL)(ay - 42), 84.0f, 84.0f);
        TextC(g, 3, true, L"鲸", ax, ay - 2, Color((BYTE)a, 200, 226, 255));
        TextC(g, 2, false, L"鲸鱼娘", ax, ay + 76, Color((BYTE)a, 236, 242, 252));
    }
    TextC(g, 7, true, L"鲸鱼OS", g_W / 2, g_H / 2 - 40, Color((BYTE)a, 240, 244, 252));
    TextC(g, 1, false, L"假桌面 v2.0", g_W / 2, g_H / 2 + 40, Color((BYTE)(a / 2), 180, 200, 232));

    int bx = g_W / 2 - 120;
    int by = g_H / 2 + 90;
    SolidBrush track(Color(90, 255, 255, 255));
    RRect(g, (REAL)bx, (REAL)by, 240.0f, 6.0f, 3.0f, &track, 0);
    SolidBrush fill(Color(255, 100, 170, 245));
    RRect(g, (REAL)bx, (REAL)by, (REAL)(240.0f * p), 6.0f, 3.0f, &fill, 0);
}

static void RenderLock(Graphics& g, ULONGLONG tick)
{
    RenderWallpaper(g, tick);
    SolidBrush dim(Color(120, 4, 6, 14));
    g.FillRectangle(&dim, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[16];
    std::swprintf(ts, 16, L"%02d:%02d", (int)st.wHour, (int)st.wMinute);
    wchar_t ds[40];
    std::swprintf(ds, 40, L"%s %d, %d", MonthName((int)st.wMonth - 1).c_str(), (int)st.wDay, (int)st.wYear);

    TextC(g, 7, true, ts, g_W / 2, g_H / 3, Color(255, 246, 249, 255));
    TextC(g, 3, false, ds, g_W / 2, g_H / 3 + 80, Color(220, 220, 230, 245));
    {
        int lax = g_W / 2;
        int lay = (int)(g_H * 0.46f);
        SolidBrush lring(Color(225, 255, 255, 255));
        g.FillEllipse(&lring, (REAL)(lax - 44), (REAL)(lay - 44), 88.0f, 88.0f);
        SolidBrush lin(Color(235, 16, 24, 44));
        g.FillEllipse(&lin, (REAL)(lax - 40), (REAL)(lay - 40), 80.0f, 80.0f);
        TextC(g, 3, true, L"鲸", lax, lay - 2, Color(255, 200, 226, 255));
        TextC(g, 2, false, L"鲸鱼娘", lax, lay + 68, Color(240, 236, 242, 252));
    }
    TextC(g, 1, false, L"点击任意位置解锁", g_W / 2, (int)(g_H * 0.62f), Color(180, 210, 222, 240));
    TextC(g, 0, false, L"鲸鱼OS", g_W / 2, g_H - 60, Color(140, 190, 204, 230));
}

static void RenderDesktopClock(Graphics& g)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[16];
    std::swprintf(ts, 16, L"%02d:%02d", (int)st.wHour, (int)st.wMinute);
    wchar_t ds[64];
    std::swprintf(ds, 64, L"%d 年 %d 月 %d 日", (int)st.wYear, (int)st.wMonth, (int)st.wDay);

    TextR(g, 7, true, ts, g_W - 72, 54, Color(240, 246, 252, 255));
    TextR(g, 2, false, ds, g_W - 80, 176, Color(228, 238, 246, 248));
}

static void RenderBSOD(Graphics& g, ULONGLONG tick)
{
    SolidBrush blue(Color(255, 0, 0, 170));
    g.FillRectangle(&blue, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);

    ULONGLONG el = (tick >= g_stateStart) ? (tick - g_stateStart) : 0ULL;
    int pct = (int)((float)el / 15.0001f);
    if (pct > 100) pct = 100;

    TextL(g, 7, false, L":(", 60, 120, Color(255, 255, 255, 255));
    TextC(g, 3, false, L"你的电脑遇到问题，需要重新启动。", g_W / 2, g_H / 2 - 60, Color(255, 255, 255, 255));
    TextC(g, 3, false, L"我们正在收集错误信息，随后会为你重新启动。", g_W / 2, g_H / 2 - 10, Color(230, 240, 245, 255));

    wchar_t pbuf[32];
    std::swprintf(pbuf, 32, L"已完成 %d%%", pct);
    TextC(g, 3, true, pbuf, g_W / 2, g_H / 2 + 60, Color(255, 255, 255, 255));

    TextL(g, 0, false, L"有关此问题的详细信息和可能的解决方法，请访问 support.example.com/stopcode", 60, g_H - 150, Color(210, 226, 236, 252));
    TextL(g, 0, false, L"如果致电支持人员，请提供以下信息：", 60, g_H - 120, Color(210, 226, 236, 252));
    TextL(g, 0, false, L"终止代码：0x0000005D（鲸鱼OS 彩蛋）0xC0000005", 60, g_H - 96, Color(255, 255, 255, 255));
    TextL(g, 0, false, L"鲸鱼OS v2 · 蒸汽彩蛋 · 假电脑会自行退出", 60, g_H - 50, Color(170, 200, 218, 240));
}

// ---------------------------------------------------------------------------
// master render
// ---------------------------------------------------------------------------
static void RenderUI(Graphics& g, ULONGLONG tick)
{
    g.Clear(Color(255, 0, 0, 0));

    if (g_shutdownAt != 0)
    {
        SolidBrush black(Color(255, 0, 0, 0));
        g.FillRectangle(&black, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);
        TextC(g, 2, false, L"正在关机…", g_W / 2, g_H / 2, Color(255, 200, 210, 220));
        return;
    }
    if (g_state == ST_BOOT_BLACK)
    {
        SolidBrush black(Color(255, 0, 0, 0));
        g.FillRectangle(&black, 0.0f, 0.0f, (REAL)g_W, (REAL)g_H);
        return;
    }
    if (g_state == ST_BOOT_ANIM)
    {
        RenderBoot(g, tick);
        return;
    }
    if (g_state == ST_LOCK)
    {
        RenderLock(g, tick);
        return;
    }
    if (g_state == ST_BSOD)
    {
        RenderBSOD(g, tick);
        return;
    }

    RenderWallpaper(g, tick);
    RenderDesktopClock(g);

    g_hoverWin = -1;
    g_hoverBtn = -1;
    {
        int hw = -1;
        int hb = -1;
        if (HitTopWindow(g_mx, g_my, hw, hb))
        {
            g_hoverWin = hw;
            g_hoverBtn = hb;
        }
    }

    RenderDesktopIcons(g);
    RenderWindows(g);
    RenderTaskbar(g);
    RenderIsland(g);
    if (g_menuOpen || g_menuAnim > 0.01f) RenderStartMenu(g);
    if (g_state == ST_STEAM) RenderSteam(g);
    if (g_settingsOpen) RenderSettings(g);
    if (g_calendarOpen) RenderCalendar(g);
    if (g_ctxOpen != CTX_NONE) RenderCtxMenu(g);
    RenderToast(g, tick);
}

// ---------------------------------------------------------------------------
// window / app management
// ---------------------------------------------------------------------------
static void StartSteam();
static void SteamShutdown();
static void RecalcActive()
{
    g_active = -1;
    int i;
    for (i = (int)g_wins.size() - 1; i >= 0; i--)
    {
        if (!g_wins[i].minimized)
        {
            g_active = i;
            return;
        }
    }
}

static void BringToFront(int idx)
{
    if (idx < 0 || idx >= (int)g_wins.size()) return;
    PosWin w = g_wins[idx];
    g_wins.erase(g_wins.begin() + idx);
    g_wins.push_back(w);
    int ni = (int)g_wins.size() - 1;
    if (g_active == idx) g_active = ni;
    else if (g_active > idx) g_active -= 1;
    if (g_paintWin == idx) g_paintWin = ni;
    else if (g_paintWin > idx) g_paintWin -= 1;
}

static void CloseWin(int idx)
{
    if (idx < 0 || idx >= (int)g_wins.size()) return;
    g_wins.erase(g_wins.begin() + idx);
    g_paintWin = -1;
    g_movingWin = -1;
    RecalcActive();
}

static void MinimizeWin(int idx)
{
    if (idx < 0 || idx >= (int)g_wins.size()) return;
    g_wins[idx].minimized = true;
    g_paintWin = -1;
    RecalcActive();
}

static void WinSizeFor(int kind, int& w, int& h)
{
    if (kind == APP_BAIDU) { w = 660; h = 420; return; }
    if (kind == APP_NOTEPAD) { w = 480; h = 340; return; }
    if (kind == APP_RECYCLE) { w = 440; h = 320; return; }
    if (kind == APP_PAINT) { w = 580; h = 420; return; }
    w = 460;
    h = 320;
}

static void OpenApp(int kind)
{
    g_ctxOpen = CTX_NONE;
    g_ctxHover = -1;
    g_menuOpen = false;

    if (kind == APP_STEAM)
    {
        if (g_state == ST_STEAM) return;
        g_steamPct = 0;
        g_state = ST_STEAM;
        g_stateStart = GetTickCount64();
        StartSteam();
        return;
    }
    if (kind == APP_SETTINGS)
    {
        g_settingsOpen = true;
        g_calendarOpen = false;
        return;
    }
    if (kind == APP_RECYCLE)
    {
        int i;
        for (i = 0; i < (int)g_wins.size(); i++)
        {
            if (g_wins[i].kind == APP_RECYCLE)
            {
                g_wins[i].minimized = false;
                BringToFront(i);
                return;
            }
        }
    }

    PosWin w;
    w.kind = kind;
    w.minimized = false;
    w.scroll = 0;
    w.paintColor = 0;

    int bw = 0;
    int bh = 0;
    WinSizeFor(kind, bw, bh);
    w.w = bw;
    w.h = bh;

    int n = (int)g_wins.size();
    w.x = 140 + (n % 6) * 26;
    w.y = 96 + (n % 6) * 26;
    if (w.x + w.w > g_W - 20) w.x = g_W - w.w - 20;
    if (w.y + w.h > TaskbarY() - 20) w.y = TaskbarY() - w.h - 20;
    if (w.x < 8) w.x = 8;
    if (w.y < 8) w.y = 8;

    if (kind == APP_BAIDU)
    {
        w.title = L"百度 · 假浏览器";
        w.url = L"";
        w.query = L"";
    }
    else if (kind == APP_NOTEPAD)
    {
        w.title = L"无标题 · 记事本";
        w.text = L"无标题 · 记事本\n\n新的一页，直接开始打字。\n\n点右上角 × 可以关掉这个窗口。";
    }
    else if (kind == APP_RECYCLE)
    {
        w.title = L"回收站";
    }
    else if (kind == APP_PAINT)
    {
        w.title = L"画图 · 无标题";
    }
    else
    {
        w.title = L"新建文本文档.txt";
        w.text = L"新建文本文档\n\n点一下正文就能直接打字。\n";
    }

    g_wins.push_back(w);
    g_active = (int)g_wins.size() - 1;
    g_urlFocus = false;
}

// ---------------------------------------------------------------------------
// desktop icon sync (real .lnk / .exe on the real desktop)
// ---------------------------------------------------------------------------
static std::wstring DesktopDir()
{
    wchar_t buf[MAX_PATH + 8];
    buf[0] = 0;
    if (SUCCEEDED(SHGetFolderPathW(0, CSIDL_DESKTOPDIRECTORY, 0, 0, buf)))
    {
        if (buf[0] != 0) return std::wstring(buf);
    }
    return std::wstring();
}

static void DestroyRealApps()
{
    size_t i;
    for (i = 0; i < g_realApps.size(); i++)
    {
        if (g_realApps[i].hIcon != 0)
        {
            DestroyIcon(g_realApps[i].hIcon);
            g_realApps[i].hIcon = 0;
        }
        if (g_realApps[i].bmp != 0)
        {
            delete g_realApps[i].bmp;
            g_realApps[i].bmp = 0;
        }
    }
    g_realApps.clear();
}

static void SyncRealDesktop()
{
    DestroyRealApps();
    std::wstring dir = DesktopDir();
    if (dir.empty()) return;

    const wchar_t* pat[2] = { L"\\*.lnk", L"\\*.exe" };
    int pi;
    for (pi = 0; pi < 2; pi++)
    {
        std::wstring q = dir + pat[pi];
        WIN32_FIND_DATAW fd;
        HANDLE hd = FindFirstFileW(q.c_str(), &fd);
        if (hd == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if ((int)g_realApps.size() >= 6) break;

            std::wstring full = dir + L"\\" + std::wstring(fd.cFileName);
            SHFILEINFOW sfi;
            ZeroMemory(&sfi, sizeof(sfi));
            DWORD flags = SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES | SHGFI_DISPLAYNAME;
            if (SHGetFileInfoW(full.c_str(), 0, &sfi, sizeof(sfi), flags) == 0) continue;

            RealApp ra;
            ra.path = full;
            ra.name = sfi.szDisplayName;
            if (ra.name.empty()) ra.name = fd.cFileName;
            ra.hIcon = sfi.hIcon;
            ra.bmp = 0;
            if (ra.hIcon != 0)
            {
                ra.bmp = Bitmap::FromHICON(ra.hIcon);
                if (ra.bmp != 0)
                {
                    if (ra.bmp->GetLastStatus() != Ok)
                    {
                        delete ra.bmp;
                        ra.bmp = 0;
                    }
                }
            }
            g_realApps.push_back(ra);
        } while (FindNextFileW(hd, &fd));
        FindClose(hd);
    }
}

static void LaunchRealApp(int idx)
{
    if (idx < 0 || idx >= (int)g_realApps.size()) return;
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    AllowSetForegroundWindow(ASFW_ANY);
    ShellExecuteW(0, L"open", g_realApps[idx].path.c_str(), 0, 0, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// steam easter egg thread (cross-thread PostMessage is part of the design)
// ---------------------------------------------------------------------------
static void SteamWorker(HWND hw)
{
    int p;
    for (p = 0; p <= 100; p += 2)
    {
        Sleep(50);
        if (!IsWindow(hw)) break;
        PostMessageW(hw, WM_APP + 1, (WPARAM)p, 0);
    }
    if (IsWindow(hw)) PostMessageW(hw, WM_APP + 2, 0, 0);
    g_steamThreadLive = false;
}

static void StartSteam()
{
    if (g_steamThreadLive) return;
    if (g_steamThread.joinable()) g_steamThread.join();
    g_steamThreadLive = true;
    g_steamThread = std::thread(SteamWorker, g_hwnd);
}

static void SteamShutdown()
{
    if (g_steamThread.joinable()) g_steamThread.join();
}

// ---------------------------------------------------------------------------
// per frame state machine
// ---------------------------------------------------------------------------
static void Tick()
{
    ULONGLONG tick = GetTickCount64();

    if (g_state == ST_BOOT_BLACK)
    {
        if (tick - g_stateStart > 600)
        {
            g_state = ST_BOOT_ANIM;
            g_stateStart = tick;
        }
    }
    else if (g_state == ST_BOOT_ANIM)
    {
        if (tick - g_stateStart > 1600)
        {
            g_state = ST_LOCK;
            g_stateStart = tick;
        }
    }
    else if (g_state == ST_BSOD)
    {
        if (tick - g_stateStart > 2000) PostQuitMessage(0);
    }
    if (g_shutdownAt != 0 && tick >= g_shutdownAt)
    {
        g_shutdownAt = 0;
        PostQuitMessage(0);
    }
    else
    {
        if (g_bsodAt != 0 && tick >= g_bsodAt)
        {
            g_bsodAt = 0;
            g_state = ST_BSOD;
            g_stateStart = tick;
        }
    }

    float step = 0.18f;
    float target = g_menuOpen ? 1.0f : 0.0f;
    if (g_menuAnim < target) g_menuAnim += step;
    else if (g_menuAnim > target) g_menuAnim -= step;
    if (g_menuAnim < 0.0f) g_menuAnim = 0.0f;
    if (g_menuAnim > 1.0f) g_menuAnim = 1.0f;

    if (g_drawerOpen && g_toastUntil != 0 && tick > g_toastUntil + 400)
        g_drawerOpen = false;

    // 开机动画结束 3 秒后，灵动岛向上弹一条提醒；2.5 秒后自动收回
    if (g_state == ST_DESKTOP && !g_bootToastDone && tick - g_stateStart > 3000)
    {
        g_bootToastDone = true;
        SetToast(L"鲸鱼娘已上线 · 点我看今天要做什么", 2500);
    }
    target = g_drawerOpen ? 1.0f : 0.0f;
    if (g_drawerAnim < target) g_drawerAnim += step;
    else if (g_drawerAnim > target) g_drawerAnim -= step;
    if (g_drawerAnim < 0.0f) g_drawerAnim = 0.0f;
    if (g_drawerAnim > 1.0f) g_drawerAnim = 1.0f;
}

// ---------------------------------------------------------------------------
// hit helpers for UI chrome
// ---------------------------------------------------------------------------
static int StartTileHit(int mx, int my)
{
    int i;
    for (i = 0; i < 6; i++)
    {
        int x, y, s;
        StartTileRect(i, x, y, s);
        if (PtInRectI(mx, my, x, y, s, s)) return i;
    }
    return -1;
}

static int StartListHit(int mx, int my)
{
    int mx0, my0, mw, mh;
    StartMenuRect(mx0, my0, mw, mh);
    int i;
    for (i = 0; i < 4; i++)
    {
        int lx = mx0 + 360;
        int ly = my0 + 138 + i * 44;
        if (PtInRectI(mx, my, lx, ly, 256, 40)) return i;
    }
    return -1;
}

static int StartPowerHit(int mx, int my)
{
    int i;
    for (i = 0; i < 2; i++)
    {
        int x, y, w, h;
        StartPowerRect(i, x, y, w, h);
        if (PtInRectI(mx, my, x, y, w, h)) return i;
    }
    return -1;
}

static int CtxHitTest(int mx, int my)
{
    CtxLayout L;
    CtxMainRect(L, g_ctxX, g_ctxY);

    if (g_ctxOpen == CTX_NEW || g_ctxOpen == CTX_WALL)
    {
        int anchor = (g_ctxOpen == CTX_NEW) ? 3 : 5;
        int subCount = (g_ctxOpen == CTX_NEW) ? 2 : 3;
        int sx, sy, sw, sh;
        CtxSubRect(L, anchor, subCount, sx, sy, sw, sh);
        int k;
        for (k = 0; k < subCount; k++)
        {
            if (PtInRectI(mx, my, sx + 4, sy + 4 + k * 32, sw - 8, 30)) return 100 + k;
        }
    }

    if (g_ctxOpen == CTX_MAIN)
    {
        int i;
        for (i = 0; i < L.count; i++)
        {
            if (PtInRectI(mx, my, L.x + 4, L.y + 4 + i * L.itemH, L.w - 8, L.itemH - 2)) return i;
        }
    }
    return -1;
}

static void DoCtxAction(int id)
{
    if (id == 0)
    {
        ToastNow(L"这是假桌面，只有一种视图");
    }
    else if (id == 1)
    {
        ToastNow(L"排序方式：名称 / 大小 / 类型（演示）");
    }
    else if (id == 2)
    {
        SyncRealDesktop();
        ToastNow(L"桌面已刷新，已重新扫描真实快捷方式。");
    }
    else if (id == 3)
    {
        g_ctxOpen = CTX_NEW;
        g_ctxHover = -1;
        return;
    }
    else if (id == 4)
    {
        OpenApp(APP_RECYCLE);
    }
    else if (id == 5)
    {
        g_ctxOpen = CTX_WALL;
        g_ctxHover = -1;
        return;
    }
    else if (id == 6)
    {
        g_settingsOpen = true;
        g_calendarOpen = false;
    }
    else if (id == 7)
    {
        ToastNow(L"鲸鱼OS v2 · 单文件假桌面");
    }
    g_ctxOpen = CTX_NONE;
    g_ctxHover = -1;
}

static void DoCtxSubAction(int id)
{
    if (g_ctxOpen == CTX_NEW)
    {
        if (id == 0) OpenApp(APP_TEXTDOC);
        else if (id == 1) OpenApp(APP_PAINT);
    }
    else if (g_ctxOpen == CTX_WALL)
    {
        g_wallpaperType = id;
        ToastNow(L"壁纸已更换");
    }
    g_ctxOpen = CTX_NONE;
    g_ctxHover = -1;
}

// ---------------------------------------------------------------------------
// mouse / keyboard
// ---------------------------------------------------------------------------
static void OnLButtonDown(int mx, int my)
{
    g_mx = mx;
    g_my = my;
    g_lastX = mx;
    g_lastY = my;

    ULONGLONG tick = GetTickCount64();

    if (g_state == ST_LOCK)
    {
        g_state = ST_DESKTOP;
        g_stateStart = tick;
        RecalcActive();
        return;
    }
    if (g_state != ST_DESKTOP) return;

    // drag handles of the floating panels
    if (g_settingsOpen)
    {
        if (PtInRectI(mx, my, g_setX, g_setY, 520, 560))
        {
            if (PtInRectI(mx, my, g_setX + 520 - 44, g_setY + 6, 34, 30))
            {
                g_settingsOpen = false;
                return;
            }
            int i;
            for (i = 0; i < 3; i++)
            {
                if (PtInRectI(mx, my, g_setX + 40 + i * 150, g_setY + 140, 140, 40))
                {
                    g_wallpaperType = i;
                    SetToast(L"壁纸已更换", 1600);
                    return;
                }
                if (PtInRectI(mx, my, g_setX + 40 + i * 150, g_setY + 268, 140, 40))
                {
                    g_materialType = i;
                    SetToast(L"材质已切换", 1600);
                    return;
                }
            }
            if (PtInRectI(mx, my, g_setX + 40, g_setY + 392, 140, 40))
            {
                g_islandOn = true;
                return;
            }
            if (PtInRectI(mx, my, g_setX + 190, g_setY + 392, 140, 40))
            {
                g_islandOn = false;
                return;
            }
            if (PtInRectI(mx, my, g_setX + 8, g_setY + 4, 520 - 60, 32))
            {
                g_dragPanel = 1;
                return;
            }
            return;
        }
    }

    if (g_calendarOpen)
    {
        if (PtInRectI(mx, my, g_calX, g_calY, 300, 364))
        {
            if (PtInRectI(mx, my, g_calX + 300 - 44, g_calY + 44, 40, 30))
            {
                g_calendarOpen = false;
                return;
            }
            if (PtInRectI(mx, my, g_calX + 8, g_calY + 8, 32, 28))
            {
                g_calOffset -= 1;
                return;
            }
            if (PtInRectI(mx, my, g_calX + 300 - 40, g_calY + 8, 32, 28))
            {
                g_calOffset += 1;
                return;
            }
            if (PtInRectI(mx, my, g_calX + 44, g_calY + 4, 300 - 92, 36))
            {
                g_dragPanel = 2;
                return;
            }
            return;
        }
    }

    // context menu has the highest priority
    if (g_ctxOpen != CTX_NONE)
    {
        int id = CtxHitTest(mx, my);
        if (id >= 100)
        {
            DoCtxSubAction(id - 100);
            return;
        }
        if (id >= 0)
        {
            DoCtxAction(id);
            return;
        }
        // click inside the menu but not on an item: keep it open
        {
            CtxLayout L;
            CtxMainRect(L, g_ctxX, g_ctxY);
            if (PtInRectI(mx, my, L.x, L.y, L.w, L.count * L.itemH + 8)) return;
        }
        g_ctxOpen = CTX_NONE;
        g_ctxHover = -1;
        return;
    }

    // 灵动岛：先判四个竖排快捷按钮，再判胶囊本体
    if (g_islandOn)
    {
        int i;
        for (i = 0; i < 4; i++)
        {
            int bx, by, bs;
            IslandBtnRect(i, bx, by, bs);
            if (PtInRectI(mx, my, bx, by, bs, bs))
            {
                if (i == 0) g_menuOpen = true;
                else if (i == 1) g_settingsOpen = true;
                else if (i == 2) OpenApp(APP_RECYCLE);
                else g_calendarOpen = true;
                return;
            }
        }
    }

    if (g_islandOn)
    {
        int cx, cy, cw, ch;
        IslandCapsule(cx, cy, cw, ch);
        if (PtInRectI(mx, my, cx, cy, cw, ch))
        {
            if (g_drawerOpen) { g_drawerOpen = false; g_toastText.clear(); g_toastUntil = 0; }
            else g_drawerOpen = true;
            return;
        }
    }

    // start menu
    if (g_menuOpen && g_menuAnim > 0.5f)
    {
        int ti = StartTileHit(mx, my);
        if (ti >= 0)
        {
            OpenApp(DESKTOP_KIND[ti]);
            return;
        }
        int li = StartListHit(mx, my);
        if (li >= 0)
        {
            if (li == 0) OpenApp(APP_RECYCLE);
            else if (li == 1) OpenApp(APP_TEXTDOC);
            else if (li == 2)
            {
                g_wallpaperType = (g_wallpaperType + 1) % 3;
                ToastNow(L"壁纸已更换");
            }
            else ToastNow(L"鲸鱼OS v2 · 假桌面");
            g_menuOpen = false;
            return;
        }
        int pi = StartPowerHit(mx, my);
        if (pi == 0)
        {
            g_menuOpen = false;
            g_state = ST_BOOT_BLACK;
            g_stateStart = tick;
            return;
        }
        if (pi == 1)
        {
            g_menuOpen = false;
            g_shutdownAt = tick + 800;
            return;
        }
        {
            int mx0, my0, mw, mh;
            StartMenuRect(mx0, my0, mw, mh);
            if (!PtInRectI(mx, my, mx0, my0, mw, mh)) g_menuOpen = false;
            return;
        }
    }

    // taskbar
    if (my >= TaskbarY())
    {
        if (mx < 60)
        {
            g_menuOpen = !g_menuOpen;
            g_ctxOpen = CTX_NONE;
            return;
        }
        int i;
        for (i = 0; i < 4; i++)
        {
            int x, y, s;
            TileRect(i, x, y, s);
            if (!PtInRectI(mx, my, x, y, s, s)) continue;
            int wi = FindWinByKind(PINNED_KIND[i]);
            if (wi < 0)
            {
                OpenApp(PINNED_KIND[i]);
                return;
            }
            if (wi == g_active && !g_wins[wi].minimized)
            {
                MinimizeWin(wi);
                return;
            }
            g_wins[wi].minimized = false;
            BringToFront(wi);
            RecalcActive();
            return;
        }
        int rc = RunBtnCount();
        for (i = 0; i < rc; i++)
        {
            int x, y, w, h;
            RunBtnRect(i, x, y, w, h);
            if (!PtInRectI(mx, my, x, y, w, h)) continue;
            int wi = RunBtnWindow(i);
            if (wi < 0) return;
            if (wi == g_active && !g_wins[wi].minimized)
            {
                MinimizeWin(wi);
                return;
            }
            g_wins[wi].minimized = false;
            BringToFront(wi);
            RecalcActive();
            return;
        }
        if (PtInRectI(mx, my, 46, TaskbarY(), 236, TASKBAR_H))
        {
            ToastNow(L"搜索：这是演示，不会真的搜");
            return;
        }
        if (PtInRectI(mx, my, 290, TaskbarY(), 40, TASKBAR_H))
        {
            ToastNow(L"任务视图：演示");
            return;
        }
        if (PtInRectI(mx, my, g_W - 176, TaskbarY(), 40, TASKBAR_H))
        {
            ToastNow(L"没有新消息");
            return;
        }
        if (PtInRectI(mx, my, g_W - 100, TaskbarY(), 100, TASKBAR_H))
        {
            g_calendarOpen = !g_calendarOpen;
            return;
        }
        return;
    }

    // windows
    {
        int wi = -1;
        int btn = -1;
        if (HitTopWindow(mx, my, wi, btn))
        {
            if (btn == 1)
            {
                CloseWin(wi);
                return;
            }
            if (btn == 0)
            {
                MinimizeWin(wi);
                return;
            }
            PosWin& w = g_wins[wi];
            if (my < w.y + 34)
            {
                BringToFront(wi);
                g_movingWin = (int)g_wins.size() - 1;
                return;
            }

            // body clicks
            if (w.kind == APP_BAIDU)
            {
                if (my >= w.y + 34 + 8 && my <= w.y + 34 + 42)
                {
                    g_urlFocus = true;
                }
                else
                {
                    g_urlFocus = false;
                }
            }
            else if (w.kind == APP_PAINT)
            {
                int bx, by, bw, bh;
                WinBodyRect(w, bx, by, bw, bh);
                int i;
                bool handled = false;
                for (i = 0; i < 5; i++)
                {
                    int sx = bx + 8 + i * 32;
                    int sy = by + 8;
                    if (PtInRectI(mx, my, sx - 2, sy - 2, 28, 28))
                    {
                        w.paintColor = i;
                        handled = true;
                        break;
                    }
                }
                if (!handled && PtInRectI(mx, my, bx + bw - 80, by + 6, 72, 28))
                {
                    w.strokes.clear();
                    handled = true;
                }
                if (!handled && PtInRectI(mx, my, bx, by + 40, bw, bh - 40))
                {
                    InkStroke st;
                    st.color = w.paintColor;
                    Point p0((INT)mx, (INT)my);
                    st.pts.push_back(p0);
                    w.strokes.push_back(st);
                    g_paintWin = wi;
                    handled = true;
                }
                if (handled)
                {
                    BringToFront(wi);
                    RecalcActive();
                    return;
                }
            }
            else if (w.kind == APP_RECYCLE)
            {
                int bx, by, bw, bh;
                WinBodyRect(w, bx, by, bw, bh);
                if (PtInRectI(mx, my, bx + bw / 2 - 80, by + bh - 48, 160, 36))
                {
                    g_recycleCount = 0;
                    ToastNow(L"回收站已清空");
                    return;
                }
            }

            g_wins[wi].minimized = false;
            BringToFront(wi);
            RecalcActive();
            return;
        }
    }

    // desktop icons
    {
        int i;
        for (i = 0; i < 6; i++)
        {
            int x, y, s;
            DesktopIconRect(i, x, y, s);
            if (PtInRectI(mx, my, x, y, s, s + 18))
            {
                OpenApp(DESKTOP_KIND[i]);
                return;
            }
        }
        for (i = 0; i < (int)g_realApps.size(); i++)
        {
            int x, y, s;
            DesktopIconRect(6 + i, x, y, s);
            if (PtInRectI(mx, my, x, y, s, s + 18))
            {
                LaunchRealApp(i);
                return;
            }
        }
    }

    // empty desktop
    g_menuOpen = false;
    g_settingsOpen = false;
    g_calendarOpen = false;
    g_drawerOpen = false;
    g_urlFocus = false;
    g_active = -1;
}

static void OnRButtonDown(int mx, int my)
{
    if (g_state != ST_DESKTOP) return;

    // right click on a running window button closes that window
    if (my >= TaskbarY())
    {
        int i;
        for (i = 0; i < (int)g_wins.size(); i++)
        {
            int x, y, w, h;
            RunBtnRect(i, x, y, w, h);
            if (PtInRectI(mx, my, x, y, w, h))
            {
                CloseWin(i);
                return;
            }
        }
        return;
    }

    int wi = -1;
    int btn = -1;
    if (HitTopWindow(mx, my, wi, btn)) return;
    if (g_settingsOpen && PtInRectI(mx, my, g_setX, g_setY, 480, 540)) return;
    if (g_calendarOpen && PtInRectI(mx, my, g_calX, g_calY, 300, 364)) return;

    g_ctxX = mx;
    g_ctxY = my;
    g_ctxOpen = CTX_MAIN;
    g_ctxHover = -1;
    g_menuOpen = false;
    g_drawerOpen = false;
}

static void OnMouseMove(int mx, int my)
{
    int dx = mx - g_lastX;
    int dy = my - g_lastY;
    g_lastX = mx;
    g_lastY = my;
    g_mx = mx;
    g_my = my;

    if (g_dragPanel == 1)
    {
        g_setX += dx;
        g_setY += dy;
        g_setX = ClampI(g_setX, 8, g_W - 528);
        g_setY = ClampI(g_setY, 8, TaskbarY() - 568);
        return;
    }
    if (g_dragPanel == 2)
    {
        g_calX += dx;
        g_calY += dy;
        g_calX = ClampI(g_calX, 8, g_W - 308);
        g_calY = ClampI(g_calY, 8, TaskbarY() - 372);
        return;
    }
    if (g_movingWin >= 0 && g_movingWin < (int)g_wins.size())
    {
        PosWin& w = g_wins[g_movingWin];
        w.x += dx;
        w.y += dy;
        w.x = ClampI(w.x, 4 - w.w / 2, g_W - w.w / 2);
        w.y = ClampI(w.y, 4, TaskbarY() - 40);
        return;
    }
    if (g_paintWin >= 0 && g_paintWin < (int)g_wins.size())
    {
        PosWin& w = g_wins[g_paintWin];
        int bx, by, bw, bh;
        WinBodyRect(w, bx, by, bw, bh);
        if (PtInRectI(mx, my, bx, by + 40, bw, bh - 40))
        {
            if (!w.strokes.empty())
            {
                InkStroke& st = w.strokes[w.strokes.size() - 1];
                Point p0((INT)mx, (INT)my);
                if (st.pts.empty())
                {
                    st.pts.push_back(p0);
                }
                else
                {
                    Point last = st.pts[st.pts.size() - 1];
                    int ddx = (int)p0.X - (int)last.X;
                    int ddy = (int)p0.Y - (int)last.Y;
                    if (ddx * ddx + ddy * ddy > 6) st.pts.push_back(p0);
                }
            }
        }
        return;
    }

    if (g_ctxOpen != CTX_NONE)
    {
        g_ctxHover = CtxHitTest(mx, my);
    }
}

static void OnLButtonUp()
{
    g_movingWin = -1;
    g_paintWin = -1;
    g_dragPanel = 0;
}

static void OnChar(wchar_t ch)
{
    if (g_state != ST_DESKTOP) return;
    if (g_active < 0 || g_active >= (int)g_wins.size()) return;
    PosWin& w = g_wins[g_active];

    if (w.kind == APP_NOTEPAD || w.kind == APP_TEXTDOC)
    {
        if (ch == (wchar_t)8)
        {
            if (!w.text.empty()) w.text.erase(w.text.size() - 1);
        }
        else if (ch == (wchar_t)13)
        {
            if (w.text.size() < 4000) w.text += L"\n";
        }
        else if (ch >= (wchar_t)32 && ch < (wchar_t)127)
        {
            if (w.text.size() < 4000) w.text += ch;
        }
        return;
    }
    if (w.kind == APP_BAIDU && g_urlFocus)
    {
        if (ch == (wchar_t)8)
        {
            if (!w.url.empty()) w.url.erase(w.url.size() - 1);
        }
        else if (ch == (wchar_t)13)
        {
            if (!w.url.empty())
            {
                w.query = w.url;
                w.url = L"搜索：" + w.query;
            }
        }
        else if (ch >= (wchar_t)32 && ch < (wchar_t)127)
        {
            if (w.url.size() < 120) w.url += ch;
        }
    }
}

// ---------------------------------------------------------------------------
// GDI resources / startup
// ---------------------------------------------------------------------------
static void EnsureDIB()
{
    if (g_dib != 0) return;
    g_dcScr = GetDC(g_hwnd);
    g_dcMem = CreateCompatibleDC(g_dcScr);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_W;
    bi.bmiHeader.biHeight = -g_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(g_dcScr, &bi, DIB_RGB_COLORS, &g_bits, 0, 0);
    if (g_dib != 0) SelectObject(g_dcMem, g_dib);
}

static void InitFonts()
{
    int i;
    for (i = 0; i < 8; i++)
    {
        g_font[0][i] = new Font(L"Microsoft YaHei UI", (REAL)FONT_SIZE[i], FontStyleRegular, UnitPixel);
        g_font[1][i] = new Font(L"Microsoft YaHei UI", (REAL)FONT_SIZE[i], FontStyleBold, UnitPixel);
    }
}

static void FreeFonts()
{
    int i;
    for (i = 0; i < 8; i++)
    {
        if (g_font[0][i] != 0) { delete g_font[0][i]; g_font[0][i] = 0; }
        if (g_font[1][i] != 0) { delete g_font[1][i]; g_font[1][i] = 0; }
    }
}

static void PrecomputeStars()
{
    g_stars.clear();
    srand(1234);
    int i;
    for (i = 0; i < 140; i++)
    {
        Star s;
        s.x = rand() % (g_W > 1 ? g_W : 2);
        s.y = rand() % (g_H > 100 ? (g_H - 100) : 2);
        s.r = 1 + rand() % 2;
        s.phase = (float)(rand() % 628) / 100.0f;
        g_stars.push_back(s);
    }
}

static void CleanupAll()
{
    SteamShutdown();
    DestroyRealApps();
    FreeFonts();
    if (g_dcMem != 0)
    {
        if (g_dib != 0)
        {
            DeleteObject(g_dib);
            g_dib = 0;
        }
        DeleteDC(g_dcMem);
        g_dcMem = 0;
    }
    if (g_dcScr != 0)
    {
        ReleaseDC(g_hwnd, g_dcScr);
        g_dcScr = 0;
    }
    if (g_gdiToken != 0)
    {
        Gdiplus::GdiplusShutdown(g_gdiToken);
        g_gdiToken = 0;
    }
}

static void RenderFrame()
{
    if (g_dcMem == 0) return;
    {
        Graphics g(g_dcMem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        RenderUI(g, GetTickCount64());
    }

    POINT ptDst;
    ptDst.x = 0;
    ptDst.y = 0;
    SIZE sz;
    sz.cx = g_W;
    sz.cy = g_H;
    POINT ptSrc;
    ptSrc.x = 0;
    ptSrc.y = 0;
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, g_dcScr, &ptDst, &sz, g_dcMem, &ptSrc, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------------------
// window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TIMER)
    {
        if (wParam == 1)
        {
            Tick();
            RenderFrame();
        }
        return 0;
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
    {
        bool ctrl = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
        bool alt = ((GetKeyState(VK_MENU) & 0x8000) != 0);
        if (ctrl && alt && (wParam == (WPARAM)'Q'))
        {
            PostQuitMessage(0);
            return 0;
        }
        // Win 键：开始菜单
        if (wParam == VK_LWIN || wParam == VK_RWIN)
        {
            g_menuOpen = !g_menuOpen;
            return 0;
        }
        // Esc：关掉所有浮层
        if (wParam == VK_ESCAPE)
        {
            g_menuOpen = false;
            g_settingsOpen = false;
            g_calendarOpen = false;
            g_ctxOpen = CTX_NONE;
            g_drawerOpen = false;
            return 0;
        }
        // F11：换壁纸
        if (wParam == VK_F11)
        {
            g_wallpaperType = (g_wallpaperType + 1) % 3;
            ToastNow(L"壁纸已更换");
            return 0;
        }
        // F1：快捷键提示
        if (wParam == VK_F1)
        {
            SetToast(L"Win 开始菜单 · F11 换壁纸 · Esc 关浮层 · Ctrl+Alt+Q 退出", 5000);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_CHAR)
    {
        OnChar((wchar_t)wParam);
        return 0;
    }

    if (msg == WM_LBUTTONDOWN)
    {
        OnLButtonDown((int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
        return 0;
    }
    if (msg == WM_LBUTTONUP)
    {
        OnLButtonUp();
        return 0;
    }
    if (msg == WM_RBUTTONDOWN)
    {
        OnRButtonDown((int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
        return 0;
    }
    if (msg == WM_MOUSEMOVE)
    {
        OnMouseMove((int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
        return 0;
    }

    if (msg == WM_ACTIVATE)
    {
        if (LOWORD(wParam) == WA_ACTIVE)
        {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }

    if (msg == WM_APP + 1)
    {
        g_steamPct = (int)wParam;
        if (g_steamPct < 0) g_steamPct = 0;
        if (g_steamPct > 100) g_steamPct = 100;
        return 0;
    }

    if (msg == WM_APP + 2)
    {
        if (g_state == ST_STEAM)
        {
            g_steamPct = 96;                       // 卡在 96%，开始刷红弹窗
            g_bsodAt = GetTickCount64() + 1600;    // 1.6 秒后蓝屏
        }
        return 0;
    }

    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    if (msg == WM_DESTROY)
    {
        KillTimer(hwnd, 1);
        CleanupAll();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&g_gdiToken, &gsi, 0) != Ok)
    {
        return 1;
    }

    g_W = GetSystemMetrics(SM_CXSCREEN);
    g_H = GetSystemMetrics(SM_CYSCREEN);
    if (g_W < 800) g_W = 800;
    if (g_H < 600) g_H = 600;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hbrBackground = 0;
    wc.lpszClassName = L"WHALE_OS_FAKE_DESKTOP";
    if (RegisterClassExW(&wc) == 0) return 1;

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"鲸鱼OS v2",
        WS_POPUP,
        0, 0, g_W, g_H,
        0, 0, hInst, 0);
    if (g_hwnd == 0) return 1;

    EnsureDIB();
    InitFonts();
    PrecomputeStars();
    SyncRealDesktop();

    g_setX = (g_W - 520) / 2;
    g_setY = 100;
    g_calX = g_W - 316;
    if (g_calX < 8) g_calX = 8;
    g_calY = 96;

    g_state = ST_BOOT_BLACK;
    g_stateStart = GetTickCount64();

    AllowSetForegroundWindow(ASFW_ANY);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    SetTimer(g_hwnd, 1, 16, 0);

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hwnd != 0) DestroyWindow(g_hwnd);
    CleanupAll();
    return 0;
}

// ============================================================================
//  end of file
// ============================================================================
