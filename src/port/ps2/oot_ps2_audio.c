#include "oot_port_audio_backend.h"
#include "audio.h"
#include "dma.h"
#include "oot_port_mixer.h"
#include "oot_ps2_platform.h"
#include <audsrv.h>
#include <ps2_audio_driver.h>
#include <kernel.h>
#include <delaythread.h>
#include <timer.h>
#include <stdio.h>
#include <string.h>

#define OOT_PS2_AUDIO_EXTERNAL_POOL_SIZE (2U * 1024U * 1024U)

static s32 sOotPortAudioInitialized;
static u8 sAudioExternalPool[OOT_PS2_AUDIO_EXTERNAL_POOL_SIZE] __attribute__((aligned(64)));

void AudioThread_InitExternalPool(void* ramAddr, u32 size);

s32 OotPortAudioBackend_Init(void) {
    return 0;
}

s32 OotPortAudioBackend_Queue(const void* buf, u32 size) {
    return 0;
}

s32 OotPortAudioBackend_SetFrequency(u32 frequency) {
    return 0;
}

u32 OotPortAudioBackend_GetLength(void) {
    return 0;
}

s32 OotPortAudioBackend_NeedsRefillUrgently(void) {
    return 0;
}

s32 OotPortAudioBackend_NeedsRefillDuringIo(void) {
    return 0;
}

void OotPortAudioBackend_SubmitCommands(const Acmd* cmdList, s32 cmdCount) {
}

void OotPortAudioBackend_SubmitCommandsAndQueue(const Acmd* cmdList, s32 cmdCount, const void* buf, u32 size) {
}

void OotPortAudioBackend_SubmitSynthesis(Acmd* cmdList, s16* aiBuffer, s32 aiFrames) {
}

void OotPortAudioBackend_ExecuteCommands(const Acmd* cmdList, s32 cmdCount) {
}

void OotPortAudio_Init(void) {
    if (sOotPortAudioInitialized) {
        return;
    }

    if (OotPortAudioBackend_Init() < 0) {
        return;
    }

    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);
    AudioThread_InitExternalPool(sAudioExternalPool, sizeof(sAudioExternalPool));
    Audio_Init();
    Audio_InitSound();

    sOotPortAudioInitialized = true;
}

void OotPortAudio_Update(void) {
}
