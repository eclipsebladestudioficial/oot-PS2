#include "oot_ps2_renderer.h"

#include <stdbool.h>

#include "gfx/gfx_fast3d.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_manager_api.h"
#include "oot_ps2_platform.h"
#include "oot_ps2_crash.h"

extern struct GfxRenderingAPI gfx_ps2_rapi;
extern struct GfxWindowManagerAPI gfx_ps2_wapi;

extern void gfx_ps2_render_home_menu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage,
                                uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue);
extern void gfx_ps2_set_home_menu_background_active(bool active);
extern void gfx_ps2_request_home_menu_background(void);
extern int gfx_ps2_apply_video_mode(void);

void gfx_ps2_render_menu(const char* title, const char* const* lines, int lineCount, int selectedIndex,
                         const char* statusMessage, int firstRow, int totalRows);
void gfx_ps2_request_pause_background(void);
bool gfx_ps2_pause_background_active(void);
void gfx_ps2_set_pause_background_active(bool active);
void gfx_ps2_set_boot_progress(int code, const char* label);
void gfx_ps2_render_boot_progress(int code, const char* label);
void gfx_ps2_set_fps_overlay_enabled(bool enabled);
bool gfx_ps2_get_fps_overlay_enabled(void);
void gfx_ps2_set_prerender_room_state(bool active, u32 roomKey);
void gfx_ps2_depth_query_begin(void);
bool gfx_ps2_depth_read(int32_t x, int32_t y, uint16_t* depth);
void OotPs2Renderer_RenderHomeMenu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage, uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue);


static bool sInitialized;

void OotPs2Renderer_Init(void) {
    if (sInitialized) {
        return;
    }

    OotPs2Crash_SetStage("RENDERER_GFX_INIT");
    gfx_init(&gfx_ps2_wapi, &gfx_ps2_rapi, "oot-ps2", false);
    sInitialized = true;
}

void OotPs2Renderer_RenderDisplayList(Gfx* dl) {
    OotPs2Renderer_Init();
    OotPs2Crash_SetStage("RENDER_START_FRAME");
    gfx_start_frame();
    OotPs2Crash_SetStage("RENDER_DISPLAY_LIST");
    gfx_run(dl);
    OotPs2Crash_SetStage("RENDER_END_FRAME");
    gfx_end_frame();
    OotPs2Crash_SetStage("RENDER_DONE");
}

void OotPs2Renderer_RenderTask(const OSTask* task) {
    if (task == NULL || task->t.data_ptr == NULL) {
        return;
    }

    OotPs2Renderer_RenderDisplayList((Gfx*)task->t.data_ptr);
}

typedef struct OotPs2MenuRenderArgs {
    const char* title;
    const char* const* lines;
    int lineCount;
    int selectedIndex;
    const char* statusMessage;
    int firstRow;
    int totalRows;
} OotPs2MenuRenderArgs;

static void OotPs2Renderer_DrawMenu(void* arg) {
    const OotPs2MenuRenderArgs* menu = (const OotPs2MenuRenderArgs*)arg;
    gfx_ps2_render_menu(menu->title, menu->lines, menu->lineCount, menu->selectedIndex, menu->statusMessage,
                        menu->firstRow, menu->totalRows);
}

void OotPs2Renderer_RenderMenu(const char* title, const char* const* lines, int lineCount, int selectedIndex,
                               const char* statusMessage, int firstRow, int totalRows) {
    OotPs2MenuRenderArgs args = { title, lines, lineCount, selectedIndex, statusMessage, firstRow, totalRows };

    OotPs2Renderer_Init();
    gfx_render_callback_frame(OotPs2Renderer_DrawMenu, &args);
}

static void OotPs2Renderer_DrawHomeMenu(void* arg) {
    const OotPs2HomeMenuRenderArgs* menu = (const OotPs2HomeMenuRenderArgs*)arg;

    gfx_ps2_render_home_menu(menu->selectedIndex, menu->screen, menu->submenuSelectedIndex, menu->statusMessage,
                               menu->highlightRed, menu->highlightGreen, menu->highlightBlue);
}

void OotPs2Renderer_SetWidescreen(bool enabled) {
    gfx_set_ps2_widescreen(enabled);
}

bool OotPs2Renderer_IsWidescreen(void) {
    return gfx_get_ps2_widescreen();
}

void OotPs2Renderer_BeginDepthQueries(void) {
    OotPs2Renderer_Init();
    gfx_ps2_depth_query_begin();
}

bool OotPs2Renderer_IsDepthClear(int32_t x, int32_t y) {
    uint16_t depth;

    OotPs2Renderer_Init();
    if (!gfx_ps2_depth_read(x, y, &depth)) {
        return true;
    }
    return depth == 0;
}

bool OotPs2Renderer_DepthTest(int32_t x, int32_t y, float projectedZ) {
    uint16_t depth;
    uint32_t projectedDepth;

    OotPs2Renderer_Init();
    if (!gfx_ps2_depth_read(x, y, &depth)) {
      
        return true;
    }
    if (projectedZ < 0.0f) projectedZ = 0.0f;
    else if (projectedZ > 1.0f) projectedZ = 1.0f;

    projectedDepth = (uint32_t)((1.0f - projectedZ) * 65535.0f);
    return projectedDepth >= depth;
}

void OotPs2Renderer_RequestPauseBackground(void) {
    OotPs2Renderer_Init();
    gfx_ps2_request_pause_background();
}

bool OotPs2Renderer_IsPauseBackgroundReady(void) {
    return gfx_ps2_pause_background_active();
}

void OotPs2Renderer_SetPauseBackgroundActive(bool active) {
    gfx_ps2_set_pause_background_active(active);
}

void OotPs2Renderer_SetFpsVisible(bool enabled) {
    gfx_ps2_set_fps_overlay_enabled(enabled);
}

bool OotPs2Renderer_IsFpsVisible(void) {
    return gfx_ps2_get_fps_overlay_enabled();
}

void OotPs2Renderer_SetPrerenderRoomState(bool active, u32 roomKey) {
    gfx_ps2_set_prerender_room_state(active, roomKey);
}

typedef struct OotPs2BootProgressArgs {
    int code;
    const char* label;
} OotPs2BootProgressArgs;

static void OotPs2Renderer_DrawBootProgress(void* arg) {
    const OotPs2BootProgressArgs* progress = (const OotPs2BootProgressArgs*)arg;
    gfx_ps2_render_boot_progress(progress->code, progress->label);
}

void OotPs2Renderer_SetBootProgress(int code, const char* label) {
    gfx_ps2_set_boot_progress(code, label);
}

void OotPs2Renderer_ShowBootProgress(int code, const char* label) {
    OotPs2BootProgressArgs args = { code, label };
    OotPs2Renderer_Init();

    gfx_render_callback_frame(OotPs2Renderer_DrawBootProgress, &args);
}

void OotPs2Renderer_HideBootProgress(void) {
    gfx_ps2_set_boot_progress(-1, NULL);
}

void OotPs2Renderer_SetHomeMenuBackgroundActive(bool active) {
    gfx_ps2_set_home_menu_background_active(active);
}

void OotPs2Renderer_RequestHomeMenuBackground() {
    gfx_ps2_request_home_menu_background();
}

void OotPs2Renderer_RenderHomeMenu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage, uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue) {
    OotPs2HomeMenuRenderArgs args;

    OotPs2Renderer_Init();
    args.selectedIndex = selectedIndex;
    args.screen = screen;
    args.submenuSelectedIndex = submenuSelectedIndex;
    args.statusMessage = statusMessage;
    args.highlightRed = highlightRed;
    args.highlightGreen = highlightGreen;
    args.highlightBlue = highlightBlue;

    gfx_render_callback_frame(OotPs2Renderer_DrawHomeMenu, &args);
}

int OotPs2Renderer_ApplyVideoSettings(void) {
    int result;

    OotPs2Renderer_Init();
    result = gfx_ps2_apply_video_mode();
    
    //gfx_invalidate_texture_cache(); gs texflush
    //gfx_invalidate_render_state();
    return result;
}
