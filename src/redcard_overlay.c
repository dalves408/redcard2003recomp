/* ESC quit + F1 settings overlays (GDI layered windows), ported from NHL Hitz Pro.
 * Hook from redcard_debug_message / redcard_present_tick. Settings live in
 * %LOCALAPPDATA%\RedCard2003\settings.ini [display]. */

#include "redcard_overlay.h"

#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "xinput.lib")

extern void xbox_InputKeysClear(void);
extern void xbox_InputKeySet(int vk, int down);
extern volatile int redcard_input_block;
extern void xbox_host_pause_begin(void);
extern void xbox_host_pause_end(void);
extern void xa2_set_muted(int muted);
extern void xbox_FramebufferToggleFullscreen(HWND h);
extern void xbox_FramebufferSetStretch(int on);
extern void xbox_FramebufferSetVsync(int on);
extern void redcard_set_anisotropy(int aniso);
extern void redcard_set_resolution_scale(int scale);
extern void redcard_set_msaa(int samples);
extern void redcard_set_hires(int on);
extern int redcard_pack_installed(void);

/* Red-card chrome */
#define ACCENT RGB(220, 40, 40)
#define PANEL  RGB(20, 12, 14)
#define ROW_OFF RGB(48, 28, 32)
#define ROW_DIS RGB(36, 22, 26)
#define TXT_DIM RGB(180, 160, 165)
#define TXT_DIS RGB(110, 90, 95)

/* Hitz F1 order including Hi-res textures. */
enum { IT_RES, IT_FS, IT_STRETCH, IT_VSYNC, IT_FPS, IT_AA, IT_ANISO, IT_HIRES, IT_CLOSE, IT_QUIT };
#define SET_ITEMS 10

static const unsigned SET_FPS_HZ[6] = { 30, 60, 120, 144, 240, 360 };
static const char *const SET_LABEL[SET_ITEMS] = {
    "Render Scale", "Fullscreen", "Stretch image", "VSync", "Frame Rate",
    "Anti-aliasing", "Anisotropic filter", "Hi-res textures", "Close menu", "Quit game"
};
static const char *const SET_OFFON[2] = { "Off", "On" };
static const char *const SET_RES[4] = {
    "1x (640x480)", "2x (1280x960)", "3x (1920x1440)", "4x (2560x1920)"
};
static const char *const SET_AA[4] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
static const char *const SET_ANISO[5] = { "Off", "2x", "4x", "8x", "16x" };
static const char *const SET_STRETCH[2] = { "Off (4:3)", "On (fills window)" };
static const char *const SET_FPS[6] = {
    "30", "60 (original)", "120", "144", "240", "Uncapped (up to 360)"
};

static int s_q_active, s_q_sel;            /* sel: 0 = No, 1 = Yes */
static int s_set_active, s_set_sel, s_set_armed;
static HWND s_q_hwnd, s_game;
static RECT s_q_btn[2];
static RECT s_set_row[SET_ITEMS];
static int s_q_cw, s_q_ch, s_q_ox, s_q_oy;

static int s_fs, s_stretch, s_vsync, s_aniso = 1, s_fpsi = 1, s_scale = 1, s_msaa = 1, s_hires = 1;
static int s_pad_valid, s_unblock_wait;
static DWORD s_unblock_t;
static WORD s_pad_prev;
static DWORD s_pad_rep;

typedef struct {
    DWORD pkt;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
} RedPadState;

static int overlay_key(WPARAM w, LPARAM l, int down);
static void quit_draw(void);
static void quit_close(void);
static void set_draw(void);

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int cfg_path(char *out, size_t n)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return 0;
    snprintf(out, n, "%s\\RedCard2003", base);
    CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\RedCard2003\\settings.ini", base);
    return 1;
}

static int cfg_get(const char *key, int def)
{
    char path[MAX_PATH];
    return cfg_path(path, sizeof path) ? (int)GetPrivateProfileIntA("display", key, def, path) : def;
}

static void cfg_put(const char *key, int v)
{
    char path[MAX_PATH], val[16];
    if (!cfg_path(path, sizeof path)) return;
    snprintf(val, sizeof val, "%d", v);
    WritePrivateProfileStringA("display", key, val, path);
}

static int cfg_get_s(const char *section, const char *key, int def)
{
    char path[MAX_PATH];
    return cfg_path(path, sizeof path) ? (int)GetPrivateProfileIntA(section, key, def, path) : def;
}

static void cfg_put_s(const char *section, const char *key, int v)
{
    char path[MAX_PATH], val[16];
    if (!cfg_path(path, sizeof path)) return;
    snprintf(val, sizeof val, "%d", v);
    WritePrivateProfileStringA(section, key, val, path);
}

static void refresh_fs(HWND game)
{
    LONG style;
    if (!game) return;
    style = GetWindowLongA(game, GWL_STYLE);
    s_fs = ((style & WS_POPUP) != 0) && ((style & WS_OVERLAPPEDWINDOW) == 0);
}

static void settings_load_state(void)
{
    int a, hz, i, best = 1, bd = 1 << 30;
    s_scale = clampi(cfg_get("resolution_scale", 1), 1, 4);
    s_stretch = cfg_get("stretch", 0) != 0;
    s_vsync = cfg_get("vsync", 1) != 0;
    a = cfg_get("msaa", 1);
    s_msaa = a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
    a = cfg_get("anisotropy", 1);
    s_aniso = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
    hz = cfg_get("frame_rate", 60);
    for (i = 0; i < 6; i++)
        if (abs((int)SET_FPS_HZ[i] - hz) < bd) {
            bd = abs((int)SET_FPS_HZ[i] - hz);
            best = i;
        }
    s_fpsi = best;
    s_fs = cfg_get("fullscreen", 0) != 0;
    s_hires = cfg_get_s("textures", "enabled", 1) != 0;
}

void redcard_settings_apply_env(void)
{
    char buf[16];
    int fs, stretch, vsync, aniso, hz, a, i, best = 1, bd = 1 << 30;
    int scale, msaa;

    scale = clampi(cfg_get("resolution_scale", 1), 1, 4);
    fs = cfg_get("fullscreen", 0) != 0;
    stretch = cfg_get("stretch", 0) != 0;
    vsync = cfg_get("vsync", 1) != 0;
    a = cfg_get("msaa", 1);
    msaa = a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
    a = cfg_get("anisotropy", 1);
    aniso = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
    hz = cfg_get("frame_rate", 60);
    for (i = 0; i < 6; i++)
        if (abs((int)SET_FPS_HZ[i] - hz) < bd) {
            bd = abs((int)SET_FPS_HZ[i] - hz);
            best = i;
        }
    hz = (int)SET_FPS_HZ[best];

    /* Always write so F1/config choices win over empty env. */
    snprintf(buf, sizeof buf, "%d", scale);
    _putenv_s("RECOMP_D3D11_SCALE", buf);
    {   /* RecompLauncher's Big Picture mode (controller, couch): RECOMPLAUNCHER_MODE=bigpicture
         * (or "bigscreen") starts fullscreen whatever the setting says. The saved setting is
         * not changed, so a desktop start still uses it. */
        const char *mode = getenv("RECOMPLAUNCHER_MODE");
        if (mode && (!_stricmp(mode, "bigpicture") || !_stricmp(mode, "bigscreen"))) {
            if (!fs) fprintf(stderr, "[RC] RECOMPLAUNCHER_MODE=%s: starting fullscreen\n", mode);
            fs = 1;
        }
    }
    snprintf(buf, sizeof buf, "%d", fs ? 1 : 0);
    _putenv_s("RECOMP_FULLSCREEN", buf);
    snprintf(buf, sizeof buf, "%d", stretch ? 1 : 0);
    _putenv_s("RECOMP_STRETCH", buf);
    snprintf(buf, sizeof buf, "%d", vsync ? 1 : 0);
    _putenv_s("RECOMP_PRESENT_VSYNC", buf);
    snprintf(buf, sizeof buf, "%d", msaa);
    _putenv_s("RECOMP_D3D11_MSAA", buf);
    snprintf(buf, sizeof buf, "%d", aniso);
    _putenv_s("RECOMP_D3D11_ANISO", buf);
    snprintf(buf, sizeof buf, "%d", hz);
    _putenv_s("RECOMP_VBLANK_HZ", buf);

    s_scale = scale;
    s_fs = fs;
    s_stretch = stretch;
    s_vsync = vsync;
    s_msaa = msaa;
    s_aniso = aniso;
    s_fpsi = best;

    s_hires = cfg_get_s("textures", "enabled", 1) != 0;
    redcard_set_resolution_scale(scale);
    redcard_set_msaa(msaa);
    redcard_set_anisotropy(aniso);
    redcard_set_hires(s_hires);
}

static int set_disabled(int i)
{
    return i == IT_HIRES && !redcard_pack_installed();
}

static int set_count(int i)
{
    switch (i) {
    case IT_RES: return 4;
    case IT_ANISO: return 5;
    case IT_AA: return 4;
    case IT_FPS: return 6;
    case IT_FS: case IT_STRETCH: case IT_VSYNC: return 2;
    case IT_HIRES: return set_disabled(IT_HIRES) ? 0 : 2;
    }
    return 0;
}

static int set_get(int i)
{
    switch (i) {
    case IT_RES: return s_scale - 1;
    case IT_FS: return s_fs ? 1 : 0;
    case IT_STRETCH: return s_stretch ? 1 : 0;
    case IT_VSYNC: return s_vsync ? 1 : 0;
    case IT_FPS: return s_fpsi;
    case IT_AA: return s_msaa >= 8 ? 3 : s_msaa >= 4 ? 2 : s_msaa >= 2 ? 1 : 0;
    case IT_ANISO: return s_aniso >= 16 ? 4 : s_aniso >= 8 ? 3 : s_aniso >= 4 ? 2 : s_aniso >= 2 ? 1 : 0;
    case IT_HIRES: return s_hires ? 1 : 0;
    }
    return 0;
}

static const char *set_text(int i)
{
    int v = set_get(i);
    switch (i) {
    case IT_RES: return SET_RES[v];
    case IT_STRETCH: return SET_STRETCH[v];
    case IT_FPS: return SET_FPS[v];
    case IT_AA: return SET_AA[v];
    case IT_ANISO: return SET_ANISO[v];
    case IT_FS: case IT_VSYNC: return SET_OFFON[v];
    case IT_HIRES: return !redcard_pack_installed() ? "Not installed" : SET_OFFON[v];
    }
    return "";
}

static void set_put(int i, int v)
{
    switch (i) {
    case IT_RES:
        s_scale = v + 1;
        cfg_put("resolution_scale", s_scale);
        redcard_set_resolution_scale(s_scale);
        break;
    case IT_FS:
        if (s_game) xbox_FramebufferToggleFullscreen(s_game);
        s_fs = !s_fs;
        cfg_put("fullscreen", s_fs ? 1 : 0);
        break;
    case IT_STRETCH:
        s_stretch = v;
        cfg_put("stretch", v);
        xbox_FramebufferSetStretch(v);
        break;
    case IT_VSYNC:
        s_vsync = v;
        cfg_put("vsync", v);
        xbox_FramebufferSetVsync(v);
        break;
    case IT_FPS:
        s_fpsi = v;
        cfg_put("frame_rate", (int)SET_FPS_HZ[v]);
        {
            extern void xbox_set_vblank_hz(unsigned hz);
            xbox_set_vblank_hz(SET_FPS_HZ[v]);
        }
        break;
    case IT_AA:
        s_msaa = v ? 1 << v : 1;
        cfg_put("msaa", s_msaa);
        redcard_set_msaa(s_msaa);
        break;
    case IT_ANISO:
        s_aniso = v ? 1 << v : 1;
        cfg_put("anisotropy", s_aniso);
        redcard_set_anisotropy(s_aniso);
        break;
    case IT_HIRES:
        s_hires = v;
        cfg_put_s("textures", "enabled", v);
        redcard_set_hires(v);
        break;
    }
}

static LRESULT CALLBACK quit_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcA(h, m, w, l);
}

static void set_draw(void)
{
    RECT cr, rr;
    POINT o = { 0, 0 };
    int cw, ch, dw, dh, dx, dy, rh, i, x, y;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc, mdc;
    HBITMAP bm;
    HGDIOBJ oldbm, oldf;
    HFONT ft, fr;
    uint32_t *px;
    BLENDFUNCTION bf;
    POINT p0 = { 0, 0 };
    SIZE sz;
    HBRUSH br;

    if (!s_q_hwnd || !s_game || IsIconic(s_game)) return;
    GetClientRect(s_game, &cr);
    ClientToScreen(s_game, &o);
    cw = cr.right; ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc); return; }
    oldbm = SelectObject(mdc, bm);
    px = (uint32_t *)bits;
    for (i = 0; i < cw * ch; i++) px[i] = 0x96000000u;

    rh = ch * 7 / 100; if (rh < 22) rh = 22;
    dw = rh * 17;
    if (dw > cw * 95 / 100) { dw = cw * 95 / 100; rh = dw / 17; }
    /* title + SET_ITEMS rows + hint footer */
    dh = rh * 14 / 10 + SET_ITEMS * rh + rh * 18 / 10;
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    SetRect(&rr, dx, dy, dx + dw, dy + dh);
    br = CreateSolidBrush(ACCENT); FillRect(mdc, &rr, br); DeleteObject(br);
    InflateRect(&rr, -(dh / 40 + 2), -(dh / 40 + 2));
    br = CreateSolidBrush(PANEL); FillRect(mdc, &rr, br); DeleteObject(br);

    SetBkMode(mdc, TRANSPARENT);
    ft = CreateFontA(-(rh * 80 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Impact");
    fr = CreateFontA(-(rh * 56 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Bahnschrift SemiCondensed");
    oldf = SelectObject(mdc, ft);
    SetTextColor(mdc, RGB(255, 255, 255));
    SetRect(&rr, dx, dy + rh / 4, dx + dw, dy + rh * 13 / 10);
    DrawTextA(mdc, "SETTINGS", -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mdc, fr);
    for (i = 0; i < SET_ITEMS; i++) {
        int sel = (i == s_set_sel);
        char buf[96];
        y = dy + rh * 14 / 10 + i * rh;
        x = dx + rh / 3;
        SetRect(&s_set_row[i], x, y, dx + dw - rh / 3, y + rh * 82 / 100);
        br = CreateSolidBrush(sel ? ACCENT : set_disabled(i) ? ROW_DIS : ROW_OFF);
        FillRect(mdc, &s_set_row[i], br); DeleteObject(br);
        SetTextColor(mdc, sel ? RGB(10, 10, 14) : set_disabled(i) ? TXT_DIS : TXT_DIM);
        rr = s_set_row[i]; rr.left += rh / 3;
        DrawTextA(mdc, SET_LABEL[i], -1, &rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rr = s_set_row[i]; rr.right -= rh / 3;
        if (set_count(i))
            snprintf(buf, sizeof buf, "<  %s  >", set_text(i));
        else if (i == IT_QUIT && s_set_armed)
            snprintf(buf, sizeof buf, "press again to quit");
        else if (i == IT_HIRES)
            snprintf(buf, sizeof buf, "%s", set_text(i));
        else
            buf[0] = 0;
        DrawTextA(mdc, buf, -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(mdc, RGB(140, 120, 125));
    SetRect(&rr, dx + rh / 3, dy + rh * 14 / 10 + SET_ITEMS * rh + rh / 6, dx + dw - rh / 3, dy + dh - rh / 8);
    {
        HFONT fh = CreateFontA(-(rh * 38 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Bahnschrift SemiCondensed");
        HGDIOBJ prev = SelectObject(mdc, fh);
        DrawTextA(mdc, "Up/Down choose, Left/Right change, Enter or A confirms. Saved automatically. F1, Esc or B closes.", -1, &rr, DT_LEFT | DT_WORDBREAK);
        SelectObject(mdc, prev);
        DeleteObject(fh);
    }
    SelectObject(mdc, oldf);
    DeleteObject(ft); DeleteObject(fr);
    GdiFlush();
    for (y = dy; y < dy + dh; y++)
        for (x = dx; x < dx + dw; x++)
            px[(size_t)y * cw + x] |= 0xFF000000u;

    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    sz.cx = cw; sz.cy = ch;
    UpdateLayeredWindow(s_q_hwnd, sdc, &o, &sz, mdc, &p0, 0, &bf, ULW_ALPHA);
    s_q_cw = cw; s_q_ch = ch; s_q_ox = o.x; s_q_oy = o.y;
    SelectObject(mdc, oldbm);
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

static void set_change(int i, int dir)
{
    int n = set_count(i);
    if (n) {
        int v = (set_get(i) + dir + n) % n;
        if (i == IT_FS) v = !s_fs;
        set_put(i, v);
        s_set_armed = 0;
    } else if (dir == 0) {
        if (i == IT_CLOSE) quit_close();
        else if (i == IT_QUIT) {
            if (s_set_armed) TerminateProcess(GetCurrentProcess(), 0);
            s_set_armed = 1;
        }
    }
    set_draw();
}

static int set_key(WPARAM w, LPARAM l, int down)
{
    int repeat = (l & (1L << 30)) != 0;
    if (!down) return 1;
    if (w == VK_ESCAPE || w == VK_F1) { if (!repeat) quit_close(); return 1; }
    if (w == VK_UP || w == VK_DOWN) {
        do s_set_sel = (s_set_sel + (w == VK_UP ? SET_ITEMS - 1 : 1)) % SET_ITEMS;
        while (set_disabled(s_set_sel));
        s_set_armed = 0;
        set_draw();
    } else if (w == VK_LEFT) set_change(s_set_sel, -1);
    else if (w == VK_RIGHT) set_change(s_set_sel, +1);
    else if ((w == VK_RETURN || w == VK_SPACE) && !repeat)
        set_change(s_set_sel, set_count(s_set_sel) ? +1 : 0);
    return 1;
}

static void quit_draw(void)
{
    RECT cr;
    POINT o = { 0, 0 };
    int cw, ch, dw, dh, dx, dy, bw, bh, by, x, y, i;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc, mdc;
    HBITMAP bm;
    HGDIOBJ oldbm, oldf;
    HFONT ft, fb;
    uint32_t *px;
    BLENDFUNCTION bf;
    POINT p0 = { 0, 0 };
    SIZE sz;
    RECT dr;
    HBRUSH br;

    if (s_set_active) { set_draw(); return; }
    if (!s_q_hwnd || !s_game || IsIconic(s_game)) return;
    GetClientRect(s_game, &cr);
    ClientToScreen(s_game, &o);
    cw = cr.right; ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc); return; }
    oldbm = SelectObject(mdc, bm);
    px = (uint32_t *)bits;
    for (i = 0; i < cw * ch; i++) px[i] = 0x96000000u;

    dw = ch * 4 / 5; if (dw > cw * 9 / 10) dw = cw * 9 / 10;
    dh = dw * 9 / 20;
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    SetRect(&dr, dx, dy, dx + dw, dy + dh);
    br = CreateSolidBrush(ACCENT); FillRect(mdc, &dr, br); DeleteObject(br);
    InflateRect(&dr, -dh / 40 - 2, -dh / 40 - 2);
    br = CreateSolidBrush(PANEL); FillRect(mdc, &dr, br); DeleteObject(br);

    bw = dw * 3 / 10; bh = dh * 28 / 100; by = dy + dh * 58 / 100;
    SetRect(&s_q_btn[0], dx + dw / 2 - bw - dw / 25, by, dx + dw / 2 - dw / 25, by + bh);
    SetRect(&s_q_btn[1], dx + dw / 2 + dw / 25, by, dx + dw / 2 + dw / 25 + bw, by + bh);
    SetBkMode(mdc, TRANSPARENT);
    ft = CreateFontA(-(dh * 17 / 100), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift SemiCondensed");
    fb = CreateFontA(-(dh * 15 / 100), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift SemiCondensed");
    oldf = SelectObject(mdc, ft);
    SetTextColor(mdc, RGB(255, 255, 255));
    SetRect(&dr, dx, dy + dh * 12 / 100, dx + dw, dy + dh * 50 / 100);
    DrawTextA(mdc, "Do you want to quit?", -1, &dr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mdc, fb);
    for (i = 0; i < 2; i++) {
        int on = (s_q_sel == i);
        br = CreateSolidBrush(on ? ACCENT : ROW_OFF);
        FillRect(mdc, &s_q_btn[i], br); DeleteObject(br);
        SetTextColor(mdc, on ? RGB(10, 10, 14) : TXT_DIM);
        DrawTextA(mdc, i ? "YES" : "NO", -1, &s_q_btn[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(mdc, oldf);
    DeleteObject(ft); DeleteObject(fb);
    GdiFlush();
    for (y = dy; y < dy + dh; y++)
        for (x = dx; x < dx + dw; x++)
            px[(size_t)y * cw + x] |= 0xFF000000u;

    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    sz.cx = cw; sz.cy = ch;
    UpdateLayeredWindow(s_q_hwnd, sdc, &o, &sz, mdc, &p0, 0, &bf, ULW_ALPHA);
    s_q_cw = cw; s_q_ch = ch; s_q_ox = o.x; s_q_oy = o.y;
    SelectObject(mdc, oldbm);
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

static void quit_open(void)
{
    if (!s_game) return;
    if (!s_q_hwnd) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = quit_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "RedcardQuitOverlay";
        RegisterClassA(&wc);
        s_q_hwnd = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            wc.lpszClassName, "", WS_POPUP, 0, 0, 1, 1, s_game, NULL, wc.hInstance, NULL);
        if (!s_q_hwnd) return;
    }
    settings_load_state();
    refresh_fs(s_game);
    xbox_InputKeysClear();
    redcard_input_block = 1; s_unblock_wait = 0; s_pad_valid = 0;
    s_q_sel = 0; s_q_active = 1;
    xbox_host_pause_begin();
    xa2_set_muted(1);
    quit_draw();
    ShowWindow(s_q_hwnd, SW_SHOWNOACTIVATE);
}

static void quit_close(void)
{
    if (s_q_active) {
        xa2_set_muted(0);
        xbox_host_pause_end();
        s_unblock_wait = 1;
        s_unblock_t = GetTickCount();
    }
    s_q_active = 0;
    s_set_active = 0; s_set_armed = 0;
    if (s_q_hwnd) ShowWindow(s_q_hwnd, SW_HIDE);
}

static void set_open(void)
{
    s_set_active = 1; s_set_sel = 0; s_set_armed = 0;
    quit_open();
}

static void pad_overlay_poll(void)
{
    RedPadState st;
    WORD b, press, dirs;
    DWORD now = GetTickCount();
    if (!s_q_active) return;
    if (XInputGetState(0, (XINPUT_STATE *)&st) != 0) { s_pad_prev = 0; s_pad_valid = 1; return; }
    b = st.buttons;
    if (st.lx < -16000) b |= 0x0004;
    if (st.lx > 16000)  b |= 0x0008;
    if (st.ly > 16000)  b |= 0x0001;
    if (st.ly < -16000) b |= 0x0002;
    if (!s_pad_valid) { s_pad_prev = b; s_pad_valid = 1; return; }
    press = (WORD)(b & ~s_pad_prev);
    dirs = (WORD)(b & 0x000F);
    if (press & 0x1000) overlay_key(VK_RETURN, 0, 1);              /* A */
    if (press & 0x2000) overlay_key(VK_ESCAPE, 0, 1);              /* B */
    else if (press & 0x0010) overlay_key(VK_ESCAPE, 0, 1);         /* Start */
    if (s_q_active) {
        WORD fire = 0;
        if (press & 0x000F) { fire = (WORD)(press & 0x000F); s_pad_rep = now + 350; }
        else if (dirs && now >= s_pad_rep) { fire = dirs; s_pad_rep = now + 110; }
        if (fire & 0x0001) overlay_key(VK_UP, 0, 1);
        else if (fire & 0x0002) overlay_key(VK_DOWN, 0, 1);
        else if (fire & 0x0004) overlay_key(VK_LEFT, 0, 1);
        else if (fire & 0x0008) overlay_key(VK_RIGHT, 0, 1);
    }
    s_pad_prev = b;
}

static void pad_unblock_tick(void)
{
    RedPadState st;
    if (!s_unblock_wait) return;
    if (XInputGetState(0, (XINPUT_STATE *)&st) != 0 ||
        (st.buttons == 0 && st.lx > -16000 && st.lx < 16000 &&
         st.ly > -16000 && st.ly < 16000) ||
        GetTickCount() - s_unblock_t > 2000) {
        redcard_input_block = 0;
        s_unblock_wait = 0;
    }
}

static void pad_open_poll(void)
{
    static WORD prev;
    RedPadState st;
    WORD b;
    if (s_q_active) return;
    if (XInputGetState(0, (XINPUT_STATE *)&st) != 0) { prev = 0; return; }
    b = st.buttons;
    if ((b & 0x0020) && (b & 0x0010) && !(prev & 0x0010)) {
        prev = b;
        set_open();
        return;
    }
    prev = b;
}

static void quit_tick(void)
{
    RECT cr;
    POINT o = { 0, 0 };
    if (!s_q_active || !s_game) return;
    if (IsIconic(s_game)) return;
    if (s_q_hwnd && !IsWindowVisible(s_q_hwnd)) {
        ShowWindow(s_q_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(s_q_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        s_q_cw = 0;
    }
    GetClientRect(s_game, &cr);
    ClientToScreen(s_game, &o);
    if (cr.right != s_q_cw || cr.bottom != s_q_ch || o.x != s_q_ox || o.y != s_q_oy)
        quit_draw();
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static int overlay_key(WPARAM w, LPARAM l, int down)
{
    int repeat = (l & (1L << 30)) != 0;
    if (s_q_active && s_set_active) return set_key(w, l, down);
    if (w == VK_ESCAPE) {
        if (down && !repeat) { if (s_q_active) quit_close(); else quit_open(); }
        return 1;
    }
    if (!s_q_active) return 0;
    if (down && (w == VK_LEFT || w == VK_RIGHT || w == VK_TAB || w == 'A' || w == 'D')) {
        s_q_sel = (w == VK_LEFT || w == 'A') ? 0 : (w == VK_RIGHT || w == 'D') ? 1 : !s_q_sel;
        quit_draw();
    } else if (down && !repeat && (w == VK_RETURN || w == VK_SPACE)) {
        if (s_q_sel) TerminateProcess(GetCurrentProcess(), 0);
        quit_close();
    } else if (down && !repeat && w == 'Y') {
        TerminateProcess(GetCurrentProcess(), 0);
    } else if (down && !repeat && w == 'N') {
        quit_close();
    }
    return 1;
}

int redcard_overlay_active(void)
{
    return s_q_active;
}

int redcard_overlay_message(HWND game, UINT m, WPARAM w, LPARAM l)
{
    int down;
    if (game) s_game = game;
    if (m != WM_KEYDOWN && m != WM_SYSKEYDOWN && m != WM_KEYUP && m != WM_SYSKEYUP)
        return 0;
    down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);

    if (w == VK_F1) {
        if (down && !(l & (1L << 30))) {
            if (!s_q_active) set_open();
            else if (s_set_active) quit_close();
        }
        return 1;
    }

    if (s_q_active)
        return overlay_key(w, l, down);

    if (w == VK_ESCAPE) {
        if (down && !(l & (1L << 30))) quit_open();
        return 1;
    }
    return 0;
}

/* Refresh the pad keyboard table from the OS while the game window is
 * foreground. WM_KEY* alone is not enough: a focus hand-off to an in-process
 * child (debug banner) or a missed message on the FB thread leaves the table
 * empty and the title looks like the keyboard is dead, even though music and
 * the USB pad path are fine. GetAsyncKeyState matches what the player is
 * holding right now; we still refuse to drive the game when another app has
 * focus. */
static void keyboard_sync(HWND game)
{
    const char *v;
    int vk;

    DWORD pid = 0;
    HWND fg;

    if (!game || s_q_active)
        return;
    /* Any window of this process (the game window, its presenter or a
     * banner) counts as "the game has focus". */
    fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId())
        return;
    v = getenv("RECOMP_KEYBOARD");
    if (!v || !*v || *v == '0')
        return;
    for (vk = 1; vk < 256; vk++)
        xbox_InputKeySet(vk, (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0);
}

void redcard_overlay_tick(HWND game)
{
    if (game) s_game = game;
    pad_unblock_tick();
    pad_open_poll();
    quit_tick();
    while (s_q_active) {
        Sleep(8);
        pump_messages();
        quit_tick();
        pad_overlay_poll();
    }
    keyboard_sync(s_game ? s_game : game);
}
