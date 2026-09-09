#include <stdio.h>
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
#include "oot_ps2_platform.h"
#include "oot_ps2_renderer.h"
#include "oot_ps2_runtime_patch.h"
#include "play_state.h"
#include "setup_state.h"
#include "title_setup_state.h"

void OotPortGame_Init(void);
void Graph_Init(GraphicsContext* gfxCtx);
void Graph_Destroy(GraphicsContext* gfxCtx);
void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState);

static size_t OotPs2StateSize(GameStateFunc init) {
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

static s32 OotPs2BootIsDisc(const char* path) {
    if (path == NULL) {
        return false;
    }

    return (strncmp(path, "cdrom0:", 7) == 0) || (strncmp(path, "cdrom:", 6) == 0) ||
           (strncmp(path, "cdfs:", 5) == 0);
}

static void OotPs2Drivers_Init(const char* executablePath) {
    s32 discBoot = OotPs2BootIsDisc(executablePath);

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

    OotPs2Thread_ChangePriority(OotPs2Thread_GetId(), 0x30);
    OotPs2Platform_SetDiscBoot(OotPs2BootIsDisc(executablePath));
    OotPs2Drivers_Init(executablePath);

    printf("oot-ps2 boot os init begin\n");
    osInitialize();
    printf("oot-ps2 boot os init ok\n");

    printf("oot-ps2 boot renderer begin\n");
    OotPs2Renderer_Init();
    printf("oot-ps2 boot renderer ok\n");

    printf("oot-ps2 boot assets begin\n");
    if (!OotPort_AssetInit(executablePath)) {
        printf("oot-ps2 boot assets FAILED\n");
        return 1;
    }
    printf("oot-ps2 boot assets ok\n");

    printf("oot-ps2 boot runtime patches begin\n");
    if (!OotPs2RuntimePatch_Apply()) {
        printf("oot-ps2 boot runtime patches FAILED\n");
        return 1;
    }
    printf("oot-ps2 boot runtime patches ok\n");

    printf("oot-ps2 boot game init begin\n");
    OotPortGame_Init();
    printf("oot-ps2 boot game init ok\n");

    printf("oot-ps2 boot graph init begin\n");
    Graph_Init(&gfxCtx);
    printf("oot-ps2 boot graph init ok\n");

    while (nextInit != NULL) {
        printf("oot-ps2 boot gamestate begin init=%p size=%lu\n",
               (void*)nextInit, (unsigned long)nextSize);
        gameState = SYSTEM_ARENA_MALLOC(nextSize, __FILE__, __LINE__);
        if (gameState == NULL) {
            Fault_AddHungupAndCrash(__FILE__, __LINE__);
        }

        memset(gameState, 0, nextSize);
        GameState_Init(gameState, nextInit, &gfxCtx);
        printf("oot-ps2 boot gamestate init ok\n");

        {
            u32 debugFrame = 0;

            while (GameState_IsRunning(gameState)) {
                if (debugFrame < 4) {
                    printf("oot-ps2 boot Graph_Update frame=%u begin\n", debugFrame);
                }

                Graph_Update(&gfxCtx, gameState);

                if (debugFrame < 4) {
                    printf("oot-ps2 boot Graph_Update frame=%u ok\n", debugFrame);
                }

                debugFrame++;
            }
        }

        nextInit = GameState_GetInit(gameState);
        nextSize = gameState->size != 0 ? gameState->size : OotPs2StateSize(nextInit);
        GameState_Destroy(gameState);
        SYSTEM_ARENA_FREE(gameState, __FILE__, __LINE__);
    }

    Graph_Destroy(&gfxCtx);
    return 0;
}
