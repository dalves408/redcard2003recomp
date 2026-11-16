#pragma once
#include <windows.h>
int redcard_overlay_message(HWND game, UINT m, WPARAM w, LPARAM l); /* 1 = consumed */
void redcard_overlay_tick(HWND game); /* call each present; blocks while overlay open */
void redcard_settings_apply_env(void); /* load INI → env before video/kernel init */
int redcard_overlay_active(void);
