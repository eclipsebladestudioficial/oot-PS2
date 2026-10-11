#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include <dmaKit.h>
#include <ee_regs.h>
#include <vif_registers.h>
#include <gsKit.h>
#include <gsInline.h>
#include <gif_registers.h>
#include <kernel.h>
#include <timer.h>

#include "gfx_window_manager_api.h"
#include "../oot_ps2_platform.h"

#define OOT_PS2_DISPLAY_WIDTH 640
#define OOT_PS2_DISPLAY_HEIGHT 448

GSGLOBAL* gs_global;

static volatile bool sGsInitActive;
static volatile bool sAsyncFrameSubmit;
static bool sFramePending;
static int sDisplayedBufferIndex;
static bool sDisplayedFrameValid;
static bool sPauseBackgroundActive;
static bool sPauseBackgroundCapturePending;
static bool sPauseBackgroundCaptured;

static int sPauseBackgroundSourceIndex = -1;
static u32 sPauseBackgroundCaptureCount;
static u32 sPauseBackgroundRestoreCount;
static u32* sVisMonoReadback;
static size_t sVisMonoReadbackBytes;
static u16 sDepthReadback[64] __attribute__((aligned(64)));
static int sDepthReadbackX = -1;
static int sDepthReadbackY = -1;


extern void gfx_ps2_invalidate_register_cache(void);

void __real_SetGsCrt(s16 interlace, s16 mode, s16 field);
void __real_dmaKit_wait_fast(void);
void __real_dmaKit_send_ucab(u16 channel, void* data, u32 size);
int __real_DIntr(void);
int __real_EIntr(void);
u64 __real_GsPutIMR(u64 imr);
u32 __real_gsKit_vram_alloc(GSGLOBAL* gsGlobal, u32 size, u8 type);
u32 __real_gsKit_texture_size(int width, int height, int psm);
void __real_gsKit_TexManager_init(GSGLOBAL* gsGlobal);
void __real_gsKit_clear(GSGLOBAL* gsGlobal, u64 color);
void __real_gsKit_queue_exec(GSGLOBAL* gsGlobal);
void __real_gsKit_sync_flip(GSGLOBAL* gsGlobal);
void __real_gsKit_finish(void);
void __real_gsKit_queue_reset(GSQUEUE* queue);

int __wrap_DIntr(void) {
    if (sGsInitActive && OotPs2Platform_IsDiscBoot()) {
        return 1;
    }
    return __real_DIntr();
}

int __wrap_EIntr(void) {
    if (sGsInitActive && OotPs2Platform_IsDiscBoot()) {
        return 1;
    }
    return __real_EIntr();
}

u64 __wrap_GsPutIMR(u64 imr) {
    return __real_GsPutIMR(imr);
}

u32 __wrap_gsKit_texture_size(int width, int height, int psm) {
    return __real_gsKit_texture_size(width, height, psm);
}

u32 __wrap_gsKit_vram_alloc(GSGLOBAL* gsGlobal, u32 size, u8 type) {
    return __real_gsKit_vram_alloc(gsGlobal, size, type);
}

void __wrap_gsKit_TexManager_init(GSGLOBAL* gsGlobal) {
    __real_gsKit_TexManager_init(gsGlobal);
}

void __wrap_gsKit_clear(GSGLOBAL* gsGlobal, u64 color) {
    __real_gsKit_clear(gsGlobal, color);
}

void __wrap_gsKit_queue_exec(GSGLOBAL* gsGlobal) {
    __real_gsKit_queue_exec(gsGlobal);
}

void __wrap_gsKit_finish(void) {
    if (!sAsyncFrameSubmit) {
        __real_gsKit_finish();
    }
}

void __wrap_gsKit_sync_flip(GSGLOBAL* gsGlobal) {
    __real_gsKit_sync_flip(gsGlobal);
}

void __wrap_gsKit_queue_reset(GSQUEUE* queue) {
    __real_gsKit_queue_reset(queue);
}

void __wrap_SetGsCrt(s16 interlace, s16 mode, s16 field) {
    __real_SetGsCrt(interlace, mode, field);
}

void __wrap_dmaKit_send_ucab(u16 channel, void* data, u32 size) {
    __real_dmaKit_send_ucab(channel, data, size);
}

void __wrap_dmaKit_wait_fast(void) {
    if (!sGsInitActive) {
        __real_dmaKit_wait_fast();
        return;
    }

    if (dmaKit_wait(DMA_CHANNEL_GIF, 8000000) < 0) {
        *(volatile u32*)0x1000A000 = 0;
        *(volatile u32*)0x1000A020 = 0;
        *(volatile u32*)0x1000A030 = 0;
        GIF_REG_CTRL = GIF_SET_CTRL(1, 0);
        __asm__ volatile("sync.p; nop;");
        GIF_REG_CTRL = 0;
        dmaKit_chan_init(DMA_CHANNEL_GIF);
    }
}

void gfx_ps2_prepare_crash_screen(void) {
    *(volatile u32*)0x1000A000 = 0;
    *(volatile u32*)0x1000A010 = 0;
    *(volatile u32*)0x1000A020 = 0;
    *(volatile u32*)0x1000A030 = 0;
    __asm__ volatile("sync.p; nop;");
    GIF_REG_CTRL = GIF_SET_CTRL(1, 0);
    __asm__ volatile("sync.p; nop;");
    GIF_REG_CTRL = 0;
    GS_RESET();
    __asm__ volatile("sync.p; nop;");
    *GS_CSR = 0;
    dmaKit_chan_init(DMA_CHANNEL_GIF);
}

static int GfxPs2WaitGifBounded(void) {
    u32 timeout = 8000000;

    while (((*(volatile u32*)0x1000A000) & 0x100) != 0 && timeout-- != 0) {
        __asm__ volatile("nop");
    }
    if (((*(volatile u32*)0x1000A000) & 0x100) != 0) {
        *(volatile u32*)0x1000A000 = 0;
        *(volatile u32*)0x1000A020 = 0;
        *(volatile u32*)0x1000A030 = 0;
        GIF_REG_CTRL = GIF_SET_CTRL(1, 0);
        __asm__ volatile("sync.p; nop;");
        GIF_REG_CTRL = 0;
        dmaKit_chan_init(DMA_CHANNEL_GIF);
        return 0;
    }
    return 1;
}

static int GfxPs2WaitFinishBounded(void) {
    u32 timeout = 8000000;

    while (!(GS_CSR_FINISH) && timeout-- != 0) {
        __asm__ volatile("nop");
    }
    if (!(GS_CSR_FINISH)) {
        return 0;
    }
    return 1;
}

static int GfxPs2WaitVif1Bounded(void) {
    u32 timeout = 4000000U;

    while (((*R_EE_D1_CHCR) & 0x100U) != 0U && timeout-- != 0U) {
        __asm__ volatile("nop");
    }
    return ((*R_EE_D1_CHCR) & 0x100U) == 0U;
}

static bool GfxPs2Window_ReadLocalRect(void* dst, size_t dstBytes, u32 sourceAddr, u32 sourcePsm,
                                       u32 sourceWidth, u32 x, u32 y, u32 width, u32 height) {
    u64 packet[10] __attribute__((aligned(64)));
    u64* p = packet;
    size_t transferBytes;
    u32 qwc;

    if (gs_global == NULL || dst == NULL || width == 0 || height == 0 || sourceWidth == 0) {
        return false;
    }

    if (sourcePsm == GS_PSM_CT16 || sourcePsm == GS_PSM_CT16S) {
        transferBytes = (size_t)width * (size_t)height * 2U;
    } else if (sourcePsm == GS_PSM_CT24 || sourcePsm == GS_PSM_CT32) {
        transferBytes = (size_t)width * (size_t)height * 4U;
    } else {
        return false;
    }
    if (transferBytes > dstBytes) {
        return false;
    }
    qwc = (u32)((transferBytes + 15U) >> 4);

   if (!GfxPs2WaitFinishBounded()) {
        return false;
    }
    if (dmaKit_wait(DMA_CHANNEL_GIF, 4000000U) < 0) {
        return false;
    }

    *p++ = GIF_TAG_AD(4);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_BITBLTBUF(sourceAddr / 256U, sourceWidth / 64U, sourcePsm, 0, 0, 0);
    *p++ = GS_BITBLTBUF;
    *p++ = GS_SETREG_TRXPOS(x, y, 0, 0, 0);
    *p++ = GS_TRXPOS;
    *p++ = GS_SETREG_TRXREG(width, height);
    *p++ = GS_TRXREG;
    *p++ = GS_SETREG_TRXDIR(1);
    *p++ = GS_TRXDIR;

    SyncDCache(packet, (u8*)packet + sizeof(packet));
    dmaKit_send(DMA_CHANNEL_GIF, packet, 5);
    if (dmaKit_wait(DMA_CHANNEL_GIF, 4000000U) < 0) {
        return false;
    }

    SyncDCache(dst, (u8*)dst + transferBytes);
    *DMA_REG_STAT = 1U << DMA_CHANNEL_VIF1;
    VIF1_STAT = 1U << 23;
    GS_SETREG_BUSDIR(1);
    __asm__ volatile("sync.p; nop;");

    *R_EE_D1_QWC = DMA_SET_SIZE(qwc);
    *R_EE_D1_MADR = DMA_SET_MADR((u32)(uintptr_t)dst, 0);
    *R_EE_D1_CHCR = DMA_SET_CHCR(0, 0, 0, 0, 0, 1, 0);

    if (!GfxPs2WaitVif1Bounded()) {
        *R_EE_D1_CHCR = 0;
        VIF1_STAT = 0;
        GS_SETREG_BUSDIR(0);
        return false;
    }

    __asm__ volatile("sync.p; nop;");
    VIF1_STAT = 0;
    GS_SETREG_BUSDIR(0);
    __asm__ volatile("sync.p; nop;");
    InvalidDCache(dst, (u8*)dst + transferBytes);
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

    if (gs_global == NULL || depth == NULL || gs_global->PSMZ != GS_PSMZ_16) {
        return false;
    }

   {
        const float scale = (float)gs_global->Height / 240.0f;
        screenX = (int)(((float)gs_global->Width * 0.5f) + ((x - 160) * scale));
        screenY = (int)(y * scale);
    }
    if (screenX < 0 || screenX >= gs_global->Width || screenY < 0 || screenY >= gs_global->Height) {
        return false;
    }

    baseX = screenX - (screenX % blockW);
    baseY = screenY - (screenY % blockH);
    if (baseX > gs_global->Width - blockW) baseX = gs_global->Width - blockW;
    if (baseY > gs_global->Height - blockH) baseY = gs_global->Height - blockH;

    if (baseX != sDepthReadbackX || baseY != sDepthReadbackY) {
        if (!GfxPs2Window_ReadLocalRect(sDepthReadback, sizeof(sDepthReadback), gs_global->ZBuffer,
                                        gs_global->PSMZ, gs_global->Width, baseX, baseY, blockW, blockH)) {
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

void gfx_ps2_apply_vismono(uint8_t primR, uint8_t primG, uint8_t primB, uint8_t alpha,
                           uint8_t envR, uint8_t envG, uint8_t envB) {
    const size_t pixels = gs_global != NULL ? (size_t)gs_global->Width * (size_t)gs_global->Height : 0U;
    const size_t bytes = pixels * sizeof(u32);
    u32 frameAddr;

    if (gs_global == NULL || alpha == 0 || pixels == 0 || gs_global->PSM != GS_PSM_CT24) {
        return;
    }

    if (sVisMonoReadback == NULL || sVisMonoReadbackBytes < bytes) {
        if (sVisMonoReadback != NULL) free(sVisMonoReadback);
        const size_t allocBytes = (bytes + 63U) & ~63U;
        sVisMonoReadback = memalign(64, allocBytes);
        sVisMonoReadbackBytes = sVisMonoReadback != NULL ? allocBytes : 0U;
        if (sVisMonoReadback == NULL) return;
    }

    if (gs_global->Os_Queue->tag_size != 0) {
        gsKit_queue_exec(gs_global);
        if (dmaKit_wait(DMA_CHANNEL_GIF, 8000000U) < 0 || !GfxPs2WaitFinishBounded()) {
            return;
        }
        gsKit_queue_reset(gs_global->Os_Queue);
    }

    frameAddr = gs_global->ScreenBuffer[gs_global->ActiveBuffer & 1];
    if (!GfxPs2Window_ReadLocalRect(sVisMonoReadback, sVisMonoReadbackBytes, frameAddr, gs_global->PSM,
                                    gs_global->Width, 0, 0, gs_global->Width, gs_global->Height)) {
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

    gsKit_texture_send_inline(gs_global, sVisMonoReadback, gs_global->Width, gs_global->Height,
                              frameAddr, gs_global->PSM, gs_global->Width / 64U, GS_CLUT_NONE);
    gfx_ps2_invalidate_register_cache();
}

static void GfxPs2SetBufferAttributesDisc(GSGLOBAL* g) {
    g->StartXOffset = 0;
    g->StartYOffset = 0;
    g->StartX = 492;
    g->StartY = 34;
    g->DW = 2880;
    g->DH = 480;
    g->MagH = (g->DW / g->Width) - 1;
    g->MagV = (g->DH / g->Height) - 1;
    g->StartX += (g->DW - ((g->MagH + 1) * g->Width)) / 2;
    g->StartY += (g->DH - ((g->MagV + 1) * g->Height)) / 2;
    g->StartY &= ~1;
    g->DW = (g->MagH + 1) * g->Width;
    g->DH = (g->MagV + 1) * g->Height;
}

static int GfxPs2InitScreenDisc(GSGLOBAL* g) {
    u64* pData;
    u64* pStore;
    int size = 19;
    u32 bytes;

    GfxPs2SetBufferAttributesDisc(g);
    GS_RESET();
    __asm__ volatile("sync.p; nop;");
    *GS_CSR = 0;
    __real_GsPutIMR(0x00007F00);
    __real_SetGsCrt(g->Interlace, g->Mode, g->Field);

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

    bytes = __real_gsKit_texture_size(g->Width, g->Height, g->PSM);
    g->ScreenBuffer[0] = __real_gsKit_vram_alloc(g, bytes, GSKIT_ALLOC_SYSBUFFER);

    if (g->DoubleBuffering == GS_SETTING_OFF) {
        g->ScreenBuffer[1] = g->ScreenBuffer[0];
    } else {
        g->ScreenBuffer[1] = __real_gsKit_vram_alloc(g, bytes, GSKIT_ALLOC_SYSBUFFER);
    }

    if (g->ZBuffering == GS_SETTING_ON) {
        bytes = __real_gsKit_texture_size(g->Width, g->Height, g->PSMZ);
        g->ZBuffer = __real_gsKit_vram_alloc(g, bytes, GSKIT_ALLOC_SYSBUFFER);
    }
    g->TexturePointer = g->CurrentPointer;

    pData = pStore = (u64*)g->dma_misc;
    *pData++ = GIF_TAG(size - 1, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *pData++ = GIF_AD;
    *pData++ = 1;
    *pData++ = GS_PRMODECONT;
    *pData++ = GS_SETREG_FRAME_1(g->ScreenBuffer[0] / 8192, g->Width / 64, g->PSM, 0);
    *pData++ = GS_FRAME_1;
    *pData++ = GS_SETREG_XYOFFSET_1(g->OffsetX, g->OffsetY);
    *pData++ = GS_XYOFFSET_1;
    *pData++ = GS_SETREG_SCISSOR_1(0, g->Width - 1, 0, g->Height - 1);
    *pData++ = GS_SCISSOR_1;
    *pData++ = GS_SETREG_TEST(g->Test->ATE, g->Test->ATST, g->Test->AREF, g->Test->AFAIL,
                              g->Test->DATE, g->Test->DATM, g->Test->ZTE, g->Test->ZTST);
    *pData++ = GS_TEST_1;
    *pData++ = GS_SETREG_CLAMP(g->Clamp->WMS, g->Clamp->WMT, g->Clamp->MINU, g->Clamp->MAXU,
                               g->Clamp->MINV, g->Clamp->MAXV);
    *pData++ = GS_CLAMP_1;
    if (g->ZBuffering == GS_SETTING_ON) {
        if ((g->PSM == GS_PSM_CT16) && (g->PSMZ != GS_PSMZ_16)) {
            g->PSMZ = GS_PSMZ_16;
        }
        if ((g->PSM != GS_PSM_CT16) && (g->PSMZ == GS_PSMZ_16)) {
            g->PSMZ = GS_PSMZ_16S;
        }
        *pData++ = GS_SETREG_ZBUF_1(g->ZBuffer / 8192, g->PSMZ, 0);
        *pData++ = GS_ZBUF_1;
    } else {
        *pData++ = GS_SETREG_ZBUF_1(0, g->PSM, 1);
        *pData++ = GS_ZBUF_1;
    }
    *pData++ = GS_SETREG_COLCLAMP(255);
    *pData++ = GS_COLCLAMP;
    *pData++ = GS_SETREG_FRAME_1(g->ScreenBuffer[1] / 8192, g->Width / 64, g->PSM, 0);
    *pData++ = GS_FRAME_2;
    *pData++ = GS_SETREG_XYOFFSET_1(g->OffsetX, g->OffsetY);
    *pData++ = GS_XYOFFSET_2;
    *pData++ = GS_SETREG_SCISSOR_1(0, g->Width - 1, 0, g->Height - 1);
    *pData++ = GS_SCISSOR_2;
    *pData++ = GS_SETREG_TEST(g->Test->ATE, g->Test->ATST, g->Test->AREF, g->Test->AFAIL,
                              g->Test->DATE, g->Test->DATM, g->Test->ZTE, g->Test->ZTST);
    *pData++ = GS_TEST_2;
    *pData++ = GS_SETREG_CLAMP(g->Clamp->WMS, g->Clamp->WMT, g->Clamp->MINU, g->Clamp->MAXU,
                               g->Clamp->MINV, g->Clamp->MAXV);
    *pData++ = GS_CLAMP_2;
    if (g->ZBuffering == GS_SETTING_ON) {
        *pData++ = GS_SETREG_ZBUF_1(g->ZBuffer / 8192, g->PSMZ, 0);
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

    __real_dmaKit_send_ucab(DMA_CHANNEL_GIF, pStore, size);
    GfxPs2WaitGifBounded();
    __real_gsKit_clear(g, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));
    __real_gsKit_queue_exec(g);
    GfxPs2WaitGifBounded();
    GfxPs2WaitFinishBounded();

    GS_SET_DISPFB1(g->ScreenBuffer[0] / 8192, g->Width / 64, g->PSM, 0, 0);
    GS_SET_DISPFB2(g->ScreenBuffer[0] / 8192, g->Width / 64, g->PSM, 0, 0);
    GS_SET_DISPLAY1(g->StartX, g->StartY, g->MagH, g->MagV, g->DW - 1, g->DH - 1);
    GS_SET_DISPLAY2(g->StartX, g->StartY, g->MagH, g->MagV, g->DW - 1, g->DH - 1);
    GS_SET_PMODE(0, 1, 0, 1, 0, 0x80);
    g->ActiveBuffer = 0;
    sDisplayedBufferIndex = 0;
    g->PrimContext = 0;
    g->FirstFrame = GS_SETTING_OFF;
    __real_gsKit_queue_reset(g->Os_Queue);
    return 1;
}

static void GfxPs2Window_Init(const char* gameName, bool startInFullscreen) {
    (void)gameName;
    (void)startInFullscreen;

    gs_global = gsKit_init_global();
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
                1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gs_global->Mode = GS_MODE_NTSC;
    gs_global->Width = OOT_PS2_DISPLAY_WIDTH;
    gs_global->Height = OOT_PS2_DISPLAY_HEIGHT;
    gs_global->Interlace = GS_INTERLACED;
    gs_global->Field = GS_FIELD;
    gs_global->ZBuffering = GS_SETTING_ON;
    gs_global->DoubleBuffering = GS_SETTING_ON;
    gs_global->PrimAAEnable = GS_SETTING_OFF;

    gs_global->PSM = GS_PSM_CT24;
    gs_global->PSMZ = GS_PSMZ_16;

    ResetEE(INIT_VU1 | INIT_VIF1 | INIT_GIF);
    __asm__ volatile("sync.p; nop;");
    if (OotPs2Platform_IsDiscBoot()) {
        sGsInitActive = true;
        GfxPs2InitScreenDisc(gs_global);
        sGsInitActive = false;
    } else {
        sGsInitActive = true;
        gsKit_init_screen(gs_global);
        sGsInitActive = false;
    }
    printf("PS2PAUSEBG mode=pinned-framebuffer fb0=%08lx fb1=%08lx\n",
           (unsigned long)gs_global->ScreenBuffer[0],
           (unsigned long)gs_global->ScreenBuffer[1]);
    gsKit_TexManager_init(gs_global);
}

static bool GfxPs2Window_QueuePauseBackgroundCaptureCurrent(void) {
    if (!sPauseBackgroundActive || !sPauseBackgroundCapturePending ||
        !sDisplayedFrameValid || gs_global == NULL)
        return false;
    if (gs_global->DoubleBuffering == GS_SETTING_OFF ||
        gs_global->ScreenBuffer[0] == gs_global->ScreenBuffer[1])
        return false;
    const int srcIndex = sDisplayedBufferIndex & 1;
    if ((gs_global->ActiveBuffer & 1) == srcIndex)
        return false; 
    sPauseBackgroundSourceIndex = srcIndex;
    sPauseBackgroundCapturePending = false;
    sPauseBackgroundCaptured = true;
    ++sPauseBackgroundCaptureCount;
    printf("PS2PAUSEBG capture=%lu mode=pin source=%d draw=%d addr=%08lx\n",
           (unsigned long)sPauseBackgroundCaptureCount, srcIndex,
           (int)(gs_global->ActiveBuffer & 1),
           (unsigned long)gs_global->ScreenBuffer[srcIndex]);
    return true;
}

void gfx_ps2_capture_pause_background_current(void) {
    (void)GfxPs2Window_QueuePauseBackgroundCaptureCurrent();
}

static void GfxPs2Window_SetKeyboardCallbacks(bool (*isKeyDown)(int), bool (*isKeyPressed)(int),
                                              void (*allKeysUp)(void)) {
    (void)isKeyDown;
    (void)isKeyPressed;
    (void)allKeysUp;
}

static void GfxPs2Window_SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) {
    (void)onFullscreenChanged;
}

static void GfxPs2Window_SetFullscreen(bool fullscreen) {
    (void)fullscreen;
}

static void GfxPs2Window_MainLoop(void (*runOneGameIter)(void)) {
    for (;;) {
        runOneGameIter();
    }
}

static void GfxPs2Window_GetDimensions(uint32_t* width, uint32_t* height) {
    *width = OOT_PS2_DISPLAY_WIDTH;
    *height = OOT_PS2_DISPLAY_HEIGHT;
}

static void GfxPs2Window_HandleEvents(void) {
}

void gfx_ps2_request_pause_background(void) {
    if (!sPauseBackgroundActive) return;
    sPauseBackgroundCapturePending = true;
    sPauseBackgroundCaptured = false;
    sPauseBackgroundSourceIndex = -1;
    sPauseBackgroundRestoreCount = 0;
}

void gfx_ps2_set_pause_background_active(bool active) {
    if (!active) {
        sPauseBackgroundCapturePending = false;
        sPauseBackgroundCaptured = false;
        sPauseBackgroundSourceIndex = -1;
    }
    sPauseBackgroundActive = active;
}

bool gfx_ps2_pause_background_active(void) {
    return sPauseBackgroundActive && sPauseBackgroundCaptured &&
           sPauseBackgroundSourceIndex >= 0;
}

void gfx_ps2_restore_pause_background(void) {
    if (!gfx_ps2_pause_background_active() || gs_global == NULL) return;
    const int srcIndex = sPauseBackgroundSourceIndex;
    const int dstIndex = gs_global->ActiveBuffer & 1;
    if (srcIndex == dstIndex) {
        printf("PS2PAUSEBG ERROR refused in-place copy source=%d\n", srcIndex);
        return;
    }
    const u32 src = gs_global->ScreenBuffer[srcIndex];
    const u32 dst = gs_global->ScreenBuffer[dstIndex];
    
    u64 *p = gsKit_heap_alloc(gs_global, 4, 64, GIF_AD);
    *p++ = GIF_TAG_AD(4); *p++ = GIF_AD;
    *p++ = GS_SETREG_BITBLTBUF(src / 256U, gs_global->Width / 64U, GS_PSM_CT24,
                               dst / 256U, gs_global->Width / 64U, GS_PSM_CT24);
    *p++ = GS_BITBLTBUF;
    *p++ = GS_SETREG_TRXPOS(0, 0, 0, 0, 0); *p++ = GS_TRXPOS;
    *p++ = GS_SETREG_TRXREG(gs_global->Width, gs_global->Height); *p++ = GS_TRXREG;
    *p++ = GS_SETREG_TRXDIR(2); *p++ = GS_TRXDIR;
    ++sPauseBackgroundRestoreCount;
    if (sPauseBackgroundRestoreCount == 1) {
        printf("PS2PAUSEBG restore first_frame capture=%lu src=%08lx dst=%08lx local=1\n",
               (unsigned long)sPauseBackgroundCaptureCount,
               (unsigned long)src, (unsigned long)dst);
    }
    gfx_ps2_invalidate_register_cache();
}

static bool GfxPs2Window_StartFrame(void) {
    sDepthReadbackX = -1;
    sDepthReadbackY = -1;
    if (sFramePending) {
        u32 timeout = 12000000U;
        const int completed = gs_global->ActiveBuffer & 1;
        int nextDraw = completed;

        while (!(GS_CSR_FINISH) && timeout-- != 0) { __asm__ volatile("nop"); }

        if (!(GS_CSR_FINISH)) return false;

        sDisplayedBufferIndex = completed;
        sDisplayedFrameValid = true;
        GS_SET_DISPFB1(gs_global->ScreenBuffer[sDisplayedBufferIndex] / 8192,
                       gs_global->Width / 64, gs_global->PSM, 0, 0);
        GS_SET_DISPFB2(gs_global->ScreenBuffer[sDisplayedBufferIndex] / 8192,
                       gs_global->Width / 64, gs_global->PSM, 0, 0);

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

static void GfxPs2Window_SwapBuffersBegin(void) {
}

static void GfxPs2Window_SwapBuffersEnd(void) {
    sAsyncFrameSubmit = true;
    gsKit_queue_exec(gs_global);
    sAsyncFrameSubmit = false;
    sFramePending = true;
    gsKit_TexManager_nextFrame(gs_global);
}

static double GfxPs2Window_GetTime(void) {
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return seconds + (double)microseconds / 1000000.0;
}

struct GfxWindowManagerAPI gfx_ps2_wapi = {
    GfxPs2Window_Init,
    GfxPs2Window_SetKeyboardCallbacks,
    GfxPs2Window_SetFullscreenChangedCallback,
    GfxPs2Window_SetFullscreen,
    GfxPs2Window_MainLoop,
    GfxPs2Window_GetDimensions,
    GfxPs2Window_HandleEvents,
    GfxPs2Window_StartFrame,
    GfxPs2Window_SwapBuffersBegin,
    GfxPs2Window_SwapBuffersEnd,
    GfxPs2Window_GetTime,
};
