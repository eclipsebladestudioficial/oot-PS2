#include "mixer.h"

#include "attributes.h"
#include "audio.h"
#include "audio/commands.h"

#include <string.h>
#include <stddef.h>

typedef struct {
    const s16* input;
    s16* dryLeft;
    s16* dryRight;
    s16* wetLeft;
    s16* wetRight;
    u16 volumeLeft;
    u16 volumeRight;
    u16 reverb;
    u16 swapWetChannels;
} AudioEnvBlock;

void Audio_MixSamples(const s16* input, s16* output, s16 gain, s32 sampleCount);
void Audio_MixEnvelopeBlock8(const AudioEnvBlock* block);

#define OOT_PORT_DMEM_SIZE 0x2000
#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~15)
#define OOT_PORT_FILTER_TAP_COUNT 8
#define OOT_PORT_SAMPLE_CACHE_SLOTS 16
#define OOT_PORT_SAMPLE_CACHE_SLOT_SIZE OOT_PORT_AUDIO_SAMPLE_CACHE_PAGE_SIZE
#define OOT_PORT_BOOK_CACHE_SLOTS 8
#define OOT_PORT_BOOK_CACHE_MAX_SIZE (8 * 2 * 8 * sizeof(s16))
#define OOT_PORT_REVERB_CACHE_SLOTS 4
#define OOT_PORT_REVERB_CACHE_MAX_SAMPLES 0x2000

typedef struct {
    const void* source;
    u16 size;
    u16 age;
    u8 data[OOT_PORT_SAMPLE_CACHE_SLOT_SIZE];
} OotPortMixerSampleCacheEntry;

typedef struct {
    OotPortMixerSampleCacheEntry entries[OOT_PORT_SAMPLE_CACHE_SLOTS];
    u16 clock;
} OotPortMixerSampleCache;

typedef struct {
    const s16* source;
    u16 size;
    u16 age;
    s16 data[OOT_PORT_BOOK_CACHE_MAX_SIZE / sizeof(s16)];
} OotPortMixerBookCacheEntry;

typedef struct {
    OotPortMixerBookCacheEntry entries[OOT_PORT_BOOK_CACHE_SLOTS];
    u16 clock;
} OotPortMixerBookCache;

typedef struct {
    const s16* mainLeft;
    const s16* mainRight;
    u32 epoch;
    u16 samplesPerChan;
    s16 left[OOT_PORT_REVERB_CACHE_MAX_SAMPLES];
    s16 right[OOT_PORT_REVERB_CACHE_MAX_SAMPLES];
} OotPortMixerReverbCacheEntry;

typedef struct {
    OotPortMixerReverbCacheEntry entries[OOT_PORT_REVERB_CACHE_SLOTS];
} OotPortMixerReverbCache;

typedef struct {
    u16 in;
    u16 out;
    u16 nbytes;
    ADPCM_STATE* adpcmLoopState;
    s16 adpcmTable[8][2][8];
    s16 filterScratch[OOT_PORT_DMEM_SIZE / sizeof(s16)];
    s32 filter2Count;
    s16* filter2Lut;
    s32 filter2Valid;
    struct {
        s32 initialReverb;
        s32 rampReverb;
        s32 rampLeft;
        s32 rampRight;
        s32 volLeft;
        s32 volRight;
    } env;
    union {
        s16 s16[OOT_PORT_DMEM_SIZE / sizeof(s16)];
        u8 u8[OOT_PORT_DMEM_SIZE];
    } dmem;
    OotPortMixerSampleCache* sampleCache;
    OotPortMixerBookCache* bookCache;
    OotPortMixerReverbCache* reverbCache;
} OotPortMixerState;

static OotPortMixerState sCpuMixer __attribute__((aligned(64)));

#define OOT_PORT_MIXER_STATE() OotPortMixerState* mixer = &sCpuMixer
#define sMixer (*mixer)
#define DMEM_U8(addr) (&sMixer.dmem.u8[(u16)(addr)])
#define DMEM_S16(addr) (&sMixer.dmem.s16[(u16)(addr) / sizeof(s16)])

static s16 sResampleTable[64][4] = {
    { 0x0C39, 0x66AD, 0x0D46, 0xFFDF }, { 0x0B39, 0x6696, 0x0E5F, 0xFFD8 },
    { 0x0A44, 0x6669, 0x0F83, 0xFFD0 }, { 0x095A, 0x6626, 0x10B4, 0xFFC8 },
    { 0x087D, 0x65CD, 0x11F0, 0xFFBF }, { 0x07AB, 0x655E, 0x1338, 0xFFB6 },
    { 0x06E4, 0x64D9, 0x148C, 0xFFAC }, { 0x0628, 0x643F, 0x15EB, 0xFFA1 },
    { 0x0577, 0x638F, 0x1756, 0xFF96 }, { 0x04D1, 0x62CB, 0x18CB, 0xFF8A },
    { 0x0435, 0x61F3, 0x1A4C, 0xFF7E }, { 0x03A4, 0x6106, 0x1BD7, 0xFF71 },
    { 0x031C, 0x6007, 0x1D6C, 0xFF64 }, { 0x029F, 0x5EF5, 0x1F0B, 0xFF56 },
    { 0x022A, 0x5DD0, 0x20B3, 0xFF48 }, { 0x01BE, 0x5C9A, 0x2264, 0xFF3A },
    { 0x015B, 0x5B53, 0x241E, 0xFF2C }, { 0x0101, 0x59FC, 0x25E0, 0xFF1E },
    { 0x00AE, 0x5896, 0x27A9, 0xFF10 }, { 0x0063, 0x5720, 0x297A, 0xFF02 },
    { 0x001F, 0x559D, 0x2B50, 0xFEF4 }, { 0xFFE2, 0x540D, 0x2D2C, 0xFEE8 },
    { 0xFFAC, 0x5270, 0x2F0D, 0xFEDB }, { 0xFF7C, 0x50C7, 0x30F3, 0xFED0 },
    { 0xFF53, 0x4F14, 0x32DC, 0xFEC6 }, { 0xFF2E, 0x4D57, 0x34C8, 0xFEBD },
    { 0xFF0F, 0x4B91, 0x36B6, 0xFEB6 }, { 0xFEF5, 0x49C2, 0x38A5, 0xFEB0 },
    { 0xFEDF, 0x47ED, 0x3A95, 0xFEAC }, { 0xFECE, 0x4611, 0x3C85, 0xFEAB },
    { 0xFEC0, 0x4430, 0x3E74, 0xFEAC }, { 0xFEB6, 0x424A, 0x4060, 0xFEAF },
    { 0xFEAF, 0x4060, 0x424A, 0xFEB6 }, { 0xFEAC, 0x3E74, 0x4430, 0xFEC0 },
    { 0xFEAB, 0x3C85, 0x4611, 0xFECE }, { 0xFEAC, 0x3A95, 0x47ED, 0xFEDF },
    { 0xFEB0, 0x38A5, 0x49C2, 0xFEF5 }, { 0xFEB6, 0x36B6, 0x4B91, 0xFF0F },
    { 0xFEBD, 0x34C8, 0x4D57, 0xFF2E }, { 0xFEC6, 0x32DC, 0x4F14, 0xFF53 },
    { 0xFED0, 0x30F3, 0x50C7, 0xFF7C }, { 0xFEDB, 0x2F0D, 0x5270, 0xFFAC },
    { 0xFEE8, 0x2D2C, 0x540D, 0xFFE2 }, { 0xFEF4, 0x2B50, 0x559D, 0x001F },
    { 0xFF02, 0x297A, 0x5720, 0x0063 }, { 0xFF10, 0x27A9, 0x5896, 0x00AE },
    { 0xFF1E, 0x25E0, 0x59FC, 0x0101 }, { 0xFF2C, 0x241E, 0x5B53, 0x015B },
    { 0xFF3A, 0x2264, 0x5C9A, 0x01BE }, { 0xFF48, 0x20B3, 0x5DD0, 0x022A },
    { 0xFF56, 0x1F0B, 0x5EF5, 0x029F }, { 0xFF64, 0x1D6C, 0x6007, 0x031C },
    { 0xFF71, 0x1BD7, 0x6106, 0x03A4 }, { 0xFF7E, 0x1A4C, 0x61F3, 0x0435 },
    { 0xFF8A, 0x18CB, 0x62CB, 0x04D1 }, { 0xFF96, 0x1756, 0x638F, 0x0577 },
    { 0xFFA1, 0x15EB, 0x643F, 0x0628 }, { 0xFFAC, 0x148C, 0x64D9, 0x06E4 },
    { 0xFFB6, 0x1338, 0x655E, 0x07AB }, { 0xFFBF, 0x11F0, 0x65CD, 0x087D },
    { 0xFFC8, 0x10B4, 0x6626, 0x095A }, { 0xFFD0, 0x0F83, 0x6669, 0x0A44 },
    { 0xFFD8, 0x0E5F, 0x6696, 0x0B39 }, { 0xFFDF, 0x0D46, 0x66AD, 0x0C39 },
};

static s16 OotPortMixer_Clamp16(s32 value) {
    if (value < -0x8000) {
        return -0x8000;
    }
    if (value > 0x7FFF) {
        return 0x7FFF;
    }
    return value;
}

static s16 OotPortMixer_Vadd(s16 left, s16 right) {
    return OotPortMixer_Clamp16((s32)left + right);
}

static inline s32 OotPortMixer_ResampleProduct(s16 sample, s16 coefficient) {
    return (((s32)sample * coefficient) + 0x4000) >> 15;
}

static inline s16 OotPortMixer_ResampleSample(const s16* in, const s16* table) {
    s32 product01 = OotPortMixer_ResampleProduct(in[0], table[0]) +
                    OotPortMixer_ResampleProduct(in[1], table[1]);
    s32 product23 = OotPortMixer_ResampleProduct(in[2], table[2]) +
                    OotPortMixer_ResampleProduct(in[3], table[3]);

    return OotPortMixer_Clamp16(product01 + product23);
}

static s32 OotPortMixer_SignExtendShift2(u8 value, s32 shift) {
    return (s32)((u32)(value & 3) << 30) >> (30 - shift);
}

static s32 OotPortMixer_SignExtendShift4(u8 value, s32 shift) {
    return (s32)((u32)(value & 0xF) << 28) >> (28 - shift);
}

static inline s32 Mixer_AdpcmMultiplyAdd(s32 accumulator, s32 left, s32 right) {
    __asm__ volatile("madd %[left], %[right]"
                     : [accumulator] "+l"(accumulator)
                     : [left] "r"(left), [right] "r"(right)
                     : "hi");
    return accumulator;
}

static void Mixer_DecodeAdpcmHalf(s16** outPtr, const s16 table[2][8], const s32* ins) {
    s16* out = *outPtr;
    s32 prev1 = out[-1];
    s32 prev2 = out[-2];
    const s16* table0 = table[0];
    const s16* table1 = table[1];
    s32 acc;

    acc = ins[0] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[0], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], prev1);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    acc = ins[1] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[1], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[1], prev1);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], ins[0]);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    acc = ins[2] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[2], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[2], prev1);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[1], ins[0]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], ins[1]);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    acc = ins[3] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[3], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[3], prev1);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[2], ins[0]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[1], ins[1]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], ins[2]);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    acc = ins[4] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[4], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[4], prev1);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[3], ins[0]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[2], ins[1]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[1], ins[2]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], ins[3]);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    acc = ins[5] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[5], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[5], prev1);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[4], ins[0]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[3], ins[1]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[2], ins[2]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[1], ins[3]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], ins[4]);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    acc = ins[6] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[6], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[6], prev1);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[5], ins[0]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[4], ins[1]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[3], ins[2]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[2], ins[3]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[1], ins[4]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], ins[5]);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    acc = ins[7] << 11;
    acc = Mixer_AdpcmMultiplyAdd(acc, table0[7], prev2);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[7], prev1);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[6], ins[0]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[5], ins[1]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[4], ins[2]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[3], ins[3]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[2], ins[4]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[1], ins[5]);
    acc = Mixer_AdpcmMultiplyAdd(acc, table1[0], ins[6]);
    *out++ = OotPortMixer_Clamp16(acc >> 11);

    *outPtr = out;
}

static void OotPortMixer_DecodeAdpcmHalf(s16** outPtr, const s16 table[2][8], const s32* ins) {
    Mixer_DecodeAdpcmHalf(outPtr, table, ins);
}

void OotPortMixer_ClearBuffer(u16 dmem, s32 nbytes) {
    OOT_PORT_MIXER_STATE();
    memset(DMEM_U8(dmem), 0, ROUND_UP_16(nbytes));
}

void OotPortMixer_LoadBuffer(const void* source, u16 dmemDest, u16 nbytes) {
    OOT_PORT_MIXER_STATE();
    memcpy(DMEM_U8(dmemDest), source, ROUND_DOWN_16(nbytes));
}

static u16 OotPortMixer_NextCacheAge(u16* clock, void* entries, size_t entrySize, s32 entryCount,
                                    size_t ageOffset) {
    u16 age = ++*clock;
    s32 i;

    if (age != 0) {
        return age;
    }

    age = *clock = 1;
    for (i = 0; i < entryCount; i++) {
        *(u16*)((u8*)entries + (i * entrySize) + ageOffset) = 0;
    }
    return age;
}

static OotPortMixerSampleCacheEntry* OotPortMixer_GetSampleCachePage(OotPortMixerState* mixer,
                                                                   const u8* pageSource) {
    OotPortMixerSampleCache* cache = mixer->sampleCache;
    OotPortMixerSampleCacheEntry* victim = NULL;
    s32 i;

    if (cache == NULL) {
        return NULL;
    }

    for (i = 0; i < OOT_PORT_SAMPLE_CACHE_SLOTS; i++) {
        OotPortMixerSampleCacheEntry* entry = &cache->entries[i];

        if (entry->source == pageSource) {
            entry->age = OotPortMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                                  OOT_PORT_SAMPLE_CACHE_SLOTS,
                                                  offsetof(OotPortMixerSampleCacheEntry, age));
            return entry;
        }
        if ((victim == NULL) || (entry->source == NULL) || (entry->age < victim->age)) {
            victim = entry;
            if (entry->source == NULL) {
                break;
            }
        }
    }

    memcpy(victim->data, pageSource, OOT_PORT_SAMPLE_CACHE_SLOT_SIZE);
    victim->source = pageSource;
    victim->size = OOT_PORT_SAMPLE_CACHE_SLOT_SIZE;
    victim->age = OotPortMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                           OOT_PORT_SAMPLE_CACHE_SLOTS,
                                           offsetof(OotPortMixerSampleCacheEntry, age));
    return victim;
}

static void OotPortMixer_LoadSampleCached(const void* source, u16 dmemDest, u16 nbytes) {
    OOT_PORT_MIXER_STATE();
    const u8* src = source;
    u8* dst = DMEM_U8(dmemDest);
    u16 remaining = ROUND_DOWN_16(nbytes);

    if (sMixer.sampleCache == NULL) {
        memcpy(dst, src, remaining);
        return;
    }

    while (remaining != 0) {
        const u8* pageSource = (const u8*)((uintptr_t)src & ~(OOT_PORT_SAMPLE_CACHE_SLOT_SIZE - 1));
        u16 pageOffset = src - pageSource;
        u16 todo = OOT_PORT_SAMPLE_CACHE_SLOT_SIZE - pageOffset;
        OotPortMixerSampleCacheEntry* entry;

        if (todo > remaining) {
            todo = remaining;
        }
        entry = OotPortMixer_GetSampleCachePage(mixer, pageSource);
        if (entry == NULL) {
            memcpy(dst, src, todo);
        } else {
            memcpy(dst, entry->data + pageOffset, todo);
        }
        src += todo;
        dst += todo;
        remaining -= todo;
    }
}

void OotPortMixer_SaveBuffer(u16 dmemSrc, void* dest, u16 nbytes) {
    OOT_PORT_MIXER_STATE();
    memcpy(dest, DMEM_U8(dmemSrc), ROUND_DOWN_16(nbytes));
}

static OotPortMixerReverbCacheEntry* OotPortMixer_GetReverbCache(
    OotPortMixerState* mixer, const OotPortAudioReverbDownsampleCmd* desc) {
    OotPortMixerReverbCache* cache = mixer->reverbCache;
    OotPortMixerReverbCacheEntry* victim = NULL;
    s32 i;

    if ((cache == NULL) || (desc == NULL) || (desc->leftRingBuf == NULL) || (desc->rightRingBuf == NULL) ||
        (desc->bufSizePerChan == 0) || (desc->bufSizePerChan > OOT_PORT_REVERB_CACHE_MAX_SAMPLES)) {
        return NULL;
    }

    for (i = 0; i < OOT_PORT_REVERB_CACHE_SLOTS; i++) {
        OotPortMixerReverbCacheEntry* entry = &cache->entries[i];

        if ((entry->epoch == desc->cacheEpoch) && (entry->mainLeft == desc->leftRingBuf) &&
            (entry->mainRight == desc->rightRingBuf) && (entry->samplesPerChan == desc->bufSizePerChan)) {
            return entry;
        }
        if ((victim == NULL) && (entry->epoch != desc->cacheEpoch)) {
            victim = entry;
        }
    }

    if (victim == NULL) {
        uintptr_t key = ((uintptr_t)desc->leftRingBuf >> 6) ^ ((uintptr_t)desc->rightRingBuf >> 10);

        victim = &cache->entries[key & (OOT_PORT_REVERB_CACHE_SLOTS - 1)];
    }

    memcpy(victim->left, desc->leftRingBuf, desc->bufSizePerChan * sizeof(s16));
    memcpy(victim->right, desc->rightRingBuf, desc->bufSizePerChan * sizeof(s16));
    victim->mainLeft = desc->leftRingBuf;
    victim->mainRight = desc->rightRingBuf;
    victim->samplesPerChan = desc->bufSizePerChan;
    victim->epoch = desc->cacheEpoch;
    return victim;
}

static void OotPortMixer_ReverbDownsample(u16 dmemLeft, const OotPortAudioReverbDownsampleCmd* desc) {
    OOT_PORT_MIXER_STATE();
    OotPortMixerReverbCacheEntry* cacheEntry = OotPortMixer_GetReverbCache(mixer, desc);
    const s16* left = DMEM_S16(dmemLeft);
    const s16* right = DMEM_S16(dmemLeft + DMEM_1CH_SIZE);
    s16* leftRing = (cacheEntry != NULL) ? cacheEntry->left : desc->leftRingBuf;
    s16* rightRing = (cacheEntry != NULL) ? cacheEntry->right : desc->rightRingBuf;
    s32 startPos = desc->startPos;
    s32 step = desc->downsampleRate;
    s32 lengthASamples = desc->lengthA / (s32)sizeof(s16);
    s32 lengthBSamples = desc->lengthB / (s32)sizeof(s16);
    s32 i;
    s32 j;

    for (j = 0, i = 0; i < lengthASamples; i++, j += step) {
        leftRing[startPos + i] = left[j];
        rightRing[startPos + i] = right[j];
    }

    for (i = 0; i < lengthBSamples; i++, j += step) {
        leftRing[i] = left[j];
        rightRing[i] = right[j];
    }
}

static void OotPortMixer_ReverbSave(u16 dmemLeft, const OotPortAudioReverbDownsampleCmd* desc) {
    OOT_PORT_MIXER_STATE();
    OotPortMixerReverbCacheEntry* cacheEntry = OotPortMixer_GetReverbCache(mixer, desc);
    const s16* left = DMEM_S16(dmemLeft);
    const s16* right = DMEM_S16(dmemLeft + DMEM_1CH_SIZE);
    s16* leftRing = (cacheEntry != NULL) ? cacheEntry->left : desc->leftRingBuf;
    s16* rightRing = (cacheEntry != NULL) ? cacheEntry->right : desc->rightRingBuf;
    s32 startPos = desc->startPos;
    s32 lengthA = desc->lengthA;
    s32 lengthB = desc->lengthB;

    memcpy(&leftRing[startPos], left, lengthA);
    memcpy(&rightRing[startPos], right, lengthA);

    if (lengthB != 0) {
        left += lengthA / (s32)sizeof(s16);
        right += lengthA / (s32)sizeof(s16);
        memcpy(leftRing, left, lengthB);
        memcpy(rightRing, right, lengthB);
    }
}

static void OotPortMixer_ReverbLoad(u16 dmemLeft, const OotPortAudioReverbDownsampleCmd* desc) {
    OOT_PORT_MIXER_STATE();
    OotPortMixerReverbCacheEntry* cacheEntry = OotPortMixer_GetReverbCache(mixer, desc);
    s16* left = DMEM_S16(dmemLeft);
    s16* right = DMEM_S16(dmemLeft + DMEM_1CH_SIZE);
    const s16* leftRing = (cacheEntry != NULL) ? cacheEntry->left : desc->leftRingBuf;
    const s16* rightRing = (cacheEntry != NULL) ? cacheEntry->right : desc->rightRingBuf;
    s32 startPos = desc->startPos;
    s32 lengthA = desc->lengthA;
    s32 lengthB = desc->lengthB;

    memcpy(left, &leftRing[startPos], lengthA);
    memcpy(right, &rightRing[startPos], lengthA);

    if (lengthB != 0) {
        left += lengthA / (s32)sizeof(s16);
        right += lengthA / (s32)sizeof(s16);
        memcpy(left, leftRing, lengthB);
        memcpy(right, rightRing, lengthB);
    }
}

void OotPortMixer_LoadADPCM(s32 numEntriesBytes, const s16* book) {
    OOT_PORT_MIXER_STATE();
    if ((book != NULL) && (numEntriesBytes > 0) && (numEntriesBytes <= (s32)sizeof(sMixer.adpcmTable))) {
        memcpy(sMixer.adpcmTable, book, numEntriesBytes);
    }
}

static void OotPortMixer_LoadADPCMCached(s32 numEntriesBytes, const s16* book) {
    OOT_PORT_MIXER_STATE();
    OotPortMixerBookCache* cache = sMixer.bookCache;
    OotPortMixerBookCacheEntry* victim = NULL;
    s32 i;

    if ((book == NULL) || (numEntriesBytes <= 0) || (numEntriesBytes > (s32)sizeof(sMixer.adpcmTable))) {
        return;
    }
    if (cache == NULL) {
        memcpy(sMixer.adpcmTable, book, numEntriesBytes);
        return;
    }

    for (i = 0; i < OOT_PORT_BOOK_CACHE_SLOTS; i++) {
        OotPortMixerBookCacheEntry* entry = &cache->entries[i];

        if ((entry->source == book) && (entry->size == numEntriesBytes)) {
            entry->age = OotPortMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                                  OOT_PORT_BOOK_CACHE_SLOTS,
                                                  offsetof(OotPortMixerBookCacheEntry, age));
            memcpy(sMixer.adpcmTable, entry->data, numEntriesBytes);
            return;
        }
        if ((victim == NULL) || (entry->source == NULL) || (entry->age < victim->age)) {
            victim = entry;
            if (entry->source == NULL) {
                break;
            }
        }
    }

    memcpy(victim->data, book, numEntriesBytes);
    victim->source = book;
    victim->size = numEntriesBytes;
    victim->age = OotPortMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                           OOT_PORT_BOOK_CACHE_SLOTS,
                                           offsetof(OotPortMixerBookCacheEntry, age));
    memcpy(sMixer.adpcmTable, victim->data, numEntriesBytes);
}

void OotPortMixer_SetBuffer(UNUSED u8 flags, u16 dmemIn, u16 dmemOut, u16 nbytes) {
    OOT_PORT_MIXER_STATE();
    sMixer.in = dmemIn;
    sMixer.out = dmemOut;
    sMixer.nbytes = nbytes;
}

void OotPortMixer_Interleave(u16 dmemOut, u16 dmemLeft, u16 dmemRight, u16 count) {
    OOT_PORT_MIXER_STATE();
    s16* out = DMEM_S16(dmemOut);
    s16* left = DMEM_S16(dmemLeft);
    s16* right = DMEM_S16(dmemRight);
    s32 frames = ROUND_DOWN_16(count) / 2;
    s32 i;

    for (i = 0; i < frames; i++) {
        out[(i * 2) + 0] = left[i];
        out[(i * 2) + 1] = right[i];
    }
}

void OotPortMixer_Interl(u16 dmemIn, u16 dmemOut, u16 count) {
    OOT_PORT_MIXER_STATE();
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 samples = ROUND_UP_8(count);
    s32 i;

    for (i = 0; i < samples; i++) {
        out[i] = in[i * 2];
    }
}

void OotPortMixer_DMEMMove(u16 dmemIn, u16 dmemOut, s32 nbytes) {
    OOT_PORT_MIXER_STATE();
    u8 block[16];
    s32 count = ROUND_UP_16(nbytes);
    s32 offset;

    for (offset = 0; offset < count; offset += sizeof(block)) {
        memcpy(block, DMEM_U8(dmemIn + offset), sizeof(block));
        memcpy(DMEM_U8(dmemOut + offset), block, sizeof(block));
    }
}

void OotPortMixer_SetLoop(ADPCM_STATE* state) {
    OOT_PORT_MIXER_STATE();
    sMixer.adpcmLoopState = state;
}

void OotPortMixer_ADPCMdec(u8 flags, ADPCM_STATE state) {
    OOT_PORT_MIXER_STATE();
    u8* in = DMEM_U8(sMixer.in);
    s16* out = DMEM_S16(sMixer.out);
    s32 nbytes = ROUND_UP_32(sMixer.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if ((flags & A_LOOP) && (sMixer.adpcmLoopState != NULL)) {
        memcpy(out, sMixer.adpcmLoopState, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }
    out += 16;

    while (nbytes > 0) {
        s32 shift = *in >> 4;
        s32 tableIndex = *in++ & 0xF;
        s16(*tbl)[8] = sMixer.adpcmTable[tableIndex & 7];
        s32 half;

        if (flags & 4) {
            if (shift > 14) {
                shift = 14;
            }
        } else if (shift > 12) {
            shift = 12;
        }

        for (half = 0; half < 2; half++) {
            s32 ins[8];
            s32 j;

            if (flags & 4) {
                for (j = 0; j < 2; j++) {
                    u8 packed = *in++;

                    ins[(j * 4) + 0] = OotPortMixer_SignExtendShift2(packed >> 6, shift);
                    ins[(j * 4) + 1] = OotPortMixer_SignExtendShift2(packed >> 4, shift);
                    ins[(j * 4) + 2] = OotPortMixer_SignExtendShift2(packed >> 2, shift);
                    ins[(j * 4) + 3] = OotPortMixer_SignExtendShift2(packed, shift);
                }
            } else {
                for (j = 0; j < 4; j++) {
                    u8 packed = *in++;

                    ins[j * 2] = OotPortMixer_SignExtendShift4(packed >> 4, shift);
                    ins[(j * 2) + 1] = OotPortMixer_SignExtendShift4(packed, shift);
                }
            }

            OotPortMixer_DecodeAdpcmHalf(&out, tbl, ins);
        }

        nbytes -= 16 * sizeof(s16);
    }

    memcpy(state, out - 16, 16 * sizeof(s16));
}

void OotPortMixer_S8Dec(u8 flags, ADPCM_STATE state) {
    OOT_PORT_MIXER_STATE();
    s8* in = (s8*)DMEM_U8(sMixer.in);
    s16* out = DMEM_S16(sMixer.out);
    s32 samples = ROUND_UP_32(sMixer.nbytes) / sizeof(s16);
    s32 i;

    if (flags & A_INIT) {
        memset(out, 0, sizeof(ADPCM_STATE));
    } else if ((flags & A_LOOP) && (sMixer.adpcmLoopState != NULL)) {
        memcpy(out, sMixer.adpcmLoopState, sizeof(ADPCM_STATE));
    } else {
        memcpy(out, state, sizeof(ADPCM_STATE));
    }
    out += 16;

    for (i = 0; i < samples; i++) {
        out[i] = in[i] << 8;
    }

    memcpy(state, out + samples - 16, sizeof(ADPCM_STATE));
}

void OotPortMixer_Resample(u8 flags, u16 pitch, RESAMPLE_STATE state) {
    OOT_PORT_MIXER_STATE();
    s16 tmp[16];
    s16* inInitial = DMEM_S16(sMixer.in);
    s16* in = inInitial;
    s16* out = DMEM_S16(sMixer.out);
    s32 nbytes = ROUND_UP_16(sMixer.nbytes);
    u32 pitchAccumulator;
    s32 i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(s16));
    } else {
        memcpy(tmp, state, 16 * sizeof(s16));
    }

    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(s16));
        in -= tmp[5] / sizeof(s16);
    }

    in -= 4;
    pitchAccumulator = (u16)tmp[4];
    memcpy(in, tmp, 4 * sizeof(s16));

    while (nbytes > 0) {
        for (i = 0; i < 8; i++) {
            s16* tbl = sResampleTable[(pitchAccumulator * 64) >> 16];
            s16 sample = OotPortMixer_ResampleSample(in, tbl);

            *out++ = sample;
            pitchAccumulator += pitch << 1;
            in += pitchAccumulator >> 16;
            pitchAccumulator &= 0xFFFF;
        }
        nbytes -= 8 * sizeof(s16);
    }

    state[4] = (s16)pitchAccumulator;
    memcpy(state, in, 4 * sizeof(s16));
    i = (in - inInitial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(s16));
}

void OotPortMixer_ResampleZoh(u16 pitch, u16 pitchAccu) {
    OOT_PORT_MIXER_STATE();
    s16* out = DMEM_S16(sMixer.out);
    s32 samples = ROUND_UP_8(sMixer.nbytes) / sizeof(s16);
    u32 accumulator = ((u32)sMixer.in << 16) | pitchAccu;
    u32 step = (u32)pitch << 2;
    s32 i;

    for (i = 0; i < samples; i++) {
        out[i] = *DMEM_S16((accumulator >> 16) & 0xFFFE);
        accumulator += step;
    }
}

void OotPortMixer_EnvSetup1(s32 initialReverb, s32 rampReverb, s32 rampLeft, s32 rampRight) {
    OOT_PORT_MIXER_STATE();
    sMixer.env.initialReverb = initialReverb;
    sMixer.env.rampReverb = rampReverb;
    sMixer.env.rampLeft = rampLeft;
    sMixer.env.rampRight = rampRight;
}

void OotPortMixer_EnvSetup2(s32 volLeft, s32 volRight) {
    OOT_PORT_MIXER_STATE();
    sMixer.env.volLeft = volLeft;
    sMixer.env.volRight = volRight;
}

static s16 OotPortMixer_MulHighSignedUnsigned(s16 sample, u16 volume) {
    return (s16)(((s32)sample * volume) >> 16);
}

static void OotPortMixer_EnvMixerProducts(s16 sample, u16 volLeft, u16 volRight, u16 reverb, s16 dryLeftMask,
                                         s16 dryRightMask, s16 wetLeftMask, s16 wetRightMask, s32 applyMasks,
                                         s16* leftOut, s16* rightOut, s16* wetLeftOut, s16* wetRightOut) {
    s16 left = OotPortMixer_MulHighSignedUnsigned(sample, volLeft);
    s16 right = OotPortMixer_MulHighSignedUnsigned(sample, volRight);
    s16 wetLeft;
    s16 wetRight;

    if (__builtin_expect(applyMasks != 0, 0)) {
        left ^= dryLeftMask;
        right ^= dryRightMask;
        wetLeft = OotPortMixer_MulHighSignedUnsigned(left, reverb);
        wetRight = OotPortMixer_MulHighSignedUnsigned(right, reverb);
        wetLeft ^= wetLeftMask;
        wetRight ^= wetRightMask;
    } else {

        wetLeft = OotPortMixer_MulHighSignedUnsigned(left, reverb);
        wetRight = OotPortMixer_MulHighSignedUnsigned(right, reverb);
    }

    *leftOut = left;
    *rightOut = right;
    *wetLeftOut = wetLeft;
    *wetRightOut = wetRight;
}

static s32 Audio_CanVectorizeEnvelope(u16 volumeLeft, u16 volumeRight, u16 reverb) {
    return ((volumeLeft | volumeRight | reverb) & 0x8000U) == 0;
}

static void Audio_RunEnvelopeBlock8(const s16* input, s16* dryLeft, s16* dryRight, s16* wetLeft,
                                          s16* wetRight, u16 volumeLeft, u16 volumeRight, u16 reverb,
                                          s32 swapWetChannels) {
    AudioEnvBlock block = {
        .input = input,
        .dryLeft = dryLeft,
        .dryRight = dryRight,
        .wetLeft = wetLeft,
        .wetRight = wetRight,
        .volumeLeft = volumeLeft,
        .volumeRight = volumeRight,
        .reverb = reverb,
        .swapWetChannels = (u16)(swapWetChannels != 0),
    };

    Audio_MixEnvelopeBlock8(&block);
}


static void OotPortMixer_EnvMixerCpu(u16 dmemSrc, s32 aiBufLen, s32 swapLR, s32 x0, s32 x1, s32 x2, s32 x3,
                                    u32 dmemDests) {
    OOT_PORT_MIXER_STATE();
    s16* in = DMEM_S16(dmemSrc);
    s16* dryLeft = DMEM_S16(((dmemDests >> 24) & 0xFF) << 4);
    s16* dryRight = DMEM_S16(((dmemDests >> 16) & 0xFF) << 4);
    s16* wetLeft = DMEM_S16(((dmemDests >> 8) & 0xFF) << 4);
    s16* wetRight = DMEM_S16((dmemDests & 0xFF) << 4);
    u16 volLeft0 = (u16)sMixer.env.volLeft;
    u16 volRight0 = (u16)sMixer.env.volRight;
    u16 reverb0 = (u16)(sMixer.env.initialReverb << 8);
    u16 volLeft1 = (u16)(volLeft0 + (s16)sMixer.env.rampLeft);
    u16 volRight1 = (u16)(volRight0 + (s16)sMixer.env.rampRight);
    u16 reverb1 = (u16)(reverb0 + (s16)sMixer.env.rampReverb);
    s16 rampLeft = (s16)(sMixer.env.rampLeft * 2);
    s16 rampRight = (s16)(sMixer.env.rampRight * 2);
    s16 rampReverb = (s16)(sMixer.env.rampReverb * 2);
    s16 dryLeftMask = x2 ? -1 : 0;
    s16 dryRightMask = x3 ? -1 : 0;
    s16 wetLeftMask = x0 ? -4 : 0;
    s16 wetRightMask = x1 ? -2 : 0;
    s32 applyMasks = dryLeftMask | dryRightMask | wetLeftMask | wetRightMask;
    s32 remaining = ROUND_UP_16(aiBufLen);

    while (remaining > 0) {
        if ((applyMasks == 0) && Audio_CanVectorizeEnvelope(volLeft0, volRight0, reverb0) &&
            Audio_CanVectorizeEnvelope(volLeft1, volRight1, reverb1)) {
            Audio_RunEnvelopeBlock8(in, dryLeft, dryRight, wetLeft, wetRight, volLeft0, volRight0, reverb0,
                                          swapLR);
            Audio_RunEnvelopeBlock8(in + 8, dryLeft + 8, dryRight + 8, wetLeft + 8, wetRight + 8,
                                          volLeft1, volRight1, reverb1, swapLR);
            in += 16;
            dryLeft += 16;
            dryRight += 16;
            wetLeft += 16;
            wetRight += 16;
        } else {
            s32 sampleIndex;

            for (sampleIndex = 0; sampleIndex < 16; sampleIndex++) {
                s16 sample = *in++;
                u16 volumeLeft = (sampleIndex < 8) ? volLeft0 : volLeft1;
                u16 volumeRight = (sampleIndex < 8) ? volRight0 : volRight1;
                u16 reverb = (sampleIndex < 8) ? reverb0 : reverb1;
                s16 left;
                s16 right;
                s16 wetL;
                s16 wetR;

                OotPortMixer_EnvMixerProducts(sample, volumeLeft, volumeRight, reverb, dryLeftMask, dryRightMask,
                                             wetLeftMask, wetRightMask, applyMasks, &left, &right, &wetL, &wetR);

                *dryLeft = OotPortMixer_Clamp16(*dryLeft + left);
                dryLeft++;
                *dryRight = OotPortMixer_Clamp16(*dryRight + right);
                dryRight++;
                if (swapLR) {
                    *wetLeft = OotPortMixer_Clamp16(*wetLeft + wetR);
                    wetLeft++;
                    *wetRight = OotPortMixer_Clamp16(*wetRight + wetL);
                    wetRight++;
                } else {
                    *wetLeft = OotPortMixer_Clamp16(*wetLeft + wetL);
                    wetLeft++;
                    *wetRight = OotPortMixer_Clamp16(*wetRight + wetR);
                    wetRight++;
                }
            }
        }

        volLeft0 = (u16)(volLeft0 + rampLeft);
        volLeft1 = (u16)(volLeft1 + rampLeft);
        volRight0 = (u16)(volRight0 + rampRight);
        volRight1 = (u16)(volRight1 + rampRight);
        reverb0 = (u16)(reverb0 + rampReverb);
        reverb1 = (u16)(reverb1 + rampReverb);
        remaining -= 16;
    }
}


void OotPortMixer_EnvMixer(u16 dmemSrc, s32 aiBufLen, s32 swapLR, s32 x0, s32 x1, s32 x2, s32 x3, u32 dmemDests,
                          UNUSED u32 bits) {

    OotPortMixer_EnvMixerCpu(dmemSrc, aiBufLen, swapLR, x0, x1, x2, x3, dmemDests);
}

void OotPortMixer_Mix(s32 countQuads, s16 gain, u16 dmemIn, u16 dmemOut) {
    OOT_PORT_MIXER_STATE();
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 samples = ROUND_UP_32((countQuads & 0xFF) << 4) / sizeof(s16);

    if (samples > 0) {
        Audio_MixSamples(in, out, gain, samples);
    }
}

void OotPortMixer_AddMixer(s32 nbytes, u16 dmemIn, u16 dmemOut, UNUSED s16 gain) {
    OOT_PORT_MIXER_STATE();
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 encodedBytes = ROUND_DOWN_16(nbytes);
    s32 samples = ROUND_UP_64(encodedBytes) / sizeof(s16);
    s32 i;

    if (encodedBytes == 0) {
        return;
    }

    for (i = 0; i < samples; i++) {
        out[i] = OotPortMixer_Vadd(out[i], in[i]);
    }
}

void OotPortMixer_Duplicate(s32 numCopies, u16 dmemSrc, u16 dmemDest) {
    OOT_PORT_MIXER_STATE();
    s32 i;

    for (i = 0; i < numCopies; i++) {
        memcpy(DMEM_U8(dmemDest + (i * 128)), DMEM_U8(dmemSrc), 128);
    }
}

void OotPortMixer_CopyBlocks(s32 numBlocks, u16 dmemSrc, u16 dmemDest, s32 blockSize) {
    OOT_PORT_MIXER_STATE();
    u8 block[32];
    s32 blockBytes = ROUND_UP_32(blockSize);
    s32 blockIndex;
    s32 offset;

    for (blockIndex = 0; blockIndex < numBlocks; blockIndex++) {
        for (offset = 0; offset < blockBytes; offset += sizeof(block)) {
            memcpy(block, DMEM_U8(dmemSrc), sizeof(block));
            memcpy(DMEM_U8(dmemDest), block, sizeof(block));
            dmemSrc += sizeof(block);
            dmemDest += sizeof(block);
        }
    }
}

void OotPortMixer_Filter(u8 flags, s32 countOrBuf, void* state) {
    OOT_PORT_MIXER_STATE();
    s16* save = state;
    s16* input;
    s16* out = sMixer.filterScratch;
    s16 history[OOT_PORT_FILTER_TAP_COUNT];
    s16 taps[OOT_PORT_FILTER_TAP_COUNT];
    s32 count;
    s32 processedCount;
    s32 i;

    if (flags > 1) {
        sMixer.filter2Count = ROUND_UP_16(countOrBuf);
        sMixer.filter2Lut = state;
        sMixer.filter2Valid = state != NULL;
        return;
    }

    if ((save == NULL) || !sMixer.filter2Valid || (sMixer.filter2Count <= 0)) {
        return;
    }

    if (flags & A_INIT) {
        memset(save, 0, 16 * sizeof(s16));
    }

    for (i = 0; i < OOT_PORT_FILTER_TAP_COUNT; i++) {
        s16 avg = ((s32)save[OOT_PORT_FILTER_TAP_COUNT + i] + sMixer.filter2Lut[i]) >> 1;

        history[i] = save[i];
        taps[i] = avg;
        save[OOT_PORT_FILTER_TAP_COUNT + i] = avg;
        sMixer.filter2Lut[i] = avg;
    }

    input = DMEM_S16((u16)countOrBuf);
    count = sMixer.filter2Count;
    if (count > (s32)sizeof(sMixer.filterScratch)) {
        count = sizeof(sMixer.filterScratch);
    }
    processedCount = count;

    while (count > 0) {
        s16 blockInput[OOT_PORT_FILTER_TAP_COUNT];

        s32 out1[OOT_PORT_FILTER_TAP_COUNT];

        memcpy(blockInput, input, sizeof(blockInput));

        out1[1] = history[0] * taps[6];
        out1[1] += history[3] * taps[7];
        out1[1] += history[2] * taps[4];
        out1[1] += history[5] * taps[5];
        out1[1] += history[4] * taps[2];
        out1[1] += history[7] * taps[3];
        out1[1] += history[6] * taps[0];
        out1[1] += blockInput[1] * taps[1];

        out1[0] = history[3] * taps[6];
        out1[0] += history[2] * taps[7];
        out1[0] += history[5] * taps[4];
        out1[0] += history[4] * taps[5];
        out1[0] += history[7] * taps[2];
        out1[0] += history[6] * taps[3];
        out1[0] += blockInput[1] * taps[0];
        out1[0] += blockInput[0] * taps[1];

        out1[3] = history[2] * taps[6];
        out1[3] += history[5] * taps[7];
        out1[3] += history[4] * taps[4];
        out1[3] += history[7] * taps[5];
        out1[3] += history[6] * taps[2];
        out1[3] += blockInput[1] * taps[3];
        out1[3] += blockInput[0] * taps[0];
        out1[3] += blockInput[3] * taps[1];

        out1[2] = history[5] * taps[6];
        out1[2] += history[4] * taps[7];
        out1[2] += history[7] * taps[4];
        out1[2] += history[6] * taps[5];
        out1[2] += blockInput[1] * taps[2];
        out1[2] += blockInput[0] * taps[3];
        out1[2] += blockInput[3] * taps[0];
        out1[2] += blockInput[2] * taps[1];

        out1[5] = history[4] * taps[6];
        out1[5] += history[7] * taps[7];
        out1[5] += history[6] * taps[4];
        out1[5] += blockInput[1] * taps[5];
        out1[5] += blockInput[0] * taps[2];
        out1[5] += blockInput[3] * taps[3];
        out1[5] += blockInput[2] * taps[0];
        out1[5] += blockInput[5] * taps[1];

        out1[4] = history[7] * taps[6];
        out1[4] += history[6] * taps[7];
        out1[4] += blockInput[1] * taps[4];
        out1[4] += blockInput[0] * taps[5];
        out1[4] += blockInput[3] * taps[2];
        out1[4] += blockInput[2] * taps[3];
        out1[4] += blockInput[5] * taps[0];
        out1[4] += blockInput[4] * taps[1];

        out1[7] = history[6] * taps[6];
        out1[7] += blockInput[1] * taps[7];
        out1[7] += blockInput[0] * taps[4];
        out1[7] += blockInput[3] * taps[5];
        out1[7] += blockInput[2] * taps[2];
        out1[7] += blockInput[5] * taps[3];
        out1[7] += blockInput[4] * taps[0];
        out1[7] += blockInput[7] * taps[1];

        out1[6] = blockInput[1] * taps[6];
        out1[6] += blockInput[0] * taps[7];
        out1[6] += blockInput[3] * taps[4];
        out1[6] += blockInput[2] * taps[5];
        out1[6] += blockInput[5] * taps[2];
        out1[6] += blockInput[4] * taps[3];
        out1[6] += blockInput[7] * taps[0];
        out1[6] += blockInput[6] * taps[1];

        out[1] = OotPortMixer_Clamp16((out1[1] + 0x4000) >> 15);
        out[0] = OotPortMixer_Clamp16((out1[0] + 0x4000) >> 15);
        out[3] = OotPortMixer_Clamp16((out1[3] + 0x4000) >> 15);
        out[2] = OotPortMixer_Clamp16((out1[2] + 0x4000) >> 15);
        out[5] = OotPortMixer_Clamp16((out1[5] + 0x4000) >> 15);
        out[4] = OotPortMixer_Clamp16((out1[4] + 0x4000) >> 15);
        out[7] = OotPortMixer_Clamp16((out1[7] + 0x4000) >> 15);
        out[6] = OotPortMixer_Clamp16((out1[6] + 0x4000) >> 15);

        memcpy(history, blockInput, sizeof(history));
        input += OOT_PORT_FILTER_TAP_COUNT;
        out += OOT_PORT_FILTER_TAP_COUNT;
        count -= OOT_PORT_FILTER_TAP_COUNT * sizeof(s16);
    }

    memcpy(state, input - OOT_PORT_FILTER_TAP_COUNT, OOT_PORT_FILTER_TAP_COUNT * sizeof(s16));
    memcpy(DMEM_S16((u16)countOrBuf), sMixer.filterScratch, processedCount);
}

void OotPortMixer_HiLoGain(s32 gain, u16 dmemIn, UNUSED u16 dmemOut, s32 nbytes) {
    OOT_PORT_MIXER_STATE();
    s16* out = DMEM_S16(dmemIn);
    s32 samples = ROUND_UP_32(nbytes) / sizeof(s16);
    s32 i;

    gain &= 0xFF;
    for (i = 0; i < samples; i++) {
        out[i] = OotPortMixer_Clamp16((out[i] * gain) >> 4);
    }
}

void OotPortMixer_UnkCmd3(s32 arg1, s32 arg2, s32 size) {
    OOT_PORT_MIXER_STATE();
    s16* src = DMEM_S16(arg1);
    s16* dst = DMEM_S16(arg2);
    s32 samples = ROUND_UP_32(size) / sizeof(s16);
    s32 i;

    for (i = 0; i < samples; i += 16) {
        s32 lane;

        for (lane = 0; lane < 8; lane++) {
            s32 product = (u16)src[i + 8 + lane] * (s32)src[i + lane];
            u16 highResult;

            if (product < -0x40000000) {
                highResult = 0;
            } else if (product > 0x3FFFFFFF) {
                highResult = 0xFFFF;
            } else {
                highResult = (u16)((u16)product << 1);
            }

            dst[i + lane] = (s16)highResult;
            dst[i + 8 + lane] = (s16)(u16)product;
        }
    }
}

void OotPortMixer_UnkCmd19(UNUSED s32 arg1, UNUSED s32 arg2, UNUSED s32 size, UNUSED s32 arg4) {

}


static void OotPortMixer_ExecuteCommandListInternal(const Acmd* cmdList, s32 cmdCount) {
    s32 i;

    for (i = 0; i < cmdCount; i++) {
        const Acmd* cmd = &cmdList[i];
        u32 w0 = cmd->words.w0;
        u32 w1 = cmd->words.w1;
        u32 opcode = w0 >> 24;

        switch (opcode) {
            case A_SPNOOP:
                break;

            case A_ADPCM:
                OotPortMixer_ADPCMdec((w0 >> 16) & 0xFF, (s16*)(uintptr_t)w1);
                break;

            case A_CLEARBUFF:
                OotPortMixer_ClearBuffer(w0 & 0xFFFF, w1);
                break;

            case A_UNK3:
                OotPortMixer_UnkCmd3((w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF);
                break;

            case A_ADDMIXER:
                OotPortMixer_AddMixer(((w0 >> 16) & 0xFF) << 4, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF, (s16)w0);
                break;

            case A_RESAMPLE:
                OotPortMixer_Resample((w0 >> 16) & 0xFF, w0 & 0xFFFF, (s16*)(uintptr_t)w1);
                break;

            case A_RESAMPLE_ZOH:
                OotPortMixer_ResampleZoh(w0 & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_FILTER:
                OotPortMixer_Filter((w0 >> 16) & 0xFF, w0 & 0xFFFF, (void*)(uintptr_t)w1);
                break;

            case A_SETBUFF:
                OotPortMixer_SetBuffer((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_DUPLICATE:
                OotPortMixer_Duplicate((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF);
                break;

            case A_DMEMMOVE:
                OotPortMixer_DMEMMove(w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_LOADADPCM:
                OotPortMixer_LoadADPCM(w0 & 0xFFFFFF, (const s16*)(uintptr_t)w1);
                break;

            case OOT_PORT_A_LOAD_ADPCM_CACHED:
                OotPortMixer_LoadADPCMCached(w0 & 0xFFFFFF, (const s16*)(uintptr_t)w1);
                break;

            case A_MIXER:
                OotPortMixer_Mix((w0 >> 16) & 0xFF, (s16)w0, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_INTERLEAVE:
                OotPortMixer_Interleave(w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF,
                                       ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_HILOGAIN:
                OotPortMixer_HiLoGain((w0 >> 16) & 0xFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF);
                break;

            case A_SETLOOP:
                OotPortMixer_SetLoop((ADPCM_STATE*)(uintptr_t)w1);
                break;

            case OOT_PORT_A_COPYBLOCKS:
                OotPortMixer_CopyBlocks((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case OOT_PORT_A_REVERB_DOWNSAMPLE:
                OotPortMixer_ReverbDownsample(w0 & 0xFFFF,
                                             (const OotPortAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case OOT_PORT_A_REVERB_SAVE:
                OotPortMixer_ReverbSave(w0 & 0xFFFF, (const OotPortAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case OOT_PORT_A_REVERB_LOAD:
                OotPortMixer_ReverbLoad(w0 & 0xFFFF, (const OotPortAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case A_INTERL:
                OotPortMixer_Interl((w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF);
                break;

            case A_ENVSETUP1:
                OotPortMixer_EnvSetup1((w0 >> 16) & 0xFF, (s16)w0, (s16)(w1 >> 16), (s16)w1);
                break;

            case A_ENVMIXER:
                OotPortMixer_EnvMixer(((w0 >> 16) & 0xFF) << 4, (w0 >> 8) & 0xFF, (w0 >> 4) & 1,
                                      (w0 >> 3) & 1, (w0 >> 2) & 1, (w0 >> 1) & 1, w0 & 1, w1,
                                      A_ENVMIXER << 24);
                break;

            case A_LOADBUFF:
                OotPortMixer_LoadBuffer((const void*)(uintptr_t)w1, w0 & 0xFFFF, ((w0 >> 16) & 0xFF) << 4);
                break;

            case OOT_PORT_A_LOAD_SAMPLE_CACHED:
                OotPortMixer_LoadSampleCached((const void*)(uintptr_t)w1, w0 & 0xFFFF,
                                             ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_SAVEBUFF:
                OotPortMixer_SaveBuffer(w0 & 0xFFFF, (void*)(uintptr_t)w1, ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_ENVSETUP2:
                OotPortMixer_EnvSetup2((u16)(w1 >> 16), (u16)w1);
                break;

            case A_S8DEC:
                OotPortMixer_S8Dec((w0 >> 16) & 0xFF, (s16*)(uintptr_t)w1);
                break;

            case A_UNK19:
                OotPortMixer_UnkCmd19((w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF, (w0 >> 16) & 0xFF);
                break;

            default:
                break;
        }

    }

}

void OotPortMixer_ExecuteCommandList(const Acmd* cmdList, s32 cmdCount) {
    OotPortMixer_ExecuteCommandListInternal(cmdList, cmdCount);
}

void OotPortMixer_InvalidateStateCache(void) {

}
