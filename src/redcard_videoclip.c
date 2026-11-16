/* Record ~3 seconds of the presented game image to an AVI under the xrdbg
 * folder when Insert is pressed. Debug-kit builds only (compiled with XRDBG).
 *
 * Capture prefers the host scanout BGRA buffer already handed to
 * redcard_window_frame (same path as kit screenshots). Frames are throttled
 * to ~30 fps so high menu present rates do not balloon memory. Output is an
 * uncompressed BI_RGB24 AVI — no extra deps beyond Win32. */
#include "redcard_videoclip.h"

#ifdef XRDBG

#include "xrdbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

enum {
    CLIP_MS = 3000,
    CLIP_FPS = 30,
    CLIP_MAX_FRAMES = CLIP_FPS * (CLIP_MS / 1000) + CLIP_FPS, /* ~4s headroom */
    CLIP_MIN_FRAME_MS = 1000 / CLIP_FPS
};

typedef struct {
    unsigned char *bgr; /* tightly packed BGR24, width*height*3 */
} ClipFrame;

static volatile LONG s_busy;          /* 1 while recording or writing */
static ULONGLONG s_start_ms;
static ULONGLONG s_last_frame_ms;
static int s_w, s_h;
static int s_count;
static ClipFrame s_frames[CLIP_MAX_FRAMES];
static char s_status[96];
static char s_out_path[MAX_PATH];

const char *redcard_videoclip_status(void)
{
    return s_status[0] ? s_status : NULL;
}

static void clear_frames(void)
{
    int i;
    for (i = 0; i < s_count; i++) {
        free(s_frames[i].bgr);
        s_frames[i].bgr = NULL;
    }
    s_count = 0;
    s_w = s_h = 0;
}

static void set_status(const char *msg)
{
    if (!msg || !msg[0]) { s_status[0] = 0; return; }
    snprintf(s_status, sizeof s_status, "%s", msg);
}

static int write_u16(FILE *f, uint16_t v)
{
    return fwrite(&v, 1, 2, f) == 2;
}
static int write_u32(FILE *f, uint32_t v)
{
    return fwrite(&v, 1, 4, f) == 4;
}

/* Minimal RIFF AVI with one uncompressed 24bpp video stream + old-style index. */
static int write_avi(const char *path, int w, int h, int fps, const ClipFrame *frames, int n)
{
    FILE *f;
    uint32_t i;
    uint32_t row_bytes = (uint32_t)((w * 3 + 3) & ~3); /* DWORD-aligned BGR rows */
    uint32_t frame_bytes = row_bytes * (uint32_t)h;
    uint32_t frames_payload = n * (8u + frame_bytes); /* each: '00db'+size+data */
    uint32_t idx1_bytes = n * 16u;
    uint32_t avih_size = 56;
    uint32_t strl_list;
    uint32_t strh_size = 56;
    uint32_t strf_size = 40;
    long riff_size_pos;
    unsigned char *rowpad = NULL;
    int ok = 0;

    if (n <= 0 || w <= 0 || h <= 0 || fps <= 0) return 0;

    /* LIST size fields include list-type fourcc + payload.
     * movi LIST size = 4('movi') + frames_payload.
     * hdrl LIST size = 4('hdrl') + avih + strl LIST. */
    strl_list = 4 + (8 + strh_size) + (8 + strf_size);

    f = fopen(path, "wb");
    if (!f) return 0;

    rowpad = (unsigned char *)calloc(1, row_bytes);
    if (!rowpad) goto done;

    /* RIFF header */
    if (fwrite("RIFF", 1, 4, f) != 4) goto done;
    riff_size_pos = ftell(f);
    if (!write_u32(f, 0)) goto done; /* patch later */
    if (fwrite("AVI ", 1, 4, f) != 4) goto done;

    /* LIST hdrl */
    if (fwrite("LIST", 1, 4, f) != 4) goto done;
    if (!write_u32(f, 4 + (8 + avih_size) + (8 + 4 + strl_list))) goto done;
    if (fwrite("hdrl", 1, 4, f) != 4) goto done;

    /* avih */
    if (fwrite("avih", 1, 4, f) != 4) goto done;
    if (!write_u32(f, avih_size)) goto done;
    if (!write_u32(f, 1000000u / (uint32_t)fps)) goto done; /* us/frame */
    if (!write_u32(f, frame_bytes * (uint32_t)fps)) goto done; /* max bytes/sec */
    if (!write_u32(f, 0)) goto done; /* padding */
    if (!write_u32(f, 0x10)) goto done; /* AVIF_HASINDEX */
    if (!write_u32(f, (uint32_t)n)) goto done; /* total frames */
    if (!write_u32(f, 0)) goto done; /* initial frames */
    if (!write_u32(f, 1)) goto done; /* streams */
    if (!write_u32(f, frame_bytes)) goto done; /* suggested buffer */
    if (!write_u32(f, (uint32_t)w)) goto done;
    if (!write_u32(f, (uint32_t)h)) goto done;
    if (!write_u32(f, 0) || !write_u32(f, 0) || !write_u32(f, 0) || !write_u32(f, 0)) goto done;

    /* LIST strl */
    if (fwrite("LIST", 1, 4, f) != 4) goto done;
    if (!write_u32(f, 4 + (8 + strh_size) + (8 + strf_size))) goto done;
    if (fwrite("strl", 1, 4, f) != 4) goto done;

    /* strh */
    if (fwrite("strh", 1, 4, f) != 4) goto done;
    if (!write_u32(f, strh_size)) goto done;
    if (fwrite("vids", 1, 4, f) != 4) goto done;
    if (fwrite("DIB ", 1, 4, f) != 4) goto done; /* handler */
    if (!write_u32(f, 0)) goto done; /* flags */
    if (!write_u16(f, 0) || !write_u16(f, 0)) goto done; /* priority/language */
    if (!write_u32(f, 0)) goto done; /* initial frames */
    if (!write_u32(f, 1)) goto done; /* scale */
    if (!write_u32(f, (uint32_t)fps)) goto done; /* rate */
    if (!write_u32(f, 0)) goto done; /* start */
    if (!write_u32(f, (uint32_t)n)) goto done; /* length */
    if (!write_u32(f, frame_bytes)) goto done; /* suggested buffer */
    if (!write_u32(f, 0xFFFFFFFFu)) goto done; /* quality */
    if (!write_u32(f, 0)) goto done; /* sample size */
    if (!write_u16(f, 0) || !write_u16(f, 0) || !write_u16(f, (uint16_t)w) || !write_u16(f, (uint16_t)h))
        goto done; /* rcFrame */

    /* strf = BITMAPINFOHEADER */
    if (fwrite("strf", 1, 4, f) != 4) goto done;
    if (!write_u32(f, strf_size)) goto done;
    if (!write_u32(f, 40)) goto done; /* biSize */
    if (!write_u32(f, (uint32_t)w)) goto done;
    if (!write_u32(f, (uint32_t)h)) goto done; /* positive = bottom-up DIBs */
    if (!write_u16(f, 1)) goto done; /* planes */
    if (!write_u16(f, 24)) goto done; /* bit count */
    if (!write_u32(f, 0)) goto done; /* BI_RGB */
    if (!write_u32(f, frame_bytes)) goto done;
    if (!write_u32(f, 0) || !write_u32(f, 0) || !write_u32(f, 0) || !write_u32(f, 0)) goto done;

    /* LIST movi */
    if (fwrite("LIST", 1, 4, f) != 4) goto done;
    if (!write_u32(f, 4 + frames_payload)) goto done;
    if (fwrite("movi", 1, 4, f) != 4) goto done;

    for (i = 0; i < (uint32_t)n; i++) {
        int y;
        if (fwrite("00db", 1, 4, f) != 4) goto done;
        if (!write_u32(f, frame_bytes)) goto done;
        /* Bottom-up BGR rows (DIB). Source scanout is top-down BGRA. */
        for (y = h - 1; y >= 0; y--) {
            const unsigned char *src = frames[i].bgr + (size_t)y * (size_t)w * 3u;
            memcpy(rowpad, src, (size_t)w * 3u);
            if (fwrite(rowpad, 1, row_bytes, f) != row_bytes) goto done;
        }
    }

    /* idx1 */
    if (fwrite("idx1", 1, 4, f) != 4) goto done;
    if (!write_u32(f, idx1_bytes)) goto done;
    for (i = 0; i < (uint32_t)n; i++) {
        /* Offset from the start of the 'movi' fourcc (first chunk at 4). */
        uint32_t offset = 4u + (uint32_t)(i * (8u + frame_bytes));
        if (fwrite("00db", 1, 4, f) != 4) goto done;
        if (!write_u32(f, 0x10)) goto done; /* AVIIF_KEYFRAME */
        if (!write_u32(f, offset)) goto done;
        if (!write_u32(f, frame_bytes)) goto done;
    }

    {
        long end = ftell(f);
        uint32_t size = (uint32_t)(end - 8);
        if (fseek(f, riff_size_pos, SEEK_SET) != 0) goto done;
        if (!write_u32(f, size)) goto done;
    }
    ok = 1;
done:
    free(rowpad);
    if (f) fclose(f);
    if (!ok && path && path[0]) DeleteFileA(path);
    return ok;
}

static void finish_clip(void)
{
    const char *dir = xrdbg_dir();
    SYSTEMTIME st;
    int n = s_count;
    int w = s_w, h = s_h;
    char path[MAX_PATH];

    set_status("CLIP: saving...");
    fprintf(stderr, "[RC CLIP] saving %d frames (%dx%d) ...\n", n, w, h);

    GetLocalTime(&st);
    if (!dir || !dir[0]) dir = ".";
    snprintf(path, sizeof path, "%s\\clip_%04d%02d%02d_%02d%02d%02d.avi",
             dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    if (n <= 0) {
        fprintf(stderr, "[RC CLIP] no frames captured; nothing written\n");
        set_status("CLIP: empty");
    } else if (!write_avi(path, w, h, CLIP_FPS, s_frames, n)) {
        fprintf(stderr, "[RC CLIP] failed writing %s\n", path);
        set_status("CLIP: write failed");
    } else {
        snprintf(s_out_path, sizeof s_out_path, "%s", path);
        fprintf(stderr, "[RC CLIP] wrote %s (%d frames, ~%d ms @ %d fps)\n",
                path, n, CLIP_MS, CLIP_FPS);
        set_status("CLIP: saved");
    }

    clear_frames();
    InterlockedExchange(&s_busy, 0);
}

void redcard_videoclip_on_key(HWND game, WPARAM vk, int repeat)
{
    (void)game;
    if (vk != VK_INSERT || repeat) return;
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) {
        fprintf(stderr, "[RC CLIP] already recording/saving; Insert ignored\n");
        return;
    }
    clear_frames();
    s_start_ms = GetTickCount64();
    s_last_frame_ms = 0;
    s_out_path[0] = 0;
    set_status("CLIP: recording...");
    fprintf(stderr, "[RC CLIP] recording ~%d ms of present frames (Insert)\n", CLIP_MS);
}

void redcard_videoclip_on_frame(const void *bgra, int width, int height, int pitch)
{
    ULONGLONG now;
    unsigned char *dst;
    int y, x;
    const unsigned char *src_base;

    if (!bgra || width <= 0 || height <= 0 || pitch < width * 4) return;
    if (InterlockedCompareExchange(&s_busy, 1, 1) != 1) return;
    /* Still marked busy after finish_clip clears frames — only capture while status says recording. */
    if (strncmp(s_status, "CLIP: recording", 15) != 0) return;

    now = GetTickCount64();
    if (now - s_start_ms >= CLIP_MS || s_count >= CLIP_MAX_FRAMES) {
        set_status("CLIP: saving...");
        finish_clip();
        return;
    }
    if (s_last_frame_ms && (now - s_last_frame_ms) < CLIP_MIN_FRAME_MS) return;

    if (s_count == 0) {
        s_w = width;
        s_h = height;
    } else if (width != s_w || height != s_h) {
        fprintf(stderr, "[RC CLIP] resolution changed %dx%d -> %dx%d; stopping early\n",
                s_w, s_h, width, height);
        set_status("CLIP: saving...");
        finish_clip();
        return;
    }

    dst = (unsigned char *)malloc((size_t)width * (size_t)height * 3u);
    if (!dst) {
        fprintf(stderr, "[RC CLIP] out of memory at frame %d; stopping\n", s_count);
        set_status("CLIP: saving...");
        finish_clip();
        return;
    }

    src_base = (const unsigned char *)bgra;
    for (y = 0; y < height; y++) {
        const unsigned char *src = src_base + (size_t)y * (size_t)pitch;
        unsigned char *row = dst + (size_t)y * (size_t)width * 3u;
        for (x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 4 + 0]; /* B */
            row[x * 3 + 1] = src[x * 4 + 1]; /* G */
            row[x * 3 + 2] = src[x * 4 + 2]; /* R */
        }
    }

    s_frames[s_count].bgr = dst;
    s_count++;
    s_last_frame_ms = now;
    snprintf(s_status, sizeof s_status, "CLIP: recording %d/%d", s_count, CLIP_MAX_FRAMES);
}

#endif /* XRDBG */
