#ifndef OOT_PS2_RENDERER_H
#define OOT_PS2_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include "sched.h"

typedef struct HomeMenuRenderArgs {
    int selectedIndex;
    int screen;
    int submenuSelectedIndex;
    const char* statusMessage;
    uint8_t highlightRed;
    uint8_t highlightGreen;
    uint8_t highlightBlue;
} HomeMenuRenderArgs;

void Renderer_Init(void);
void Renderer_RenderDisplayList(Gfx* dl);
void Renderer_RenderTask(const OSTask* task);
void Renderer_RenderMenu(const char* title, const char* const* lines, int lineCount, int selectedIndex,
                               const char* statusMessage, int firstRow, int totalRows);
void Renderer_SetWidescreen(bool enabled);
bool Renderer_IsWidescreen(void);
void Renderer_BeginDepthQueries(void);
bool Renderer_IsDepthClear(int32_t x, int32_t y);
bool Renderer_DepthTest(int32_t x, int32_t y, float projectedZ);
void Renderer_RequestPauseBackground(void);
bool Renderer_IsPauseBackgroundReady(void);
void Renderer_SetPauseBackgroundActive(bool active);
void Renderer_SetFpsVisible(bool enabled);
bool Renderer_IsFpsVisible(void);
void Renderer_SetPrerenderRoomState(bool active, u32 roomKey);
void Renderer_SetBootProgress(int code, const char* label);
void Renderer_ShowBootProgress(int code, const char* label);
void Renderer_HideBootProgress(void);
void Renderer_SetHomeMenuBackgroundActive(bool value);
void Renderer_RequestHomeMenuBackground();
void Renderer_RenderHomeMenu(int selectedIndex, int screen, int submenuSelectedIndex,
                                   const char* statusMessage, uint8_t highlightRed, uint8_t highlightGreen,
                                   uint8_t highlightBlue);
int Renderer_ApplyVideoSettings(void);
#endif
