#ifndef GFX_WINDOW_PS2_H
#define GFX_WINDOW_PS2_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx_window_manager_api.h"

struct gsGlobal;

extern struct gsGlobal* gs_global;
extern struct GfxWindowManagerAPI gfx_ps2_wapi;

void gfx_ps2_prepare_crash_screen(void);
int gfx_ps2_apply_display_mode(int mode, int interlace, int field);

void gfx_ps2_depth_query_begin(void);
bool gfx_ps2_depth_read(int32_t x, int32_t y, uint16_t* depth);

void gfx_ps2_apply_vismono(uint8_t primR, uint8_t primG, uint8_t primB, uint8_t alpha, uint8_t envR, uint8_t envG,
                           uint8_t envB);

void gfx_ps2_capture_pause_background_current(void);
void gfx_ps2_request_pause_background(void);
void gfx_ps2_set_pause_background_active(bool active);
bool gfx_ps2_pause_background_active(void);
void gfx_ps2_restore_pause_background(void);

#endif
