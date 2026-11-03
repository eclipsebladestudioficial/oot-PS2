#include "renderer.h"

#include <stdbool.h>

#include "gfx/gfx_fast3d.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_ps2.h"
#include "gfx/gfx_window_manager_api.h"
#include "platform.h"
#include "renderer/video.h"

extern struct GfxRenderingAPI gfx_ps2_rapi;

extern void gfx_ps2_render_home_menu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage,
                                uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue);
extern void gfx_ps2_set_home_menu_background_active(bool active);
extern void gfx_ps2_request_home_menu_background(void);
extern int gfx_ps2_apply_video_mode(void);

void gfx_ps2_render_menu(const char* title, const char* const* lines, int lineCount, int selectedIndex,
                         const char* statusMessage, int firstRow, int totalRows);
void gfx_ps2_set_boot_progress(int code, const char* label);
void gfx_ps2_render_boot_progress(int code, const char* label);
void gfx_ps2_set_fps_overlay_enabled(bool enabled);
bool gfx_ps2_get_fps_overlay_enabled(void);
void gfx_ps2_set_prerender_room_state(bool active, u32 roomKey);
void Renderer_RenderHomeMenu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage, uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue);


static bool sInitialized;

void Renderer_Init(void) {
    if (sInitialized) {
        return;
    }

    gfx_init(&gfx_ps2_wapi, &gfx_ps2_rapi, "oot-ps2", false);
    sInitialized = true;
}

void Renderer_RenderDisplayList(Gfx* dl) {
    Renderer_Init();
    gfx_start_frame();
    gfx_run(dl);
    gfx_end_frame();
}

void Renderer_RenderTask(const OSTask* task) {
    if (task == NULL || task->t.data_ptr == NULL) {
        return;
    }

    Renderer_RenderDisplayList((Gfx*)task->t.data_ptr);
}

typedef struct MenuRenderArgs {
    const char* title;
    const char* const* lines;
    int lineCount;
    int selectedIndex;
    const char* statusMessage;
    int firstRow;
    int totalRows;
} MenuRenderArgs;

static void Renderer_DrawMenu(void* arg) {
    const MenuRenderArgs* menu = (const MenuRenderArgs*)arg;
    gfx_ps2_render_menu(menu->title, menu->lines, menu->lineCount, menu->selectedIndex, menu->statusMessage,
                        menu->firstRow, menu->totalRows);
}

void Renderer_RenderMenu(const char* title, const char* const* lines, int lineCount, int selectedIndex,
                               const char* statusMessage, int firstRow, int totalRows) {
    MenuRenderArgs args = { title, lines, lineCount, selectedIndex, statusMessage, firstRow, totalRows };

    Renderer_Init();
    gfx_render_callback_frame(Renderer_DrawMenu, &args);
}

static void Renderer_DrawHomeMenu(void* arg) {
    const HomeMenuRenderArgs* menu = (const HomeMenuRenderArgs*)arg;

    gfx_ps2_render_home_menu(menu->selectedIndex, menu->screen, menu->submenuSelectedIndex, menu->statusMessage,
                               menu->highlightRed, menu->highlightGreen, menu->highlightBlue);
}

void Renderer_SetWidescreen(bool enabled) {
    gfx_set_ps2_widescreen(enabled);
}

bool Renderer_IsWidescreen(void) {
    return gfx_get_ps2_widescreen();
}

void Renderer_BeginDepthQueries(void) {
    Renderer_Init();
    gfx_ps2_depth_query_begin();
}

bool Renderer_IsDepthClear(int32_t x, int32_t y) {
    uint16_t depth;

    Renderer_Init();
    if (!gfx_ps2_depth_read(x, y, &depth)) {
        return true;
    }
    return depth == 0;
}

bool Renderer_DepthTest(int32_t x, int32_t y, float projectedZ) {
    uint16_t depth;
    uint32_t projectedDepth;

    Renderer_Init();
    if (!gfx_ps2_depth_read(x, y, &depth)) {
      
        return true;
    }
    if (projectedZ < 0.0f) projectedZ = 0.0f;
    else if (projectedZ > 1.0f) projectedZ = 1.0f;

    projectedDepth = (uint32_t)((1.0f - projectedZ) * 65535.0f);
    return projectedDepth >= depth;
}

void Renderer_RequestPauseBackground(void) {
    Renderer_Init();
    gfx_ps2_request_pause_background();
}

bool Renderer_IsPauseBackgroundReady(void) {
    return gfx_ps2_pause_background_active();
}

void Renderer_SetPauseBackgroundActive(bool active) {
    gfx_ps2_set_pause_background_active(active);
}

void Renderer_SetFpsVisible(bool enabled) {
    gfx_ps2_set_fps_overlay_enabled(enabled);
}

bool Renderer_IsFpsVisible(void) {
    return gfx_ps2_get_fps_overlay_enabled();
}

void Renderer_SetPrerenderRoomState(bool active, u32 roomKey) {
    gfx_ps2_set_prerender_room_state(active, roomKey);
}

typedef struct BootProgressArgs {
    int code;
    const char* label;
} BootProgressArgs;

static void Renderer_DrawBootProgress(void* arg) {
    const BootProgressArgs* progress = (const BootProgressArgs*)arg;
    gfx_ps2_render_boot_progress(progress->code, progress->label);
}

void Renderer_SetBootProgress(int code, const char* label) {
    gfx_ps2_set_boot_progress(code, label);
}

void Renderer_ShowBootProgress(int code, const char* label) {
    BootProgressArgs args = { code, label };
    Renderer_Init();

    gfx_render_callback_frame(Renderer_DrawBootProgress, &args);
}

void Renderer_HideBootProgress(void) {
    gfx_ps2_set_boot_progress(-1, NULL);
}

void Renderer_SetHomeMenuBackgroundActive(bool active) {
    gfx_ps2_set_home_menu_background_active(active);
}

void Renderer_RequestHomeMenuBackground() {
    gfx_ps2_request_home_menu_background();
}

void Renderer_RenderHomeMenu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage, uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue) {
    HomeMenuRenderArgs args;

    Renderer_Init();
    args.selectedIndex = selectedIndex;
    args.screen = screen;
    args.submenuSelectedIndex = submenuSelectedIndex;
    args.statusMessage = statusMessage;
    args.highlightRed = highlightRed;
    args.highlightGreen = highlightGreen;
    args.highlightBlue = highlightBlue;

    gfx_render_callback_frame(Renderer_DrawHomeMenu, &args);
}

int Renderer_ApplyVideoSettings(void) {
    int result;

    Renderer_Init();
    result = gfx_ps2_apply_video_mode();
    if (result != 0) {
        return result;
    }
    Video_CommitMode();
    return Video_Save();
}
