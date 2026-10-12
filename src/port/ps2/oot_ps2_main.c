#include <string.h>

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
#include "oot_port_asset_loader.h"
#include "oot_port_controls.h"
#include "oot_ps2_platform.h"
#include "oot_ps2_renderer.h"
#include "oot_ps2_runtime_patch.h"
#include "oot_ps2_home_menu.h"
#include "play_state.h"
#include "player.h"
#include "setup_state.h"
#include "title_setup_state.h"

const Color_RGB8* tunicColor = NULL;

void OotPortGame_Init(void);
void Graph_Init(GraphicsContext* gfxCtx);
void Graph_Destroy(GraphicsContext* gfxCtx);
void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState);

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

static void Drivers_Init(const char* executablePath) {
    s32 discBoot = BootIsDisc(executablePath);

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
}

int main(int argc, char** argv) {
    GraphicsContext gfxCtx;
    GameState* gameState;
    GameStateFunc nextInit = Setup_Init;
    size_t nextSize = sizeof(SetupState);
    const char* executablePath = ((argc > 0) && (argv != NULL) && (argv[0] != NULL)) ? argv[0] : NULL;

    Thread_ChangePriority(Thread_GetId(), 0x30);
    Platform_SetDiscBoot(BootIsDisc(executablePath));
    Drivers_Init(executablePath);
    Video_Init(executablePath);
    osInitialize();
    Renderer_Init();

    if (!OotPort_AssetInit(executablePath)) {
        return 1;
    }

    if (!RuntimePatch_Apply()) {
        return 1;
    }

    OotPortControls_Load();
    OotPortGame_Init();
    HomeMenu_Init();
    Graph_Init(&gfxCtx);

    while (nextInit != NULL) {
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
