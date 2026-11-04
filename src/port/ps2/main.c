#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <delaythread.h>
#include <iopcontrol.h>
#include <ps2_filesystem_driver.h>
#include <ps2_joystick_driver.h>
#include <sbv_patches.h>
#include <sifrpc.h>

#include "console_logo_state.h"
#include "fault.h"
#include "file_select_state.h"
#include "game.h"
#include "gfx.h"
#include "libc64/malloc.h"
#include "map_select_state.h"
#include "assets/loader.h"
#include "controls/controls.h"
#include "platform.h"
#include "renderer/renderer.h"
#include "runtime/patch.h"
#include "ui/home_menu.h"
#include "play_state.h"
#include "player.h"
#include "setup_state.h"
#include "title_setup_state.h"

const Color_RGB8* tunicColor = NULL;

void OotPortGame_Init(void);
void Graph_Init(GraphicsContext* gfxCtx);
void Graph_Destroy(GraphicsContext* gfxCtx);
void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState);

typedef enum BootProgressCode {
    BOOT_PROGRESS_RENDERER_READY = 0,
    BOOT_PROGRESS_ASSET_INIT_BEGIN,
    BOOT_PROGRESS_ASSET_INIT_DONE,
    BOOT_PROGRESS_RUNTIME_PATCH_BEGIN,
    BOOT_PROGRESS_RUNTIME_PATCH_DONE,
    BOOT_PROGRESS_CONTROLS_BEGIN,
    BOOT_PROGRESS_CONTROLS_DONE,
    BOOT_PROGRESS_GAME_BEGIN,
    BOOT_PROGRESS_GAME_DONE,
    BOOT_PROGRESS_HOME_BEGIN,
    BOOT_PROGRESS_HOME_DONE,
    BOOT_PROGRESS_GRAPH_BEGIN,
    BOOT_PROGRESS_GRAPH_DONE,
    BOOT_PROGRESS_SETUP_BEGIN,
    BOOT_PROGRESS_CONSOLE_LOGO_BEGIN,
} BootProgressCode;

static size_t StateSize(GameStateFunc init) {
    if (init == Setup_Init) {
        return sizeof(SetupState);
    }
    if (init == ConsoleLogo_Init) {
        return sizeof(ConsoleLogoState);
    }
    if (init == TitleSetup_Init) {
        return sizeof(TitleSetupState);
    }
    if (init == Play_Init) {
        return sizeof(PlayState);
    }
    if (init == MapSelect_Init) {
        return sizeof(MapSelectState);
    }
    if (init == FileSelect_Init) {
        return sizeof(FileSelectState);
    }
    return sizeof(GameState);
}

static s32 BootIsDisc(const char* path) {
    if (path == NULL) {
        return false;
    }

    return (strncmp(path, "cdrom0:", 7) == 0) || (strncmp(path, "cdrom:", 6) == 0) ||
           (strncmp(path, "cdfs:", 5) == 0);
}

static s32 BootGetDirectory(const char* path, char* directory, size_t directorySize) {
    const char* slash;
    const char* backslash;
    const char* colon;
    size_t length;

    if ((path == NULL) || (path[0] == '\0') || (directory == NULL) || (directorySize == 0)) {
        return false;
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }

    if (slash != NULL) {
        length = (size_t)(slash - path) + 1;
    } else {
        colon = strchr(path, ':');
        if (colon == NULL) {
            return false;
        }
        length = (size_t)(colon - path) + 1;
    }

    if (length >= directorySize) {
        return false;
    }

    memcpy(directory, path, length);
    directory[length] = '\0';
    return true;
}

static s32 BootWaitForDirectory(const char* directory) {
    struct stat info;
    s32 attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        if (stat(directory, &info) == 0) {
            return true;
        }
        DelayThread(100000);
    }
    return false;
}

static s32 Drivers_Init(const char* executablePath) {
    s32 discBoot = BootIsDisc(executablePath);
    char bootDirectory[256];
    s32 hasBootDirectory = BootGetDirectory(executablePath, bootDirectory, sizeof(bootDirectory));

    if (!discBoot && (!hasBootDirectory || (chdir(bootDirectory) != 0))) {
        printf("oot-ps2 could not select boot directory path=%s\n",
               executablePath != NULL ? executablePath : "(null)");
        return false;
    }

    SifInitRpc(0);
    if (!discBoot) {
        while (!SifIopReset(NULL, 0)) {
        }
        while (!SifIopSync()) {
        }
        SifInitRpc(0);
    }

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    if (discBoot) {
        init_ps2_filesystem_driver();
    } else {
        init_only_boot_ps2_filesystem_driver();
    }

    init_joystick_driver(true);
    init_memcard_driver(false);

    if (!discBoot && !BootWaitForDirectory(bootDirectory)) {
        printf("oot-ps2 timed out waiting for boot directory path=%s\n", bootDirectory);
        return false;
    }

    return true;
}

int main(int argc, char** argv) {
    GraphicsContext gfxCtx;
    GameState* gameState;
    GameStateFunc nextInit = Setup_Init;
    size_t nextSize = sizeof(SetupState);
    const char* executablePath = ((argc > 0) && (argv != NULL) && (argv[0] != NULL)) ? argv[0] : NULL;

    Thread_ChangePriority(Thread_GetId(), 0x30);
    Platform_SetDiscBoot(BootIsDisc(executablePath));
    if (!Drivers_Init(executablePath)) {
        return 1;
    }
    Video_Init(executablePath);
    osInitialize();
    Renderer_Init();
    Renderer_ShowBootProgress(BOOT_PROGRESS_RENDERER_READY, NULL);

    Renderer_ShowBootProgress(BOOT_PROGRESS_ASSET_INIT_BEGIN, NULL);
    if (!OotPort_AssetInit(executablePath)) {
        return 1;
    }
    Renderer_ShowBootProgress(BOOT_PROGRESS_ASSET_INIT_DONE, NULL);

    Renderer_ShowBootProgress(BOOT_PROGRESS_RUNTIME_PATCH_BEGIN, NULL);
    if (!RuntimePatch_Apply()) {
        return 1;
    }
    Renderer_ShowBootProgress(BOOT_PROGRESS_RUNTIME_PATCH_DONE, NULL);

    Renderer_ShowBootProgress(BOOT_PROGRESS_CONTROLS_BEGIN, NULL);
    OotPortControls_Load();
    Renderer_ShowBootProgress(BOOT_PROGRESS_CONTROLS_DONE, NULL);
    Renderer_ShowBootProgress(BOOT_PROGRESS_GAME_BEGIN, NULL);
    OotPortGame_Init();
    Renderer_ShowBootProgress(BOOT_PROGRESS_GAME_DONE, NULL);
    Renderer_ShowBootProgress(BOOT_PROGRESS_HOME_BEGIN, NULL);
    HomeMenu_Init();
    Renderer_ShowBootProgress(BOOT_PROGRESS_HOME_DONE, NULL);
    Renderer_ShowBootProgress(BOOT_PROGRESS_GRAPH_BEGIN, NULL);
    Graph_Init(&gfxCtx);
    Renderer_ShowBootProgress(BOOT_PROGRESS_GRAPH_DONE, NULL);

    while (nextInit != NULL) {
        if (nextInit == Setup_Init) {
            Renderer_ShowBootProgress(BOOT_PROGRESS_SETUP_BEGIN, NULL);
        } else if (nextInit == ConsoleLogo_Init) {
            Renderer_ShowBootProgress(BOOT_PROGRESS_CONSOLE_LOGO_BEGIN, NULL);
            Renderer_HideBootProgress();
        }
        gameState = SYSTEM_ARENA_MALLOC(nextSize, __FILE__, __LINE__);
        if (gameState == NULL) {
            Fault_AddHungupAndCrash(__FILE__, __LINE__);
        }

        memset(gameState, 0, nextSize);
        GameState_Init(gameState, nextInit, &gfxCtx);
        Input_SetPlayState(nextInit == Play_Init ? gameState : NULL);

        while (GameState_IsRunning(gameState)) {
            HomeMenu_PollHomeButton();

            if (HomeMenu_IsOpen()) {

                if (nextInit == Play_Init) {
                    Player* player = GET_PLAYER((PlayState*)gameState);

                    if ((player != NULL) && (player->currentTunic >= PLAYER_TUNIC_KOKIRI) &&
                        (player->currentTunic < PLAYER_TUNIC_MAX)) {
                        tunicColor = &sTunicColors[player->currentTunic];
                    }
                }

                if (HomeMenu_RunFrame(tunicColor) == OOT_PS2_HOME_MENU_RESULT_EXIT_GAME) {
                    gameState->running = false;
                    nextInit = NULL;
                    break;
                }
                continue;
            }
            Graph_Update(&gfxCtx, gameState);
        }

        nextInit = GameState_GetInit(gameState);
        nextSize = gameState->size != 0 ? gameState->size : StateSize(nextInit);
        Input_SetPlayState(NULL);
        GameState_Destroy(gameState);
        SYSTEM_ARENA_FREE(gameState, __FILE__, __LINE__);
    }

    Graph_Destroy(&gfxCtx);
    return 0;
}
