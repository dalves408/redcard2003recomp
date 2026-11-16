/* Runtime wiring derived from the USA XBE's XDK 4432 device code. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "xbox_memory_layout.h"
#include "recomp_types.h"
#include "keyboard.h"
#include <xinput.h>

extern int usb_gamepad_report(int pad, uint8_t *out, int max);

/* Dual-GPU laptops: ask the NVIDIA Optimus and AMD PowerXpress / Switchable Graphics
 * drivers to run this exe on the dedicated GPU. Both drivers look for these exported
 * names in the .exe itself. A per-app choice in the NVIDIA/AMD control panel or in
 * Windows Settings > Graphics still overrides them, and RECOMP_D3D11_ADAPTER picks
 * an adapter explicitly. */
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* Exercise the actual key table, host merge and Xbox USB packet encoder.
 * No system keyboard events or fabricated host controller are involved. */
int redcard_input_check(void)
{
    unsigned failures = 0, checks = 0;
    xbox_InputInit();
    for (DWORD port = 0; port < 4; ++port) {
        XINPUT_STATE host;
        DWORD status = XInputGetState(port, &host);
        fprintf(stderr, "[INPUT CHECK] physical XInput port %lu: %s\n",
                (unsigned long)port, status == 0 ? "connected" : "not connected");
    }
    for (size_t row = 0; row < xbox_key_map_count; ++row) {
        const xbox_key_row *binding = &xbox_key_map[row];
        for (int alternate = 0; alternate < 2 && binding->vk[alternate]; ++alternate) {
            XBOX_INPUT_STATE keys, merged;
            uint8_t report[20];
            xbox_InputKeysClear();
            xbox_InputKeySet(binding->vk[alternate], 1);
            xbox_InputKeyboardState(&keys);
            DWORD status = xbox_InputGetState(0, &merged);
            int length = usb_gamepad_report(0, report, sizeof report);
            int good = status == 0 && length == 20 && report[1] == 20;
            if (binding->kind == XBOX_KEY_BUTTON) {
                good &= (keys.Gamepad.wButtons & binding->target) != 0;
                good &= (merged.Gamepad.wButtons & binding->target) != 0;
                good &= ((report[2] | report[3] << 8) & binding->target) != 0;
            } else if (binding->kind == XBOX_KEY_ANALOG) {
                good &= keys.Gamepad.bAnalogButtons[binding->target] == 255;
                good &= merged.Gamepad.bAnalogButtons[binding->target] == 255;
                good &= report[4 + binding->target] == 255;
            } else {
                int16_t key_axes[4], merged_axes[4], usb_axes[4];
                memcpy(key_axes, &keys.Gamepad.sThumbLX, sizeof key_axes);
                memcpy(merged_axes, &merged.Gamepad.sThumbLX, sizeof merged_axes);
                memcpy(usb_axes, report + 12, sizeof usb_axes);
                good &= key_axes[binding->target] * binding->sign > 0;
                good &= merged_axes[binding->target] == key_axes[binding->target];
                good &= usb_axes[binding->target] == key_axes[binding->target];
            }
            xbox_InputKeysClear();
            xbox_InputKeyboardState(&keys);
            XBOX_GAMEPAD neutral = {0};
            good &= memcmp(&keys.Gamepad, &neutral, sizeof neutral) == 0;
            fprintf(stderr, "[INPUT CHECK] %s -> %s: %s\n", binding->keys,
                    binding->guest, good ? "PASS" : "FAIL");
            checks++;
            if (!good) failures++;
        }
    }
    fprintf(stderr, "[INPUT CHECK] %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

extern RECOMP_TLS uint32_t g_esp;
extern RECOMP_TLS uint32_t g_eax;
extern RECOMP_TLS uint32_t g_fs_base;

typedef struct RedcardFiber {
    uint32_t guest_address;
    void *host_fiber;
    struct RedcardFiber *next;
} RedcardFiber;

static RedcardFiber *redcard_fibers;
static void *redcard_current_host_fiber;

extern void redcard_original_CreateFiber(void);
extern void redcard_original_DeleteFiber(void);
extern void redcard_original_SwitchToFiber(void);
extern void redcard_original_ConvertThreadToFiber(void);
extern void sub_001D0055(void);

static uint32_t guest_word(uint32_t va)
{
    return *(uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

/* RedCard's D3D BlockOnTime queues a software-method interrupt and waits
 * at 001F43E9. As in the Hitz port, wait on the actual GPU semaphore instead
 * of the absent interrupt. The executor must retire releases, not mirror PUT. */
void redcard_gpu_wait(void)
{
    uint32_t caller = guest_word(g_esp);
    if (caller == 0x001F43EBu) {
        uint32_t device = guest_word(0x00201480u);
        uint32_t word = guest_word(device + 0x34u);
        uint32_t target = guest_word(g_ebx);
        if (word < 0x80000000u || word > 0x83FFFFFCu || (word & 3u)) abort();
        volatile LONG *completed = (volatile LONG *)((uintptr_t)xbox_GetMemoryOffset() + word);
        ULONGLONG began = GetTickCount64();
        while ((int32_t)((uint32_t)InterlockedCompareExchange(completed, 0, 0) - target) < 0) {
            /* 20s: F1 scale/MSAA reset drains D3D11 on the PB thread; on slow
             * quad-core Intel PCs a 10s budget aborted mid-cycle. */
            if (GetTickCount64() - began > 20000) {
                fprintf(stderr, "[RC GPUWAIT] stalled word=%08X completed=%08X target=%08X\n",
                        word, (uint32_t)*completed, target);
                abort();
            }
            Sleep(1);
        }
        g_eax = 0;
        g_esp += 24;
        return;
    }
    recomp_func_t original = recomp_lookup_kernel(guest_word(0x003F78A4u));
    if (!original) abort();
    original();
}

uint32_t redcard_wait_thunk(void) { return guest_word(0x003F78A4u); }

extern void redcard_original_input_state(void);
void sub_0022A1DB(void)
{
    uint32_t output = guest_word(g_esp + 8);
    redcard_original_input_state();
    if (getenv("REDCARD_TRACE_BOOT")) {
        static DWORD last;
        static uint32_t previous;
        uint32_t buttons = guest_word(output + 4) & 0xFFFFu;
        DWORD now = GetTickCount();
        if (buttons != previous || now - last > 2000) {
            fprintf(stderr, "[RC PAD] status=%08X packet=%u buttons=%04X A=%u\n",
                    g_eax, guest_word(output), buttons,
                    *(uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + output + 6));
            last = now;
            previous = buttons;
        }
    }
}

/* Optional development fast boot: omit the two noninteractive publisher
 * movies. Leave the title's menu/loading/gameplay routines untouched. */
int redcard_skip_logo(void)
{
    if (!getenv("REDCARD_SKIP_LOGOS")) return 0;
    uint32_t movie = guest_word(g_esp + 4);
    if (movie != guest_word(0x2B3F90) && movie != guest_word(0x2B4098)) return 0;
    fprintf(stderr, "[RC] skipping publisher logo %u (development option)\n", movie);
    g_esp += 4;
    return 1;
}

static RedcardFiber *redcard_find_fiber(uint32_t guest_address)
{
    for (RedcardFiber *fiber = redcard_fibers; fiber; fiber = fiber->next)
        if (fiber->guest_address == guest_address)
            return fiber;
    return NULL;
}

static void redcard_register_fiber(uint32_t guest_address, void *host_fiber)
{
    RedcardFiber *fiber = redcard_find_fiber(guest_address);
    if (!fiber) {
        fiber = (RedcardFiber *)malloc(sizeof(*fiber));
        if (!fiber) {
            fprintf(stderr, "[FIBER] Out of memory registering guest fiber %08X\n",
                    guest_address);
            abort();
        }
        fiber->guest_address = guest_address;
        fiber->next = redcard_fibers;
        redcard_fibers = fiber;
    }
    fiber->host_fiber = host_fiber;
}

static uint32_t redcard_current_guest_fiber(void)
{
    uint32_t tls_index = guest_word(0x003BDCF0u);
    uint32_t tls_array = guest_word(g_fs_base + 4u);
    uint32_t thread_data = guest_word(tls_array + tls_index * 4u);
    return guest_word(thread_data + 8u);
}

static VOID WINAPI redcard_fiber_entry(void *parameter)
{
    redcard_current_host_fiber = GetCurrentFiber();
    if (getenv("REDCARD_TRACE_BOOT"))
        fprintf(stderr, "[FIBER] Starting guest fiber %08X at ESP=%08X\n",
                (uint32_t)(uintptr_t)parameter, g_esp);
    sub_001D0055();
    fprintf(stderr, "[FIBER] Guest fiber %08X returned from its startup routine\n",
            (uint32_t)(uintptr_t)parameter);
    abort();
}

void sub_001D009A(void)
{
    uint32_t stack_size = guest_word(g_esp + 4u);
    redcard_original_CreateFiber();

    uint32_t guest_fiber = g_eax;
    if (!guest_fiber)
        return;

    void *host_fiber = CreateFiber(stack_size, redcard_fiber_entry,
                                   (void *)(uintptr_t)guest_fiber);
    if (!host_fiber) {
        fprintf(stderr, "[FIBER] CreateFiber failed for guest fiber %08X: %lu\n",
                guest_fiber, (unsigned long)GetLastError());
        g_eax = 0;
        return;
    }
    redcard_register_fiber(guest_fiber, host_fiber);
    if (getenv("REDCARD_TRACE_BOOT"))
        fprintf(stderr, "[FIBER] Created guest fiber %08X (stack=%u)\n",
                guest_fiber, stack_size);
}

void sub_001D0126(void)
{
    uint32_t guest_fiber = guest_word(g_esp + 4u);
    RedcardFiber *fiber = redcard_find_fiber(guest_fiber);
    redcard_original_DeleteFiber();
    if (fiber) {
        if (fiber->host_fiber == redcard_current_host_fiber) {
            fprintf(stderr, "[FIBER] Refusing to delete the currently running fiber %08X\n",
                    guest_fiber);
            abort();
        }
        DeleteFiber(fiber->host_fiber);
        fiber->host_fiber = NULL;
    }
}

void sub_001D0139(void)
{
    uint32_t target = guest_word(g_esp + 4u);
    uint32_t current = redcard_current_guest_fiber();
    RedcardFiber *fiber = redcard_find_fiber(target);
    if (!fiber || !fiber->host_fiber) {
        fprintf(stderr, "[FIBER] No host fiber registered for guest target %08X\n",
                target);
        abort();
    }

    redcard_original_SwitchToFiber();
    if (target != current) {
        SwitchToFiber(fiber->host_fiber);
        redcard_current_host_fiber = GetCurrentFiber();
    }
}

void sub_001D017F(void)
{
    redcard_original_ConvertThreadToFiber();
    uint32_t guest_fiber = redcard_current_guest_fiber();
    void *host_fiber = IsThreadAFiber() ? GetCurrentFiber()
                                        : ConvertThreadToFiber(NULL);
    if (!host_fiber) {
        fprintf(stderr, "[FIBER] ConvertThreadToFiber failed: %lu\n",
                (unsigned long)GetLastError());
        abort();
    }
    redcard_current_host_fiber = host_fiber;
    redcard_register_fiber(guest_fiber, host_fiber);
    if (getenv("REDCARD_TRACE_BOOT"))
        fprintf(stderr, "[FIBER] Registered main guest fiber %08X\n", guest_fiber);
}

void redcard_trace_event(void)
{
    static unsigned count;
    if (!getenv("REDCARD_TRACE_BOOT")) return;
    uint32_t event = guest_word(g_esp + 4);
    if (event == 0x12 || count++ >= 100) return;
    fprintf(stderr, "[RC EVENT] event=%u arg=%08X caller=%08X\n",
            event, guest_word(g_esp + 8), guest_word(g_esp));
}

void redcard_trace_frame(void)
{
    static DWORD last;
    if (!getenv("REDCARD_TRACE_BOOT")) return;
    DWORD now = GetTickCount();
    if (now - last < 2000) return;
    last = now;
    fprintf(stderr, "[RC FRAME] state=%u camera=%08X quit=%u render_skip=%u\n",
            guest_word(0x294268), guest_word(0x33F860),
            guest_word(0x3B3F74), guest_word(0x36956C));
}

void redcard_runtime_init(void)
{
    extern void redcard_d3d11_register(void);
    extern void redcard_settings_apply_env(void);
    /* INI first so later defaults only fill gaps the player did not set. */
    redcard_settings_apply_env();
    redcard_d3d11_register();
    /* This title submits NV2A pushbuffers through its statically linked D3D. */
    if (!getenv("RECOMP_PB_EXEC")) _putenv_s("RECOMP_PB_EXEC", "1");
    if (!getenv("RECOMP_PB_WALK")) _putenv_s("RECOMP_PB_WALK", "dma");
    if (!getenv("RECOMP_FB_WINDOW")) _putenv_s("RECOMP_FB_WINDOW", "1");
    if (!getenv("RECOMP_KEYBOARD")) _putenv_s("RECOMP_KEYBOARD", "1");
    /* XDK 4432 polls the emulated USB controller, rather than the host HLE
     * XInput entry points. Without OHCI the host pad state never reaches it. */
    if (!getenv("RECOMP_USB")) _putenv_s("RECOMP_USB", "1");
    if (!getenv("RECOMP_FENCE_EXEC")) _putenv_s("RECOMP_FENCE_EXEC", "1");
    if (!getenv("RECOMP_TITLE_KEVENTS")) _putenv_s("RECOMP_TITLE_KEVENTS", "1");
    if (!getenv("RECOMP_VBLANK")) _putenv_s("RECOMP_VBLANK", "paced");
    /* One-CPU dispatch gate: DPCs and device interrupts run where the guest
     * allows them, as on the single-CPU Xbox. Without it a timer DPC in the
     * sound code (sub_0022FDCD) raced the guest thread and crashed a match
     * about four minutes in; NHL Hitz 2003 makes the same gate its default. */
    if (!getenv("RECOMP_DISPATCH_GATE")) _putenv_s("RECOMP_DISPATCH_GATE", "1");
    /* Hold flips to the 60 Hz guest vblank with D3D's rule (a late frame waits
     * for the next vblank). Unpaced, menus presented at up to 268 FPS and the
     * frame-driven match clock ran fast. */
    if (!getenv("RECOMP_FLIP_PACING")) _putenv_s("RECOMP_FLIP_PACING", "edge");
    /* Without reclaim, MmFreeContiguousMemory is a no-op and the 64 MB contig
     * arena fills through logos/menus (frees stay 0). Match kickoff then fails
     * a 2400 KB framebuffer alloc; the title keeps a surface at 0x80000002
     * (page 0 + 2). DX11 soft-fails/skips real draws, presents that VA, and
     * samples it as a linear texture — dark translucent checkerboard,
     * magenta noise, and ghost players with UI still on top (USA vs France
     * kickoff). Presence of RECOMP_HEAP_RECLAIM enables reclaim; value is
     * ignored by the runtime. */
    if (!getenv("RECOMP_HEAP_RECLAIM")) {
        _putenv_s("RECOMP_HEAP_RECLAIM", "1");
        fprintf(stderr, "[RC] RECOMP_HEAP_RECLAIM default on "
                "(contig + heap free reclaim; set before launch to override)\n");
    }
    /* Sound. The emulated APU (and the AC'97 codec-ready answer it needs) only
     * starts when RECOMP_AC97_READY is present, and nothing set it: every
     * build ran silent. On by default; RECOMP_AC97_READY=0 turns it off
     * (the runtime tests presence, so 0 removes the variable). Must run
     * before xbox_MemoryLayoutInit, which unmaps the APU registers. */
    {
        const char *ac97 = getenv("RECOMP_AC97_READY");
        if (!ac97)
            _putenv_s("RECOMP_AC97_READY", "1");
        else if (!strcmp(ac97, "0"))
            _putenv_s("RECOMP_AC97_READY", "");
    }

    /* D3D::BlockOnTime at 0x001F4310 loads the device from 0x00201480,
     * the submitted fence at +0x30 and the GPU semaphore pointer at +0x34.
     * Register the chain before the device exists; runtime polls are bounded.
     * This also tells the pushbuffer executor where semaphore releases land. */
    xbox_Nv2aMirrorFence(0x00201480u, 0x30u, 0x34u);
}
