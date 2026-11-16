#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include "xrdbg.h"
#include "xbox_memory_layout.h"
#include "recomp_exit_hook.h"
#include "redcard_overlay.h"
#include "redcard_videoclip.h"

#ifndef REDCARD_VERSION
#define REDCARD_VERSION "1.0.0"
#endif

static volatile LONG64 frames;
extern ptrdiff_t g_xbox_mem_offset;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp, g_ebp;
static uint64_t frame_count(void) { return (uint64_t)InterlockedCompareExchange64(&frames,0,0); }
void redcard_present_tick(void) { InterlockedIncrement64(&frames); }
static void crash_context(PEXCEPTION_POINTERS ep)
{
    (void)ep;
    fprintf(stderr,"[RC CPU] eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X ebp=%08X\n",
            g_eax,g_ecx,g_edx,g_ebx,g_esi,g_edi,g_esp,g_ebp);
}
#ifndef XRDBG
/* Release builds (no debug kit): everything the game prints goes to
 * %LOCALAPPDATA%\RedCard 2003\logs\redcard_<date>_<time>.log, or logs\ next to the exe
 * when portable.txt is there. The newest 10 logs are kept. RecompLauncher attaches the
 * newest .log in its "logs" folder to a player's report. RECOMP_LOG=0 turns it off; a
 * stderr already redirected to a file (scripts, tests) is left alone. */
static char s_log_path[MAX_PATH];

static void log_prune(const char *dir)
{
    char pattern[MAX_PATH], names[64][64], path[MAX_PATH], tmp[64];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i, j;
    snprintf(pattern, sizeof pattern, "%s\\redcard_*.log", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64 && strlen(fd.cFileName) < sizeof names[0]) strcpy(names[n++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (i = 1; i < n; i++)                      /* names sort by their date and time */
        for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            strcpy(tmp, names[j]); strcpy(names[j], names[j - 1]); strcpy(names[j - 1], tmp);
        }
    for (i = 0; i + 9 < n; i++) {                /* this run's log is created next: keep 9 */
        snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        DeleteFileA(path);
    }
}

static LONG WINAPI log_unhandled(PEXCEPTION_POINTERS ep)
{
    fprintf(stderr, "[CRASH] unhandled exception %08lX at %p; the game is closing\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    crash_context(ep);
    fflush(stdout); fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void release_log_init(void)
{
    char dir[MAX_PATH], exe[MAX_PATH], base[MAX_PATH], *slash;
    const char *off = getenv("RECOMP_LOG");
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    SYSTEMTIME t;
    FILE *f;
    if (off && off[0] == '0') return;
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) == FILE_TYPE_DISK) return;
    exe[0] = 0;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    strcpy(dir, exe);
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    strcpy(base, dir);
    strncat(base, "\\portable.txt", sizeof base - strlen(base) - 1);
    if (slash && GetFileAttributesA(base) != INVALID_FILE_ATTRIBUTES)
        strncat(dir, "\\logs", sizeof dir - strlen(dir) - 1);
    else {
        if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40) return;
        snprintf(dir, sizeof dir, "%s\\RedCard 2003", base);
        CreateDirectoryA(dir, NULL);
        strncat(dir, "\\logs", sizeof dir - strlen(dir) - 1);
    }
    CreateDirectoryA(dir, NULL);
    log_prune(dir);
    GetLocalTime(&t);
    snprintf(s_log_path, sizeof s_log_path, "%s\\redcard_%04u%02u%02u_%02u%02u%02u.log", dir,
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (!freopen(s_log_path, "a", stderr)) { s_log_path[0] = 0; return; }
    f = freopen(s_log_path, "a", stdout);
    (void)f;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetUnhandledExceptionFilter(log_unhandled);
    {
        OSVERSIONINFOW v = { sizeof v };
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn rgv = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        const char *rl = getenv("RECOMPLAUNCHER_VERSION"), *mode = getenv("RECOMPLAUNCHER_MODE");
        if (rgv) rgv(&v);
        fprintf(stderr, "[LOG] RedCard 2003 %s, %04u-%02u-%02u %02u:%02u:%02u\n", REDCARD_VERSION,
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fprintf(stderr, "[LOG] exe %s\n[LOG] Windows %lu.%lu build %lu\n", exe,
                (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber);
        fprintf(stderr, "[LOG] command line: %s\n", GetCommandLineA());
        if (rl) fprintf(stderr, "[LOG] started by RecompLauncher %s (%s)\n", rl, mode ? mode : "desktop");
    }
}
#endif

void redcard_debug_init(void)
{
#ifndef XRDBG
    release_log_init();
#endif
#ifdef XRDBG
    XrDbgConfig cfg = {0};
    cfg.game_name="RedCard 20-03"; cfg.data_folder="RedCardRecomp";
    cfg.build_tag="bringup-2026-11-07"; cfg.window_class="XboxRecompFramebuffer";
    cfg.banner_in_game=1; cfg.frame_count=frame_count;
    cfg.print_crash_context=crash_context; cfg.guest_offset=&g_xbox_mem_offset;
    cfg.n_dump_ranges=2;
    cfg.dump_ranges[0].guest_addr=0x10000; cfg.dump_ranges[0].size=(64u<<20)-0x10000;
    cfg.dump_ranges[1].guest_addr=0x80000000u; cfg.dump_ranges[1].size=64u<<20;
    xrdbg_init(&cfg);
    recomp_exit_hook_set_observer(xrdbg_note_exit);
#endif
}
extern void xbox_InputKeysClear(void);

int redcard_debug_message(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* fb_present clears the key table on every WM_KILLFOCUS. That also fires
     * when focus moves to an in-process child (xrdbg banner), which emptied
     * the pad keyboard while the game window still looked active. Only clear
     * when focus leaves this process; return 1 so fb_present skips its clear. */
    if (m == WM_KILLFOCUS) {
        HWND neo = (HWND)w;
        DWORD pid = 0;
        (void)h;
        if (neo)
            GetWindowThreadProcessId(neo, &pid);
        if (!neo || pid != GetCurrentProcessId())
            xbox_InputKeysClear();
        return 1;
    }
    if (redcard_overlay_message(h, m, w, l)) return 1;
#ifdef XRDBG
    if (m==WM_KEYDOWN && w==VK_DELETE) { XRDBG_MARK(); return 1; }
    if (m==WM_KEYDOWN && w==VK_INSERT) {
        redcard_videoclip_on_key(h, w, (l & (1L << 30)) != 0);
        return 1;
    }
    return XRDBG_WINDOW_MESSAGE(h,m,w,l);
#else
    (void)h;(void)m;(void)w;(void)l;
    return 0;
#endif
}

/* FPS/CPU/GPU overlay and title-bar stats are debug-kit only. Ship builds
 * configure -DREDCARD_DEBUGKIT=OFF so XRDBG is undefined and HUD is skipped.
 * ESC/F1 overlays still run every frame via redcard_overlay_tick. */
void redcard_window_frame(HWND h,HDC dc,void *pixels,int width,int height,int pitch)
{
    /* Blocks while ESC/F1 overlay is open (pumps messages, holds present). */
    redcard_overlay_tick(h);
#ifndef XRDBG
    (void)dc;(void)pixels;(void)width;(void)height;(void)pitch;
#else
    static HWND attached;
    static ULONGLONG last, previous_cpu;
    static uint64_t previous_frames;
    static DWORD processors;
    static PDH_HQUERY query;
    static PDH_HCOUNTER counter;
    static char label[128]="FPS: -- | CPU: -- | GPU: N/A";
    if (attached!=h) {
        SYSTEM_INFO info; GetSystemInfo(&info); processors=info.dwNumberOfProcessors;
        XRDBG_ATTACH_WINDOW(h); attached=h;
        if (PdhOpenQueryA(NULL,0,&query)==ERROR_SUCCESS) {
            if (PdhAddEnglishCounterA(query,"\\GPU Engine(*)\\Utilization Percentage",0,&counter)!=ERROR_SUCCESS)
                counter=NULL;
            PdhCollectQueryData(query);
        }
    }
    char path[MAX_PATH];
    while (xrdbg_capture_due(path,sizeof path))
        xrdbg_write_bmp(path,pixels,width,height,pitch);
    /* Insert clip captures the game image before the performance HUD. */
    redcard_videoclip_on_frame(pixels,width,height,pitch);
    ULONGLONG now=GetTickCount64();
    if (now-last>=1000) {
        FILETIME created,exit_time,kernel,user;
        ULARGE_INTEGER kt,ut;
        GetProcessTimes(GetCurrentProcess(),&created,&exit_time,&kernel,&user);
        kt.LowPart=kernel.dwLowDateTime; kt.HighPart=kernel.dwHighDateTime;
        ut.LowPart=user.dwLowDateTime; ut.HighPart=user.dwHighDateTime;
        ULONGLONG cpu=kt.QuadPart+ut.QuadPart;
        uint64_t count=frame_count();
        double fps=last?(count-previous_frames)*1000.0/(now-last):0;
        double percent=last?(cpu-previous_cpu)/((now-last)*100.0*processors):0;
        double gpu=-1;
        if (counter && PdhCollectQueryData(query)==ERROR_SUCCESS) {
            DWORD bytes=0,n=0;
            if (PdhGetFormattedCounterArrayA(counter,PDH_FMT_DOUBLE,&bytes,&n,NULL)==PDH_MORE_DATA) {
                PDH_FMT_COUNTERVALUE_ITEM_A *items=malloc(bytes);
                char pid[32]; snprintf(pid,sizeof pid,"pid_%lu_",GetCurrentProcessId());
                if (items && PdhGetFormattedCounterArrayA(counter,PDH_FMT_DOUBLE,&bytes,&n,items)==ERROR_SUCCESS)
                    for(DWORD i=0;i<n;i++) if(strstr(items[i].szName,pid) &&
                        (items[i].FmtValue.CStatus==PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus==PDH_CSTATUS_NEW_DATA)) {
                        double value=items[i].FmtValue.doubleValue;
                        if(value>gpu) gpu=value;
                    }
                free(items);
            }
        }
        char gpu_label[24]="N/A";
        if(gpu>=0) snprintf(gpu_label,sizeof gpu_label,"%.1f%%",gpu>100?100:gpu);
        snprintf(label,sizeof label,"FPS: %.1f | CPU: %.1f%% | GPU: %s",fps,percent,gpu_label);
        const char *renderer=getenv("RECOMP_D3D11");
        char title[180]; snprintf(title,sizeof title,"RedCard 20-03 | %s | %s",renderer && !strcmp(renderer,"0")?"Software":"DX11",label); SetWindowTextA(h,title);
        previous_cpu=cpu; previous_frames=count; last=now;
    }
    /* Draw onto the host scanout copy, never guest memory. This survives
     * either GDI or D3D presentation and appears in diagnostic frame dumps. */
    static HDC hud;
    static HBITMAP bitmap;
    static void *bits;
    static int hud_width, hud_band;
    const char *clip=redcard_videoclip_status();
    int band=clip?44:28;
    int rows=height<band?height:band;
    if (!hud || hud_width!=width || hud_band!=band) {
        if (hud) DeleteDC(hud);
        if (bitmap) DeleteObject(bitmap);
        BITMAPINFO bi={0}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth=width; bi.bmiHeader.biHeight=-band;
        bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
        hud=CreateCompatibleDC(dc); bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,NULL,0);
        if(hud && bitmap) SelectObject(hud,bitmap);
        hud_width=width; hud_band=band;
    }
    if(hud && bitmap && bits) {
        for(int y=0;y<rows;y++) memcpy((char*)bits+y*width*4,(char*)pixels+y*pitch,width*4);
        SetBkMode(hud,OPAQUE); SetBkColor(hud,RGB(0,0,0)); SetTextColor(hud,RGB(255,255,255));
        TextOutA(hud,8,6,label,(int)strlen(label));
        if (clip) {
            SetTextColor(hud,RGB(255,80,80));
            TextOutA(hud,8,22,clip,(int)strlen(clip));
        }
        GdiFlush();
        for(int y=0;y<rows;y++) memcpy((char*)pixels+y*pitch,(char*)bits+y*width*4,width*4);
    }
#endif
}
