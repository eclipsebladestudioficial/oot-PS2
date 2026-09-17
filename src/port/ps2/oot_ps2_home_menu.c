#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <timer.h>
#include "oot_ps2_home_menu.h"

#define OOT_PS2_HOME_MENU_ITEM_RESUME_GAME 0
#define OOT_PS2_HOME_MENU_ITEM_CONTROLLER_MAPPING 1
#define OOT_PS2_HOME_MENU_ITEM_VIDEO_SETTINGS 2
#define OOT_PS2_HOME_MENU_ITEM_ABOUT 3
#define OOT_PS2_HOME_MENU_ITEM_EXIT_GAME 4
#define OOT_PS2_HOME_MENU_ITEM_COUNT 5

#define OOT_PS2_HOME_MENU_SCREEN_MAIN 0
#define OOT_PS2_HOME_MENU_SCREEN_CONTROLLER_MAPPING 1
#define OOT_PS2_HOME_MENU_SCREEN_VIDEO_SETTINGS 2
#define OOT_PS2_HOME_MENU_SCREEN_ABOUT 3

#define OOT_PS2_HOME_MENU_ABOUT_MAX_SCROLL 16

#define OOT_PS2_VIDEO_MENU_ROW_OUTPUT -1
#define OOT_PS2_VIDEO_MENU_ROW_RESOLUTION 0
#define OOT_PS2_VIDEO_MENU_ROW_ASPECT 1
#define OOT_PS2_VIDEO_MENU_ROW_APPLY 2
#define OOT_PS2_VIDEO_MENU_ROW_SAVE 3
#define OOT_PS2_VIDEO_MENU_ROW_RESET 4
#define OOT_PS2_VIDEO_MENU_ROW_BACK 5
#define OOT_PS2_VIDEO_MENU_ROW_COUNT 6

#define OOT_PS2_HOME_MENU_INPUT_LOCKOUT_US 500000
#define OOT_PS2_HOME_MENU_INPUT_DEBOUNCE_US 150000
#define OOT_PS2_HOME_MENU_FALLBACK_HIGHLIGHT_RED 38
#define OOT_PS2_HOME_MENU_FALLBACK_HIGHLIGHT_GREEN 92
#define OOT_PS2_HOME_MENU_FALLBACK_HIGHLIGHT_BLUE 78
#define OOT_PS2_HOME_MENU_BUTTON_MASK \
    (OOT_PS2_BUTTON_CIRCLE | OOT_PS2_BUTTON_CROSS | OOT_PS2_BUTTON_START | OOT_PS2_BUTTON_UP | OOT_PS2_BUTTON_DOWN | OOT_PS2_BUTTON_LEFT | OOT_PS2_BUTTON_RIGHT)

static volatile int sOpenRequested;
static bool sActive;
static int sSelectedIndex;
static int sScreen;
static int sControlSelectedIndex;
static int sVideoSelectedIndex;
static int sAboutScrollOffset;
static int sStatusTimer;
static u32 sCurrentButtons;
static u32 sLastButtons;
static u32 sLastPolledButtons;
static u32 sMenuInputLockoutStartUsec;
static u32 sLastMenuInputUsec;
static char sStatusMessage[64];
static OotPs2PadState padState;

static u32 OotPs2HomeMenu_ReadButtons(void) {

    OotPs2Pad_Read(&padState);
    sCurrentButtons = padState.buttons;

    return sCurrentButtons;
}

static void OotPs2HomeMenu_Open(void) {
    sActive = true;
    sMenuInputLockoutStartUsec = GetTimerSystemTime(); //sceKernelGetSystemTimeLow();
    sSelectedIndex = OOT_PS2_HOME_MENU_ITEM_RESUME_GAME;
    sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
    sControlSelectedIndex = 0;
    sVideoSelectedIndex = 0;
    sAboutScrollOffset = 0;
    sStatusTimer = 0;
    sStatusMessage[0] = '\0';
    sLastButtons = sCurrentButtons;
    sLastMenuInputUsec = sMenuInputLockoutStartUsec;

    OotPs2Renderer_SetHomeMenuBackgroundActive(true);
    OotPs2Renderer_RequestHomeMenuBackground();
}

static void OotPs2HomeMenu_Close(void) {
    sActive = false;
    sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
    sStatusTimer = 0;
    OotPs2Renderer_SetHomeMenuBackgroundActive(false);
}

static int OotPs2HomeMenu_ControlDeadzoneRow(void) {
    return OotPortControls_GetBindingCount();
}

static int OotPs2HomeMenu_ControlN64StickScalingRow(void) {
    return OotPortControls_GetBindingCount() + 1;
}

static int OotPs2HomeMenu_ControlSaveRow(void) {
    return OotPortControls_GetBindingCount() + 2;
}

static int OotPs2HomeMenu_ControlResetRow(void) {
    return OotPortControls_GetBindingCount() + 3;
}

static int OotPs2HomeMenu_ControlBackRow(void) {
    return OotPortControls_GetBindingCount() + 4;
}

static int OotPs2HomeMenu_ControlRowCount(void) {
    return OotPortControls_GetBindingCount() + 5;
}

static void OotPs2HomeMenu_SetStatus(const char* message) {
    snprintf(sStatusMessage, sizeof(sStatusMessage), "%s", message);
    sStatusTimer = 120;
}

static void OotPs2HomeMenu_MoveControlSelection(int direction) {
    int rowCount = OotPs2HomeMenu_ControlRowCount();

    sControlSelectedIndex += direction;
    if (sControlSelectedIndex < 0) {
        sControlSelectedIndex = rowCount - 1;
    } else if (sControlSelectedIndex >= rowCount) {
        sControlSelectedIndex = 0;
    }
}

static void OotPs2HomeMenu_AdjustControlRow(int direction) {
    int bindingCount = OotPortControls_GetBindingCount();

    if (sControlSelectedIndex < bindingCount) {
        OotPortControls_CycleBinding(sControlSelectedIndex, direction);
    } else if (sControlSelectedIndex == OotPs2HomeMenu_ControlDeadzoneRow()) {
        OotPortControls_AdjustDeadzone(direction * 2);
    } else if (sControlSelectedIndex == OotPs2HomeMenu_ControlN64StickScalingRow()) {
        OotPortControls_SetN64StickScalingEnabled(!OotPortControls_IsN64StickScalingEnabled());
    }
}

static void OotPs2HomeMenu_ActivateControlRow(void) {
    if (sControlSelectedIndex < OotPortControls_GetBindingCount()) {
        OotPortControls_CycleBinding(sControlSelectedIndex, 1);
    } else if (sControlSelectedIndex == OotPs2HomeMenu_ControlDeadzoneRow()) {
        OotPortControls_AdjustDeadzone(2);
    } else if (sControlSelectedIndex == OotPs2HomeMenu_ControlN64StickScalingRow()) {
        OotPortControls_SetN64StickScalingEnabled(!OotPortControls_IsN64StickScalingEnabled());
    } else if (sControlSelectedIndex == OotPs2HomeMenu_ControlSaveRow()) {
        if (OotPortControls_Save() == 0) {
            OotPs2HomeMenu_SetStatus("Saved controls.ini");
        } else {
            OotPs2HomeMenu_SetStatus("Save failed");
        }
    } else if (sControlSelectedIndex == OotPs2HomeMenu_ControlResetRow()) {
        OotPortControls_ResetDefaults();
        if (OotPortControls_Save() == 0) {
            OotPs2HomeMenu_SetStatus("Defaults saved");
        } else {
            OotPs2HomeMenu_SetStatus("Defaults restored");
        }
    } else if (sControlSelectedIndex == OotPs2HomeMenu_ControlBackRow()) {
        sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
    }
}

static void OotPs2HomeMenu_MoveVideoSelection(int direction) {
    sVideoSelectedIndex += direction;
    if (sVideoSelectedIndex < 0) {
        sVideoSelectedIndex = OOT_PS2_VIDEO_MENU_ROW_COUNT - 1;
    } else if (sVideoSelectedIndex >= OOT_PS2_VIDEO_MENU_ROW_COUNT) {
        sVideoSelectedIndex = 0;
    }
}

static void OotPs2HomeMenu_AdjustVideoRow(int direction) {
    if (sVideoSelectedIndex == OOT_PS2_VIDEO_MENU_ROW_OUTPUT) {
        OotPs2Video_CycleOutput(direction);
    } else if (sVideoSelectedIndex == OOT_PS2_VIDEO_MENU_ROW_RESOLUTION) {
        OotPs2Video_CycleResolution(direction);
    } else if (sVideoSelectedIndex == OOT_PS2_VIDEO_MENU_ROW_ASPECT) {
        OotPs2Video_CycleAspect(direction);
    }
}

static void OotPs2HomeMenu_ActivateVideoRow(void) {
    if (sVideoSelectedIndex <= OOT_PS2_VIDEO_MENU_ROW_ASPECT) {
        OotPs2HomeMenu_AdjustVideoRow(1);
    } else if (sVideoSelectedIndex == OOT_PS2_VIDEO_MENU_ROW_APPLY) {
        int result;

        result = OotPs2Renderer_ApplyVideoSettings();
        if (result == 0) {
            OotPs2HomeMenu_SetStatus("Video mode applied");
        } else {
            char message[64];

            snprintf(message, sizeof(message), "Video mode failed: %d", result);
            OotPs2HomeMenu_SetStatus(message);
        }
    } else if (sVideoSelectedIndex == OOT_PS2_VIDEO_MENU_ROW_SAVE) {
        if (OotPs2Video_Save() == 0) {
            OotPs2HomeMenu_SetStatus("Saved video.ini");
        } else {
            OotPs2HomeMenu_SetStatus("Save failed");
        }
    } else if (sVideoSelectedIndex == OOT_PS2_VIDEO_MENU_ROW_RESET) {
        OotPs2Video_ResetDefaults();
        if (OotPs2Renderer_ApplyVideoSettings() == 0) {
            OotPs2HomeMenu_SetStatus("Video defaults restored");
        } else {
            OotPs2HomeMenu_SetStatus("Defaults unavailable");
        }
    } else if (sVideoSelectedIndex == OOT_PS2_VIDEO_MENU_ROW_BACK) {
        sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
    }
}

void OotPs2HomeMenu_Init(void) {
    sOpenRequested = false;
    sActive = false;
    sSelectedIndex = OOT_PS2_HOME_MENU_ITEM_RESUME_GAME;
    sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
    sControlSelectedIndex = 0;
    sVideoSelectedIndex = 0;
    sAboutScrollOffset = 0;
    sStatusTimer = 0;
    sCurrentButtons = 0;
    sLastButtons = 0;
    sLastPolledButtons = OotPs2HomeMenu_ReadButtons();
    sMenuInputLockoutStartUsec = 0;
    sLastMenuInputUsec = 0;
    sStatusMessage[0] = '\0';

    // what actually is this?
    //sceImposeSetHomePopup(0);
}

static void OotPs2HomeMenu_RequestOpen(void) {
    sOpenRequested = true;
}

static void OotPs2HomeMenu_MoveAboutScroll(int direction) {
    sAboutScrollOffset += direction;
    if (sAboutScrollOffset < 0) {
        sAboutScrollOffset = 0;
    } else if (sAboutScrollOffset > OOT_PS2_HOME_MENU_ABOUT_MAX_SCROLL) {
        sAboutScrollOffset = OOT_PS2_HOME_MENU_ABOUT_MAX_SCROLL;
    }
}

static void OotPs2HomeMenu_Render(const char* statusMessage, const Color_RGB8* tunicColor) {
    u8 highlightRed = OOT_PS2_HOME_MENU_FALLBACK_HIGHLIGHT_RED;
    u8 highlightGreen = OOT_PS2_HOME_MENU_FALLBACK_HIGHLIGHT_GREEN;
    u8 highlightBlue = OOT_PS2_HOME_MENU_FALLBACK_HIGHLIGHT_BLUE;
    int submenuSelectedIndex = sControlSelectedIndex;

    if (tunicColor != NULL) {
        highlightRed = tunicColor->r;
        highlightGreen = tunicColor->g;
        highlightBlue = tunicColor->b;
    }

    if (sScreen == OOT_PS2_HOME_MENU_SCREEN_VIDEO_SETTINGS) {
        submenuSelectedIndex = sVideoSelectedIndex;
    } else if (sScreen == OOT_PS2_HOME_MENU_SCREEN_ABOUT) {
        submenuSelectedIndex = sAboutScrollOffset;
    }

    OotPs2Renderer_RenderHomeMenu(sSelectedIndex, sScreen, submenuSelectedIndex,
                                  statusMessage, highlightRed, highlightGreen, highlightBlue);
}

void OotPs2HomeMenu_PollHomeButton(void) {
    u32 buttons = OotPs2HomeMenu_ReadButtons();
    u32 pressed = buttons & ~sLastPolledButtons;

    sLastPolledButtons = buttons;

    if (((pressed & OOT_PS2_BUTTON_START) && (pressed & OOT_PS2_BUTTON_UP)) && !sActive) { // home btn
        OotPs2HomeMenu_RequestOpen();
    }
}

bool OotPs2HomeMenu_IsOpen(void) {
    return sActive || sOpenRequested;
}

bool OotPs2HomeMenu_PollToggle(void) {
    return false;
}

OotPs2HomeMenuResult OotPs2HomeMenu_RunFrame(const Color_RGB8* tunicColor) {
    u32 buttons;
    u32 pressed;
    u32 now;

    if (sOpenRequested) {
        sOpenRequested = false;

        if (!sActive) {
            OotPs2HomeMenu_Open();
        }
    }

    if (!sActive) {
        return OOT_PS2_HOME_MENU_RESULT_NONE;
    }

    buttons = sCurrentButtons;
    pressed = buttons & ~sLastButtons;
    sLastButtons = buttons;
    sLastPolledButtons = buttons;

    if (sStatusTimer > 0) {
        sStatusTimer--;
    }

    now = GetTimerSystemTime(); //now = sceKernelGetSystemTimeLow();
    if ((now - sMenuInputLockoutStartUsec) < OOT_PS2_HOME_MENU_INPUT_LOCKOUT_US) {
        OotPs2HomeMenu_Render(NULL, tunicColor);
        return OOT_PS2_HOME_MENU_RESULT_NONE;
    }

    pressed &= OOT_PS2_HOME_MENU_BUTTON_MASK;
    if (pressed != 0) {
        if ((now - sLastMenuInputUsec) < OOT_PS2_HOME_MENU_INPUT_DEBOUNCE_US) {
            pressed = 0;
        } else {
            sLastMenuInputUsec = now;
        }
    }

    if (sScreen == OOT_PS2_HOME_MENU_SCREEN_CONTROLLER_MAPPING) {
        if (pressed & OOT_PS2_BUTTON_CIRCLE) {
            sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
        } else if (pressed & OOT_PS2_BUTTON_UP) {
            OotPs2HomeMenu_MoveControlSelection(-1);
        } else if (pressed & OOT_PS2_BUTTON_DOWN) {
            OotPs2HomeMenu_MoveControlSelection(1);
        } else if (pressed & OOT_PS2_BUTTON_LEFT) {
            OotPs2HomeMenu_AdjustControlRow(-1);
        } else if (pressed & OOT_PS2_BUTTON_RIGHT) {
            OotPs2HomeMenu_AdjustControlRow(1);
        } else if (pressed & (OOT_PS2_BUTTON_CROSS | OOT_PS2_BUTTON_START)) {
            OotPs2HomeMenu_ActivateControlRow();
        }
    } else if (sScreen == OOT_PS2_HOME_MENU_SCREEN_VIDEO_SETTINGS) {
        if (pressed & OOT_PS2_BUTTON_CIRCLE) {
            sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
        } else if (pressed & OOT_PS2_BUTTON_UP) {
            OotPs2HomeMenu_MoveVideoSelection(-1);
        } else if (pressed & OOT_PS2_BUTTON_DOWN) {
            OotPs2HomeMenu_MoveVideoSelection(1);
        } else if (pressed & OOT_PS2_BUTTON_LEFT) {
            OotPs2HomeMenu_AdjustVideoRow(-1);
        } else if (pressed & OOT_PS2_BUTTON_RIGHT) {
            OotPs2HomeMenu_AdjustVideoRow(1);
        } else if (pressed & (OOT_PS2_BUTTON_CROSS | OOT_PS2_BUTTON_START)) {
            OotPs2HomeMenu_ActivateVideoRow();
        }
    } else if (sScreen == OOT_PS2_HOME_MENU_SCREEN_ABOUT) {
        if (pressed & (OOT_PS2_BUTTON_CIRCLE | OOT_PS2_BUTTON_CROSS | OOT_PS2_BUTTON_START)) {
            sScreen = OOT_PS2_HOME_MENU_SCREEN_MAIN;
        } else if (pressed & OOT_PS2_BUTTON_UP) {
            OotPs2HomeMenu_MoveAboutScroll(-1);
        } else if (pressed & OOT_PS2_BUTTON_DOWN) {
            OotPs2HomeMenu_MoveAboutScroll(1);
        }
    } else {
        if (pressed & OOT_PS2_BUTTON_UP) {
            sSelectedIndex = (sSelectedIndex + OOT_PS2_HOME_MENU_ITEM_COUNT - 1) % OOT_PS2_HOME_MENU_ITEM_COUNT;
        } else if (pressed & OOT_PS2_BUTTON_DOWN) {
            sSelectedIndex = (sSelectedIndex + 1) % OOT_PS2_HOME_MENU_ITEM_COUNT;
        }

        if (pressed & (OOT_PS2_BUTTON_CROSS | OOT_PS2_BUTTON_START)) {
            if (sSelectedIndex == OOT_PS2_HOME_MENU_ITEM_RESUME_GAME) {
                OotPs2HomeMenu_Close();
                return OOT_PS2_HOME_MENU_RESULT_NONE;
            } else if (sSelectedIndex == OOT_PS2_HOME_MENU_ITEM_EXIT_GAME) {
                OotPs2HomeMenu_Close();
                return OOT_PS2_HOME_MENU_RESULT_EXIT_GAME;
            } else if (sSelectedIndex == OOT_PS2_HOME_MENU_ITEM_VIDEO_SETTINGS) {
                sScreen = OOT_PS2_HOME_MENU_SCREEN_VIDEO_SETTINGS;
                sVideoSelectedIndex = 0;
            } else if (sSelectedIndex == OOT_PS2_HOME_MENU_ITEM_ABOUT) {
                sScreen = OOT_PS2_HOME_MENU_SCREEN_ABOUT;
                sAboutScrollOffset = 0;
            } else {
                sScreen = OOT_PS2_HOME_MENU_SCREEN_CONTROLLER_MAPPING;
            }
        }
    }

    OotPs2HomeMenu_Render((sStatusTimer > 0) ? sStatusMessage : NULL, tunicColor);
    return OOT_PS2_HOME_MENU_RESULT_NONE;
}
