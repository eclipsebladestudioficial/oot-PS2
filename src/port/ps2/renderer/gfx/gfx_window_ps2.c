#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include <dmaKit.h>
#include <ee_regs.h>
#include <vif_registers.h>
#include <gsKit.h>
#include <gsInline.h>
#include <gif_registers.h>
#include <kernel.h>
#include <timer.h>

#include "gfx_window_ps2.h"
#include "platform.h"

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 448

/*
antes voce falar alguma coisa pequeno garfanhoto a resolução interna do jogo continua a mesma que é 640 x 448
 a logical width é só resolução logica que veio herdada do n64 então eu preciso por ela pra eu pegar as cordenadas
e converter pro ps2 antes de falar bosta leia o codigo seu dumbo
*/

#define LOGICAL_WIDTH 320
#define LOGICAL_HEIGHT 240

#define GIF_DMA_TIMEOUT_ITERATIONS 8000000U
#define GS_FINISH_TIMEOUT_ITERATIONS 8000000U
#define FRAME_FINISH_TIMEOUT_ITERATIONS 12000000U
#define LOCAL_TRANSFER_TIMEOUT_ITERATIONS 4000000U

#define DMA_CHCR_STR_MASK (1U << 8)
#define VIF1_STAT_FDR_MASK (1U << 23)

#define DEPTH_TRANSFER_PSM(psm) ((u32)(psm) | 0x30U)

#define LOCAL_TRANSFER_REGISTER_COUNT 4U
#define LOCAL_TRANSFER_PACKET_QWORDS (LOCAL_TRANSFER_REGISTER_COUNT + 1U)
#define LOCAL_TRANSFER_PACKET_U64S (LOCAL_TRANSFER_PACKET_QWORDS * 2U)

#define NTSC_START_X 492
#define NTSC_START_Y 34
#define NTSC_DISPLAY_WIDTH 2880
#define NTSC_DISPLAY_HEIGHT 480

enum PauseBackgroundState {
    PAUSE_BACKGROUND_DISABLED,
    PAUSE_BACKGROUND_IDLE,
    PAUSE_BACKGROUND_CAPTURE_PENDING,
    PAUSE_BACKGROUND_CAPTURED,
};

GSGLOBAL* gs_global;

static volatile bool sGsInitActive;
static volatile bool sGsInitFailed;
static volatile bool sAsyncFrameSubmit;
static bool sFramePending;
static int sDisplayedBufferIndex;
static bool sDisplayedFrameValid;
static enum PauseBackgroundState sPauseBackgroundState = PAUSE_BACKGROUND_DISABLED;
static int sPauseBackgroundSourceIndex = -1;
static u32* sVisMonoReadback;
static size_t sVisMonoReadbackBytes;
static u16 sDepthReadback[64] __attribute__((aligned(64)));
static int sDepthReadbackX = -1;
static int sDepthReadbackY = -1;

extern void gfx_ps2_invalidate_register_cache(void);

void __real_dmaKit_wait_fast(void);
int __real_DIntr(void);
int __real_EIntr(void);
void __real_gsKit_finish(void);

static inline void GfxPipelineSync(void) {
    __asm__ volatile("sync.p; nop;");
}

static void GfxRecoverGifDma(bool resetGs) {
    *R_EE_D2_CHCR = 0;
    *R_EE_D2_MADR = 0;
    *R_EE_D2_QWC = 0;
    *R_EE_D2_TADR = 0;

    GIF_REG_CTRL = GIF_SET_CTRL(1, 0);
    GfxPipelineSync();
    GIF_REG_CTRL = 0;

    if (resetGs) {
        GS_RESET();
        GfxPipelineSync();
        *GS_CSR = 0;
    }

    dmaKit_chan_init(DMA_CHANNEL_GIF);
}

int __wrap_DIntr(void) {
    if (sGsInitActive && Platform_IsDiscBoot()) {
        return 1;
    }
    return __real_DIntr();
}

int __wrap_EIntr(void) {
    if (sGsInitActive && Platform_IsDiscBoot()) {
        return 1;
    }
    return __real_EIntr();
}

void __wrap_gsKit_finish(void) {
    if (!sAsyncFrameSubmit) {
        __real_gsKit_finish();
    }
}

void __wrap_dmaKit_wait_fast(void) {
    if (!sGsInitActive) {
        __real_dmaKit_wait_fast();
        return;
    }

    if (dmaKit_wait(DMA_CHANNEL_GIF, GIF_DMA_TIMEOUT_ITERATIONS) < 0) {
        sGsInitFailed = true;
        GfxRecoverGifDma(false);
    }
}

void gfx_ps2_prepare_crash_screen(void) {
    GfxRecoverGifDma(true);
}

static bool GfxWaitGifBounded(void) {
    u32 remaining = GIF_DMA_TIMEOUT_ITERATIONS;

    while ((*R_EE_D2_CHCR & DMA_CHCR_STR_MASK) != 0U && remaining-- != 0U) {
        __asm__ volatile("nop");
    }
    if ((*R_EE_D2_CHCR & DMA_CHCR_STR_MASK) != 0U) {
        GfxRecoverGifDma(false);
        return false;
    }
    return true;
}

static bool GfxWaitFinishBounded(u32 iterations) {
    while (!GS_CSR_FINISH && iterations-- != 0U) {
        __asm__ volatile("nop");
    }
    return (GS_CSR_FINISH) != 0;
}

static bool GfxWaitVif1Bounded(void) {
    u32 remaining = LOCAL_TRANSFER_TIMEOUT_ITERATIONS;

    while ((*R_EE_D1_CHCR & DMA_CHCR_STR_MASK) != 0U && remaining-- != 0U) {
        __asm__ volatile("nop");
    }
    return (*R_EE_D1_CHCR & DMA_CHCR_STR_MASK) == 0U;
}

static bool GfxWindow_ReadLocalRect(void* dst, size_t dstBytes, u32 sourceAddr, u32 sourcePsm, u32 sourceWidth, u32 x,
                                    u32 y, u32 width, u32 height) {
    u64 packet[LOCAL_TRANSFER_PACKET_U64S] __attribute__((aligned(64)));
    u64* p = packet;
    size_t bytesPerPixel;
    size_t pixelCount;
    size_t transferBytes;
    size_t dmaBytes;
    size_t qwcRemaining;
    u8* writePtr;

    if (gs_global == NULL || dst == NULL || width == 0 || height == 0 || sourceWidth == 0 ||
        (uintptr_t)dst % 16U != 0U || sourceAddr % GS_VRAM_BLOCKSIZE_256 != 0U ||
        sourceWidth % GS_VRAM_TBWALIGN != 0U) {
        return false;
    }

    switch (sourcePsm) {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case DEPTH_TRANSFER_PSM(GS_PSMZ_16):
        case DEPTH_TRANSFER_PSM(GS_PSMZ_16S):
            bytesPerPixel = 2U;
            break;
        case GS_PSM_CT24:
        case GS_PSM_CT32:
        case DEPTH_TRANSFER_PSM(GS_PSMZ_24):
        case DEPTH_TRANSFER_PSM(GS_PSMZ_32):
            bytesPerPixel = 4U;
            break;
        default:
            return false;
    }

    if ((size_t)width > SIZE_MAX / (size_t)height) {
        return false;
    }
    pixelCount = (size_t)width * (size_t)height;
    if (pixelCount > SIZE_MAX / bytesPerPixel) {
        return false;
    }
    transferBytes = pixelCount * bytesPerPixel;
    if (transferBytes > SIZE_MAX - 15U) {
        return false;
    }
    dmaBytes = (transferBytes + 15U) & ~(size_t)15U;
    if (dmaBytes > dstBytes) {
        return false;
    }
    qwcRemaining = dmaBytes >> 4;
    writePtr = dst;

    if (!GfxWaitFinishBounded(GS_FINISH_TIMEOUT_ITERATIONS)) {
        return false;
    }
    if (dmaKit_wait(DMA_CHANNEL_GIF, LOCAL_TRANSFER_TIMEOUT_ITERATIONS) < 0) {
        GfxRecoverGifDma(false);
        return false;
    }

    *p++ = GIF_TAG_AD(LOCAL_TRANSFER_REGISTER_COUNT);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_BITBLTBUF(sourceAddr / GS_VRAM_BLOCKSIZE_256, sourceWidth / GS_VRAM_TBWALIGN, sourcePsm, 0, 0, 0);
    *p++ = GS_BITBLTBUF;
    *p++ = GS_SETREG_TRXPOS(x, y, 0, 0, 0);
    *p++ = GS_TRXPOS;
    *p++ = GS_SETREG_TRXREG(width, height);
    *p++ = GS_TRXREG;
    *p++ = GS_SETREG_TRXDIR(1);
    *p++ = GS_TRXDIR;

    SyncDCache(packet, (u8*)packet + sizeof(packet));
    dmaKit_send(DMA_CHANNEL_GIF, packet, LOCAL_TRANSFER_PACKET_QWORDS);
    if (dmaKit_wait(DMA_CHANNEL_GIF, LOCAL_TRANSFER_TIMEOUT_ITERATIONS) < 0) {
        GfxRecoverGifDma(false);
        return false;
    }

    SyncDCache(dst, (u8*)dst + dmaBytes);
    VIF1_STAT = VIF1_STAT_FDR_MASK;
    GS_SETREG_BUSDIR(1);
    GfxPipelineSync();

    while (qwcRemaining != 0U) {
        const u32 chunkQwc = qwcRemaining > DMA_MAX_SIZE ? DMA_MAX_SIZE : (u32)qwcRemaining;

        *DMA_REG_STAT = 1U << DMA_CHANNEL_VIF1;
        *R_EE_D1_QWC = DMA_SET_SIZE(chunkQwc);
        *R_EE_D1_MADR = DMA_SET_MADR((u32)(uintptr_t)writePtr, 0);
        *R_EE_D1_CHCR = DMA_SET_CHCR(0, 0, 0, 0, 0, 1, 0);

        if (!GfxWaitVif1Bounded()) {
            *R_EE_D1_CHCR = 0;
            VIF1_STAT = 0;
            GS_SETREG_BUSDIR(0);
            GfxPipelineSync();
            return false;
        }

        writePtr += (size_t)chunkQwc << 4;
        qwcRemaining -= chunkQwc;
    }

    GfxPipelineSync();
    VIF1_STAT = 0;
    GS_SETREG_BUSDIR(0);
    GfxPipelineSync();
    InvalidDCache(dst, (u8*)dst + dmaBytes);
    return true;
}

void gfx_ps2_depth_query_begin(void) {
    sDepthReadbackX = -1;
    sDepthReadbackY = -1;
}

bool gfx_ps2_depth_read(int32_t x, int32_t y, uint16_t* depth) {
    const int blockW = 16;
    const int blockH = 4;
    int screenX;
    int screenY;
    int baseX;
    int baseY;

    if (gs_global == NULL || depth == NULL || gs_global->PSMZ != GS_PSMZ_16 || gs_global->Width < blockW ||
        gs_global->Height < blockH) {
        return false;
    }

    {
        const float scale = (float)gs_global->Height / (float)LOGICAL_HEIGHT;
        screenX = (int)(((float)gs_global->Width * 0.5f) + ((x - (LOGICAL_WIDTH / 2)) * scale));
        screenY = (int)(y * scale);
    }
    if (screenX < 0 || screenX >= gs_global->Width || screenY < 0 || screenY >= gs_global->Height) {
        return false;
    }

    baseX = screenX - (screenX % blockW);
    baseY = screenY - (screenY % blockH);
    if (baseX > gs_global->Width - blockW)
        baseX = gs_global->Width - blockW;
    if (baseY > gs_global->Height - blockH)
        baseY = gs_global->Height - blockH;

    if (baseX != sDepthReadbackX || baseY != sDepthReadbackY) {
        if (!GfxWindow_ReadLocalRect(sDepthReadback, sizeof(sDepthReadback), gs_global->ZBuffer,
                                     DEPTH_TRANSFER_PSM(gs_global->PSMZ), gs_global->Width, baseX, baseY, blockW,
                                     blockH)) {
            sDepthReadbackX = -1;
            sDepthReadbackY = -1;
            return false;
        }
        sDepthReadbackX = baseX;
        sDepthReadbackY = baseY;
    }

    *depth = sDepthReadback[(screenY - baseY) * blockW + (screenX - baseX)];
    return true;
}

void gfx_ps2_apply_vismono(uint8_t primR, uint8_t primG, uint8_t primB, uint8_t alpha, uint8_t envR, uint8_t envG,
                           uint8_t envB) {
    size_t pixels;
    size_t bytes;
    u32 frameAddr;

    if (gs_global == NULL || alpha == 0 || gs_global->Width <= 0 || gs_global->Height <= 0 ||
        gs_global->PSM != GS_PSM_CT24 || (size_t)gs_global->Width > SIZE_MAX / (size_t)gs_global->Height) {
        return;
    }

    pixels = (size_t)gs_global->Width * (size_t)gs_global->Height;
    if (pixels > (SIZE_MAX - 63U) / sizeof(u32)) {
        return;
    }
    bytes = pixels * sizeof(u32);

    if (sVisMonoReadback == NULL || sVisMonoReadbackBytes < bytes) {
        const size_t allocBytes = (bytes + 63U) & ~63U;
        u32* newReadback = memalign(64, allocBytes);

        if (newReadback == NULL) {
            return;
        }
        free(sVisMonoReadback);
        sVisMonoReadback = newReadback;
        sVisMonoReadbackBytes = allocBytes;
    }

    if (gs_global->Os_Queue->tag_size != 0) {
        gsKit_queue_exec(gs_global);
        if (dmaKit_wait(DMA_CHANNEL_GIF, GIF_DMA_TIMEOUT_ITERATIONS) < 0) {
            GfxRecoverGifDma(false);
            return;
        }
        if (!GfxWaitFinishBounded(GS_FINISH_TIMEOUT_ITERATIONS)) {
            return;
        }
        gsKit_queue_reset(gs_global->Os_Queue);
    }

    frameAddr = gs_global->ScreenBuffer[gs_global->ActiveBuffer & 1];
    if (!GfxWindow_ReadLocalRect(sVisMonoReadback, sVisMonoReadbackBytes, frameAddr, gs_global->PSM, gs_global->Width,
                                 0, 0, gs_global->Width, gs_global->Height)) {
        return;
    }

    for (size_t i = 0; i < pixels; i++) {
        const u32 pixel = sVisMonoReadback[i];
        const unsigned int r = pixel & 0xFFU;
        const unsigned int g = (pixel >> 8) & 0xFFU;
        const unsigned int b = (pixel >> 16) & 0xFFU;
        const unsigned int intensity = ((r * 2U) + (g * 4U) + b + 3U) / 7U;
        const unsigned int tintR = envR + (((int)primR - envR) * (int)intensity + 127) / 255;
        const unsigned int tintG = envG + (((int)primG - envG) * (int)intensity + 127) / 255;
        const unsigned int tintB = envB + (((int)primB - envB) * (int)intensity + 127) / 255;
        const unsigned int outR = ((tintR * alpha) + (r * (255U - alpha)) + 127U) / 255U;
        const unsigned int outG = ((tintG * alpha) + (g * (255U - alpha)) + 127U) / 255U;
        const unsigned int outB = ((tintB * alpha) + (b * (255U - alpha)) + 127U) / 255U;

        sVisMonoReadback[i] = (pixel & 0xFF000000U) | (outB << 16) | (outG << 8) | outR;
    }
    SyncDCache(sVisMonoReadback, (u8*)sVisMonoReadback + bytes);

    gsKit_texture_send_inline(gs_global, sVisMonoReadback, gs_global->Width, gs_global->Height, frameAddr,
                              gs_global->PSM, gs_global->Width / 64U, GS_CLUT_NONE);
    gfx_ps2_invalidate_register_cache();
}

static void GfxSetBufferAttributesDisc(GSGLOBAL* g) {
    g->StartXOffset = 0;
    g->StartYOffset = 0;
    g->StartX = NTSC_START_X;
    g->StartY = NTSC_START_Y;
    g->DW = NTSC_DISPLAY_WIDTH;
    g->DH = NTSC_DISPLAY_HEIGHT;
    g->MagH = (g->DW / g->Width) - 1;
    g->MagV = (g->DH / g->Height) - 1;
    g->StartX += (g->DW - ((g->MagH + 1) * g->Width)) / 2;
    g->StartY += (g->DH - ((g->MagV + 1) * g->Height)) / 2;
    g->StartY &= ~1;
    g->DW = (g->MagH + 1) * g->Width;
    g->DH = (g->MagV + 1) * g->Height;
}

static bool GfxAllocateSystemVram(GSGLOBAL* g, u32 bytes, u32* address) {
    const u32 previousPointer = g->CurrentPointer;
    const u32 allocation = gsKit_vram_alloc(g, bytes, GSKIT_ALLOC_SYSBUFFER);

    if (bytes == 0U || g->CurrentPointer <= previousPointer) {
        return false;
    }

    *address = allocation;
    return true;
}

static bool GfxInitScreenDisc(GSGLOBAL* g) {
    u64* pData;
    u64* pStore;
    const int registerCount = 18;
    const int packetQwords = registerCount + 1;
    u32 bytes;

    GfxSetBufferAttributesDisc(g);
    GS_RESET();
    GfxPipelineSync();
    *GS_CSR = 0;
    GsPutIMR(0x00007F00);
    SetGsCrt(g->Interlace, g->Mode, g->Field);

    GS_SET_PMODE(0, 1, 0, 1, 0, 0x80);
    GS_SET_DISPFB1(0, g->Width / 64, g->PSM, 0, 0);
    GS_SET_DISPFB2(0, g->Width / 64, g->PSM, 0, 0);
    GS_SET_DISPLAY1(g->StartX, g->StartY, g->MagH, g->MagV, g->DW - 1, g->DH - 1);
    GS_SET_DISPLAY2(g->StartX, g->StartY, g->MagH, g->MagV, g->DW - 1, g->DH - 1);
    GS_SET_BGCOLOR(g->BGColor->Red, g->BGColor->Green, g->BGColor->Blue);

    g->FirstFrame = GS_SETTING_ON;
    if (g->ZBuffering == GS_SETTING_OFF) {
        g->Test->ZTE = GS_SETTING_ON;
        g->Test->ZTST = 1;
    }
    g->CurrentPointer = 0;

    bytes = gsKit_texture_size(g->Width, g->Height, g->PSM);
    if (!GfxAllocateSystemVram(g, bytes, &g->ScreenBuffer[0])) {
        return false;
    }

    if (g->DoubleBuffering == GS_SETTING_OFF) {
        g->ScreenBuffer[1] = g->ScreenBuffer[0];
    } else {
        if (!GfxAllocateSystemVram(g, bytes, &g->ScreenBuffer[1])) {
            return false;
        }
    }

    if (g->ZBuffering == GS_SETTING_ON) {
        bytes = gsKit_texture_size(g->Width, g->Height, g->PSMZ);
        if (!GfxAllocateSystemVram(g, bytes, &g->ZBuffer)) {
            return false;
        }
    }
    g->TexturePointer = g->CurrentPointer;

    pData = pStore = (u64*)g->dma_misc;
    *pData++ = GIF_TAG(registerCount, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *pData++ = GIF_AD;
    *pData++ = 1;
    *pData++ = GS_PRMODECONT;
    *pData++ = GS_SETREG_FRAME_1(g->ScreenBuffer[0] / GS_VRAM_BLOCKSIZE_8K, g->Width / GS_VRAM_TBWALIGN, g->PSM, 0);
    *pData++ = GS_FRAME_1;
    *pData++ = GS_SETREG_XYOFFSET_1(g->OffsetX, g->OffsetY);
    *pData++ = GS_XYOFFSET_1;
    *pData++ = GS_SETREG_SCISSOR_1(0, g->Width - 1, 0, g->Height - 1);
    *pData++ = GS_SCISSOR_1;
    *pData++ = GS_SETREG_TEST(g->Test->ATE, g->Test->ATST, g->Test->AREF, g->Test->AFAIL, g->Test->DATE, g->Test->DATM,
                              g->Test->ZTE, g->Test->ZTST);
    *pData++ = GS_TEST_1;
    *pData++ =
        GS_SETREG_CLAMP(g->Clamp->WMS, g->Clamp->WMT, g->Clamp->MINU, g->Clamp->MAXU, g->Clamp->MINV, g->Clamp->MAXV);
    *pData++ = GS_CLAMP_1;
    if (g->ZBuffering == GS_SETTING_ON) {
        if ((g->PSM == GS_PSM_CT16) && (g->PSMZ != GS_PSMZ_16)) {
            g->PSMZ = GS_PSMZ_16;
        }
        if ((g->PSM != GS_PSM_CT16) && (g->PSMZ == GS_PSMZ_16)) {
            g->PSMZ = GS_PSMZ_16S;
        }
        *pData++ = GS_SETREG_ZBUF_1(g->ZBuffer / GS_VRAM_BLOCKSIZE_8K, g->PSMZ, 0);
        *pData++ = GS_ZBUF_1;
    } else {
        *pData++ = GS_SETREG_ZBUF_1(0, g->PSM, 1);
        *pData++ = GS_ZBUF_1;
    }
    *pData++ = GS_SETREG_COLCLAMP(255);
    *pData++ = GS_COLCLAMP;
    *pData++ = GS_SETREG_FRAME_1(g->ScreenBuffer[1] / GS_VRAM_BLOCKSIZE_8K, g->Width / GS_VRAM_TBWALIGN, g->PSM, 0);
    *pData++ = GS_FRAME_2;
    *pData++ = GS_SETREG_XYOFFSET_1(g->OffsetX, g->OffsetY);
    *pData++ = GS_XYOFFSET_2;
    *pData++ = GS_SETREG_SCISSOR_1(0, g->Width - 1, 0, g->Height - 1);
    *pData++ = GS_SCISSOR_2;
    *pData++ = GS_SETREG_TEST(g->Test->ATE, g->Test->ATST, g->Test->AREF, g->Test->AFAIL, g->Test->DATE, g->Test->DATM,
                              g->Test->ZTE, g->Test->ZTST);
    *pData++ = GS_TEST_2;
    *pData++ =
        GS_SETREG_CLAMP(g->Clamp->WMS, g->Clamp->WMT, g->Clamp->MINU, g->Clamp->MAXU, g->Clamp->MINV, g->Clamp->MAXV);
    *pData++ = GS_CLAMP_2;
    if (g->ZBuffering == GS_SETTING_ON) {
        *pData++ = GS_SETREG_ZBUF_1(g->ZBuffer / GS_VRAM_BLOCKSIZE_8K, g->PSMZ, 0);
        *pData++ = GS_ZBUF_2;
    } else {
        *pData++ = GS_SETREG_ZBUF_1(0, g->PSM, 1);
        *pData++ = GS_ZBUF_2;
    }
    *pData++ = GS_BLEND_BACK2FRONT;
    *pData++ = GS_ALPHA_1;
    *pData++ = GS_BLEND_BACK2FRONT;
    *pData++ = GS_ALPHA_2;
    *pData++ = GS_SETREG_DIMX(g->DitherMatrix[0], g->DitherMatrix[1], g->DitherMatrix[2], g->DitherMatrix[3],
                              g->DitherMatrix[4], g->DitherMatrix[5], g->DitherMatrix[6], g->DitherMatrix[7],
                              g->DitherMatrix[8], g->DitherMatrix[9], g->DitherMatrix[10], g->DitherMatrix[11],
                              g->DitherMatrix[12], g->DitherMatrix[13], g->DitherMatrix[14], g->DitherMatrix[15]);
    *pData++ = GS_DIMX;
    *pData++ = GS_SETREG_TEXA(0x00, 0, 0x80);
    *pData++ = GS_TEXA;

    dmaKit_send_ucab(DMA_CHANNEL_GIF, pStore, packetQwords);
    if (!GfxWaitGifBounded()) {
        return false;
    }
    gsKit_clear(g, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));
    gsKit_queue_exec(g);
    if (!GfxWaitGifBounded() || !GfxWaitFinishBounded(GS_FINISH_TIMEOUT_ITERATIONS)) {
        return false;
    }

    GS_SET_DISPFB1(g->ScreenBuffer[0] / GS_VRAM_BLOCKSIZE_8K, g->Width / GS_VRAM_TBWALIGN, g->PSM, 0, 0);
    GS_SET_DISPFB2(g->ScreenBuffer[0] / GS_VRAM_BLOCKSIZE_8K, g->Width / GS_VRAM_TBWALIGN, g->PSM, 0, 0);
    GS_SET_DISPLAY1(g->StartX, g->StartY, g->MagH, g->MagV, g->DW - 1, g->DH - 1);
    GS_SET_DISPLAY2(g->StartX, g->StartY, g->MagH, g->MagV, g->DW - 1, g->DH - 1);
    GS_SET_PMODE(0, 1, 0, 1, 0, 0x80);
    g->ActiveBuffer = 0;
    sDisplayedBufferIndex = 0;
    g->PrimContext = 0;
    g->FirstFrame = GS_SETTING_OFF;
    gsKit_queue_reset(g->Os_Queue);
    return true;
}

static void GfxWindow_Init(const char* gameName, bool startInFullscreen) {
    bool initialized;

    (void)gameName;
    (void)startInFullscreen;

    gs_global = gsKit_init_global();
    if (gs_global == NULL) {
        printf("oot-ps2: failed to allocate gsKit global state\n");
        abort();
    }

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
                1U << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gs_global->Mode = GS_MODE_NTSC;
    gs_global->Width = WINDOW_WIDTH;
    gs_global->Height = WINDOW_HEIGHT;
    gs_global->Interlace = GS_INTERLACED;
    gs_global->Field = GS_FIELD;
    gs_global->ZBuffering = GS_SETTING_ON;
    gs_global->DoubleBuffering = GS_SETTING_ON;
    gs_global->PrimAAEnable = GS_SETTING_OFF;

    gs_global->PSM = GS_PSM_CT24;
    gs_global->PSMZ = GS_PSMZ_16;

    ResetEE(INIT_VU1 | INIT_VIF1 | INIT_GIF);
    GfxPipelineSync();
    sGsInitFailed = false;
    sGsInitActive = true;
    if (Platform_IsDiscBoot()) {
        initialized = GfxInitScreenDisc(gs_global);
    } else {
        gsKit_init_screen(gs_global);
        initialized = !sGsInitFailed;
    }
    sGsInitActive = false;

    if (!initialized || sGsInitFailed) {
        printf("oot-ps2: GS initialization failed\n");
        abort();
    }

    sFramePending = false;
    sDisplayedBufferIndex = 0;
    sDisplayedFrameValid = false;
    sPauseBackgroundState = PAUSE_BACKGROUND_DISABLED;
    sPauseBackgroundSourceIndex = -1;
    gsKit_TexManager_init(gs_global);
}

static bool GfxWindow_PinPauseBackgroundCurrent(void) {
    if (sPauseBackgroundState != PAUSE_BACKGROUND_CAPTURE_PENDING || !sDisplayedFrameValid || gs_global == NULL) {
        return false;
    }
    if (gs_global->DoubleBuffering == GS_SETTING_OFF || gs_global->ScreenBuffer[0] == gs_global->ScreenBuffer[1]) {
        return false;
    }

    const int srcIndex = sDisplayedBufferIndex & 1;
    if ((gs_global->ActiveBuffer & 1) == srcIndex) {
        return false;
    }

    sPauseBackgroundSourceIndex = srcIndex;
    sPauseBackgroundState = PAUSE_BACKGROUND_CAPTURED;
    return true;
}

void gfx_ps2_capture_pause_background_current(void) {
    (void)GfxWindow_PinPauseBackgroundCurrent();
}

static void GfxWindow_SetKeyboardCallbacks(bool (*isKeyDown)(int), bool (*isKeyPressed)(int), void (*allKeysUp)(void)) {
    (void)isKeyDown;
    (void)isKeyPressed;
    (void)allKeysUp;
}

static void GfxWindow_SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) {
    (void)onFullscreenChanged;
}

static void GfxWindow_SetFullscreen(bool fullscreen) {
    (void)fullscreen;
}

static void GfxWindow_MainLoop(void (*runOneGameIter)(void)) {
    for (;;) {
        runOneGameIter();
    }
}

static void GfxWindow_GetDimensions(uint32_t* width, uint32_t* height) {
    *width = WINDOW_WIDTH;
    *height = WINDOW_HEIGHT;
}

static void GfxWindow_HandleEvents(void) {
}

void gfx_ps2_request_pause_background(void) {
    if (sPauseBackgroundState == PAUSE_BACKGROUND_DISABLED) {
        return;
    }

    sPauseBackgroundState = PAUSE_BACKGROUND_CAPTURE_PENDING;
    sPauseBackgroundSourceIndex = -1;
}

void gfx_ps2_set_pause_background_active(bool active) {
    if (!active) {
        sPauseBackgroundState = PAUSE_BACKGROUND_DISABLED;
        sPauseBackgroundSourceIndex = -1;
    } else if (sPauseBackgroundState == PAUSE_BACKGROUND_DISABLED) {
        sPauseBackgroundState = PAUSE_BACKGROUND_IDLE;
    }
}

bool gfx_ps2_pause_background_active(void) {
    return sPauseBackgroundState == PAUSE_BACKGROUND_CAPTURED && sPauseBackgroundSourceIndex >= 0;
}

void gfx_ps2_restore_pause_background(void) {
    const int registerCount = 4;
    u64* p;

    if (!gfx_ps2_pause_background_active() || gs_global == NULL) {
        return;
    }

    const int srcIndex = sPauseBackgroundSourceIndex;
    const int dstIndex = gs_global->ActiveBuffer & 1;
    if (srcIndex == dstIndex) {
        printf("oot-ps2: refused in-place pause background copy from buffer %d\n", srcIndex);
        return;
    }

    const u32 src = gs_global->ScreenBuffer[srcIndex];
    const u32 dst = gs_global->ScreenBuffer[dstIndex];

    p = gsKit_heap_alloc(gs_global, registerCount, registerCount * 16, GIF_AD);
    if (p == NULL) {
        printf("oot-ps2: failed to allocate pause background GIF packet\n");
        return;
    }

    *p++ = GIF_TAG_AD(registerCount);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_BITBLTBUF(src / GS_VRAM_BLOCKSIZE_256, gs_global->Width / GS_VRAM_TBWALIGN, gs_global->PSM,
                               dst / GS_VRAM_BLOCKSIZE_256, gs_global->Width / GS_VRAM_TBWALIGN, gs_global->PSM);
    *p++ = GS_BITBLTBUF;
    *p++ = GS_SETREG_TRXPOS(0, 0, 0, 0, 0);
    *p++ = GS_TRXPOS;
    *p++ = GS_SETREG_TRXREG(gs_global->Width, gs_global->Height);
    *p++ = GS_TRXREG;
    *p++ = GS_SETREG_TRXDIR(2);
    *p++ = GS_TRXDIR;
    gfx_ps2_invalidate_register_cache();
}

static bool GfxWindow_StartFrame(void) {
    sDepthReadbackX = -1;
    sDepthReadbackY = -1;
    if (sFramePending) {
        const int completed = gs_global->ActiveBuffer & 1;
        int nextDraw = completed;

        if (!GfxWaitFinishBounded(FRAME_FINISH_TIMEOUT_ITERATIONS)) {
            printf("oot-ps2: timed out waiting for frame completion; resetting GIF DMA\n");
            GfxRecoverGifDma(false);
            gfx_ps2_invalidate_register_cache();
            sFramePending = false;
            return false;
        }

        sDisplayedBufferIndex = completed;
        sDisplayedFrameValid = true;
        GS_SET_DISPFB1(gs_global->ScreenBuffer[sDisplayedBufferIndex] / GS_VRAM_BLOCKSIZE_8K,
                       gs_global->Width / GS_VRAM_TBWALIGN, gs_global->PSM, 0, 0);
        GS_SET_DISPFB2(gs_global->ScreenBuffer[sDisplayedBufferIndex] / GS_VRAM_BLOCKSIZE_8K,
                       gs_global->Width / GS_VRAM_TBWALIGN, gs_global->PSM, 0, 0);

        if (gfx_ps2_pause_background_active()) {
            nextDraw = sPauseBackgroundSourceIndex ^ 1;
        } else if (gs_global->DoubleBuffering != GS_SETTING_OFF) {
            nextDraw = completed ^ 1;
        }

        gs_global->ActiveBuffer = (u8)nextDraw;
        gsKit_setactive(gs_global);
        sFramePending = false;
    }
    return true;
}

static void GfxWindow_SwapBuffersBegin(void) {
}

static void GfxWindow_SwapBuffersEnd(void) {
    sAsyncFrameSubmit = true;
    gsKit_queue_exec(gs_global);
    sAsyncFrameSubmit = false;
    sFramePending = true;
    gsKit_TexManager_nextFrame(gs_global);
}

static double GfxWindow_GetTime(void) {
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return seconds + (double)microseconds / 1000000.0;
}

struct GfxWindowManagerAPI gfx_ps2_wapi = {
    .init = GfxWindow_Init,
    .set_keyboard_callbacks = GfxWindow_SetKeyboardCallbacks,
    .set_fullscreen_changed_callback = GfxWindow_SetFullscreenChangedCallback,
    .set_fullscreen = GfxWindow_SetFullscreen,
    .main_loop = GfxWindow_MainLoop,
    .get_dimensions = GfxWindow_GetDimensions,
    .handle_events = GfxWindow_HandleEvents,
    .start_frame = GfxWindow_StartFrame,
    .swap_buffers_begin = GfxWindow_SwapBuffersBegin,
    .swap_buffers_end = GfxWindow_SwapBuffersEnd,
    .get_time = GfxWindow_GetTime,
};
