#include "sched.h"

#include "array_count.h"
#include "oot_ps2_platform.h"
#include "oot_ps2_renderer.h"

#include <string.h>

#define OOT_PS2_VI_RATE_HZ 60U
#define OOT_PS2_FRAME_BASE_USEC 16666U
#define OOT_PS2_FRAME_REMAINDER 40U
#define OOT_PS2_PRESENT_SAFETY_USEC 750U

static u32 sNextGfxCompletionUsec;
static u32 sGfxPacingRemainder;
static bool sGfxPacingInitialized;
static u32 sLastUpdateRate;

static inline s32 SchedPs2_TimeDiff(u32 a, u32 b) {
    return (s32)(a - b);
}

static u32 SchedPs2_GetUpdateRate(const OSScTask* task) {
    if ((task == NULL) || (task->framebuffer == NULL) || (task->framebuffer->updateRate <= 0)) {
        return 1U;
    }

    return (u32)task->framebuffer->updateRate;
}

static u32 SchedPs2_GetFrameUsec(u32 updateRate) {
    u32 frameUsec = OOT_PS2_FRAME_BASE_USEC * updateRate;

    sGfxPacingRemainder += OOT_PS2_FRAME_REMAINDER * updateRate;
    if (sGfxPacingRemainder >= OOT_PS2_VI_RATE_HZ) {
        u32 extraUsec = sGfxPacingRemainder / OOT_PS2_VI_RATE_HZ;

        frameUsec += extraUsec;
        sGfxPacingRemainder -= extraUsec * OOT_PS2_VI_RATE_HZ;
    }

    return frameUsec;
}

static void SchedPs2_ResetPacing(u32 updateRate) {
    if (sLastUpdateRate == updateRate) {
        return;
    }

    sNextGfxCompletionUsec = 0U;
    sGfxPacingRemainder = 0U;
    sGfxPacingInitialized = false;
    sLastUpdateRate = updateRate;
}

static u32 SchedPs2_RenderTask(const OSScTask* task) {
    u32 begin = OotPs2Time_GetUsecLow();

    OotPs2Renderer_RenderTask(&task->list);
    return OotPs2Time_GetUsecLow() - begin;
}

static void SchedPs2_PresentUnlocked(const OSScTask* task, u32 updateRate) {
    u32 frameUsec = SchedPs2_GetFrameUsec(updateRate);
    u32 now = OotPs2Time_GetUsecLow();
    u32 deadline;
    u32 firstBegin = now;
    u32 lastRenderUsec = 0U;
    u32 presents = 0U;

    if (!sGfxPacingInitialized) {
        sNextGfxCompletionUsec = now + frameUsec;
        sGfxPacingInitialized = true;
    } else if (SchedPs2_TimeDiff(now, sNextGfxCompletionUsec) >= 0) {
        sNextGfxCompletionUsec = now + frameUsec;
    }

    deadline = sNextGfxCompletionUsec;

    while (presents < updateRate) {
        if (presents != 0U) {
            u32 targetStart = firstBegin + (u32)(((u64)1000000U * presents) / OOT_PS2_VI_RATE_HZ);
            s32 waitUsec;

            now = OotPs2Time_GetUsecLow();
            if (lastRenderUsec == 0U ||
                SchedPs2_TimeDiff(deadline, now) <=
                    (s32)(lastRenderUsec + OOT_PS2_PRESENT_SAFETY_USEC)) {
                break;
            }

            waitUsec = SchedPs2_TimeDiff(targetStart, now);
            if (waitUsec > 0) {
                if (SchedPs2_TimeDiff(deadline, targetStart) <=
                    (s32)(lastRenderUsec + OOT_PS2_PRESENT_SAFETY_USEC)) {
                    break;
                }
                OotPs2Thread_Delay((u32)waitUsec);
            }
        }

        lastRenderUsec = SchedPs2_RenderTask(task);
        presents++;

        if (SchedPs2_TimeDiff(OotPs2Time_GetUsecLow(), deadline) >= 0) {
            break;
        }
    }

    now = OotPs2Time_GetUsecLow();
    if (SchedPs2_TimeDiff(deadline, now) > 0) {
        OotPs2Thread_Delay((u32)SchedPs2_TimeDiff(deadline, now));
        sNextGfxCompletionUsec = deadline + frameUsec;
    } else {
        sNextGfxCompletionUsec = now + frameUsec;
    }
}

static void SchedPs2_PresentNormal(const OSScTask* task, u32 updateRate) {
    u32 now;
    u32 frameUsec;
    s32 waitUsec;

    SchedPs2_RenderTask(task);
    now = OotPs2Time_GetUsecLow();
    frameUsec = SchedPs2_GetFrameUsec(updateRate);

    if (!sGfxPacingInitialized) {
        sNextGfxCompletionUsec = now;
        sGfxPacingInitialized = true;
    }

    waitUsec = SchedPs2_TimeDiff(sNextGfxCompletionUsec, now);
    if (waitUsec > 0) {
        OotPs2Thread_Delay((u32)waitUsec);
    } else if (waitUsec < 0) {
        sNextGfxCompletionUsec = now;
    }

    sNextGfxCompletionUsec += frameUsec;
}

void Sched_Notify(Scheduler* sc) {
    OSScTask* task;

    if (sc == NULL) {
        return;
    }

    while (osRecvMesg(&sc->cmdQueue, (OSMesg*)&task, OS_MESG_NOBLOCK) == 0) {
        if (task == NULL) {
            continue;
        }

        if (task->list.t.type == M_GFXTASK) {
            bool presents = ((task->flags & OS_SC_SWAPBUFFER) != 0) && (task->framebuffer != NULL);

            if (presents) {
                u32 updateRate = SchedPs2_GetUpdateRate(task);

                SchedPs2_ResetPacing(updateRate);

                if (updateRate >= 3U) {
                    SchedPs2_PresentUnlocked(task, updateRate);
                } else {
                    SchedPs2_PresentNormal(task, updateRate);
                }

                osViSwapBuffer(task->framebuffer->swapBuffer);
            } else {
                SchedPs2_RenderTask(task);
            }
        }

        if (task->msgQueue != NULL) {
            osSendMesg(task->msgQueue, task->msg, OS_MESG_NOBLOCK);
        }
    }
}

void Sched_Init(Scheduler* sc, void* stack, OSPri priority, u8 viModeType, UNK_TYPE arg4, IrqMgr* irqMgr) {
    if (sc == NULL) {
        return;
    }

    memset(sc, 0, sizeof(*sc));
    osCreateMesgQueue(&sc->interruptQueue, sc->interruptMsgBuf, ARRAY_COUNT(sc->interruptMsgBuf));
    osCreateMesgQueue(&sc->cmdQueue, sc->cmdMsgBuf, ARRAY_COUNT(sc->cmdMsgBuf));

    sc->retraceCount = 1;
    sc->isFirstSwap = true;

    sNextGfxCompletionUsec = 0U;
    sGfxPacingRemainder = 0U;
    sGfxPacingInitialized = false;
    sLastUpdateRate = 0U;

    OotPs2Renderer_Init();

    (void)stack;
    (void)priority;
    (void)viModeType;
    (void)arg4;
    (void)irqMgr;
}

void Sched_FlushTaskQueue(void) {
}
