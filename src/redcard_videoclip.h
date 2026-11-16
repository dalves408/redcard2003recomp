/* Insert-key ~3s present-framebuffer video clip (debug-kit builds only). */
#ifndef REDCARD_VIDEOCLIP_H
#define REDCARD_VIDEOCLIP_H

#include <windows.h>

#ifdef XRDBG
void redcard_videoclip_on_key(HWND game, WPARAM vk, int repeat);
/* Copy one present frame while a clip is armed. Safe no-op otherwise. */
void redcard_videoclip_on_frame(const void *bgra, int width, int height, int pitch);
/* Non-NULL while recording / finishing; drawn onto the scanout HUD. */
const char *redcard_videoclip_status(void);
#else
static inline void redcard_videoclip_on_key(HWND game, WPARAM vk, int repeat)
{ (void)game; (void)vk; (void)repeat; }
static inline void redcard_videoclip_on_frame(const void *bgra, int width, int height, int pitch)
{ (void)bgra; (void)width; (void)height; (void)pitch; }
static inline const char *redcard_videoclip_status(void) { return NULL; }
#endif

#endif
