#include "oot_port_audio_backend.h"
#include "audio.h"
#include "dma.h"
#include "oot_port_mixer.h"
#include "oot_ps2_platform.h"
#include <audsrv.h>
#include <ps2_audio_driver.h>
#include <stdio.h>
#include <string.h>

#define OOT_PS2_AUDIO_CHANNELS 2
#define OOT_PS2_AUDIO_FRAME_BYTES (OOT_PS2_AUDIO_CHANNELS * (int)sizeof(s16))
#define OOT_PS2_AUDIO_EXTERNAL_POOL_SIZE (2 * 1024 * 1024)
#define OOT_PS2_AUDIO_CHUNK_BYTES 2048
#define OOT_PS2_AUDIO_CHUNK_FRAMES (OOT_PS2_AUDIO_CHUNK_BYTES / OOT_PS2_AUDIO_FRAME_BYTES)
#define OOT_PS2_AUDIO_IOP_PRIME_CHUNKS 1
#define OOT_PS2_AUDIO_PREBUFFER_BYTES (OOT_PS2_AUDIO_CHUNK_BYTES * 2)
#define OOT_PS2_AUDIO_TARGET_BYTES (OOT_PS2_AUDIO_CHUNK_BYTES * 3)
#define OOT_PS2_AUDIO_HIGH_BYTES (OOT_PS2_AUDIO_CHUNK_BYTES * 6)
#define OOT_PS2_AUDIO_RING_BYTES (128 * 1024)
#define OOT_PS2_AUDIO_OUTPUT_STACK_SIZE (16 * 1024)
#define OOT_PS2_AUDIO_PRODUCER_STACK_SIZE (512 * 1024)
#define OOT_PS2_AUDIO_OUTPUT_PRIORITY 0x16
#define OOT_PS2_AUDIO_PRODUCER_PRIORITY 0x17
#define OOT_PS2_AUDIO_UPDATE_USEC_BASE (1000000U / 60U)
#define OOT_PS2_AUDIO_UPDATE_USEC_REM (1000000U % 60U)

static u8 sAudioExternalPool[OOT_PS2_AUDIO_EXTERNAL_POOL_SIZE] __attribute__((aligned(64)));
static u8 sAudioPcmRing[OOT_PS2_AUDIO_RING_BYTES] __attribute__((aligned(64)));
static u8 sAudioFeedChunk[OOT_PS2_AUDIO_CHUNK_BYTES] __attribute__((aligned(64)));
static s32 sAudioInitialized;
static s32 sAudioDriverReady;
static u32 sAudioFrequency = 32000;
static volatile u32 sAudioRingRead;
static volatile u32 sAudioRingWrite;
static volatile u32 sAudioRingCount;
static u32 sAudioQuantumPhase;
static OotPs2Handle sAudioRingMutex = -1;
static OotPs2Handle sAudioDataSema = -1;
static OotPs2Handle sAudioOutputThreadId = -1;
static OotPs2Handle sAudioProducerThreadId = -1;

void AudioThread_InitExternalPool(void* ramAddr, u32 size);

static void OotPs2Audio_ResetRing(void) {
    if (sAudioRingMutex >= 0) {
        OotPs2Sema_Wait(sAudioRingMutex);
    }

    sAudioRingRead = 0;
    sAudioRingWrite = 0;
    sAudioRingCount = 0;

    if (sAudioRingMutex >= 0) {
        OotPs2Sema_Signal(sAudioRingMutex);
    }
}

static u32 OotPs2Audio_RingWrite(const u8* src, u32 bytes) {
    u32 first;
    u32 countAfter;

    bytes &= ~(OOT_PS2_AUDIO_FRAME_BYTES - 1);
    if (bytes == 0) {
        return 0;
    }

    if (bytes > OOT_PS2_AUDIO_RING_BYTES) {
        src += bytes - OOT_PS2_AUDIO_RING_BYTES;
        bytes = OOT_PS2_AUDIO_RING_BYTES;
    }

    OotPs2Sema_Wait(sAudioRingMutex);

    if (sAudioRingCount > OOT_PS2_AUDIO_HIGH_BYTES) {
        u32 drop = (sAudioRingCount - OOT_PS2_AUDIO_TARGET_BYTES) & ~(OOT_PS2_AUDIO_FRAME_BYTES - 1);

        sAudioRingRead = (sAudioRingRead + drop) % OOT_PS2_AUDIO_RING_BYTES;
        sAudioRingCount -= drop;
    }

    if (bytes > OOT_PS2_AUDIO_RING_BYTES - sAudioRingCount) {
        u32 drop = bytes - (OOT_PS2_AUDIO_RING_BYTES - sAudioRingCount);

        drop = (drop + OOT_PS2_AUDIO_FRAME_BYTES - 1) & ~(OOT_PS2_AUDIO_FRAME_BYTES - 1);
        if (drop > sAudioRingCount) {
            drop = sAudioRingCount;
        }

        sAudioRingRead = (sAudioRingRead + drop) % OOT_PS2_AUDIO_RING_BYTES;
        sAudioRingCount -= drop;
    }

    first = OOT_PS2_AUDIO_RING_BYTES - sAudioRingWrite;
    if (first > bytes) {
        first = bytes;
    }

    memcpy(&sAudioPcmRing[sAudioRingWrite], src, first);
    if (bytes > first) {
        memcpy(sAudioPcmRing, src + first, bytes - first);
    }

    sAudioRingWrite = (sAudioRingWrite + bytes) % OOT_PS2_AUDIO_RING_BYTES;
    sAudioRingCount += bytes;
    countAfter = sAudioRingCount;

    OotPs2Sema_Signal(sAudioRingMutex);

    if (countAfter >= OOT_PS2_AUDIO_PREBUFFER_BYTES) {
        OotPs2Sema_Signal(sAudioDataSema);
    }

    return bytes;
}

static u32 OotPs2Audio_RingRead(u8* dst, u32 bytes) {
    u32 take;
    u32 first;

    OotPs2Sema_Wait(sAudioRingMutex);

    take = sAudioRingCount;
    if (take > bytes) {
        take = bytes;
    }
    take &= ~(OOT_PS2_AUDIO_FRAME_BYTES - 1);

    first = OOT_PS2_AUDIO_RING_BYTES - sAudioRingRead;
    if (first > take) {
        first = take;
    }

    if (first != 0) {
        memcpy(dst, &sAudioPcmRing[sAudioRingRead], first);
    }
    if (take > first) {
        memcpy(dst + first, sAudioPcmRing, take - first);
    }

    sAudioRingRead = (sAudioRingRead + take) % OOT_PS2_AUDIO_RING_BYTES;
    sAudioRingCount -= take;

    OotPs2Sema_Signal(sAudioRingMutex);
    return take;
}

static s32 OotPs2Audio_FeedOneChunk(void) {
    u32 got;
    s32 result;

    result = audsrv_wait_audio(OOT_PS2_AUDIO_CHUNK_BYTES);
    if (result != AUDSRV_ERR_NOERROR) {
        return -1;
    }

    got = OotPs2Audio_RingRead(sAudioFeedChunk, OOT_PS2_AUDIO_CHUNK_BYTES);
    if (got < OOT_PS2_AUDIO_CHUNK_BYTES) {
        memset(sAudioFeedChunk + got, 0, OOT_PS2_AUDIO_CHUNK_BYTES - got);
    }

    return audsrv_play_audio((const char*)sAudioFeedChunk, OOT_PS2_AUDIO_CHUNK_BYTES) < 0 ? -1 : 0;
}

static s32 OotPs2Audio_OutputThread(OotPs2ThreadArgSize args, void* argp) {
    u64 nextFeed;
    u64 periodNumerator;
    u64 periodBase;
    u32 periodRemainder;
    u32 periodPhase = 0;
    u32 i;

    (void)args;
    (void)argp;

    while (sAudioRingCount < OOT_PS2_AUDIO_PREBUFFER_BYTES) {
        OotPs2Sema_Wait(sAudioDataSema);
    }

    for (i = 0; i < OOT_PS2_AUDIO_IOP_PRIME_CHUNKS; i++) {
        if (OotPs2Audio_FeedOneChunk() < 0) {
            OotPs2Thread_Delay(1000);
            i--;
        }
    }

    periodNumerator = (u64)OOT_PS2_AUDIO_CHUNK_FRAMES * 1000000ULL;
    periodBase = periodNumerator / sAudioFrequency;
    periodRemainder = (u32)(periodNumerator % sAudioFrequency);
    nextFeed = OotPs2Time_GetUsec() + periodBase;

    for (;;) {
        u64 now = OotPs2Time_GetUsec();
        s64 delay = (s64)(nextFeed - now);

        if (delay > 0) {
            OotPs2Thread_Delay((u32)delay);
        }

        if (OotPs2Audio_FeedOneChunk() < 0) {
            OotPs2Thread_Delay(1000);
        }

        nextFeed += periodBase;
        periodPhase += periodRemainder;
        if (periodPhase >= sAudioFrequency) {
            nextFeed++;
            periodPhase -= sAudioFrequency;
        }

        now = OotPs2Time_GetUsec();
        if (now > nextFeed + 100000ULL) {
            nextFeed = now;
            periodPhase = 0;
        }
    }

    return 0;
}

static s32 OotPs2Audio_ProducerThread(OotPs2ThreadArgSize args, void* argp) {
    u64 nextUpdate = OotPs2Time_GetUsec();
    u32 updateUsecPhase = 0;

    (void)args;
    (void)argp;

    for (;;) {
        u64 now;
        s64 delay;

        AudioThread_Update();

        nextUpdate += OOT_PS2_AUDIO_UPDATE_USEC_BASE;
        updateUsecPhase += OOT_PS2_AUDIO_UPDATE_USEC_REM;
        if (updateUsecPhase >= 60U) {
            nextUpdate++;
            updateUsecPhase -= 60U;
        }

        now = OotPs2Time_GetUsec();
        delay = (s64)(nextUpdate - now);
        if (delay > 0) {
            OotPs2Thread_Delay((u32)delay);
        }

        now = OotPs2Time_GetUsec();
        if (now > nextUpdate + 50000ULL) {
            nextUpdate = now;
            updateUsecPhase = 0;
        }
    }

    return 0;
}

static s32 OotPs2Audio_StartThread(const char* name, OotPs2ThreadEntry entry, s32 priority, s32 stackSize,
                                   OotPs2Handle* outThreadId) {
    OotPs2Handle threadId = OotPs2Thread_Create(name, entry, priority, stackSize);

    if (threadId < 0) {
        return -1;
    }
    if (OotPs2Thread_Start(threadId, 0, NULL) < 0) {
        OotPs2Thread_Delete(threadId);
        return -1;
    }

    *outThreadId = threadId;
    return 0;
}

static s32 OotPs2Audio_StartOutputThread(void) {
    sAudioRingMutex = OotPs2Sema_Create(1, 1);
    if (sAudioRingMutex < 0) {
        return -1;
    }

    sAudioDataSema = OotPs2Sema_Create(0, 1);
    if (sAudioDataSema < 0) {
        OotPs2Sema_Delete(sAudioRingMutex);
        sAudioRingMutex = -1;
        return -1;
    }

    OotPs2Audio_ResetRing();
    if (OotPs2Audio_StartThread("oot-audio-out", OotPs2Audio_OutputThread, OOT_PS2_AUDIO_OUTPUT_PRIORITY,
                                OOT_PS2_AUDIO_OUTPUT_STACK_SIZE, &sAudioOutputThreadId) < 0) {
        OotPs2Sema_Delete(sAudioDataSema);
        OotPs2Sema_Delete(sAudioRingMutex);
        sAudioDataSema = -1;
        sAudioRingMutex = -1;
        return -1;
    }

    return 0;
}

static s32 OotPs2Audio_StartProducerThread(void) {
    if (sAudioProducerThreadId >= 0) {
        return 0;
    }

    return OotPs2Audio_StartThread("oot-audio-prod", OotPs2Audio_ProducerThread, OOT_PS2_AUDIO_PRODUCER_PRIORITY,
                                   OOT_PS2_AUDIO_PRODUCER_STACK_SIZE, &sAudioProducerThreadId);
}

static s32 OotPs2Audio_SetFormat(u32 frequency) {
    audsrv_fmt_t format;
    s32 result;

    if (!sAudioDriverReady) {
        return -1;
    }

    if (frequency < 8000) {
        frequency = 8000;
    } else if (frequency > 48000) {
        frequency = 48000;
    }

    format.freq = (s32)frequency;
    format.bits = 16;
    format.channels = OOT_PS2_AUDIO_CHANNELS;

    result = audsrv_set_format(&format);
    if (result != AUDSRV_ERR_NOERROR) {
        return -1;
    }

    sAudioFrequency = frequency;
    sAudioQuantumPhase = 0;
    return (s32)frequency;
}

s32 OotPortAudioBackend_Init(void) {
    if (sAudioDriverReady) {
        return 0;
    }

    if (init_audio_driver() < 0) {
        return -1;
    }

    sAudioDriverReady = 1;
    if (OotPs2Audio_SetFormat(sAudioFrequency) < 0) {
        deinit_audio_driver();
        sAudioDriverReady = 0;
        return -1;
    }

    audsrv_set_volume(MAX_VOLUME);
    if (OotPs2Audio_StartOutputThread() < 0) {
        deinit_audio_driver();
        sAudioDriverReady = 0;
        return -1;
    }

    return 0;
}

s32 OotPortAudioBackend_Queue(const void* buf, u32 size) {
    if (!sAudioDriverReady || buf == NULL || size == 0) {
        return -1;
    }

    size &= ~(OOT_PS2_AUDIO_FRAME_BYTES - 1);
    return OotPs2Audio_RingWrite((const u8*)buf, size) == size ? 0 : -1;
}

s32 OotPortAudioBackend_SetFrequency(u32 frequency) {
    if (!sAudioDriverReady) {
        sAudioFrequency = frequency;
        return (s32)frequency;
    }

    if (frequency == sAudioFrequency) {
        return (s32)frequency;
    }

    audsrv_stop_audio();
    OotPs2Audio_ResetRing();
    return OotPs2Audio_SetFormat(frequency);
}

u32 OotPortAudioBackend_GetLength(void) {
    s32 target = gAudioCtx.audioBufferParameters.samplesPerFrameTarget;
    s32 desired;
    s32 remaining;
    u32 quantaPerSecond;
    u32 baseQuanta;
    u32 remQuanta;

    if (target <= 0) {
        return 0;
    }

    quantaPerSecond = sAudioFrequency / 16U;
    baseQuanta = quantaPerSecond / 60U;
    remQuanta = quantaPerSecond % 60U;
    desired = (s32)(baseQuanta * 16U);

    sAudioQuantumPhase += remQuanta;
    if (sAudioQuantumPhase >= 60U) {
        desired += 16;
        sAudioQuantumPhase -= 60U;
    }

    if (desired < gAudioCtx.audioBufferParameters.minAiBufferLength) {
        desired = gAudioCtx.audioBufferParameters.minAiBufferLength;
    }
    if (desired > gAudioCtx.audioBufferParameters.maxAiBufferLength) {
        desired = gAudioCtx.audioBufferParameters.maxAiBufferLength;
    }

    remaining = target - desired + 0x90;
    if (remaining < 0) {
        remaining = 0;
    }

    return (u32)remaining * OOT_PS2_AUDIO_FRAME_BYTES;
}

s32 OotPortAudioBackend_NeedsRefillUrgently(void) {
    return sAudioRingCount < OOT_PS2_AUDIO_PREBUFFER_BYTES;
}

s32 OotPortAudioBackend_NeedsRefillDuringIo(void) {
    return sAudioRingCount < OOT_PS2_AUDIO_TARGET_BYTES;
}

void OotPortAudioBackend_SubmitCommands(const Acmd* cmdList, s32 cmdCount) {
    if (cmdList != NULL && cmdCount > 0) {
        OotPortMixer_ExecuteCommandList(cmdList, cmdCount);
    }
}

void OotPortAudioBackend_SubmitCommandsAndQueue(const Acmd* cmdList, s32 cmdCount, const void* buf, u32 size) {
    OotPortAudioBackend_SubmitCommands(cmdList, cmdCount);
    OotPortAudioBackend_Queue(buf, size);
}

void OotPortAudioBackend_SubmitSynthesis(Acmd* cmdList, s16* aiBuffer, s32 aiFrames) {
    s32 cmdCount = 0;

    if (!sAudioDriverReady || cmdList == NULL || aiBuffer == NULL || aiFrames <= 0) {
        return;
    }

    AudioSynth_BuildCommandList(cmdList, &cmdCount, aiBuffer, aiFrames);
    if (cmdCount > 0) {
        OotPortMixer_ExecuteCommandList(cmdList, cmdCount);
    }

    OotPortAudioBackend_Queue(aiBuffer, (u32)aiFrames * OOT_PS2_AUDIO_FRAME_BYTES);
}

void OotPortAudioBackend_ExecuteCommands(const Acmd* cmdList, s32 cmdCount) {
    OotPortAudioBackend_SubmitCommands(cmdList, cmdCount);
}

void OotPortAudio_Init(void) {
    if (sAudioInitialized) {
        return;
    }

    if (OotPortAudioBackend_Init() < 0) {
        return;
    }

    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);
    AudioThread_InitExternalPool(sAudioExternalPool, sizeof(sAudioExternalPool));
    Audio_Init();
    Audio_InitSound();
    sAudioInitialized = 1;

    OotPs2Audio_StartProducerThread();
}

void OotPortAudio_Update(void) {
    if (sAudioInitialized && sAudioProducerThreadId < 0) {
        AudioThread_Update();
    }
}
