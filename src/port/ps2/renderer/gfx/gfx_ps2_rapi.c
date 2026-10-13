/* Mano papo reto se alguem vier e ter a cara de pau e falar que foi ia que escreveu tudo isso aqui
vou mandar tomar no cu, vai tomar no cu namoral essa porra gastei muito tempo nessa desgraça
pra vim pessoa falando que foi ia que escreveu vai tomar no seu cu ja deu até preguiça
de documentar a porra desse codigo
presta atenção nessa porra antes de sair mexendo como se tudo aqui fosse igual ao n64
esse arquivo pega o que vem do fast3d e transforma em registrador e pacote gif que o gs do ps2 consegue entender
o gs não faz todos os combines do n64 em uma passada, por isso tem cache, textura montada na cpu e desenho repetido
se alguma coisa parece estranha primeiro entende o caminho inteiro e só depois inventa de apagar
*/

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <kernel.h>
#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>
#include "gfx_rendering_api.h"
#include "gfx_cc.h"
#include "gfx_window_ps2.h"
#include "gfx_window_manager_api.h"
#include "platform.h"
#include "controls/controls.h"
#include "renderer/video.h"
#include "array_count.h"
#include "renderer/renderer.h"

#define MAX_TEXTURES 768
#define MAX_SHADERS 1024
#define TEXCACHE_SIZE (4 * 1024 * 1024)
#define SHADER_HASH_SLOTS 2048U
#define REVERSE_TINT_CACHE_SLOTS 16
#define BLEND_CACHE_SLOTS 256
#define BLEND_CACHE_BUDGET (4U * 1024U * 1024U)
#define ALIGN(v, a) (((v) + ((a) - 1)) & ~((a) - 1))
#define BMODE_BLEND GS_SETREG_ALPHA(0, 1, 0, 1, 128)

#define SETREG_FOGCOL(r, g, b) ((u64)((r) & 0xff) | ((u64)((g) & 0xff) << 8) | ((u64)((b) & 0xff) << 16))

/*
antes que algum gênio ache que esses números saíram do cu, eles são o tamanho dos pools e da memória de textura
o texcache fica na cpu segurando os dados e a vram continua sendo cuidada pelo texmanager do gskit
*/

extern void gfx_set_ps2_widescreen(bool enabled);

static void restore_prerender_fallback(void);
void texman_clear(void);

/*
não inventa de organizar esses campos do seu jeito porque eles são lidos direto do buffer de vértices
alguns ainda viram qword no pacote gif, então se tirar packed ou aligned o gs recebe lixo e desenha uma merda
*/
typedef union TexCoord {
    struct {
        float s;
        float t;
    };

    uint64_t word;
} __attribute__((packed, aligned(8))) TexCoord;

typedef union ColorQ {
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
        float q;
    };

    uint32_t rgba;
    uint64_t word;
} __attribute__((packed, aligned(8))) ColorQ;

typedef struct FastVertex {
    float u;
    float v;
    float real_u;
    float real_v;
    uint32_t rgba;
    float x;
    float y;
    float z;
    float q;
    uint32_t fog_color;
    uint32_t fog;
} __attribute__((packed, aligned(4))) FastVertex;

typedef struct FogColorVertex {
    uint32_t rgba;
    float x;
    float y;
    float z;
} __attribute__((packed, aligned(4))) FogColorVertex;

typedef struct FogTexturedVertex {
    float u;
    float v;
    uint32_t rgba;
    float x;
    float y;
    float z;
    float q;
} __attribute__((packed, aligned(4))) FogTexturedVertex;

typedef struct VertexColor2D {
    int16_t u;
    int16_t v;
    uint32_t rgba;
    uint16_t x;
    uint16_t y;
    uint16_t z;
} __attribute__((packed, aligned(4))) VertexColor2D;

enum TextureMode {
    TEXTURE_MODE_MODULATE = 0,
    TEXTURE_MODE_DECAL,
    TEXTURE_MODE_BLEND,
    TEXTURE_MODE_REPLACE,
};

enum TextureFormat {
    TEXTURE_FORMAT_5551 = 1,
    TEXTURE_FORMAT_4444 = 2,
    TEXTURE_FORMAT_T8 = 5,
    TEXTURE_FORMAT_8888_GS_NATIVE = 8,
};

/*
não seja burro seu dumbo, shader aqui não é shader programável igual numa gpu moderna
isso é o combiner do fast3d já traduzido para os estados e caminhos que o gs consegue usar
*/
struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    int mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;

    bool alpha_test;
    uint8_t alpha_ref;
    bool depth_only;
    bool texture_alpha;
    uint8_t tfx;
    uint8_t texture_mode;
};

struct Texture {
    GSTEXTURE tex;
    uint32_t clamp_s;
    uint32_t clamp_t;
    uint8_t cms;
    uint8_t cmt;
    uint8_t masks;
    uint8_t maskt;
    size_t backing_size;
    uint32_t content_hash;
    uint32_t upload_serial;
};

struct Viewport {
    float x;
    float y;
    float w;
    float h;
    float hw;
    float hh;
    float cx;
    float cy;
};

struct Clip {
    int x0;
    int y0;
    int x1;
    int y1;
};

typedef struct ReverseTintCacheEntry {
    struct Texture tex;
    uint8_t* mem;
    size_t capacity;
    const struct Texture* source;
    uint32_t source_serial;
    uint32_t prim_rgb;
    uint32_t env_rgb;
    uint32_t stamp;
    uint32_t last_used_frame;
    bool valid;
} ReverseTintCacheEntry;

typedef struct BlendCacheEntry {
    struct Texture tex;
    uint8_t* mem;
    size_t capacity;
    const struct Texture* source;
    uint32_t source_serial;
    uint32_t base_rgb;
    uint32_t target_rgb;
    uint8_t kind;
    uint32_t stamp;
    uint32_t last_used_frame;
    bool valid;
} BlendCacheEntry;

typedef struct FogPassState {
    bool active;
    bool saved_z_write;
    bool saved_blend;
    bool saved_alpha_test;
    bool saved_texture_alpha;
    uint8_t saved_tfx;
    struct ShaderProgram* saved_shader;
} FogPassState;

typedef struct PrerenderFallback {
    struct Texture* texture;
    float x1;
    float y1;
    float x2;
    float y2;
    float u1;
    float v1;
    float u2;
    float v2;
    int z1;
    int z2;
    u64 color;
    unsigned missedFrames;
    bool valid;
    bool seenThisFrame;
} PrerenderFallback;

static struct ShaderProgram shader_pool[MAX_SHADERS];
static uint16_t shader_hash[SHADER_HASH_SLOTS];
static unsigned shader_count;
static struct ShaderProgram* cur_shader;

static struct Texture tex_pool[MAX_TEXTURES];
static uint32_t tex_count;
static struct Texture* cur_tex[2];
static struct Texture* last_tex;

static u32 sIntensityT8Clut[256] __attribute__((aligned(128)));
static bool sIntensityT8ClutReady;

static bool sampler_linear[2];
static uint32_t sampler_clamp_s[2] = { GS_CMODE_REPEAT, GS_CMODE_REPEAT };
static uint32_t sampler_clamp_t[2] = { GS_CMODE_REPEAT, GS_CMODE_REPEAT };
static uint8_t sampler_cms[2];
static uint8_t sampler_cmt[2];
static uint8_t sampler_masks[2];
static uint8_t sampler_maskt[2];

static uint8_t* tex_cache;
static uint8_t* tex_ptr;
static uint8_t* tex_end;
static uint32_t sTextureUploadSerial = 1;
static bool sSkipContentHash;

static struct Viewport r_view;
static struct Clip r_clip;

static bool z_test = true;
static bool z_write = true;
static bool z_decal;
static bool do_blend;
static float z_offset;

static uint32_t sEnvColor = 0xffffffffU;
static bool sTextureBlendReverse;
static bool sTextureBlendPrecolor;
static bool sDinFireTint;
static bool sTwoTextureBlendActive;
static bool sTwoTextureAlphaBlendActive;
static bool sTwoTextureEnvPrimTint;

/*
esses valores guardados não estão aqui de enfeite, eles evitam mandar o mesmo registrador pro gs toda santa hora
se algum código mexer no gs por fora tem que invalidar o cache ou o renderer vai confiar num estado que já mudou
*/
static int sLastTexFilter = -1;
static u64 sLastTestReg = ~0ULL;
static u64 sLastClampReg = ~0ULL;
static u64 sLastScissorReg = ~0ULL;
static u64 sLastZbufReg = ~0ULL;
static u64 sLastAlphaReg = ~0ULL;
static u64 sLastTex0Reg = ~0ULL;
static uint32_t sLastFogColor = 0xffffffffU;
static GSTEXTURE* sLastBoundGsTex;

static ReverseTintCacheEntry sReverseTintCache[REVERSE_TINT_CACHE_SLOTS];
static uint32_t sReverseTintStamp;

static BlendCacheEntry sBlendCache[BLEND_CACHE_SLOTS];
static uint32_t sBlendStamp;
static size_t sBlendCacheBytes;

static FogPassState sFogPass;
static bool sSkipNextFogPass;

static PrerenderFallback sPrerenderFallback;
static bool sPrerender3dStarted;
static bool sPrerenderRoomActive;
static bool sPrerenderDepthOnlyPass;
static u32 sPrerenderRoomKey = 0xffffffffU;

static uint32_t sRenderFrameSerial = 1;

static bool sSuppressFpsOverlay;
static bool sFpsOverlayEnabled = true;
static int sBootProgressCode = -1;
static char sBootProgressLabel[40];

static bool sHomeMenuBgActive;
static bool sHomeMenuBgCaptureRequested;
static bool sHomeMenuBgCaptured;

/*
essas flags dizem qual gambiarra de duas texturas o combiner precisa usar no ps2
elas são limpas em todo frame porque um material não pode deixar sua sujeira no material seguinte
*/
void gfx_ps2_set_texture_blend_reverse(bool enabled) {
    sTextureBlendReverse = enabled;
}

void gfx_ps2_set_texture_blend_precolor(bool enabled) {
    sTextureBlendPrecolor = enabled;
}

void gfx_ps2_set_din_fire_tint(bool enabled) {
    sDinFireTint = enabled;
}

void gfx_ps2_set_two_texture_blend_active(bool enabled) {
    sTwoTextureBlendActive = enabled;
}

void gfx_ps2_set_two_texture_alpha_blend_active(bool enabled) {
    sTwoTextureAlphaBlendActive = enabled;
}

void gfx_ps2_set_two_texture_env_prim_tint(bool enabled) {
    sTwoTextureEnvPrimTint = enabled;
}

void gfx_ps2_set_skip_content_hash(bool enabled) {
    sSkipContentHash = enabled;
}

void gfx_ps2_set_fps_overlay_enabled(bool enabled) {
    sFpsOverlayEnabled = enabled;
}

bool gfx_ps2_get_fps_overlay_enabled(void) {
    return sFpsOverlayEnabled;
}

void gfx_ps2_set_prerender_room_state(bool active, u32 roomKey) {
    if (sPrerenderRoomActive != active || sPrerenderRoomKey != roomKey) {
        sPrerenderFallback.valid = false;
        sPrerenderFallback.texture = NULL;
        sPrerenderFallback.seenThisFrame = false;
        sPrerenderFallback.missedFrames = 0;
        sPrerenderRoomActive = active;
        sPrerenderRoomKey = roomKey;
    }
}

void gfx_ps2_invalidate_register_cache(void) {
    sLastTexFilter = -1;
    sLastTestReg = ~0ULL;
    sLastClampReg = ~0ULL;
    sLastScissorReg = ~0ULL;
    sLastZbufReg = ~0ULL;
    sLastAlphaReg = ~0ULL;
    sLastTex0Reg = ~0ULL;
    sLastFogColor = 0xffffffffU;
    sSkipNextFogPass = false;
    sLastBoundGsTex = NULL;
}

/*
antes de achar que essa divisão por dois está errada lembra que no gs o alpha vai de 0 a 0x80
no modulate com textura o rgb neutro também é 0x80, então a cor precisa chegar pela metade no pacote
*/
static inline uint64_t colorq_from_rgba(uint32_t rgba, float q, bool textured) {
    ColorQ c;

    const uint32_t alphaHalf = ((((rgba >> 24) + 1U) >> 1) & 0xffU) << 24;
    if (textured) {
        const uint32_t rgbHalf = ((rgba & 0x00fefefeU) >> 1) + (rgba & 0x00010101U);
        c.rgba = rgbHalf | alphaHalf;
    } else {
        c.rgba = (rgba & 0x00ffffffU) | alphaHalf;
    }
    c.q = q;
    return c.word;
}
static struct ShaderProgram* lookup_shader(uint32_t id);
static struct ShaderProgram* effective_shader(struct ShaderProgram* p);

static bool depth_range_is_zero_to_one(void) {
    return true;
}

static void unload_shader(struct ShaderProgram* p) {
    struct ShaderProgram* effective = effective_shader(p);
    if (p == NULL || cur_shader == effective) {
        cur_shader = NULL;
    }
}

static void load_shader(struct ShaderProgram* p) {
    cur_shader = effective_shader(p);
}

/*
isso traduz o id do combiner uma vez e guarda o resultado pra não fazer a mesma merda em todo desenho
os ids separados não são número aleatório, são combines que precisam de blend, decal ou replace no lugar de modulate
*/
static struct ShaderProgram* create_shader(uint32_t id) {
    struct CCFeatures f;
    gfx_cc_get_features(id, &f);

    if (shader_count >= MAX_SHADERS) {
        printf("oot-ps2 shader pool exhausted count=%u id=%08x\n", shader_count, (unsigned)id);
        abort();
    }
    const unsigned shaderIndex = shader_count++;
    struct ShaderProgram* p = &shader_pool[shaderIndex];
    memset(p, 0, sizeof(*p));
    p->enabled = true;
    p->shader_id = id;
    p->cc = f;
    p->num_inputs = f.num_inputs;

    p->texture_used[0] = f.used_textures[0] || f.used_textures[1];
    p->texture_used[1] = false;
    p->alpha_test = f.opt_texture_edge || ((id & SHADER_OPT_ALPHA_THRESHOLD) != 0);
    p->alpha_ref = f.opt_texture_edge ? 0x2a : 0;
    p->depth_only = (id & SHADER_OPT_DEPTH_ONLY) != 0;
    p->texture_alpha = false;
    if (f.opt_alpha) {
        for (int i = 0; i < 4; i++) {
            if (f.c[1][i] == SHADER_TEXEL0 || f.c[1][i] == SHADER_TEXEL0A || f.c[1][i] == SHADER_TEXEL1) {
                p->texture_alpha = true;
                break;
            }
        }
    }

    p->texture_mode = TEXTURE_MODE_MODULATE;
    if (p->texture_used[0] && f.num_inputs) {
        p->mix = 4;
        if (f.opt_texture_blend) {
            p->texture_mode = TEXTURE_MODE_BLEND;
        } else {
            switch (id) {
                case 0x0000038DU:
                case 0x01200A00U:
                    p->texture_mode = TEXTURE_MODE_DECAL;
                    break;
                case 0x00000551U:
                    p->texture_mode = TEXTURE_MODE_BLEND;
                    break;
                default:
                    break;
            }
        }
    } else if (p->texture_used[0]) {
        p->mix = 1;
    } else if (f.num_inputs > 1) {
        p->mix = 5;
    } else if (f.num_inputs) {
        p->mix = 2;
    }
    if (id == 0x01A00045U) {
        p->texture_mode = TEXTURE_MODE_REPLACE;
    }

    p->tfx = (p->texture_mode == TEXTURE_MODE_REPLACE || (p->texture_mode == TEXTURE_MODE_DECAL && !p->texture_alpha))
                 ? 1
                 : 0;

    uint32_t slot = (id * 2654435761U) & (SHADER_HASH_SLOTS - 1U);
    for (uint32_t probe = 0; probe < SHADER_HASH_SLOTS; probe++) {
        if (shader_hash[slot] == 0U) {
            shader_hash[slot] = (uint16_t)(shaderIndex + 1U);
            break;
        }
        slot = (slot + 1U) & (SHADER_HASH_SLOTS - 1U);
    }
    cur_shader = effective_shader(p);
    return p;
}

static struct ShaderProgram* lookup_shader(uint32_t id) {
    uint32_t slot = (id * 2654435761U) & (SHADER_HASH_SLOTS - 1U);
    for (uint32_t probe = 0; probe < SHADER_HASH_SLOTS; probe++) {
        const uint16_t encoded = shader_hash[slot];
        if (encoded == 0U) {
            return NULL;
        }
        struct ShaderProgram* p = &shader_pool[(unsigned)encoded - 1U];
        if (p->shader_id == id) {
            return p;
        }
        slot = (slot + 1U) & (SHADER_HASH_SLOTS - 1U);
    }
    return NULL;
}

static void shader_info(struct ShaderProgram* p, uint8_t* n, bool t[2]) {
    *n = (uint8_t)p->num_inputs;
    t[0] = p->texture_used[0];
    t[1] = p->texture_used[1];
}

static struct ShaderProgram* effective_shader(struct ShaderProgram* p) {
    if (p == NULL) {
        return NULL;
    }

    switch (p->shader_id) {
        case 0x09200045U:
        case 0x09200200U:
        case 0x0920038DU:
        case 0x09200A00U: {
            struct ShaderProgram* remap = lookup_shader(0x00000045U);
            return remap != NULL ? remap : p;
        }
        default:
            return p;
    }
}

static void release_texture_residency(GSTEXTURE* tex) {
    if (!gs_global || !tex) {
        return;
    }

    gsKit_TexManager_free(gs_global, tex);
    tex->Vram = 0;
    tex->VramClut = 0;
    if (sLastBoundGsTex == tex) {
        sLastBoundGsTex = NULL;
        sLastTex0Reg = ~0ULL;
        sLastTexFilter = -1;
    }
}

static uint32_t new_texture(void) {
    if (tex_count >= MAX_TEXTURES) {
        texman_clear();
    }

    const uint32_t id = tex_count++;
    struct Texture* texture = &tex_pool[id];

    if (sPrerenderFallback.texture == texture) {
        sPrerenderFallback.valid = false;
    }
    if (texture->tex.Mem != NULL || texture->upload_serial != 0) {
        release_texture_residency(&texture->tex);
    }

    memset(texture, 0, sizeof(*texture));
    texture->tex.Filter = GS_FILTER_NEAREST;
    texture->clamp_s = GS_CMODE_REPEAT;
    texture->clamp_t = GS_CMODE_REPEAT;
    last_tex = texture;
    return id;
}

static void select_texture(int tile, uint32_t id) {
    if (tile < 0 || tile > 1 || id >= MAX_TEXTURES) {
        return;
    }

    cur_tex[tile] = &tex_pool[id];
    last_tex = cur_tex[tile];
    last_tex->tex.Filter = sampler_linear[tile] ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    last_tex->clamp_s = sampler_clamp_s[tile];
    last_tex->clamp_t = sampler_clamp_t[tile];
    last_tex->cms = sampler_cms[tile];
    last_tex->cmt = sampler_cmt[tile];
    last_tex->masks = sampler_masks[tile];
    last_tex->maskt = sampler_maskt[tile];
}

static void* alloc_tex_bytes(size_t size) {
    if (!tex_cache || size > (size_t)(tex_end - tex_ptr)) {
        printf("oot-ps2 texture backing invariant failed need=%u remain=%u\n", (unsigned)size,
               tex_cache ? (unsigned)(tex_end - tex_ptr) : 0U);
        abort();
    }

    void* allocation = tex_ptr;
    tex_ptr += size;
    return allocation;
}

/*
as texturas t8 daqui são intensidade e a clut transforma cada índice em cinza com alpha
essa troca de blocos parece errada mas é a ordem torta que o csm1 do gs espera, então não desfaz essa porra
*/
static void init_intensity_t8_clut(void) {
    if (sIntensityT8ClutReady) {
        return;
    }

    for (unsigned i = 0; i < ARRAY_COUNT(sIntensityT8Clut); i++) {
        const uint32_t intensity = i;
        const uint32_t alpha = (intensity + 1U) >> 1;
        sIntensityT8Clut[i] = intensity | (intensity << 8) | (intensity << 16) | (alpha << 24);
    }

    for (unsigned i = 0; i < ARRAY_COUNT(sIntensityT8Clut); i++) {
        if ((i & 0x18U) == 0x08U) {
            const uint32_t temporary = sIntensityT8Clut[i];
            sIntensityT8Clut[i] = sIntensityT8Clut[i + 8U];
            sIntensityT8Clut[i + 8U] = temporary;
        }
    }

    SyncDCache(sIntensityT8Clut, (uint8_t*)sIntensityT8Clut + sizeof(sIntensityT8Clut));
    sIntensityT8ClutReady = true;
}

static int texture_psm(unsigned int type) {
    switch (type) {
        case TEXTURE_FORMAT_5551:
            return GS_PSM_CT16;

        case TEXTURE_FORMAT_T8:
            return GS_PSM_T8;

        default:
            return GS_PSM_CT32;
    }
}

static size_t texture_texel_size(int width, int height, int psm) {
    const size_t pixelCount = (size_t)width * (size_t)height;
    size_t bytesPerPixel;

    switch (psm) {
        case GS_PSM_CT16:
            bytesPerPixel = 2;
            break;

        case GS_PSM_T8:
            bytesPerPixel = 1;
            break;

        default:
            bytesPerPixel = 4;
            break;
    }

    return pixelCount * bytesPerPixel;
}

size_t texman_texture_backing_size(int width, int height, unsigned int type) {
    if (width <= 0 || height <= 0) {
        return 0;
    }

    const int psm = texture_psm(type);
    const size_t texelSize = texture_texel_size(width, height, psm);
    const size_t gsKitSize = (size_t)gsKit_texture_size(width, height, psm);
    const size_t requiredSize = texelSize > gsKitSize ? texelSize : gsKitSize;

    return ALIGN(requiredSize, 128);
}

static void allocate_texture_backing(struct Texture* texture, size_t backingSize, int psm, int width, int height) {
    GSTEXTURE* gsTexture = &texture->tex;
    const bool canReuse = gsTexture->Mem != NULL && texture->backing_size >= backingSize && gsTexture->PSM == psm &&
                          gsTexture->Width == (u32)width && gsTexture->Height == (u32)height;

    if (canReuse) {
        gsKit_TexManager_invalidate(gs_global, gsTexture);
        return;
    }

    if (gsTexture->Mem != NULL || texture->upload_serial != 0) {
        release_texture_residency(gsTexture);
    }

    gsTexture->Mem = alloc_tex_bytes(backingSize);
    texture->backing_size = backingSize;
}

static void configure_texture_clut(GSTEXTURE* texture) {
    texture->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (texture->PSM == GS_PSM_T8) {
        init_intensity_t8_clut();
        texture->ClutPSM = GS_PSM_CT32;
        texture->Clut = sIntensityT8Clut;
        return;
    }

    texture->ClutPSM = 0;
    texture->Clut = NULL;
}

static void copy_texture_pixels(GSTEXTURE* texture, const uint8_t* source, size_t pixelCount, size_t texelSize,
                                unsigned int type) {
    if (type == TEXTURE_FORMAT_5551 || type == TEXTURE_FORMAT_T8 || type == TEXTURE_FORMAT_8888_GS_NATIVE) {
        memcpy(texture->Mem, source, texelSize);
        return;
    }

    u32* destination = (u32*)texture->Mem;

    if (type == TEXTURE_FORMAT_4444) {
        const uint16_t* sourcePixels = (const uint16_t*)source;

        for (size_t i = 0; i < pixelCount; i++) {
            const uint16_t value = sourcePixels[i];
            const uint8_t red = (value & 0xf) * 17;
            const uint8_t green = ((value >> 4) & 0xf) * 17;
            const uint8_t blue = ((value >> 8) & 0xf) * 17;
            const uint8_t alpha = (uint8_t)(((((value >> 12) & 0xf) * 17) + 1) >> 1);

            destination[i] = red | (green << 8) | (blue << 16) | ((u32)alpha << 24);
        }

        return;
    }

    const u32* sourcePixels = (const u32*)source;

    for (size_t i = 0; i < pixelCount; i++) {
        const u32 value = sourcePixels[i];
        const u32 alpha = (((value >> 24) & 0xff) + 1) >> 1;

        destination[i] = (value & 0x00ffffffU) | (alpha << 24);
    }
}

static uint32_t texture_content_hash(const void* data, size_t size) {
    const uint8_t* bytes = data;
    uint32_t hash = 2166136261U;

    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }

    return hash;
}

static void upload_texture(const uint8_t* buffer, int width, int height, unsigned int type) {
    if (last_tex == NULL || width <= 0 || height <= 0) {
        return;
    }

    struct Texture* texture = last_tex;
    GSTEXTURE* gsTexture = &texture->tex;

    if (sPrerenderFallback.texture == texture) {
        sPrerenderFallback.valid = false;
    }

    const int psm = texture_psm(type);
    const size_t pixelCount = (size_t)width * (size_t)height;
    const size_t texelSize = texture_texel_size(width, height, psm);
    const size_t backingSize = texman_texture_backing_size(width, height, type);

    allocate_texture_backing(texture, backingSize, psm, width, height);

    gsTexture->Width = width;
    gsTexture->Height = height;
    gsTexture->PSM = psm;
    gsTexture->Filter = GS_FILTER_NEAREST;
    gsTexture->Vram = 0;
    gsTexture->VramClut = 0;

    configure_texture_clut(gsTexture);

    texture->upload_serial = ++sTextureUploadSerial;
    if (sTextureUploadSerial == 0) {
        sTextureUploadSerial = 1;
        texture->upload_serial = 1;
    }

    copy_texture_pixels(gsTexture, buffer, pixelCount, texelSize, type);

    if (backingSize > texelSize) {
        memset((uint8_t*)gsTexture->Mem + texelSize, 0, backingSize - texelSize);
    }

    texture->content_hash = 0;
    if (!sSkipContentHash && psm == GS_PSM_CT32 && width == 32 && height == 32) {
        texture->content_hash = texture_content_hash(gsTexture->Mem, texelSize);
    }

    SyncDCache(gsTexture->Mem, (u8*)gsTexture->Mem + backingSize);
}

float identity_matrix[4][4]
    __attribute__((aligned(16))) = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

int gfx_vram_space_available(void) {
    return tex_cache != NULL && tex_ptr < tex_end;
}

int texman_vram_space_available(size_t size) {
    if (tex_cache == NULL) {
        return 0;
    }
    return size <= (size_t)(tex_end - tex_ptr);
}

int texman_texture_slot_available(void) {
    return tex_count < MAX_TEXTURES;
}

void texman_clear(void) {
    /*
    sim, quando os 4 mb acabam isso espera a fila e reaproveita o bloco inteiro
    salvar só um pedaço deixa ponteiro velho nos caches e transforma a textura em lixo
    */
    sPrerenderFallback.valid = false;
    sPrerenderFallback.texture = NULL;

    if (gs_global != NULL) {
        gsKit_queue_exec(gs_global);
        dmaKit_wait_fast();
        gsKit_queue_reset(gs_global->Os_Queue);
        gsKit_TexManager_init(gs_global);
        sLastBoundGsTex = NULL;
        sLastTex0Reg = ~0ULL;
    }

    for (unsigned i = 0; i < ARRAY_COUNT(sBlendCache); i++) {
        sBlendCache[i].valid = false;
        sBlendCache[i].source = NULL;
        sBlendCache[i].source_serial = 0;
        sBlendCache[i].last_used_frame = 0;
        sBlendCache[i].tex.tex.Vram = 0;
        sBlendCache[i].tex.tex.VramClut = 0;
    }

    for (unsigned i = 0; i < ARRAY_COUNT(sReverseTintCache); i++) {
        sReverseTintCache[i].valid = false;
        sReverseTintCache[i].source = NULL;
        sReverseTintCache[i].source_serial = 0;
        sReverseTintCache[i].last_used_frame = 0;
        sReverseTintCache[i].tex.tex.Vram = 0;
        sReverseTintCache[i].tex.tex.VramClut = 0;
    }

    memset(tex_pool, 0, sizeof(tex_pool));
    tex_count = 0;
    cur_tex[0] = NULL;
    cur_tex[1] = NULL;
    last_tex = NULL;

    if (tex_cache != NULL) {
        tex_ptr = tex_cache;
        tex_end = tex_cache + TEXCACHE_SIZE;
    }
}

void texman_upload(int width, int height, unsigned int type, const void* buffer) {
    upload_texture((const uint8_t*)buffer, width, height, type);
}

static inline void bind_texture(GSTEXTURE* texture) {
    if (__builtin_expect(sLastBoundGsTex == texture && texture->Vram != 0, 1)) {
        return;
    }

    const u32 oldVram = texture->Vram;
    const unsigned transferred = gsKit_TexManager_bind(gs_global, texture);

    if (transferred || texture->Vram != oldVram || sLastBoundGsTex != texture) {
        sLastTex0Reg = ~0ULL;
        sLastTexFilter = -1;
    }
    sLastBoundGsTex = texture;
}

static inline uint32_t cm_ps2(uint32_t mode, uint32_t mask) {
    return ((mode & 2) || mask == 0) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
}

static void sampler(int tile, bool linear, uint32_t cms, uint32_t cmt, uint32_t masks, uint32_t maskt) {
    if (tile < 0 || tile > 1) {
        return;
    }

    sampler_linear[tile] = linear;
    sampler_clamp_s[tile] = cm_ps2(cms, masks);
    sampler_clamp_t[tile] = cm_ps2(cmt, maskt);
    sampler_cms[tile] = (uint8_t)cms;
    sampler_cmt[tile] = (uint8_t)cmt;
    sampler_masks[tile] = (uint8_t)masks;
    sampler_maskt[tile] = (uint8_t)maskt;

    if (cur_tex[tile] != NULL) {
        cur_tex[tile]->tex.Filter = linear ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
        cur_tex[tile]->clamp_s = sampler_clamp_s[tile];
        cur_tex[tile]->clamp_t = sampler_clamp_t[tile];
        cur_tex[tile]->cms = (uint8_t)cms;
        cur_tex[tile]->cmt = (uint8_t)cmt;
        cur_tex[tile]->masks = (uint8_t)masks;
        cur_tex[tile]->maskt = (uint8_t)maskt;
    }
}

static void env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    sEnvColor = r | (g << 8) | (b << 16) | (a << 24);
}

static void depth_test(bool enabled) {
    z_test = enabled;
}

static void depth_mask(bool write) {
    z_write = write;
    const u64 reg = GS_SETREG_ZBUF_1(gs_global->ZBuffer / 8192, gs_global->PSMZ, !write);

    if (reg == sLastZbufReg) {
        return;
    }

    sLastZbufReg = reg;
    u64* packet = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *packet++ = GIF_TAG_AD(1);
    *packet++ = GIF_AD;
    *packet++ = reg;
    *packet++ = GS_ZBUF_1 + gs_global->PrimContext;
}

static void zdecal(bool enabled) {
    z_decal = enabled;
    z_offset = enabled ? 32.0f : 0.0f;
}

static void viewport(int x, int y, int width, int height) {
    const int top = gs_global->Height - y - height;

    r_view.x = x;
    r_view.y = top;
    r_view.w = width;
    r_view.h = height;
    r_view.hw = width * 0.5f;
    r_view.hh = height * 0.5f;
    r_view.cx = x + r_view.hw;
    r_view.cy = top + r_view.hh;
}

static void set_scissor_raw(int x0, int y0, int x1, int y1) {
    const u64 reg = GS_SETREG_SCISSOR_1(x0, x1, y0, y1);

    if (reg == sLastScissorReg) {
        return;
    }

    sLastScissorReg = reg;
    u64* packet = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *packet++ = GIF_TAG_AD(1);
    *packet++ = GIF_AD;
    *packet++ = reg;
    *packet++ = GS_SCISSOR_1 + gs_global->PrimContext;
}

static void scissor(int x, int y, int width, int height) {
    const int top = gs_global->Height - y - height;

    r_clip.x0 = x;
    r_clip.y0 = top;
    r_clip.x1 = x + width - 1;
    r_clip.y1 = top + height - 1;

    if (r_clip.x0 < 0) {
        r_clip.x0 = 0;
    }
    if (r_clip.y0 < 0) {
        r_clip.y0 = 0;
    }
    if (r_clip.x1 >= gs_global->Width) {
        r_clip.x1 = gs_global->Width - 1;
    }
    if (r_clip.y1 >= gs_global->Height) {
        r_clip.y1 = gs_global->Height - 1;
    }

    set_scissor_raw(r_clip.x0, r_clip.y0, r_clip.x1, r_clip.y1);
}

static void set_blend(bool enabled) {
    const u64 reg = enabled ? BMODE_BLEND : 0;

    do_blend = enabled;
    gs_global->PrimAlphaEnable = enabled;
    gs_global->PrimAlpha = reg;

    if (reg == sLastAlphaReg) {
        return;
    }

    sLastAlphaReg = reg;
    u64* packet = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *packet++ = GIF_TAG_AD(1);
    *packet++ = GIF_AD;
    *packet++ = reg;
    *packet++ = GS_ALPHA_1 + gs_global->PrimContext;
}

static void set_alpha_enabled(bool enabled) {
    set_blend(enabled);
}

static inline unsigned ilog2ceil(unsigned v);

static void update_test(void) {
    gs_global->Test->ATE = cur_shader != NULL && cur_shader->alpha_test;
    gs_global->Test->ATST = gs_global->Test->ATE ? 6 : 1;
    gs_global->Test->AREF = (cur_shader != NULL && cur_shader->alpha_test) ? cur_shader->alpha_ref : 0;
    gs_global->Test->ZTE = 1;
    gs_global->Test->ZTST = z_test ? 2 : 1;

    const u64 reg =
        GS_SETREG_TEST(gs_global->Test->ATE, gs_global->Test->ATST, gs_global->Test->AREF, gs_global->Test->AFAIL,
                       gs_global->Test->DATE, gs_global->Test->DATM, gs_global->Test->ZTE, gs_global->Test->ZTST);
    if (reg == sLastTestReg) {
        return;
    }

    sLastTestReg = reg;
    u64* packet = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *packet++ = GIF_TAG_AD(1);
    *packet++ = GIF_AD;
    *packet++ = reg;
    *packet++ = GS_TEST_1 + gs_global->PrimContext;
}

/*
nem pensa em trocar isso por um repeat simples do gs porque cms, cmt e mask ainda seguem as regras do n64
clamp tem que ganhar de mirror e repeat, enquanto region repeat cuida quando o período é menor que a textura
essa porra também é o que mantém mirror + clamp dos olhos da epona funcionando, então deixa a ordem quieta
*/
static void clamp_tex(struct Texture* t, bool edge_s, bool edge_t) {
    uint32_t wms = t->clamp_s;
    uint32_t wmt = t->clamp_t;
    uint32_t minu = 0;
    uint32_t maxu = 0;
    uint32_t minv = 0;
    uint32_t maxv = 0;
    const uint32_t width = t->tex.Width > 0 ? (uint32_t)t->tex.Width : 1U;
    const uint32_t height = t->tex.Height > 0 ? (uint32_t)t->tex.Height : 1U;

    if ((t->cms & 2) || t->masks == 0) {
        wms = GS_CMODE_REGION_CLAMP;
        minu = 0;
        maxu = width - 1;
    } else if (!(t->cms & 1) && t->masks > 0 && t->masks < 10) {

        uint32_t period = 1u << t->masks;
        uint32_t texpot = 1u << ilog2ceil(width);
        if (period < texpot) {
            wms = GS_CMODE_REGION_REPEAT;
            minu = period - 1;
            maxu = 0;
        }
    } else if (!(t->cms & 1) && t->masks == 0 && edge_s && t->tex.Filter == GS_FILTER_LINEAR) {

        wms = GS_CMODE_REGION_CLAMP;
        minu = 0;
        maxu = width - 1;
    }
    if ((t->cmt & 2) || t->maskt == 0) {
        wmt = GS_CMODE_REGION_CLAMP;
        minv = 0;
        maxv = height - 1;
    } else if (!(t->cmt & 1) && t->maskt > 0 && t->maskt < 10) {
        uint32_t period = 1u << t->maskt;
        uint32_t texpot = 1u << ilog2ceil(height);
        if (period < texpot) {
            wmt = GS_CMODE_REGION_REPEAT;
            minv = period - 1;
            maxv = 0;
        }
    } else if (!(t->cmt & 1) && t->maskt == 0 && edge_t && t->tex.Filter == GS_FILTER_LINEAR) {
        wmt = GS_CMODE_REGION_CLAMP;
        minv = 0;
        maxv = height - 1;
    }
    gs_global->Clamp->WMS = wms;
    gs_global->Clamp->WMT = wmt;
    u64 reg = GS_SETREG_CLAMP(wms, wmt, minu, maxu, minv, maxv);
    if (reg == sLastClampReg) {
        return;
    }
    sLastClampReg = reg;
    u64* p = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = reg;
    *p++ = GS_CLAMP_1 + gs_global->PrimContext;
}

#define GIF_TAG_TRI_ST_REGS                                                                                      \
    ((u64)GS_PRIM << 0) | ((u64)GS_ST << 4) | ((u64)GS_RGBAQ << 8) | ((u64)GS_XYZ2 << 12) | ((u64)GS_ST << 16) | \
        ((u64)GS_RGBAQ << 20) | ((u64)GS_XYZ2 << 24) | ((u64)GS_ST << 28) | ((u64)GS_RGBAQ << 32) |              \
        ((u64)GS_XYZ2 << 36)
#define GIF_TAG_TRI_ST_FOG_REGS                                                                                   \
    ((u64)GS_PRIM << 0) | ((u64)GS_ST << 4) | ((u64)GS_RGBAQ << 8) | ((u64)GS_XYZF2 << 12) | ((u64)GS_ST << 16) | \
        ((u64)GS_RGBAQ << 20) | ((u64)GS_XYZF2 << 24) | ((u64)GS_ST << 28) | ((u64)GS_RGBAQ << 32) |              \
        ((u64)GS_XYZF2 << 36)
#define GIF_TAG_TRI_FOG_REGS                                                                    \
    ((u64)GS_PRIM << 0) | ((u64)GS_RGBAQ << 4) | ((u64)GS_XYZF2 << 8) | ((u64)GS_RGBAQ << 12) | \
        ((u64)GS_XYZF2 << 16) | ((u64)GS_RGBAQ << 20) | ((u64)GS_XYZF2 << 24) | ((u64)GIF_NOP << 28)
#define GIF_PRIM_TRIANGLE_GOURAUD_FOG 0x7F01
#define GIF_PRIM_TRIANGLE_TEXTURED_ST 0x7F02
#define GIF_PRIM_TRIANGLE_TEXTURED_ST_FOG 0x7F03
#define GIF_PRIM_SPRITE_TEXTURED 0x7F04
#define GIF_PRIM_TRIANGLE_GOURAUD 0x7F05

/*
esses helpers juntam vários triângulos no mesmo pacote gif porque mandar um pacote pra cada um acaba com o desempenho
same_obj e os tipos acima avisam até onde o gskit pode continuar escrevendo sem estourar o bloco reservado
*/
static inline unsigned ilog2ceil(unsigned v) {
    return v <= 1U ? 0U : (32U - (unsigned)__builtin_clz(v - 1U));
}

static void set_fog_color(uint32_t rgb) {
    if (rgb == sLastFogColor) {
        return;
    }
    sLastFogColor = rgb;
    u64* p = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = SETREG_FOGCOL(rgb & 0xff, (rgb >> 8) & 0xff, (rgb >> 16) & 0xff);
    *p++ = GS_FOGCOL;
}

static inline void set_texfilter_cached(int filter) {
    if (filter == sLastTexFilter) {
        return;
    }
    gsKit_set_texfilter(gs_global, filter);
    sLastTexFilter = filter;
}

static inline int filter_for_texture(const GSTEXTURE* tex) {
    if (tex != NULL && tex->PSM == GS_PSM_CT16 && tex->Width <= 4U && tex->Height <= 4U) {
        return GS_FILTER_NEAREST;
    }
    return tex != NULL ? tex->Filter : GS_FILTER_NEAREST;
}

static inline u64 tex0_reg(const GSTEXTURE* t, int tw, int th) {
    const bool indexed = t->VramClut != 0;
    return GS_SETREG_TEX0(t->Vram / 256, t->TBW, t->PSM, tw, th, (cur_shader && cur_shader->texture_alpha) ? 1 : 0,
                          cur_shader ? cur_shader->tfx : 0, indexed ? t->VramClut / 256 : 0, indexed ? t->ClutPSM : 0,
                          indexed ? t->ClutStorageMode : 0, 0,
                          indexed ? GS_CLUT_STOREMODE_LOAD : GS_CLUT_STOREMODE_NOLOAD);
}

static inline void set_tex0_cached(GSTEXTURE* t) {
    const int tw = ilog2ceil((unsigned)t->Width);
    const int th = ilog2ceil((unsigned)t->Height);
    const u64 reg = tex0_reg(t, tw, th);
    if (reg == sLastTex0Reg) {
        return;
    }
    sLastTex0Reg = reg;
    u64* p = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = reg;
    *p++ = GS_TEX0_1 + gs_global->PrimContext;
}

static inline float colorq_q(uint64_t word) {
    ColorQ c;
    c.word = word;
    return c.q;
}

static const float sMaskInvPeriod[10] = { 0.0f,     0.5f,      0.25f,      0.125f,      0.0625f,
                                          0.03125f, 0.015625f, 0.0078125f, 0.00390625f, 0.001953125f };

/*
se deixar coordenada repetida crescer pra caralho o gs perde precisão e a textura começa a virar merda
aqui saem períodos inteiros sem mudar a imagem, mirror dobra esse período e q continua junto pra manter a perspectiva
*/
static inline void normalize_repeat_axis(float* a1, float* a2, float* a3, float q1, float q2, float q3, uint8_t cm,
                                         uint8_t mask) {
    if ((cm & 2) || mask == 0 || mask >= 10) {
        return;
    }
    if (!__builtin_isfinite(q1) || !__builtin_isfinite(q2) || !__builtin_isfinite(q3)) {
        return;
    }
    if ((q1 > -0.000001f && q1 < 0.000001f) || (q2 > -0.000001f && q2 < 0.000001f) ||
        (q3 > -0.000001f && q3 < 0.000001f)) {
        return;
    }
    const float u1 = *a1 / q1;
    const float u2 = *a2 / q2;
    const float u3 = *a3 / q3;
    if (!__builtin_isfinite(u1) || !__builtin_isfinite(u2) || !__builtin_isfinite(u3)) {
        return;
    }
    float period = (float)(1u << mask);
    float invPeriod = sMaskInvPeriod[mask];

    if (cm & 1) {
        period *= 2.0f;
        invPeriod *= 0.5f;
    }
    const float center = (u1 + u2 + u3) * (1.0f / 3.0f);
    float cycles = center * invPeriod;
    const int whole = (int)(cycles >= 0.0f ? cycles + 0.5f : cycles - 0.5f);
    const float shift = (float)whole * period;
    *a1 = (u1 - shift) * q1;
    *a2 = (u2 - shift) * q2;
    *a3 = (u3 - shift) * q3;
}

static inline void normalize_repeat_axis_known(float* a1, float* a2, float* a3, float u1, float u2, float u3, float q1,
                                               float q2, float q3, uint8_t cm, uint8_t mask) {
    if ((cm & 2) || mask == 0 || mask >= 10) {
        return;
    }
    if (!__builtin_isfinite(u1) || !__builtin_isfinite(u2) || !__builtin_isfinite(u3)) {
        return;
    }
    float period = (float)(1u << mask);
    float invPeriod = sMaskInvPeriod[mask];
    if (cm & 1) {
        period *= 2.0f;
        invPeriod *= 0.5f;
    }
    const float center = (u1 + u2 + u3) * (1.0f / 3.0f);
    const float cycles = center * invPeriod;
    const int whole = (int)(cycles >= 0.0f ? cycles + 0.5f : cycles - 0.5f);
    const float shift = (float)whole * period;
    *a1 = (u1 - shift) * q1;
    *a2 = (u2 - shift) * q2;
    *a3 = (u3 - shift) * q3;
}

static inline void tri_tex_emit_reserved_known(u64** pp, struct Texture* pt, float x1, float y1, int z1, float s1,
                                               float tt1, float ru1, float rv1, uint64_t c1, float x2, float y2, int z2,
                                               float s2, float tt2, float ru2, float rv2, uint64_t c2, float x3,
                                               float y3, int z3, float s3, float tt3, float ru3, float rv3, uint64_t c3,
                                               bool fog, uint8_t f1, uint8_t f2, uint8_t f3) {
    const float q1 = colorq_q(c1);
    const float q2 = colorq_q(c2);
    const float q3 = colorq_q(c3);
    normalize_repeat_axis_known(&s1, &s2, &s3, ru1, ru2, ru3, q1, q2, q3, pt->cms, pt->masks);
    normalize_repeat_axis_known(&tt1, &tt2, &tt3, rv1, rv2, rv3, q1, q2, q3, pt->cmt, pt->maskt);
    const int ix1 = gsKit_float_to_int_x(gs_global, x1);
    const int iy1 = gsKit_float_to_int_y(gs_global, y1);
    const int ix2 = gsKit_float_to_int_x(gs_global, x2);
    const int iy2 = gsKit_float_to_int_y(gs_global, y2);
    const int ix3 = gsKit_float_to_int_x(gs_global, x3);
    const int iy3 = gsKit_float_to_int_y(gs_global, y3);
    TexCoord a = { { s1, tt1 } };
    TexCoord b = { { s2, tt2 } };
    TexCoord c = { { s3, tt3 } };
    u64* p = *pp;
    *p++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 1, fog ? 1 : 0, gs_global->PrimAlphaEnable, 0, 0,
                          gs_global->PrimContext, 0);
    *p++ = a.word;
    *p++ = c1;
    *p++ = (fog ? GS_SETREG_XYZF2(ix1, iy1, z1, f1) : GS_SETREG_XYZ2(ix1, iy1, z1));
    *p++ = b.word;
    *p++ = c2;
    *p++ = (fog ? GS_SETREG_XYZF2(ix2, iy2, z2, f2) : GS_SETREG_XYZ2(ix2, iy2, z2));
    *p++ = c.word;
    *p++ = c3;
    *p++ = (fog ? GS_SETREG_XYZF2(ix3, iy3, z3, f3) : GS_SETREG_XYZ2(ix3, iy3, z3));
    *pp = p;
}

static inline void tri_tex_emit_reserved(u64** pp, struct Texture* pt, float x1, float y1, int z1, float s1, float tt1,
                                         uint64_t c1, float x2, float y2, int z2, float s2, float tt2, uint64_t c2,
                                         float x3, float y3, int z3, float s3, float tt3, uint64_t c3, bool fog,
                                         uint8_t f1, uint8_t f2, uint8_t f3) {
    const float q1 = colorq_q(c1);
    const float q2 = colorq_q(c2);
    const float q3 = colorq_q(c3);
    normalize_repeat_axis(&s1, &s2, &s3, q1, q2, q3, pt->cms, pt->masks);
    normalize_repeat_axis(&tt1, &tt2, &tt3, q1, q2, q3, pt->cmt, pt->maskt);
    const int ix1 = gsKit_float_to_int_x(gs_global, x1);
    const int iy1 = gsKit_float_to_int_y(gs_global, y1);
    const int ix2 = gsKit_float_to_int_x(gs_global, x2);
    const int iy2 = gsKit_float_to_int_y(gs_global, y2);
    const int ix3 = gsKit_float_to_int_x(gs_global, x3);
    const int iy3 = gsKit_float_to_int_y(gs_global, y3);
    TexCoord a = { { s1, tt1 } };
    TexCoord b = { { s2, tt2 } };
    TexCoord c = { { s3, tt3 } };
    u64* p = *pp;
    *p++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 1, fog ? 1 : 0, gs_global->PrimAlphaEnable, 0, 0,
                          gs_global->PrimContext, 0);
    *p++ = a.word;
    *p++ = c1;
    *p++ = (fog ? GS_SETREG_XYZF2(ix1, iy1, z1, f1) : GS_SETREG_XYZ2(ix1, iy1, z1));
    *p++ = b.word;
    *p++ = c2;
    *p++ = (fog ? GS_SETREG_XYZF2(ix2, iy2, z2, f2) : GS_SETREG_XYZ2(ix2, iy2, z2));
    *p++ = c.word;
    *p++ = c3;
    *p++ = (fog ? GS_SETREG_XYZF2(ix3, iy3, z3, f3) : GS_SETREG_XYZ2(ix3, iy3, z3));
    *pp = p;
}

static size_t tri_tex_batch_safe_count(size_t requested, bool fog) {
    const int type = fog ? GIF_PRIM_TRIANGLE_TEXTURED_ST_FOG : GIF_PRIM_TRIANGLE_TEXTURED_ST;
    size_t room = GS_GIF_BLOCKSIZE;

    if (gs_global->CurQueue->last_type == type && gs_global->CurQueue->same_obj > 0 &&
        gs_global->CurQueue->same_obj < GS_GIF_BLOCKSIZE) {
        room = (size_t)(GS_GIF_BLOCKSIZE - gs_global->CurQueue->same_obj);
    }
    if (room == 0) {
        room = 1;
    }
    return requested < room ? requested : room;
}

static u64* tri_tex_batch_begin(struct Texture* pt, size_t count, bool fog) {
    if (count == 0) {
        return NULL;
    }
    GSTEXTURE* t = &pt->tex;
    set_texfilter_cached(filter_for_texture(t));
    set_tex0_cached(t);
    const int type = fog ? GIF_PRIM_TRIANGLE_TEXTURED_ST_FOG : GIF_PRIM_TRIANGLE_TEXTURED_ST;

    if (count > GS_GIF_BLOCKSIZE) {
        count = GS_GIF_BLOCKSIZE;
    }
    u64* p_store = (u64*)gsKit_heap_alloc(gs_global, (int)(5U * count), (int)(80U * count), type);
    u64* p = p_store;
    if (p_store == gs_global->CurQueue->last_tag) {
        *p++ = GIF_TAG(0, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, 10);
        *p++ = fog ? GIF_TAG_TRI_ST_FOG_REGS : GIF_TAG_TRI_ST_REGS;
    }
    if (count > 1) {
        gs_global->CurQueue->same_obj += (int)(count - 1U);
    }
    return p;
}

static void tri_tex(struct Texture* pt, float x1, float y1, int z1, float s1, float tt1, uint64_t c1, float x2,
                    float y2, int z2, float s2, float tt2, uint64_t c2, float x3, float y3, int z3, float s3, float tt3,
                    uint64_t c3, bool fog, uint8_t f1, uint8_t f2, uint8_t f3) {
    u64* p = tri_tex_batch_begin(pt, 1, fog);
    tri_tex_emit_reserved(&p, pt, x1, y1, z1, s1, tt1, c1, x2, y2, z2, s2, tt2, c2, x3, y3, z3, s3, tt3, c3, fog, f1,
                          f2, f3);
}

static size_t tri_col_batch_safe_count(size_t requested, bool fog) {
    const int type = fog ? GIF_PRIM_TRIANGLE_GOURAUD_FOG : GIF_PRIM_TRIANGLE_GOURAUD;
    size_t room = GS_GIF_BLOCKSIZE;
    if (gs_global->CurQueue->last_type == type && gs_global->CurQueue->same_obj > 0 &&
        gs_global->CurQueue->same_obj < GS_GIF_BLOCKSIZE) {
        room = (size_t)(GS_GIF_BLOCKSIZE - gs_global->CurQueue->same_obj);
    }
    if (room == 0) {
        room = 1;
    }
    return requested < room ? requested : room;
}

static u64* tri_col_batch_begin(size_t count, bool fog) {
    if (count == 0) {
        return NULL;
    }
    const int type = fog ? GIF_PRIM_TRIANGLE_GOURAUD_FOG : GIF_PRIM_TRIANGLE_GOURAUD;
    if (count > GS_GIF_BLOCKSIZE) {
        count = GS_GIF_BLOCKSIZE;
    }
    u64* p_store = (u64*)gsKit_heap_alloc(gs_global, (int)(4U * count), (int)(64U * count), type);
    u64* p = p_store;
    if (p_store == gs_global->CurQueue->last_tag) {
        *p++ = GIF_TAG_TRIANGLE_GOURAUD(0);
        *p++ = fog ? GIF_TAG_TRI_FOG_REGS : GIF_TAG_TRIANGLE_GOURAUD_REGS;
    }
    if (count > 1) {
        gs_global->CurQueue->same_obj += (int)(count - 1U);
    }
    return p;
}

static inline void tri_col_emit_reserved(u64** pp, float x1, float y1, int z1, uint64_t c1, float x2, float y2, int z2,
                                         uint64_t c2, float x3, float y3, int z3, uint64_t c3, bool fog, uint8_t f1,
                                         uint8_t f2, uint8_t f3) {
    const int ix1 = gsKit_float_to_int_x(gs_global, x1);
    const int iy1 = gsKit_float_to_int_y(gs_global, y1);
    const int ix2 = gsKit_float_to_int_x(gs_global, x2);
    const int iy2 = gsKit_float_to_int_y(gs_global, y2);
    const int ix3 = gsKit_float_to_int_x(gs_global, x3);
    const int iy3 = gsKit_float_to_int_y(gs_global, y3);
    u64* p = *pp;
    *p++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 0, fog ? 1 : 0, gs_global->PrimAlphaEnable, 0, 0,
                          gs_global->PrimContext, 0);
    *p++ = c1;
    *p++ = (fog ? GS_SETREG_XYZF2(ix1, iy1, z1, f1) : GS_SETREG_XYZ2(ix1, iy1, z1));
    *p++ = c2;
    *p++ = (fog ? GS_SETREG_XYZF2(ix2, iy2, z2, f2) : GS_SETREG_XYZ2(ix2, iy2, z2));
    *p++ = c3;
    *p++ = (fog ? GS_SETREG_XYZF2(ix3, iy3, z3, f3) : GS_SETREG_XYZ2(ix3, iy3, z3));
    *p++ = 0;
    *pp = p;
}

static void sprite_tex(GSTEXTURE* t, float x1, float y1, int z1, float u1, float v1, float x2, float y2, int z2,
                       float u2, float v2, uint64_t col) {
    set_texfilter_cached(filter_for_texture(t));
    const int tw = ilog2ceil((unsigned)t->Width);
    const int th = ilog2ceil((unsigned)t->Height);
    const int ix1 = gsKit_float_to_int_x(gs_global, x1);
    const int iy1 = gsKit_float_to_int_y(gs_global, y1);
    const int ix2 = gsKit_float_to_int_x(gs_global, x2);
    const int iy2 = gsKit_float_to_int_y(gs_global, y2);
    const int iu1 = gsKit_float_to_int_u(t, u1);
    const int iv1 = gsKit_float_to_int_v(t, v1);
    const int iu2 = gsKit_float_to_int_u(t, u2);
    const int iv2 = gsKit_float_to_int_v(t, v2);
    u64* p_store;
    u64* p;
    p_store = p = gsKit_heap_alloc(gs_global, 4, 64, GIF_PRIM_SPRITE_TEXTURED);
    if (p_store == gs_global->CurQueue->last_tag) {
        *p++ = GIF_TAG_SPRITE_TEXTURED(0);
        *p++ = GIF_TAG_SPRITE_TEXTURED_REGS(gs_global->PrimContext);
    }
    *p++ = tex0_reg(t, tw, th);
    sLastTex0Reg = ~0ULL;
    *p++ = GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 1, gs_global->PrimFogEnable, gs_global->PrimAlphaEnable,
                          gs_global->PrimAAEnable, 1, gs_global->PrimContext, 0);
    *p++ = col;
    *p++ = GS_SETREG_UV(iu1, iv1);
    *p++ = GS_SETREG_XYZ2(ix1, iy1, z1);
    *p++ = GS_SETREG_UV(iu2, iv2);
    *p++ = GS_SETREG_XYZ2(ix2, iy2, z2);
    *p++ = 0;
}

static inline void set_frame_write_mask(u32 mask) {
    u64* p = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_FRAME_1(gs_global->ScreenBuffer[gs_global->ActiveBuffer & 1] / 8192, gs_global->Width / 64,
                             gs_global->PSM, mask);
    *p++ = GS_FRAME_1 + gs_global->PrimContext;
}

void gfx_ps2_set_prerender_depth_only(bool enabled) {
    sPrerenderDepthOnlyPass = enabled;
    set_frame_write_mask(enabled ? 0xffffffffU : 0U);
}

/*
quando o gs não consegue fechar o combine sozinho a cpu monta uma cópia rgba da textura
essas funções leem ct32, ct16 e t8 do mesmo jeito pra tint e blend não precisarem adivinhar o formato original
*/
static inline bool texture_cpu_rgba_supported(const struct Texture* source) {
    return source != NULL && source->tex.Mem != NULL && source->tex.Width > 0 && source->tex.Height > 0 &&
           (source->tex.PSM == GS_PSM_CT32 || source->tex.PSM == GS_PSM_CT16 || source->tex.PSM == GS_PSM_T8);
}

static inline uint32_t texture_cpu_rgba(const struct Texture* source, size_t index) {
    if (source->tex.PSM == GS_PSM_CT32) {
        return ((const uint32_t*)source->tex.Mem)[index];
    }
    if (source->tex.PSM == GS_PSM_T8) {
        const uint32_t v = ((const uint8_t*)source->tex.Mem)[index];
        return v | (v << 8) | (v << 16) | (((v + 1U) >> 1) << 24);
    }
    const uint16_t v = ((const uint16_t*)source->tex.Mem)[index];
    const uint32_t r = (uint32_t)(v & 0x1fU) * 255U / 31U;
    const uint32_t g = (uint32_t)((v >> 5) & 0x1fU) * 255U / 31U;
    const uint32_t b = (uint32_t)((v >> 10) & 0x1fU) * 255U / 31U;
    const uint32_t a = (v & 0x8000U) ? 0x80U : 0U;
    return r | (g << 8) | (b << 16) | (a << 24);
}

static struct Texture* reverse_tint_texture(struct Texture* source, uint32_t prim, uint32_t env) {
    /*
    essa conta é o combine prim + (env - prim) * texel e não uma cor tirada do nada
    o alpha original fica e a cópia entra no cache porque refazer a textura por triângulo mataria a cpu do ps2
    */
    const size_t pixels = (size_t)source->tex.Width * (size_t)source->tex.Height;
    const size_t bytes = pixels * 4U;
    const uint32_t primRgb = prim & 0x00ffffffU;
    const uint32_t envRgb = env & 0x00ffffffU;
    ReverseTintCacheEntry* entry = NULL;
    ReverseTintCacheEntry* sameSourceOld = NULL;
    ReverseTintCacheEntry* victim = NULL;

    if (!texture_cpu_rgba_supported(source) || pixels == 0) {
        return source;
    }

    for (unsigned i = 0; i < REVERSE_TINT_CACHE_SLOTS; i++) {
        ReverseTintCacheEntry* e = &sReverseTintCache[i];
        if (e->valid && e->source == source && e->source_serial == source->upload_serial && e->prim_rgb == primRgb &&
            e->env_rgb == envRgb) {
            e->tex.tex.Filter = source->tex.Filter;
            e->tex.clamp_s = source->clamp_s;
            e->tex.clamp_t = source->clamp_t;
            e->tex.cms = source->cms;
            e->tex.cmt = source->cmt;
            e->tex.masks = source->masks;
            e->tex.maskt = source->maskt;
            e->stamp = ++sReverseTintStamp;
            e->last_used_frame = sRenderFrameSerial;
            return &e->tex;
        }
        if (!e->valid && entry == NULL) {
            entry = e;
        }
        if (e->valid && e->source == source && e->source_serial == source->upload_serial &&
            e->last_used_frame != sRenderFrameSerial && (sameSourceOld == NULL || e->stamp < sameSourceOld->stamp)) {
            sameSourceOld = e;
        }
        if (e->valid && (victim == NULL || e->stamp < victim->stamp)) {
            victim = e;
        }
    }

    if (sameSourceOld != NULL) {
        entry = sameSourceOld;
    }
    if (entry == NULL) {
        entry = victim;
        if (entry == NULL) {
            return source;
        }
    }
    if (entry->valid) {
        if (entry->last_used_frame == sRenderFrameSerial && gs_global && gs_global->Os_Queue->tag_size != 0) {
            gsKit_queue_exec(gs_global);
            dmaKit_wait_fast();
            gsKit_queue_reset(gs_global->Os_Queue);
        }
        if (entry->tex.tex.Mem != NULL) {
            release_texture_residency(&entry->tex.tex);
        }
    }

    if (entry->capacity < bytes) {
        if (entry->mem) {
            free(entry->mem);
        }
        entry->capacity = ALIGN(bytes, 128);
        entry->mem = memalign(128, entry->capacity);
        if (!entry->mem) {
            entry->capacity = 0;
            entry->valid = false;
            return source;
        }
    }

    uint32_t* out = (uint32_t*)entry->mem;
    const int pr = (int)(primRgb & 0xff);
    const int pg = (int)((primRgb >> 8) & 0xff);
    const int pb = (int)((primRgb >> 16) & 0xff);
    const int er = (int)(envRgb & 0xff);
    const int eg = (int)((envRgb >> 8) & 0xff);
    const int eb = (int)((envRgb >> 16) & 0xff);

    for (size_t i = 0; i < pixels; i++) {
        const uint32_t v = texture_cpu_rgba(source, i);
        const int tr = (int)(v & 0xff);
        const int tg = (int)((v >> 8) & 0xff);
        const int tb = (int)((v >> 16) & 0xff);
        int r = pr + ((er - pr) * tr + 127) / 255;
        int g = pg + ((eg - pg) * tg + 127) / 255;
        int b = pb + ((eb - pb) * tb + 127) / 255;
        if (r < 0) {
            r = 0;
        } else if (r > 255) {
            r = 255;
        }
        if (g < 0) {
            g = 0;
        } else if (g > 255) {
            g = 255;
        }
        if (b < 0) {
            b = 0;
        } else if (b > 255) {
            b = 255;
        }
        out[i] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (v & 0xff000000U);
    }
    SyncDCache(entry->mem, entry->mem + bytes);
    entry->tex = *source;
    entry->tex.tex.Mem = (u32*)entry->mem;
    entry->tex.tex.PSM = GS_PSM_CT32;
    entry->tex.tex.Vram = 0;
    entry->tex.tex.VramClut = 0;
    entry->tex.backing_size = entry->capacity;
    if (entry->tex.tex.Width == 64 && entry->tex.tex.Height == 64) {
        entry->tex.clamp_s = GS_CMODE_CLAMP;
        entry->tex.clamp_t = GS_CMODE_CLAMP;
    }
    entry->source = source;
    entry->source_serial = source->upload_serial;
    entry->prim_rgb = primRgb;
    entry->env_rgb = envRgb;
    entry->stamp = ++sReverseTintStamp;
    entry->last_used_frame = sRenderFrameSerial;
    entry->valid = true;
    return &entry->tex;
}

static inline uint32_t reverse_neutral_color(uint32_t rgba) {
    return 0x00ffffffU | (rgba & 0xff000000U);
}

/*
esse cache segura as cópias rgba de blend e decal e joga fora a mais velha quando a memória enche
se a dma ainda estiver lendo uma cópia usada neste frame a fila termina antes de alguém sobrescrever a porra da memória
*/
static void blend_cache_sync_if_live(const BlendCacheEntry* entry) {
    if (entry == NULL || !entry->valid || entry->last_used_frame != sRenderFrameSerial || !gs_global) {
        return;
    }
    if (gs_global->Os_Queue->tag_size != 0) {
        gsKit_queue_exec(gs_global);
        dmaKit_wait_fast();
        gsKit_queue_reset(gs_global->Os_Queue);
    }
}

static void blend_cache_free_entry(BlendCacheEntry* entry) {
    if (entry == NULL) {
        return;
    }
    blend_cache_sync_if_live(entry);
    if (entry->tex.tex.Mem != NULL) {
        release_texture_residency(&entry->tex.tex);
    }
    if (entry->mem != NULL) {
        if (sBlendCacheBytes >= entry->capacity) {
            sBlendCacheBytes -= entry->capacity;
        } else {
            sBlendCacheBytes = 0;
        }
        free(entry->mem);
    }
    entry->mem = NULL;
    entry->capacity = 0;
    entry->valid = false;
    entry->source = NULL;
    entry->source_serial = 0;
    entry->kind = 0;
    entry->last_used_frame = 0;
    entry->tex.tex.Mem = NULL;
    entry->tex.tex.Vram = 0;
    entry->tex.tex.VramClut = 0;
}

static bool blend_cache_make_room(BlendCacheEntry* keep, const struct Texture* source, size_t new_capacity) {
    const size_t keep_capacity = (keep != NULL) ? keep->capacity : 0;
    if (new_capacity > BLEND_CACHE_BUDGET) {
        return false;
    }
    while (sBlendCacheBytes - ((sBlendCacheBytes >= keep_capacity) ? keep_capacity : 0) + new_capacity >
           BLEND_CACHE_BUDGET) {
        BlendCacheEntry* victim = NULL;
        for (unsigned i = 0; i < BLEND_CACHE_SLOTS; i++) {
            BlendCacheEntry* e = &sBlendCache[i];
            if (e == keep || e->mem == NULL || &e->tex == source) {
                continue;
            }
            if (victim == NULL || e->stamp < victim->stamp) {
                victim = e;
            }
        }
        if (victim == NULL) {
            return false;
        }
        blend_cache_free_entry(victim);
    }
    return true;
}

enum {
    DERIVED_BLEND = 0,
    DERIVED_DECAL = 1,
};

static struct Texture* derived_texture(struct Texture* source, uint32_t base, uint32_t target, uint8_t kind) {
    if (!texture_cpu_rgba_supported(source)) {
        return source;
    }
    const uint32_t baseRgb = base & 0x00ffffffU;
    const uint32_t targetRgb = target & 0x00ffffffU;
    const size_t pixels = (size_t)source->tex.Width * (size_t)source->tex.Height;
    const size_t bytes = pixels * 4U;
    BlendCacheEntry* entry = NULL;
    BlendCacheEntry* sameSourceOld = NULL;
    BlendCacheEntry* victim = NULL;
    for (unsigned i = 0; i < BLEND_CACHE_SLOTS; i++) {
        BlendCacheEntry* e = &sBlendCache[i];
        if (e->valid && e->source == source && e->source_serial == source->upload_serial && e->base_rgb == baseRgb &&
            e->target_rgb == targetRgb && e->kind == kind) {
            e->tex.tex.Filter = source->tex.Filter;
            e->tex.clamp_s = source->clamp_s;
            e->tex.clamp_t = source->clamp_t;
            e->tex.cms = source->cms;
            e->tex.cmt = source->cmt;
            e->tex.masks = source->masks;
            e->tex.maskt = source->maskt;
            e->stamp = ++sBlendStamp;
            e->last_used_frame = sRenderFrameSerial;
            return &e->tex;
        }
        if (!e->valid && entry == NULL) {
            entry = e;
        }
        if (e->valid && e->source == source && e->source_serial == source->upload_serial && e->kind == kind &&
            e->last_used_frame != sRenderFrameSerial && (sameSourceOld == NULL || e->stamp < sameSourceOld->stamp)) {
            sameSourceOld = e;
        }
        if (e->valid && (victim == NULL || e->stamp < victim->stamp)) {
            victim = e;
        }
    }

    if (sameSourceOld != NULL) {
        entry = sameSourceOld;
    }
    if (entry == NULL) {
        entry = victim;
        if (entry == NULL) {
            return source;
        }
    }
    if (entry->valid) {
        if (entry->last_used_frame == sRenderFrameSerial && gs_global && gs_global->Os_Queue->tag_size != 0) {
            gsKit_queue_exec(gs_global);
            dmaKit_wait_fast();
            gsKit_queue_reset(gs_global->Os_Queue);
        }
        if (entry->tex.tex.Mem != NULL) {
            release_texture_residency(&entry->tex.tex);
        }
    }
    if (entry->capacity < bytes) {
        const size_t new_capacity = ALIGN(bytes, 128);
        if (!blend_cache_make_room(entry, source, new_capacity)) {
            return source;
        }
        if (entry->mem) {
            if (sBlendCacheBytes >= entry->capacity) {
                sBlendCacheBytes -= entry->capacity;
            } else {
                sBlendCacheBytes = 0;
            }
            free(entry->mem);
        }
        entry->capacity = new_capacity;
        entry->mem = memalign(128, entry->capacity);
        if (!entry->mem) {
            entry->capacity = 0;
            entry->valid = false;
            entry->tex.tex.Mem = NULL;
            entry->tex.tex.Vram = 0;
            entry->tex.tex.VramClut = 0;
            return source;
        }
        sBlendCacheBytes += entry->capacity;
    }
    uint32_t* out = (uint32_t*)entry->mem;
    const int br = (int)(baseRgb & 0xff);
    const int bg = (int)((baseRgb >> 8) & 0xff);
    const int bb = (int)((baseRgb >> 16) & 0xff);
    const int tr = (int)(targetRgb & 0xff);
    const int tg = (int)((targetRgb >> 8) & 0xff);
    const int tb = (int)((targetRgb >> 16) & 0xff);

    for (size_t i = 0; i < pixels; i++) {
        const uint32_t v = texture_cpu_rgba(source, i);
        const int ir = (int)(v & 0xff);
        const int ig = (int)((v >> 8) & 0xff);
        const int ib = (int)((v >> 16) & 0xff);
        int r;
        int g;
        int b;
        if (kind == DERIVED_DECAL) {

            int a = (int)((v >> 24) & 0xff);
            if (a > 0x80) {
                a = 0x80;
            }

            r = (br * (128 - a) + ir * a + 64) >> 7;
            g = (bg * (128 - a) + ig * a + 64) >> 7;
            b = (bb * (128 - a) + ib * a + 64) >> 7;
        } else {

            r = br + ((tr - br) * ir + 127) / 255;
            g = bg + ((tg - bg) * ig + 127) / 255;
            b = bb + ((tb - bb) * ib + 127) / 255;
        }
        if (r < 0) {
            r = 0;
        } else if (r > 255) {
            r = 255;
        }
        if (g < 0) {
            g = 0;
        } else if (g > 255) {
            g = 255;
        }
        if (b < 0) {
            b = 0;
        } else if (b > 255) {
            b = 255;
        }
        out[i] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (v & 0xff000000U);
    }
    SyncDCache(entry->mem, entry->mem + bytes);
    entry->tex = *source;
    entry->tex.tex.Mem = (u32*)entry->mem;
    entry->tex.tex.PSM = GS_PSM_CT32;
    entry->tex.tex.Vram = 0;
    entry->tex.tex.VramClut = 0;
    entry->tex.backing_size = entry->capacity;
    entry->source = source;
    entry->source_serial = source->upload_serial;
    entry->base_rgb = baseRgb;
    entry->target_rgb = targetRgb;
    entry->kind = kind;
    entry->stamp = ++sBlendStamp;
    entry->last_used_frame = sRenderFrameSerial;
    entry->valid = true;
    return &entry->tex;
}

static struct Texture* blend_texture(struct Texture* source, uint32_t base, uint32_t target) {
    return derived_texture(source, base, target, DERIVED_BLEND);
}

static struct Texture* decal_texture(struct Texture* source, uint32_t base) {
    return derived_texture(source, base, 0, DERIVED_DECAL);
}

static inline bool is_pause_name_tint(const struct Texture* tex, uint32_t rgba) {
    return cur_shader != NULL && cur_shader->shader_id == 0x11045551U && tex != NULL && do_blend &&
           tex->tex.Width == 128 && tex->tex.Height == 16 && (rgba & 0x00ffffffU) == 0x00281e14U;
}

#define DEKU_WATER_TEX_HASH 0x95512fd3U
#define DEKU_WATER_MIN_SHADE 157U

static inline bool is_deku_water_material(const struct Texture* tex) {
    return cur_shader != NULL && cur_shader->shader_id == 0x03200045U && tex != NULL && do_blend &&
           tex->tex.PSM == GS_PSM_CT32 && tex->tex.Width == 32 && tex->tex.Height == 32 && tex->masks == 5 &&
           tex->maskt == 5 && tex->content_hash == DEKU_WATER_TEX_HASH;
}

static inline uint32_t deku_water_color(uint32_t rgba) {
    unsigned r = rgba & 0xffU;
    unsigned g = (rgba >> 8) & 0xffU;
    unsigned b = (rgba >> 16) & 0xffU;
    if (r < DEKU_WATER_MIN_SHADE) {
        r = DEKU_WATER_MIN_SHADE;
    }
    if (g < DEKU_WATER_MIN_SHADE) {
        g = DEKU_WATER_MIN_SHADE;
    }
    if (b < DEKU_WATER_MIN_SHADE) {
        b = DEKU_WATER_MIN_SHADE;
    }
    return (rgba & 0xff000000U) | r | (g << 8) | (b << 16);
}

static inline uint32_t target_rgb_preserve_alpha(uint32_t rgba) {
    return (sEnvColor & 0x00ffffffU) | (rgba & 0xff000000U);
}

static void set_alpha_packet(uint64_t alpha, bool enable);

enum PassColorMode {
    PASS_ORIGINAL = 0,
    PASS_NEUTRAL_RGB,
    PASS_TARGET_RGB,
};

static inline uint32_t pass_color(uint32_t rgba, enum PassColorMode mode, uint32_t target) {
    if (mode == PASS_NEUTRAL_RGB) {
        return 0x00ffffffU | (rgba & 0xff000000U);
    }
    if (mode == PASS_TARGET_RGB) {
        return (target & 0x00ffffffU) | (rgba & 0xff000000U);
    }
    return rgba;
}

static void draw_fast_untextured_pass(const FastVertex* v, size_t tris) {
    for (size_t i = 0; i < tris; i++) {
        const FastVertex* t = &v[i * 3U];
        float x[3];
        float y[3];
        int z[3];
        for (int k = 0; k < 3; k++) {
            x[k] = t[k].x;
            y[k] = t[k].y;
            z[k] = (int)t[k].z;
        }
        gsKit_prim_triangle_gouraud_3d(
            gs_global, x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2], colorq_from_rgba(t[0].rgba, t[0].q, false),
            colorq_from_rgba(t[1].rgba, t[1].q, false), colorq_from_rgba(t[2].rgba, t[2].q, false));
    }
}

static void draw_fast_textured_pass(const FastVertex* v, size_t tris, struct Texture* tex, enum PassColorMode mode,
                                    uint32_t target) {
    if (tex == NULL) {
        return;
    }
    bind_texture(&tex->tex);
    clamp_tex(tex, false, false);
    sLastTex0Reg = ~0ULL;
    size_t done = 0;
    while (done < tris) {
        const size_t wanted = ((tris - done) > GS_GIF_BLOCKSIZE) ? GS_GIF_BLOCKSIZE : (tris - done);
        const size_t batch = tri_tex_batch_safe_count(wanted, false);
        u64* packet = tri_tex_batch_begin(tex, batch, false);
        for (size_t i = 0; i < batch; i++) {
            const FastVertex* t = &v[(done + i) * 3U];
            uint32_t c[3];
            float x[3];
            float y[3];
            int z[3];
            for (int k = 0; k < 3; k++) {
                c[k] = pass_color(t[k].rgba, mode, target);
                x[k] = t[k].x;
                y[k] = t[k].y;
                z[k] = (int)t[k].z;
            }
            tri_tex_emit_reserved_known(&packet, tex, x[0], y[0], z[0], t[0].u, t[0].v, t[0].real_u, t[0].real_v,
                                        colorq_from_rgba(c[0], t[0].q, true), x[1], y[1], z[1], t[1].u, t[1].v,
                                        t[1].real_u, t[1].real_v, colorq_from_rgba(c[1], t[1].q, true), x[2], y[2],
                                        z[2], t[2].u, t[2].v, t[2].real_u, t[2].real_v,
                                        colorq_from_rgba(c[2], t[2].q, true), false, 255, 255, 255);
        }
        done += batch;
    }
}

/*
isso parece desenho repetido porque blend e decal do n64 não cabem num único estado do gs
cada passe resolve uma parte do combine e no final alpha, z e tfx voltam como estavam pra não ferrar o próximo material
*/
static bool draw_blend_exact(const FastVertex* v, size_t tris, struct Texture* source) {
    if (v == NULL || tris == 0 || source == NULL || cur_shader == NULL) {
        return false;
    }

    const bool saved_tex_alpha = cur_shader->texture_alpha;
    const uint8_t saved_tfx = cur_shader->tfx;
    const bool saved_z_write = z_write;
    const bool saved_blend = do_blend;
    const u64 saved_alpha = gs_global->PrimAlpha;
    const bool saved_alpha_enable = gs_global->PrimAlphaEnable;
    struct Texture* alpha_mask = NULL;

    if (saved_tex_alpha) {
        alpha_mask = blend_texture(source, 0x00ffffffU, 0x00ffffffU);
        if (alpha_mask == NULL || alpha_mask == source) {
            return false;
        }
    }

    if (saved_tex_alpha) {
        cur_shader->tfx = 0;
        cur_shader->texture_alpha = true;
        sLastTex0Reg = ~0ULL;
        draw_fast_textured_pass(v, tris, alpha_mask, PASS_ORIGINAL, sEnvColor);
    } else {
        draw_fast_untextured_pass(v, tris);
    }

    if (saved_z_write) {
        depth_mask(false);
    }
    cur_shader->tfx = 0;
    cur_shader->texture_alpha = saved_tex_alpha;
    sLastTex0Reg = ~0ULL;

    if (saved_blend) {

        set_alpha_packet(GS_SETREG_ALPHA(2, 0, 0, 1, 128), true);
        draw_fast_textured_pass(v, tris, source, PASS_ORIGINAL, sEnvColor);
        set_alpha_packet(GS_SETREG_ALPHA(0, 2, 0, 1, 128), true);
        draw_fast_textured_pass(v, tris, source, PASS_TARGET_RGB, sEnvColor);
    } else {

        set_alpha_packet(GS_SETREG_ALPHA(1, 0, 2, 2, 128), true);
        draw_fast_textured_pass(v, tris, source, PASS_ORIGINAL, sEnvColor);
        set_alpha_packet(GS_SETREG_ALPHA(0, 2, 2, 1, 128), true);
        draw_fast_textured_pass(v, tris, source, PASS_TARGET_RGB, sEnvColor);
    }

    set_alpha_packet(saved_alpha, saved_alpha_enable);
    if (saved_z_write) {
        depth_mask(true);
    }
    cur_shader->texture_alpha = saved_tex_alpha;
    cur_shader->tfx = saved_tfx;
    sLastTex0Reg = ~0ULL;
    return true;
}

static bool draw_decal_rgba_exact(const FastVertex* v, size_t tris, struct Texture* source) {
    if (v == NULL || tris == 0 || source == NULL || cur_shader == NULL || cur_shader->alpha_test) {
        return false;
    }

    const bool saved_tex_alpha = cur_shader->texture_alpha;
    const uint8_t saved_tfx = cur_shader->tfx;
    const bool saved_z_write = z_write;
    const bool saved_alpha_test = cur_shader->alpha_test;
    const bool saved_blend = do_blend;
    const u64 saved_alpha = gs_global->PrimAlpha;
    const bool saved_alpha_enable = gs_global->PrimAlphaEnable;
    struct Texture* alpha_mask = NULL;

    if (saved_blend) {
        alpha_mask = blend_texture(source, 0x00ffffffU, 0x00ffffffU);
        if (alpha_mask == NULL || alpha_mask == source) {
            return false;
        }
    }

    draw_fast_untextured_pass(v, tris);
    if (saved_z_write) {
        depth_mask(false);
    }
    cur_shader->alpha_test = false;
    update_test();

    if (saved_blend) {

        cur_shader->tfx = 0;
        cur_shader->texture_alpha = true;
        sLastTex0Reg = ~0ULL;
        set_alpha_packet(GS_SETREG_ALPHA(2, 0, 0, 1, 128), true);
        draw_fast_textured_pass(v, tris, alpha_mask, PASS_ORIGINAL, 0);
        set_alpha_packet(GS_SETREG_ALPHA(0, 2, 0, 1, 128), true);
        draw_fast_textured_pass(v, tris, source, PASS_NEUTRAL_RGB, 0);
    } else {

        cur_shader->tfx = 1;
        cur_shader->texture_alpha = true;
        sLastTex0Reg = ~0ULL;
        set_alpha_packet(BMODE_BLEND, true);
        draw_fast_textured_pass(v, tris, source, PASS_NEUTRAL_RGB, 0);
    }

    set_alpha_packet(saved_alpha, saved_alpha_enable);
    if (saved_z_write) {
        depth_mask(true);
    }
    cur_shader->alpha_test = saved_alpha_test;
    cur_shader->texture_alpha = saved_tex_alpha;
    cur_shader->tfx = saved_tfx;
    update_test();
    sLastTex0Reg = ~0ULL;
    return true;
}

static void draw_sprite_untextured_pass(float x1, float y1, int z1, float x2, float y2, int z2, uint32_t rgba) {
    (void)z2;
    gsKit_prim_sprite(gs_global, x1, y1, x2, y2, z1, colorq_from_rgba(rgba, 1.0f, false));
}

static void draw_sprite_textured_pass(struct Texture* tex, float x1, float y1, int z1, float u1, float v1, float x2,
                                      float y2, int z2, float u2, float v2, uint32_t rgba, enum PassColorMode mode,
                                      uint32_t target) {
    if (tex == NULL) {
        return;
    }
    const uint32_t c = pass_color(rgba, mode, target);
    bind_texture(&tex->tex);
    const bool edge_s = (u1 >= -1.0f && u1 <= tex->tex.Width + 1.0f && u2 >= -1.0f && u2 <= tex->tex.Width + 1.0f);
    const bool edge_t = (v1 >= -1.0f && v1 <= tex->tex.Height + 1.0f && v2 >= -1.0f && v2 <= tex->tex.Height + 1.0f);
    clamp_tex(tex, edge_s, edge_t);
    sLastTex0Reg = ~0ULL;
    sprite_tex(&tex->tex, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, colorq_from_rgba(c, 1.0f, true));
}

static bool draw_blend_exact_2d(const VertexColor2D* q, struct Texture* source, float x1, float y1, int z1, float u1,
                                float v1, float x2, float y2, int z2, float u2, float v2) {
    if (q == NULL || source == NULL || cur_shader == NULL) {
        return false;
    }

    const bool saved_tex_alpha = cur_shader->texture_alpha;
    const uint8_t saved_tfx = cur_shader->tfx;
    const bool saved_z_write = z_write;
    const bool saved_blend = do_blend;
    const u64 saved_alpha = gs_global->PrimAlpha;
    const bool saved_alpha_enable = gs_global->PrimAlphaEnable;
    struct Texture* alpha_mask = NULL;

    if (saved_tex_alpha) {
        alpha_mask = blend_texture(source, 0x00ffffffU, 0x00ffffffU);
        if (alpha_mask == NULL || alpha_mask == source) {
            return false;
        }
    }

    if (saved_tex_alpha) {
        cur_shader->tfx = 0;
        cur_shader->texture_alpha = true;
        draw_sprite_textured_pass(alpha_mask, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_ORIGINAL,
                                  sEnvColor);
    } else {
        draw_sprite_untextured_pass(x1, y1, z1, x2, y2, z2, q[0].rgba);
    }

    if (saved_z_write) {
        depth_mask(false);
    }
    cur_shader->tfx = 0;
    cur_shader->texture_alpha = saved_tex_alpha;
    sLastTex0Reg = ~0ULL;

    if (saved_blend) {
        set_alpha_packet(GS_SETREG_ALPHA(2, 0, 0, 1, 128), true);
        draw_sprite_textured_pass(source, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_ORIGINAL, sEnvColor);
        set_alpha_packet(GS_SETREG_ALPHA(0, 2, 0, 1, 128), true);
        draw_sprite_textured_pass(source, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_TARGET_RGB,
                                  sEnvColor);
    } else {
        set_alpha_packet(GS_SETREG_ALPHA(1, 0, 2, 2, 128), true);
        draw_sprite_textured_pass(source, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_ORIGINAL, sEnvColor);
        set_alpha_packet(GS_SETREG_ALPHA(0, 2, 2, 1, 128), true);
        draw_sprite_textured_pass(source, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_TARGET_RGB,
                                  sEnvColor);
    }

    set_alpha_packet(saved_alpha, saved_alpha_enable);
    if (saved_z_write) {
        depth_mask(true);
    }
    cur_shader->texture_alpha = saved_tex_alpha;
    cur_shader->tfx = saved_tfx;
    sLastTex0Reg = ~0ULL;
    return true;
}

static bool draw_decal_rgba_exact_2d(const VertexColor2D* q, struct Texture* source, float x1, float y1, int z1,
                                     float u1, float v1, float x2, float y2, int z2, float u2, float v2) {
    if (q == NULL || source == NULL || cur_shader == NULL || cur_shader->alpha_test) {
        return false;
    }

    const bool saved_tex_alpha = cur_shader->texture_alpha;
    const uint8_t saved_tfx = cur_shader->tfx;
    const bool saved_z_write = z_write;
    const bool saved_blend = do_blend;
    const u64 saved_alpha = gs_global->PrimAlpha;
    const bool saved_alpha_enable = gs_global->PrimAlphaEnable;
    struct Texture* alpha_mask = NULL;

    if (saved_blend) {
        alpha_mask = blend_texture(source, 0x00ffffffU, 0x00ffffffU);
        if (alpha_mask == NULL || alpha_mask == source) {
            return false;
        }
    }

    draw_sprite_untextured_pass(x1, y1, z1, x2, y2, z2, q[0].rgba);
    if (saved_z_write) {
        depth_mask(false);
    }

    if (saved_blend) {
        cur_shader->tfx = 0;
        cur_shader->texture_alpha = true;
        sLastTex0Reg = ~0ULL;

        set_alpha_packet(GS_SETREG_ALPHA(2, 0, 0, 1, 128), true);
        draw_sprite_textured_pass(alpha_mask, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_ORIGINAL, 0);
        set_alpha_packet(GS_SETREG_ALPHA(0, 2, 0, 1, 128), true);
        draw_sprite_textured_pass(source, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_NEUTRAL_RGB, 0);
    } else {
        cur_shader->tfx = 1;
        cur_shader->texture_alpha = true;
        sLastTex0Reg = ~0ULL;
        set_alpha_packet(BMODE_BLEND, true);
        draw_sprite_textured_pass(source, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, q[0].rgba, PASS_NEUTRAL_RGB, 0);
    }

    set_alpha_packet(saved_alpha, saved_alpha_enable);
    if (saved_z_write) {
        depth_mask(true);
    }
    cur_shader->texture_alpha = saved_tex_alpha;
    cur_shader->tfx = saved_tfx;
    sLastTex0Reg = ~0ULL;
    return true;
}

static void set_alpha_packet(uint64_t alpha, bool enable) {
    gs_global->PrimAlpha = alpha;
    gs_global->PrimAlphaEnable = enable;

    if (alpha == sLastAlphaReg) {
        return;
    }

    sLastAlphaReg = alpha;
    u64* packet = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *packet++ = GIF_TAG_AD(1);
    *packet++ = GIF_AD;
    *packet++ = alpha;
    *packet++ = GS_ALPHA_1 + gs_global->PrimContext;
}

static bool vertices_have_uniform_rgb(const FastVertex* vertices, size_t vertexCount, uint32_t rgb) {
    for (size_t i = 1; i < vertexCount; i++) {
        if ((vertices[i].rgba & 0x00ffffffU) != rgb) {
            return false;
        }
    }
    return true;
}

static void draw_exact_tint_passes(FastVertex* vertices, size_t triangleCount, struct Texture* texture) {
    for (size_t i = 0; i < triangleCount; i++) {
        FastVertex* triangle = &vertices[i * 3];
        gsKit_prim_triangle_gouraud_3d(gs_global, triangle[0].x, triangle[0].y, (int)triangle[0].z, triangle[1].x,
                                       triangle[1].y, (int)triangle[1].z, triangle[2].x, triangle[2].y,
                                       (int)triangle[2].z, colorq_from_rgba(triangle[0].rgba, triangle[0].q, false),
                                       colorq_from_rgba(triangle[1].rgba, triangle[1].q, false),
                                       colorq_from_rgba(triangle[2].rgba, triangle[2].q, false));
    }

    const u64 savedAlpha = gs_global->PrimAlpha;
    const bool savedAlphaEnable = gs_global->PrimAlphaEnable;

    set_alpha_packet(GS_SETREG_ALPHA(1, 0, 2, 2, 128), true);
    for (size_t i = 0; i < triangleCount; i++) {
        FastVertex* triangle = &vertices[i * 3];
        tri_tex(texture, triangle[0].x, triangle[0].y, (int)triangle[0].z, triangle[0].u, triangle[0].v,
                colorq_from_rgba(triangle[0].rgba, triangle[0].q, true), triangle[1].x, triangle[1].y,
                (int)triangle[1].z, triangle[1].u, triangle[1].v,
                colorq_from_rgba(triangle[1].rgba, triangle[1].q, true), triangle[2].x, triangle[2].y,
                (int)triangle[2].z, triangle[2].u, triangle[2].v,
                colorq_from_rgba(triangle[2].rgba, triangle[2].q, true), false, 255, 255, 255);
    }

    set_alpha_packet(GS_SETREG_ALPHA(0, 2, 2, 1, 128), true);
    for (size_t i = 0; i < triangleCount; i++) {
        FastVertex* triangle = &vertices[i * 3];
        const uint32_t color0 = (sEnvColor & 0x00ffffffU) | (triangle[0].rgba & 0xff000000U);
        const uint32_t color1 = (sEnvColor & 0x00ffffffU) | (triangle[1].rgba & 0xff000000U);
        const uint32_t color2 = (sEnvColor & 0x00ffffffU) | (triangle[2].rgba & 0xff000000U);

        tri_tex(texture, triangle[0].x, triangle[0].y, (int)triangle[0].z, triangle[0].u, triangle[0].v,
                colorq_from_rgba(color0, triangle[0].q, true), triangle[1].x, triangle[1].y, (int)triangle[1].z,
                triangle[1].u, triangle[1].v, colorq_from_rgba(color1, triangle[1].q, true), triangle[2].x,
                triangle[2].y, (int)triangle[2].z, triangle[2].u, triangle[2].v,
                colorq_from_rgba(color2, triangle[2].q, true), false, 255, 255, 255);
    }

    set_alpha_packet(savedAlpha, savedAlphaEnable);
}

static void draw_textured_batches(const FastVertex* vertices, size_t triangleCount, struct Texture* texture,
                                  bool useFog, bool neutralTint, bool targetTint, bool dekuWater) {
    size_t done = 0;

    while (done < triangleCount) {
        const size_t remaining = triangleCount - done;
        const size_t wanted = remaining > GS_GIF_BLOCKSIZE ? GS_GIF_BLOCKSIZE : remaining;
        const size_t batch = tri_tex_batch_safe_count(wanted, useFog);
        u64* packet = tri_tex_batch_begin(texture, batch, useFog);

        for (size_t i = 0; i < batch; i++) {
            const FastVertex* triangle = &vertices[(done + i) * 3U];
            uint32_t color0;
            uint32_t color1;
            uint32_t color2;

            if (__builtin_expect(!neutralTint && !targetTint && !dekuWater, 1)) {
                color0 = triangle[0].rgba;
                color1 = triangle[1].rgba;
                color2 = triangle[2].rgba;
            } else if (neutralTint) {
                color0 = reverse_neutral_color(triangle[0].rgba);
                color1 = reverse_neutral_color(triangle[1].rgba);
                color2 = reverse_neutral_color(triangle[2].rgba);
            } else if (targetTint) {
                color0 = target_rgb_preserve_alpha(triangle[0].rgba);
                color1 = target_rgb_preserve_alpha(triangle[1].rgba);
                color2 = target_rgb_preserve_alpha(triangle[2].rgba);
            } else {
                color0 = deku_water_color(triangle[0].rgba);
                color1 = deku_water_color(triangle[1].rgba);
                color2 = deku_water_color(triangle[2].rgba);
            }

            tri_tex_emit_reserved_known(
                &packet, texture, triangle[0].x, triangle[0].y, (int)triangle[0].z, triangle[0].u, triangle[0].v,
                triangle[0].real_u, triangle[0].real_v, colorq_from_rgba(color0, triangle[0].q, true), triangle[1].x,
                triangle[1].y, (int)triangle[1].z, triangle[1].u, triangle[1].v, triangle[1].real_u, triangle[1].real_v,
                colorq_from_rgba(color1, triangle[1].q, true), triangle[2].x, triangle[2].y, (int)triangle[2].z,
                triangle[2].u, triangle[2].v, triangle[2].real_u, triangle[2].real_v,
                colorq_from_rgba(color2, triangle[2].q, true), useFog, (uint8_t)triangle[0].fog,
                (uint8_t)triangle[1].fog, (uint8_t)triangle[2].fog);
        }

        done += batch;
    }
}

static void draw_untextured_batches(const FastVertex* vertices, size_t triangleCount, bool useFog) {
    size_t done = 0;

    while (done < triangleCount) {
        const size_t remaining = triangleCount - done;
        const size_t wanted = remaining > GS_GIF_BLOCKSIZE ? GS_GIF_BLOCKSIZE : remaining;
        const size_t batch = tri_col_batch_safe_count(wanted, useFog);
        u64* packet = tri_col_batch_begin(batch, useFog);

        for (size_t i = 0; i < batch; i++) {
            const FastVertex* triangle = &vertices[(done + i) * 3U];
            tri_col_emit_reserved(
                &packet, triangle[0].x, triangle[0].y, (int)triangle[0].z,
                colorq_from_rgba(triangle[0].rgba, triangle[0].q, false), triangle[1].x, triangle[1].y,
                (int)triangle[1].z, colorq_from_rgba(triangle[1].rgba, triangle[1].q, false), triangle[2].x,
                triangle[2].y, (int)triangle[2].z, colorq_from_rgba(triangle[2].rgba, triangle[2].q, false), useFog,
                (uint8_t)triangle[0].fog, (uint8_t)triangle[1].fog, (uint8_t)triangle[2].fog);
        }

        done += batch;
    }
}

static void draw_alpha_blended_texture_batches(const FastVertex* vertices, size_t triangleCount,
                                               struct Texture* texture, bool useFog) {
    bind_texture(&texture->tex);
    clamp_tex(texture, false, false);

    if (useFog) {
        set_fog_color(vertices[0].fog_color);
        sSkipNextFogPass = true;
    }

    size_t done = 0;
    while (done < triangleCount) {
        const size_t remaining = triangleCount - done;
        const size_t wanted = remaining > GS_GIF_BLOCKSIZE ? GS_GIF_BLOCKSIZE : remaining;
        const size_t batch = tri_tex_batch_safe_count(wanted, useFog);
        u64* packet = tri_tex_batch_begin(texture, batch, useFog);

        for (size_t i = 0; i < batch; i++) {
            const FastVertex* triangle = &vertices[(done + i) * 3U];
            const uint32_t color0 = 0x00ffffffU | (triangle[0].rgba & 0xff000000U);
            const uint32_t color1 = 0x00ffffffU | (triangle[1].rgba & 0xff000000U);
            const uint32_t color2 = 0x00ffffffU | (triangle[2].rgba & 0xff000000U);

            tri_tex_emit_reserved_known(
                &packet, texture, triangle[0].x, triangle[0].y, (int)triangle[0].z, triangle[0].u, triangle[0].v,
                triangle[0].real_u, triangle[0].real_v, colorq_from_rgba(color0, triangle[0].q, true), triangle[1].x,
                triangle[1].y, (int)triangle[1].z, triangle[1].u, triangle[1].v, triangle[1].real_u, triangle[1].real_v,
                colorq_from_rgba(color1, triangle[1].q, true), triangle[2].x, triangle[2].y, (int)triangle[2].z,
                triangle[2].u, triangle[2].v, triangle[2].real_u, triangle[2].real_v,
                colorq_from_rgba(color2, triangle[2].q, true), useFog, (uint8_t)triangle[0].fog,
                (uint8_t)triangle[1].fog, (uint8_t)triangle[2].fog);
        }

        done += batch;
    }
}

/*
é aqui que o desenho 3d decide qual caminho dessa desgraça toda vai usar
primeiro tenta o resultado exato ou a textura já pronta e só depois cai no lote normal
não troca essa ordem no chute porque fog, alpha e duas texturas brigam pelos mesmos estados do gs
*/
static void draw_triangles(float* raw, size_t length, size_t triangleCount) {
    (void)length;
    if (raw == NULL || triangleCount == 0) {
        return;
    }

    const bool restorePrerenderAfterDraw = !sPrerender3dStarted;
    if (!sPrerender3dStarted) {
        sPrerender3dStarted = true;
    }

    const bool depthOnly = cur_shader != NULL && cur_shader->depth_only;
    const bool savedAlphaEnable = gs_global->PrimAlphaEnable;
    if (depthOnly) {
        set_frame_write_mask(0xffffffffU);
        gs_global->PrimAlphaEnable = 0;
    }

    update_test();
    FastVertex* vertices = (FastVertex*)raw;
    struct Texture* drawTexture = last_tex;
    const bool textured =
        drawTexture != NULL && cur_shader != NULL && (cur_shader->texture_used[0] || cur_shader->texture_used[1]);

    const bool batch_01045 = textured && !depthOnly && cur_shader != NULL &&
                             (cur_shader->shader_id == 0x01045A00U || cur_shader->shader_id == 0x03045A00U) &&
                             cur_shader->texture_alpha && !cur_shader->alpha_test && do_blend;
    if (__builtin_expect(batch_01045, 0)) {
        draw_alpha_blended_texture_batches(vertices, triangleCount, drawTexture, cur_shader->cc.opt_fog);
        goto draw_complete;
    }

    bool blendSinglePass = false;
    bool blendFallbackTarget = false;
    if (textured && !depthOnly && cur_shader != NULL && cur_shader->texture_mode == TEXTURE_MODE_BLEND &&
        sTextureBlendPrecolor && !sTextureBlendReverse && !sDinFireTint && !sTwoTextureEnvPrimTint &&
        !sTwoTextureBlendActive && !cur_shader->cc.opt_texture_blend_shade && (do_blend || cur_shader->texture_alpha) &&
        texture_cpu_rgba_supported(drawTexture) && cur_shader->shader_id != 0x11141551U) {
        const uint32_t baseRgb = vertices[0].rgba & 0x00ffffffU;
        if (vertices_have_uniform_rgb(vertices, triangleCount * 3, baseRgb)) {
            struct Texture* derived = blend_texture(drawTexture, baseRgb, sEnvColor);
            if (derived != drawTexture) {
                drawTexture = derived;
                blendSinglePass = true;
            }
        }
    }

    if (textured && !depthOnly && cur_shader != NULL) {
        if (cur_shader->texture_mode == TEXTURE_MODE_BLEND && sTextureBlendPrecolor && !blendSinglePass &&
            !sTextureBlendReverse && !sDinFireTint && !sTwoTextureEnvPrimTint) {
            if (draw_blend_exact(vertices, triangleCount, drawTexture)) {
                goto draw_complete;
            }
            blendFallbackTarget = true;
        }
        if (cur_shader->texture_mode == TEXTURE_MODE_DECAL && cur_shader->texture_alpha &&
            draw_decal_rgba_exact(vertices, triangleCount, drawTexture)) {
            goto draw_complete;
        }
    }

    const bool reverseTint = textured && sTextureBlendReverse && texture_cpu_rgba_supported(drawTexture);
    bool blendTint = blendSinglePass;
    bool envPrimTint = false;
    if (sTwoTextureEnvPrimTint && sTwoTextureBlendActive && textured && texture_cpu_rgba_supported(drawTexture)) {
        const uint32_t primRgb = vertices[0].rgba & 0x00ffffffU;
        if (vertices_have_uniform_rgb(vertices, triangleCount * 3, primRgb)) {
            struct Texture* tinted = blend_texture(drawTexture, sEnvColor, primRgb);
            envPrimTint = tinted != drawTexture;
            drawTexture = tinted;
        }
    }

    if (reverseTint) {
        drawTexture = reverse_tint_texture(drawTexture, sEnvColor, vertices[0].rgba);
    } else if (sDinFireTint && textured && cur_shader != NULL && cur_shader->cc.opt_texture_blend &&
               !cur_shader->cc.opt_texture_blend_shade && texture_cpu_rgba_supported(drawTexture)) {
        const uint32_t baseRgb = vertices[0].rgba & 0x00ffffffU;
        if (vertices_have_uniform_rgb(vertices, triangleCount * 3, baseRgb)) {
            struct Texture* tinted = blend_texture(drawTexture, baseRgb, sEnvColor);
            blendTint = tinted != drawTexture;
            drawTexture = tinted;
        }
    } else if (!blendSinglePass && sTextureBlendPrecolor && !sTwoTextureBlendActive && textured && cur_shader != NULL &&
               cur_shader->cc.opt_texture_blend && (do_blend || cur_shader->texture_alpha) &&
               !cur_shader->cc.opt_texture_blend_shade && texture_cpu_rgba_supported(drawTexture) &&
               cur_shader->shader_id != 0x11141551U) {
        const uint32_t baseRgb = vertices[0].rgba & 0x00ffffffU;
        if (vertices_have_uniform_rgb(vertices, triangleCount * 3, baseRgb)) {
            struct Texture* tinted = blend_texture(drawTexture, baseRgb, sEnvColor);
            blendTint = tinted != drawTexture;
            drawTexture = tinted;
        }
    }

    const bool pauseNameTint =
        textured && !reverseTint && !blendTint && is_pause_name_tint(drawTexture, vertices[0].rgba);
    const bool dekuWater = textured && !reverseTint && !blendTint && is_deku_water_material(drawTexture);
    if (textured) {
        bind_texture(&drawTexture->tex);
        clamp_tex(drawTexture, false, false);
    }

    const bool has_fog = cur_shader != NULL && cur_shader->cc.opt_fog;

    const bool native_fog_alpha_safe =
        cur_shader != NULL && (!cur_shader->texture_alpha || cur_shader->alpha_test ||
                               cur_shader->shader_id == 0x03045A00U || cur_shader->shader_id == 0x13551551U ||
                               cur_shader->shader_id == 0x03A00045U || cur_shader->shader_id == 0x03045045U);
    const bool native_fog_two_texture_safe = !sTwoTextureBlendActive || !sTwoTextureAlphaBlendActive;
    const bool prefer_native_fog = has_fog && !depthOnly && cur_shader != NULL && native_fog_alpha_safe &&
                                   native_fog_two_texture_safe && !reverseTint && !envPrimTint && !pauseNameTint &&
                                   !dekuWater;
    const bool exact_tint = textured && sTextureBlendPrecolor && cur_shader != NULL &&
                            cur_shader->cc.opt_texture_blend && !reverseTint && !blendTint &&
                            !cur_shader->cc.opt_texture_blend_shade && !do_blend && !has_fog;
    sSkipNextFogPass = prefer_native_fog;
    if (prefer_native_fog) {
        set_fog_color(vertices[0].fog_color);
    }
    if (exact_tint) {
        draw_exact_tint_passes(vertices, triangleCount, drawTexture);
    } else if (textured) {
        const bool neutralTint = reverseTint || blendTint || envPrimTint;
        const bool targetTint = pauseNameTint || blendFallbackTarget;
        draw_textured_batches(vertices, triangleCount, drawTexture, prefer_native_fog, neutralTint, targetTint,
                              dekuWater);
    } else {
        draw_untextured_batches(vertices, triangleCount, prefer_native_fog);
    }
draw_complete:
    if (depthOnly) {
        gs_global->PrimAlphaEnable = savedAlphaEnable;
        if (!sPrerenderDepthOnlyPass) {
            set_frame_write_mask(0);
        }
    }
    if (restorePrerenderAfterDraw) {
        restore_prerender_fallback();
    }
}

/*
não tem dois fogs fazendo a mesma coisa, quando o alpha deixa o fog já vai no xyzf do desenho normal
quando não deixa o fast3d manda este passe separado e a flag de skip impede desenhar a porra duas vezes
*/
static void restore_fog_pass_state(bool emit_state) {
    if (!sFogPass.active) {
        return;
    }
    cur_shader = sFogPass.saved_shader;
    if (cur_shader) {
        cur_shader->alpha_test = sFogPass.saved_alpha_test;
        cur_shader->texture_alpha = sFogPass.saved_texture_alpha;
        cur_shader->tfx = sFogPass.saved_tfx;
    }
    if (emit_state) {
        depth_mask(sFogPass.saved_z_write);
        update_test();
        set_blend(sFogPass.saved_blend);
    } else {

        z_write = sFogPass.saved_z_write;
        do_blend = sFogPass.saved_blend;
        if (gs_global) {
            gs_global->PrimAlphaEnable = do_blend;
            gs_global->PrimAlpha = do_blend ? BMODE_BLEND : 0;
        }
    }
    sLastTex0Reg = ~0ULL;
    sFogPass.active = false;
}

static void draw_fog_triangles(float* raw, size_t len, size_t tris, bool texture_alpha, bool restore) {
    if (sSkipNextFogPass) {
        sSkipNextFogPass = false;
        return;
    }
    if (raw == NULL || tris == 0) {
        if (restore) {
            restore_fog_pass_state(true);
        }
        return;
    }

    if (!sFogPass.active) {
        sFogPass.active = true;
        sFogPass.saved_z_write = z_write;
        sFogPass.saved_blend = do_blend;
        sFogPass.saved_shader = cur_shader;
        sFogPass.saved_alpha_test = cur_shader ? cur_shader->alpha_test : false;
        sFogPass.saved_texture_alpha = cur_shader ? cur_shader->texture_alpha : false;
        sFogPass.saved_tfx = cur_shader ? cur_shader->tfx : 0;

        if (cur_shader) {
            cur_shader->alpha_test = false;
        }

        depth_mask(false);
        update_test();
        set_blend(true);
    }

    bool malformed = false;
    if (texture_alpha) {
        const size_t expected = tris * 3U * sizeof(FogTexturedVertex);
        if (len < expected) {
            malformed = true;
            goto fog_restore;
        }
        FogTexturedVertex* v = (FogTexturedVertex*)raw;
        struct Texture* source = last_tex;
        if (source != NULL) {

            struct Texture* mask = blend_texture(source, 0x00ffffffU, 0x00ffffffU);
            if (cur_shader) {
                cur_shader->texture_alpha = true;
                cur_shader->tfx = 0;
            }
            bind_texture(&mask->tex);
            clamp_tex(mask, false, false);
            sLastTex0Reg = ~0ULL;

            size_t done = 0;
            while (done < tris) {
                const size_t wanted = tris - done;
                const size_t batch = tri_tex_batch_safe_count(wanted, false);
                u64* packet = tri_tex_batch_begin(mask, batch, false);
                for (size_t j = 0; j < batch; j++) {
                    FogTexturedVertex* q = &v[(done + j) * 3U];
                    tri_tex_emit_reserved(&packet, mask, q[0].x, q[0].y, (int)q[0].z, q[0].u, q[0].v,
                                          colorq_from_rgba(q[0].rgba, q[0].q, true), q[1].x, q[1].y, (int)q[1].z,
                                          q[1].u, q[1].v, colorq_from_rgba(q[1].rgba, q[1].q, true), q[2].x, q[2].y,
                                          (int)q[2].z, q[2].u, q[2].v, colorq_from_rgba(q[2].rgba, q[2].q, true), false,
                                          255, 255, 255);
                }
                done += batch;
            }
        }
    } else {
        const size_t expected = tris * 3U * sizeof(FogColorVertex);
        if (len < expected) {
            malformed = true;
            goto fog_restore;
        }
        FogColorVertex* v = (FogColorVertex*)raw;
        size_t done = 0;
        while (done < tris) {
            const size_t wanted = tris - done;
            const size_t batch = tri_col_batch_safe_count(wanted, false);
            u64* packet = tri_col_batch_begin(batch, false);
            for (size_t j = 0; j < batch; j++) {
                FogColorVertex* q = &v[(done + j) * 3U];
                tri_col_emit_reserved(&packet, q[0].x, q[0].y, (int)q[0].z, colorq_from_rgba(q[0].rgba, 1.0f, false),
                                      q[1].x, q[1].y, (int)q[1].z, colorq_from_rgba(q[1].rgba, 1.0f, false), q[2].x,
                                      q[2].y, (int)q[2].z, colorq_from_rgba(q[2].rgba, 1.0f, false), false, 255, 255,
                                      255);
            }
            done += batch;
        }
    }

fog_restore:

    if (malformed || restore) {
        restore_fog_pass_state(true);
    }
}

/*
2d não significa que pode ignorar o combiner, esses sprites usam os mesmos caches e as mesmas regras do 3d
o sprite de 512x256 é o fundo pré-renderizado e fica guardado pra sala não abrir um buraco preto quando ele some
*/
void gfx_ps2_draw_triangles_2d(float* raw, size_t unused, size_t count) {
    (void)unused;
    (void)count;
    VertexColor2D* q = (VertexColor2D*)raw;
    if (!q) {
        return;
    }
    const bool depth_only = cur_shader && cur_shader->depth_only;
    const bool saved_alpha_enable = gs_global->PrimAlphaEnable;
    if (depth_only) {
        set_frame_write_mask(0xffffffffU);
        gs_global->PrimAlphaEnable = 0;
    }
    update_test();
    float x1 = q[0].x;
    float y1 = q[0].y;
    float x2 = q[1].x;
    float y2 = q[1].y;
    float u1 = q[0].u;
    float v1 = q[0].v;
    float u2 = q[1].u;
    float v2 = q[1].v;
    const int z1 = (int)q[0].z;
    const int z2 = (int)q[1].z;
    struct Texture* draw_tex = last_tex;
    const bool textured =
        draw_tex != NULL && cur_shader && (cur_shader->texture_used[0] || cur_shader->texture_used[1]);

    if (textured && cur_shader && cur_shader->shader_id == 0x41045200U && draw_tex->tex.Width == 8 &&
        draw_tex->tex.Height == 16) {
        const float glyph_h = 16.0f * ((float)gs_global->Height / 240.0f);
        if ((y2 - y1) > glyph_h * 2.0f && (v2 - v1) > 32.0f) {
            y2 = y1 + glyph_h;
            v2 = v1 + 16.0f;
        }
    }

    const bool prerender_candidate = textured && draw_tex->tex.Width == 512 && draw_tex->tex.Height == 256 &&
                                     draw_tex->tex.PSM == GS_PSM_CT16 && sPrerenderRoomActive && (x2 - x1) >= 300.0f &&
                                     (y2 - y1) >= 200.0f;

    bool blend_single_pass_2d = false;
    bool blend_fallback_target_2d = false;
    bool decal_single_pass_2d = false;
    const uint8_t saved_2d_tfx = cur_shader != NULL ? cur_shader->tfx : 0;
    const bool saved_2d_texture_alpha = cur_shader != NULL ? cur_shader->texture_alpha : false;
    if (textured && !depth_only && !prerender_candidate && cur_shader != NULL &&
        cur_shader->texture_mode == TEXTURE_MODE_BLEND && sTextureBlendPrecolor && !sTextureBlendReverse &&
        !sDinFireTint && !sTwoTextureEnvPrimTint && !sTwoTextureBlendActive &&
        !cur_shader->cc.opt_texture_blend_shade && texture_cpu_rgba_supported(draw_tex)) {
        const uint32_t baseRgb = q[0].rgba & 0x00ffffffU;
        if (baseRgb == (q[1].rgba & 0x00ffffffU)) {
            struct Texture* derived = blend_texture(draw_tex, baseRgb, sEnvColor);
            if (derived != draw_tex) {
                draw_tex = derived;
                blend_single_pass_2d = true;
            }
        }
    }

    if (textured && !depth_only && !prerender_candidate && cur_shader != NULL &&
        cur_shader->texture_mode == TEXTURE_MODE_DECAL && cur_shader->texture_alpha && !cur_shader->alpha_test &&
        texture_cpu_rgba_supported(draw_tex)) {
        const uint32_t baseRgb = q[0].rgba & 0x00ffffffU;
        if (baseRgb == (q[1].rgba & 0x00ffffffU)) {
            struct Texture* derived = decal_texture(draw_tex, baseRgb);
            if (derived != draw_tex) {
                draw_tex = derived;
                decal_single_pass_2d = true;
                cur_shader->tfx = 1;
                cur_shader->texture_alpha = false;
                sLastTex0Reg = ~0ULL;
            }
        }
    }
    if (textured && !depth_only && !prerender_candidate && cur_shader != NULL) {
        if (cur_shader->texture_mode == TEXTURE_MODE_BLEND && sTextureBlendPrecolor && !blend_single_pass_2d &&
            !sTextureBlendReverse && !sDinFireTint && !sTwoTextureEnvPrimTint) {
            if (draw_blend_exact_2d(q, draw_tex, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2)) {
                goto cleanup;
            }
            blend_fallback_target_2d = true;
        }
        if (cur_shader->texture_mode == TEXTURE_MODE_DECAL && cur_shader->texture_alpha && !decal_single_pass_2d &&
            draw_decal_rgba_exact_2d(q, draw_tex, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2)) {
            goto cleanup;
        }
    }

    const bool reverse_tint = textured && sTextureBlendReverse && texture_cpu_rgba_supported(draw_tex);
    bool blend_tint = blend_single_pass_2d;
    bool env_prim_tint = false;
    if (sTwoTextureEnvPrimTint && sTwoTextureBlendActive && textured && texture_cpu_rgba_supported(draw_tex)) {
        const uint32_t primRgb = q[0].rgba & 0x00ffffffU;
        const uint32_t primRgb1 = q[1].rgba & 0x00ffffffU;
        if (primRgb == primRgb1) {
            struct Texture* tinted = blend_texture(draw_tex, sEnvColor, primRgb);
            env_prim_tint = (tinted != draw_tex);
            draw_tex = tinted;
        }
    }
    if (reverse_tint) {
        draw_tex = reverse_tint_texture(draw_tex, sEnvColor, q[0].rgba);
    } else if (sTextureBlendPrecolor && !blend_single_pass_2d && !sTwoTextureBlendActive && textured && cur_shader &&
               cur_shader->cc.opt_texture_blend && !cur_shader->cc.opt_texture_blend_shade &&
               texture_cpu_rgba_supported(draw_tex)) {
        const uint32_t base0 = q[0].rgba & 0x00ffffffU;
        const uint32_t base1 = q[1].rgba & 0x00ffffffU;
        if (base0 == base1) {
            struct Texture* tinted = blend_texture(draw_tex, base0, sEnvColor);
            blend_tint = (tinted != draw_tex);
            draw_tex = tinted;
        }
    }
    uint32_t drawColor = q[0].rgba;
    if (reverse_tint || blend_tint || env_prim_tint) {
        drawColor = reverse_neutral_color(drawColor);
    } else if (blend_fallback_target_2d) {
        drawColor = target_rgb_preserve_alpha(drawColor);
    }
    const uint64_t col = colorq_from_rgba(drawColor, 1.0f, textured);
    if (textured) {
        bind_texture(&draw_tex->tex);

        const bool edge_s = (u1 >= -1 && u1 <= draw_tex->tex.Width + 1 && u2 >= -1 && u2 <= draw_tex->tex.Width + 1);
        const bool edge_t = (v1 >= -1 && v1 <= draw_tex->tex.Height + 1 && v2 >= -1 && v2 <= draw_tex->tex.Height + 1);
        clamp_tex(draw_tex, edge_s, edge_t);
        if (draw_tex->tex.Width == 512 && draw_tex->tex.Height == 256 && draw_tex->tex.PSM == GS_PSM_CT16) {
            if (sPrerenderRoomActive && (x2 - x1) >= 300.0f && (y2 - y1) >= 200.0f) {
                sPrerenderFallback.texture = draw_tex;
                sPrerenderFallback.x1 = x1;
                sPrerenderFallback.y1 = y1;
                sPrerenderFallback.x2 = x2;
                sPrerenderFallback.y2 = y2;
                sPrerenderFallback.u1 = u1;
                sPrerenderFallback.v1 = v1;
                sPrerenderFallback.u2 = u2;
                sPrerenderFallback.v2 = v2;
                sPrerenderFallback.z1 = z1;
                sPrerenderFallback.z2 = z2;
                sPrerenderFallback.color = col;
                sPrerenderFallback.valid = true;
                sPrerenderFallback.seenThisFrame = true;
                sPrerenderFallback.missedFrames = 0;
            }
            gsKit_prim_sprite_texture_3d(gs_global, &draw_tex->tex, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, col);
        } else {
            sprite_tex(&draw_tex->tex, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, col);
        }
    } else {
        gsKit_prim_sprite(gs_global, x1, y1, x2, y2, z1, col);
    }
cleanup:
    if (decal_single_pass_2d && cur_shader != NULL) {
        cur_shader->tfx = saved_2d_tfx;
        cur_shader->texture_alpha = saved_2d_texture_alpha;
        sLastTex0Reg = ~0ULL;
    }
    if (depth_only) {
        gs_global->PrimAlphaEnable = saved_alpha_enable;
        if (!sPrerenderDepthOnlyPass) {
            set_frame_write_mask(0);
        }
    }
}

/*
antes de falar que isso mudou a resolução interna presta atenção no que a função realmente faz
aqui só entram o tamanho real da saída, o framebuffer, a viewport e a proporção que o gs vai mostrar
*/
int gfx_ps2_apply_video_mode(void) {
    VideoMode vmode;

    Video_GetMode(&vmode);

    if (vmode.widescreen) {
        gs_global->Aspect = GS_ASPECT_16_9;
    } else {
        gs_global->Aspect = GS_ASPECT_4_3;
    }

    gs_global->Width = vmode.displayWidth;
    gs_global->Height = vmode.displayHeight;
    gs_global->Field = vmode.field;
    gs_global->Interlace = vmode.interlaced;
    gs_global->Mode = vmode.mode;

    gsKit_init_screen(gs_global);

    viewport(0, 0, vmode.displayWidth, vmode.displayHeight);
    scissor(0, 0, vmode.viewportWidth, vmode.viewportHeight);

    gfx_set_ps2_widescreen(vmode.widescreen);

    return 0;
}

/*
o init não só liga o renderer e vai embora, ele prepara oneshot, alpha e a memória alinhada que a dma precisa
depois o texmanager manda pra vram somente as texturas que realmente forem usadas
*/
static void renderer_init(void) {
    u64* p;

    sLastTexFilter = -1;
    sLastTestReg = ~0ULL;
    sLastClampReg = ~0ULL;
    sLastScissorReg = ~0ULL;
    sLastZbufReg = ~0ULL;
    sLastAlphaReg = ~0ULL;

    gsKit_mode_switch(gs_global, GS_ONESHOT);
    gs_global->Test->ZTST = 2;
    p = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_TEXA(0, 0, 0x80);
    *p++ = GS_TEXA;
    gsKit_queue_exec(gs_global);
    gsKit_queue_reset(gs_global->Os_Queue);
    tex_cache = memalign(128, TEXCACHE_SIZE);
    if (!tex_cache) {
        printf("oot-ps2: texture cache alloc failed\n");
        abort();
    }
    tex_ptr = tex_cache;
    tex_end = tex_cache + TEXCACHE_SIZE;
    viewport(0, 0, gs_global->Width, gs_global->Height);
    scissor(0, 0, gs_global->Width, gs_global->Height);

    gfx_ps2_apply_video_mode();
}

/*
algumas salas não mandam o fundo pré-renderizado de novo antes da geometria e aí aparece um buraco preto do caralho
quando isso acontece a última cópia válida volta sem z antes do 3d, não é um sprite aleatório desenhado duas vezes
*/
static void restore_prerender_fallback(void) {
    if (!sPrerenderRoomActive) {
        sPrerenderFallback.valid = false;
        sPrerenderFallback.texture = NULL;
        return;
    }
    if (!sPrerenderFallback.valid || sPrerenderFallback.seenThisFrame) {
        return;
    }
    struct Texture* t = sPrerenderFallback.texture;
    if (!t || t->tex.Vram == 0) {
        sPrerenderFallback.valid = false;
        return;
    }
    const bool old_test = z_test;
    const bool old_write = z_write;
    const bool old_blend = do_blend;
    const struct Clip old_clip = r_clip;
    z_test = false;
    depth_mask(false);
    set_blend(false);
    update_test();
    set_scissor_raw(0, 0, gs_global->Width - 1, gs_global->Height - 1);
    bind_texture(&t->tex);
    clamp_tex(t, true, true);
    gsKit_prim_sprite_texture_3d(gs_global, &t->tex, sPrerenderFallback.x1, sPrerenderFallback.y1,
                                 sPrerenderFallback.z1, sPrerenderFallback.u1, sPrerenderFallback.v1,
                                 sPrerenderFallback.x2, sPrerenderFallback.y2, sPrerenderFallback.z2,
                                 sPrerenderFallback.u2, sPrerenderFallback.v2, sPrerenderFallback.color);
    sLastBoundGsTex = NULL;
    sLastTex0Reg = ~0ULL;
    sLastTexFilter = -1;
    set_scissor_raw(old_clip.x0, old_clip.y0, old_clip.x1, old_clip.y1);
    z_test = old_test;
    depth_mask(old_write);
    update_test();
    set_blend(old_blend);
}

static void renderer_resize(void) {
}

/*
todo frame limpa o combine porque deixar estado velho aqui fode outro material que não tem nada a ver
o clear preto respeita o fundo do pause pra não apagar a imagem e tentar restaurar o nada logo depois
*/
static void start_frame(void) {
    restore_fog_pass_state(false);

    sTextureBlendReverse = false;
    sTextureBlendPrecolor = false;
    sDinFireTint = false;
    sTwoTextureBlendActive = false;
    sTwoTextureAlphaBlendActive = false;
    sTwoTextureEnvPrimTint = false;
    sRenderFrameSerial++;
    if (sRenderFrameSerial == 0) {
        sRenderFrameSerial = 1;
    }
    if (!sPrerenderRoomActive) {
        sPrerenderFallback.valid = false;
        sPrerenderFallback.texture = NULL;
    }
    sPrerenderFallback.seenThisFrame = false;
    sPrerender3dStarted = false;
    sLastTexFilter = -1;
    sLastTestReg = ~0ULL;
    sLastClampReg = ~0ULL;
    sLastScissorReg = ~0ULL;
    sLastZbufReg = ~0ULL;
    sLastAlphaReg = ~0ULL;
    sLastTex0Reg = ~0ULL;

    const bool old_write = z_write;
    const bool old_test = z_test;
    const bool old_blend = do_blend;
    const struct Clip old_clip = r_clip;
    set_blend(false);
    depth_mask(true);
    z_test = false;
    update_test();
    set_scissor_raw(0, 0, gs_global->Width - 1, gs_global->Height - 1);
    const bool pause_bg = gfx_ps2_pause_background_active();
    const u64 black = GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0);
    if (pause_bg) {
        set_frame_write_mask(0xffffffffU);
    }

    gsKit_prim_sprite(gs_global, 0, 0, gs_global->Width, gs_global->Height, 0, black);
    if (pause_bg) {
        set_frame_write_mask(0);
        gfx_ps2_restore_pause_background();
    }
    set_scissor_raw(old_clip.x0, old_clip.y0, old_clip.x1, old_clip.y1);
    z_test = old_test;
    depth_mask(old_write);
    update_test();
    set_blend(old_blend);
}

/*
o fps usa essa fonte pequena feita na mão pra não criar textura nem depender do menu inteiro
o número muda a cada meio segundo porque atualizar a conta em todo frame só deixa a leitura sambando
*/
static const uint8_t* fps_glyph(char c) {
    static const uint8_t f[7] = { 31, 16, 16, 30, 16, 16, 16 };
    static const uint8_t p[7] = { 30, 17, 17, 30, 16, 16, 16 };
    static const uint8_t sg[7] = { 15, 16, 16, 14, 1, 1, 30 };
    static const uint8_t colon[7] = { 0, 4, 4, 0, 4, 4, 0 };
    static const uint8_t dot[7] = { 0, 0, 0, 0, 0, 6, 6 };
    static const uint8_t sp[7] = { 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t num[10][7] = { { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },
                                        { 14, 17, 1, 2, 4, 8, 31 },     { 30, 1, 1, 14, 1, 1, 30 },
                                        { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 16, 30, 1, 1, 30 },
                                        { 14, 16, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
                                        { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 1, 14 } };
    if (c >= '0' && c <= '9') {
        return num[c - '0'];
    }
    if (c == 'F') {
        return f;
    }
    if (c == 'P') {
        return p;
    }
    if (c == 'S') {
        return sg;
    }
    if (c == ':') {
        return colon;
    }
    if (c == '.') {
        return dot;
    }
    return sp;
}

static void fps_char(float x, float y, char c, u64 color) {
    const uint8_t* rows = fps_glyph(c);
    const float scale = 2.0f;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = rows[row];
        int col = 0;
        while (col < 5) {
            while (col < 5 && !(bits & (1u << (4 - col)))) {
                col++;
            }
            if (col >= 5) {
                break;
            }
            int start = col;
            while (col < 5 && (bits & (1u << (4 - col)))) {
                col++;
            }
            gsKit_prim_sprite(gs_global, x + start * scale, y + row * scale, x + col * scale, y + (row + 1) * scale, 0,
                              color);
        }
    }
}

static void fps_overlay(void) {
    static u64 window_start;
    static u32 frames;
    static char text[16] = "FPS: 0.0";
    u64 now = Time_GetUsec();
    if (window_start == 0) {
        window_start = now;
    }
    frames++;
    if (now - window_start >= 500000ULL) {
        const u64 elapsed = now - window_start;
        const u32 fps10 = (u32)(((u64)frames * 10000000ULL + elapsed / 2) / elapsed);
        const u32 whole = fps10 / 10U;
        const u32 frac = fps10 % 10U;
        char* q = text;
        *q++ = 'F';
        *q++ = 'P';
        *q++ = 'S';
        *q++ = ':';
        *q++ = ' ';
        if (whole >= 100U) {
            *q++ = (char)('0' + (whole / 100U) % 10U);
        }
        if (whole >= 10U) {
            *q++ = (char)('0' + (whole / 10U) % 10U);
        }
        *q++ = (char)('0' + whole % 10U);
        *q++ = '.';
        *q++ = (char)('0' + frac);
        *q = '\0';
        frames = 0;
        window_start = now;
    }
    const bool old_test = z_test;
    const bool old_blend = do_blend;
    const struct Clip old_clip = r_clip;
    z_test = false;
    set_blend(false);
    update_test();
    set_scissor_raw(0, 0, gs_global->Width - 1, gs_global->Height - 1);
    const u64 white = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);
    float x = 5.0f;
    for (const char* q = text; *q; q++, x += 12.0f) {
        fps_char(x, 5.0f, *q, white);
    }
    set_scissor_raw(old_clip.x0, old_clip.y0, old_clip.x1, old_clip.y1);
    z_test = old_test;
    update_test();
    set_blend(old_blend);
}

static void menu_rect(float x0, float y0, float x1, float y1, u64 color) {
    gsKit_prim_sprite(gs_global, x0, y0, x1, y1, 0, color);
}

#define HOME_MENU_WIDTH 480.0f
#define HOME_MENU_HEIGHT 272.0f
#define HOME_HIGHLIGHT_R 38
#define HOME_HIGHLIGHT_G 92
#define HOME_HIGHLIGHT_B 78

/*
o menu home é só retângulo e a fonte de bits abaixo, não tem textura escondida nessa porra
480x272 é a medida lógica do menu e home_x com home_y escalam isso pro modo de vídeo sem mudar a resolução do jogo
*/
#include "home_font.inc"

static float home_scale(void) {
    const float sx = (float)gs_global->Width / HOME_MENU_WIDTH;
    const float sy = (float)gs_global->Height / HOME_MENU_HEIGHT;
    const float fit = sx < sy ? sx : sy;

    return fit * 0.86f;
}

static float home_x(float x) {
    const float sc = home_scale();
    return ((float)gs_global->Width - HOME_MENU_WIDTH * sc) * 0.5f + x * sc;
}

static float home_y(float y) {
    const float sc = home_scale();
    return ((float)gs_global->Height - HOME_MENU_HEIGHT * sc) * 0.5f + y * sc;
}

static u8 home_alpha(unsigned a255) {
    return (u8)((a255 * 128U + 127U) / 255U);
}

static u64 home_rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return GS_SETREG_RGBAQ(r >> 1, g >> 1, b >> 1, home_alpha(a), 0);
}

static void home_rect(float x, float y, float w, float h, u64 color) {
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    menu_rect(home_x(x), home_y(y), home_x(x + w), home_y(y + h), color);
}

static float home_text_width(const char* text, float size) {
    float width = 0.0f;
    if (text == NULL) {
        return 0.0f;
    }
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c < 32 || c > 126) {
            c = '?';
        }
        width += (float)sPs2HomeGlyphs[c - 32].advance * size;
    }
    return width;
}

static void home_draw_glyph(float x, float baseline, unsigned char c, float size, u64 color) {
    const Ps2HomeGlyph* glyph;
    if (c < 32 || c > 126) {
        c = '?';
    }
    glyph = &sPs2HomeGlyphs[c - 32];
    for (unsigned row = 0; row < glyph->h; row++) {
        unsigned bits = sPs2HomeFontRows[glyph->row + row];
        unsigned col = 0;
        while (col < glyph->w) {
            while (col < glyph->w && ((bits >> col) & 1U) == 0) {
                col++;
            }
            if (col >= glyph->w) {
                break;
            }
            unsigned begin = col;
            while (col < glyph->w && ((bits >> col) & 1U) != 0) {
                col++;
            }
            home_rect(x + ((float)glyph->xoff + (float)begin) * size,
                      baseline + ((float)glyph->yoff + (float)row) * size, ((float)(col - begin)) * size, size, color);
        }
    }
}

static void home_draw_text_pass(float x, float baseline, const char* text, float size, u64 color, bool centered) {
    float cursor;
    if (text == NULL || text[0] == '\0') {
        return;
    }
    cursor = centered ? x - home_text_width(text, size) * 0.5f : x;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c < 32 || c > 126) {
            c = '?';
        }
        home_draw_glyph(cursor, baseline, c, size, color);
        cursor += (float)sPs2HomeGlyphs[c - 32].advance * size;
    }
}

static void home_text(float x, float baseline, const char* text, float size, u64 color, u64 shadow, bool centered) {

    home_draw_text_pass(x + 1.0f, baseline + 1.0f, text, size, shadow, centered);
    home_draw_text_pass(x, baseline, text, size, color, centered);
}

static void home_render_main(const char* const* lines, int lineCount, int selectedIndex) {
    const u64 dim = home_rgba(0, 0, 0, 96);
    const u64 panel = home_rgba(0, 0, 0, 132);
    const u64 selected = home_rgba(HOME_HIGHLIGHT_R, HOME_HIGHLIGHT_G, HOME_HIGHLIGHT_B, 205);
    const u64 normal = home_rgba(218, 224, 218, 255);
    const u64 white = home_rgba(255, 255, 245, 255);
    const u64 shadow = home_rgba(0, 0, 0, 180);
    home_rect(0, 0, 480, 272, dim);
    home_rect(112, 34, 256, 210, panel);
    for (int i = 0; i < lineCount; i++) {
        const float y = 62.0f + (float)i * 38.0f;
        u64 color = normal;
        if (i == selectedIndex) {
            home_rect(128, y - 22.0f, 224, 28, selected);
            color = white;
        }
        home_text(240, y, lines[i], 0.72f, color, shadow, true);
    }
}

static void home_render_submenu(const char* title, const char* const* lines, int lineCount, int selectedIndex,
                                const char* statusMessage, int firstRow, int totalRows, bool video) {
    const u64 dim = home_rgba(0, 0, 0, 112);
    const u64 panel = home_rgba(0, 0, 0, 154);
    const u64 selected = home_rgba(HOME_HIGHLIGHT_R, HOME_HIGHLIGHT_G, HOME_HIGHLIGHT_B, 205);
    const u64 normal = home_rgba(218, 224, 218, 255);
    const u64 white = home_rgba(255, 255, 245, 255);
    const u64 hint = home_rgba(170, 190, 180, 255);
    const u64 shadow = home_rgba(0, 0, 0, 180);
    const float titleY = video ? 48.0f : 50.0f;
    const float firstY = video ? 76.0f : 82.0f;
    home_rect(0, 0, 480, 272, dim);
    home_rect(34, 20, 412, 232, panel);
    home_text(240, titleY, title, 0.82f, white, shadow, true);
    for (int i = 0; i < lineCount; i++) {
        const float y = firstY + (float)i * 22.0f;
        u64 color = normal;
        if (i == selectedIndex) {
            home_rect(54, y - 17.0f, 372, 22, selected);
            color = white;
        }
        home_text(70, y, lines[i], 0.68f, color, shadow, false);
    }
    if (!video) {
        if (firstRow > 0) {
            home_text(422, 82, "^", 0.48f, hint, home_rgba(0, 0, 0, 160), true);
        }
        if (firstRow + lineCount < totalRows) {
            home_text(422, 214, "v", 0.48f, hint, home_rgba(0, 0, 0, 160), true);
        }
    }
    if (statusMessage && statusMessage[0]) {
        home_text(240, 242, statusMessage, 0.48f, hint, home_rgba(0, 0, 0, 160), true);
    }
}

void gfx_ps2_set_boot_progress(int code, const char* label) {
    sBootProgressCode = code;
    if (label == NULL) {
        sBootProgressLabel[0] = '\0';
    } else {
        snprintf(sBootProgressLabel, sizeof(sBootProgressLabel), "%s", label);
    }
}

static void boot_progress_draw_contents(int code) {
    char codeText[16];
    const u64 white = home_rgba(255, 255, 245, 255);
    const u64 shadow = home_rgba(0, 0, 0, 220);

    snprintf(codeText, sizeof(codeText), "%03d", code < 0 ? 0 : code);
    home_rect(0, 0, 480, 272, home_rgba(0, 0, 0, 255));
    home_text(240, 108, "Boot Check", 0.72f, white, shadow, true);
    home_text(240, 148, codeText, 1.45f, white, shadow, true);
}

void gfx_ps2_render_boot_progress(int code, const char* label) {
    const bool old_test = z_test;
    const bool old_blend = do_blend;
    const bool old_write = z_write;
    const struct Clip old_clip = r_clip;
    (void)label;
    sSuppressFpsOverlay = true;
    z_test = false;
    z_write = false;
    set_blend(true);
    update_test();
    depth_mask(false);
    set_scissor_raw(0, 0, gs_global->Width - 1, gs_global->Height - 1);
    boot_progress_draw_contents(code);
    set_scissor_raw(old_clip.x0, old_clip.y0, old_clip.x1, old_clip.y1);
    z_test = old_test;
    z_write = old_write;
    depth_mask(old_write);
    update_test();
    set_blend(old_blend);
}

void gfx_ps2_render_menu(const char* title, const char* const* lines, int lineCount, int selectedIndex,
                         const char* statusMessage, int firstRow, int totalRows) {
    sSuppressFpsOverlay = true;
    const bool old_test = z_test;
    const bool old_blend = do_blend;
    const bool old_write = z_write;
    const struct Clip old_clip = r_clip;
    z_test = false;
    z_write = false;
    set_blend(true);
    update_test();
    depth_mask(false);
    set_scissor_raw(0, 0, gs_global->Width - 1, gs_global->Height - 1);

    if (title == NULL) {
        home_render_main(lines, lineCount, selectedIndex);
    } else if (strcmp(title, "Video Settings") == 0) {
        home_render_submenu(title, lines, lineCount, selectedIndex, statusMessage, firstRow, totalRows, true);
    } else {
        home_render_submenu(title, lines, lineCount, selectedIndex, statusMessage, firstRow, totalRows, false);
    }

    set_scissor_raw(old_clip.x0, old_clip.y0, old_clip.x1, old_clip.y1);
    z_test = old_test;
    z_write = old_write;
    depth_mask(old_write);
    update_test();
    set_blend(old_blend);
}

static void end_frame(void) {
    if (sSuppressFpsOverlay) {
        sSuppressFpsOverlay = false;
    } else if (sFpsOverlayEnabled) {
        fps_overlay();
    }
}

static void finish_render(void) {
}

static void gfx_ps2_render_home_menu_main(int selectedIndex, uint8_t highlightRed, uint8_t highlightGreen,
                                          uint8_t highlightBlue) {
    static const char* items[] = {
        "Resume Game", "Controller Mapping", "Video Settings", "About", "Exit Game",
    };
    int i;

    home_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, home_rgba(0, 0, 0, 96));
    home_rect(112, 34, 256, 210, home_rgba(0, 0, 0, 132));

    for (i = 0; i < 5; i++) {
        int y = 62 + (i * 38);
        unsigned int color = home_rgba(218, 224, 218, 255);

        if (selectedIndex == i) {
            home_rect(128, y - 22, 224, 28, home_rgba(highlightRed, highlightGreen, highlightBlue, 205));
            color = home_rgba(255, 255, 245, 255);
        }

        home_text(HOME_MENU_WIDTH / 2, y, items[i], 0.72f, color, home_rgba(0, 0, 0, 180), true);
    }
}

void gfx_ps2_set_home_menu_background_active(bool active) {
    sHomeMenuBgActive = active;

    if (!active) {
        sHomeMenuBgCaptureRequested = false;
        sHomeMenuBgCaptured = false;
    }
}

void gfx_ps2_request_home_menu_background(void) {
    sHomeMenuBgCaptureRequested = true;
}

static void gfx_ps2_render_controller_mapping(int selectedIndex, const char* statusMessage, uint8_t highlightRed,
                                              uint8_t highlightGreen, uint8_t highlightBlue) {
    char line[80];
    char value[96];
    int bindingCount = OotPortControls_GetBindingCount();
    int deadzoneRow = bindingCount;
    int n64StickScalingRow = bindingCount + 1;
    int saveRow = bindingCount + 2;
    int resetRow = bindingCount + 3;
    int backRow = bindingCount + 4;
    int totalRows = backRow + 1;
    int visibleRows = 7;
    int firstRow = selectedIndex - (visibleRows / 2);
    int row;

    if (visibleRows > totalRows) {
        visibleRows = totalRows;
    }
    if (firstRow < 0) {
        firstRow = 0;
    }
    if ((firstRow + visibleRows) > totalRows) {
        firstRow = totalRows - visibleRows;
    }

    home_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, home_rgba(0, 0, 0, 112));
    home_rect(34, 20, 412, 232, home_rgba(0, 0, 0, 154));
    home_text(HOME_MENU_WIDTH / 2, 50, "Controller Mapping", 0.82f, home_rgba(255, 255, 245, 255),
              home_rgba(0, 0, 0, 180), true);

    for (row = firstRow; row < firstRow + visibleRows; row++) {
        int y = 82 + ((row - firstRow) * 22);
        unsigned int color = home_rgba(218, 224, 218, 255);

        if (selectedIndex == row) {
            home_rect(54, y - 17, 372, 22, home_rgba(highlightRed, highlightGreen, highlightBlue, 205));
            color = home_rgba(255, 255, 245, 255);
        }

        if (row < bindingCount) {
            OotPortControls_GetBindingValueText(row, value, sizeof(value));
            snprintf(line, sizeof(line), "%s: %.32s", OotPortControls_GetBindingName(row), value);
        } else if (row == deadzoneRow) {
            snprintf(line, sizeof(line), "Deadzone: %d", OotPortControls_GetDeadzone());
        } else if (row == n64StickScalingRow) {
            snprintf(line, sizeof(line), "N64 Stick Scaling: %s",
                     OotPortControls_IsN64StickScalingEnabled() ? "On" : "Off");
        } else if (row == saveRow) {
            snprintf(line, sizeof(line), "Save controls.ini");
        } else if (row == resetRow) {
            snprintf(line, sizeof(line), "Reset defaults");
        } else {
            snprintf(line, sizeof(line), "Back");
        }

        home_text(70, y, line, 0.68f, color, home_rgba(0, 0, 0, 180), true);
    }

    if (firstRow > 0) {
        home_text(422, 82, "^", 0.48f, home_rgba(170, 190, 180, 255), home_rgba(0, 0, 0, 160), true);
    }
    if ((firstRow + visibleRows) < totalRows) {
        home_text(422, 214, "v", 0.48f, home_rgba(170, 190, 180, 255), home_rgba(0, 0, 0, 160), true);
    }

    if ((statusMessage != NULL) && (statusMessage[0] != '\0')) {
        home_text(HOME_MENU_WIDTH / 2, 242, statusMessage, 0.48f, home_rgba(170, 190, 180, 255),
                  home_rgba(0, 0, 0, 160), true);
    } else {
        home_text(HOME_MENU_WIDTH / 2, 242, "Left/Right change  Cross select  Circle back", 0.48f,
                  home_rgba(170, 190, 180, 255), home_rgba(0, 0, 0, 160), true);
    }
}

static void gfx_ps2_render_video_settings(int selectedIndex, const char* statusMessage, uint8_t highlightRed,
                                          uint8_t highlightGreen, uint8_t highlightBlue) {
    char line[80];
    int row;

    home_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, home_rgba(0, 0, 0, 112));
    home_rect(34, 20, 412, 232, home_rgba(0, 0, 0, 154));
    home_text(HOME_MENU_WIDTH / 2, 48, "Video Settings", 0.82f, home_rgba(255, 255, 245, 255), home_rgba(0, 0, 0, 180),
              true);
    for (row = 0; row < 6; row++) {
        int y = 76 + (row * 22);
        unsigned int color = home_rgba(218, 224, 218, 255);

        if (selectedIndex == row) {
            home_rect(54, y - 17, 372, 22, home_rgba(highlightRed, highlightGreen, highlightBlue, 205));
            color = home_rgba(255, 255, 245, 255);
        }

        switch (row) {
            case 0:
                snprintf(line, sizeof(line), "Resolution: %s", Video_GetResolutionName());
                break;
            case 1:
                snprintf(line, sizeof(line), "Aspect Ratio: %s", Video_GetAspectName());
                break;
            case 2:
                snprintf(line, sizeof(line), "Apply video mode");
                break;
            case 3:
                snprintf(line, sizeof(line), "Save video.ini");
                break;
            case 4:
                snprintf(line, sizeof(line), "Reset defaults");
                break;
            default:
                snprintf(line, sizeof(line), "Back");
                break;
        }

        home_text(70, y, line, 0.68f, color, home_rgba(0, 0, 0, 180), true);
    }

    if ((statusMessage != NULL) && (statusMessage[0] != '\0')) {
        home_text(HOME_MENU_WIDTH / 2, 242, statusMessage, 0.48f, home_rgba(170, 190, 180, 255),
                  home_rgba(0, 0, 0, 160), true);
    } else {
        home_text(HOME_MENU_WIDTH / 2, 242, "Left/Right change  Cross select  Circle back", 0.48f,
                  home_rgba(170, 190, 180, 255), home_rgba(0, 0, 0, 160), true);
    }
}

#define HOME_MENU_ABOUT_QR_MODULE_COUNT 33
#define HOME_MENU_ABOUT_QR_MODULE_SIZE 4
#define HOME_MENU_ABOUT_VISIBLE_LINES 6

static const char* const sHomeMenuAboutQrModules[HOME_MENU_ABOUT_QR_MODULE_COUNT] = {
    "111111101011001101010111001111111", "100000100101010100101001101000001", "101110101100100110100111001011101",
    "101110101000010001111111001011101", "101110100000111100111100101011101", "100000101000010011010010101000001",
    "111111101010101010101010101111111", "000000001101001111000110100000000", "010101111100111010101101111101101",
    "110000001011100111001100111100011", "110111111111010100111000001110101", "000110001101111110101111101011000",
    "011100101100001010101010011101010", "100001001000111001101100000110111", "111101101011010000110001111110110",
    "100001010100111010101110101101010", "010111110110101011100111101010010", "110010010011100000000011100101010",
    "011101101110100011000100100000111", "110100010000111100111010111100011", "101011101011000101111000110000000",
    "000010010100110000010101100000111", "110100101010101010010100111111001", "011001000001101101111100101100011",
    "101011101100000000101100111111011", "000000001101011111111111100011001", "111111101011110110000010101011110",
    "100000101101111100111111100010011", "101110100101100101111000111111000", "101110101100101110110110101011010",
    "101110100011111011111010110011001", "100000101001010000011100101000000", "111111100011001001011101110011010",
};

static const char* const sHomeMenuAboutLines[] = {
    "A PlayStation Portable port",
    "of The Legend of Zelda:",
    "Ocarina of Time.",
    "",
    "Port of PSP by Z2442 and the PS2",
    "port is by Eclipse Blade Studio. Thank you!",
    "To my faimily that put up with",
    "me playing around with two decade",
    "old handheld!",
    "Everyone for all of your help",
    "making this a reality.",
    "I could not done this Alone.",
    "",
    "Special thanks:",
    "Original SM64 Fast3d renderer: Mr.Neo240",
    "R&D Team: M-CID and the PS2 community",
    "Daedalus Team: Wally, Re4thewin, Dreamer72",
    "Discord Team: Mr. Iron, Acid_Snake, Krazynez",
    "N64 Decompilation Team",
    "",
    "Scan the QR code to the right",
    "to talk to the team on discord!",
};

static void gfx_ps2_draw_home_menu_about_qr(int x, int y) {
    const int codeSize = HOME_MENU_ABOUT_QR_MODULE_COUNT * HOME_MENU_ABOUT_QR_MODULE_SIZE;
    const int quietZone = HOME_MENU_ABOUT_QR_MODULE_SIZE * 2;
    int row;

    home_rect(x - quietZone, y - quietZone, codeSize + (quietZone * 2), codeSize + (quietZone * 2),
              home_rgba(255, 255, 255, 255));

    for (row = 0; row < HOME_MENU_ABOUT_QR_MODULE_COUNT; row++) {
        int column;

        for (column = 0; column < HOME_MENU_ABOUT_QR_MODULE_COUNT; column++) {
            if (sHomeMenuAboutQrModules[row][column] == '1') {
                home_rect(x + (column * HOME_MENU_ABOUT_QR_MODULE_SIZE), y + (row * HOME_MENU_ABOUT_QR_MODULE_SIZE),
                          HOME_MENU_ABOUT_QR_MODULE_SIZE, HOME_MENU_ABOUT_QR_MODULE_SIZE, home_rgba(0, 0, 0, 255));
            }
        }
    }
}

static void gfx_ps2_render_about(int scrollOffset) {
    const unsigned int titleColor = home_rgba(255, 255, 245, 255);
    const unsigned int bodyColor = home_rgba(218, 224, 218, 255);
    const unsigned int hintColor = home_rgba(170, 190, 180, 255);
    const unsigned int shadowColor = home_rgba(0, 0, 0, 180);
    const int aboutLineCount = ARRAY_COUNT(sHomeMenuAboutLines);
    int line;

    if (scrollOffset < 0) {
        scrollOffset = 0;
    }
    if (scrollOffset > (aboutLineCount - HOME_MENU_ABOUT_VISIBLE_LINES)) {
        scrollOffset = aboutLineCount - HOME_MENU_ABOUT_VISIBLE_LINES;
    }

    home_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, home_rgba(0, 0, 0, 112));
    home_rect(20, 20, 440, 232, home_rgba(0, 0, 0, 154));
    home_text(HOME_MENU_WIDTH / 2, 54, "About", 0.82f, titleColor, shadowColor, true);
    home_rect(282, 72, 1, 142, home_rgba(112, 132, 122, 180));

    for (line = 0; line < HOME_MENU_ABOUT_VISIBLE_LINES; line++) {
        home_text(48, 96 + (line * 20), sHomeMenuAboutLines[scrollOffset + line], 0.48f, bodyColor, shadowColor, true);
    }

    if (scrollOffset > 0) {
        home_text(266, 90, "^", 0.48f, hintColor, shadowColor, true);
    }
    if ((scrollOffset + HOME_MENU_ABOUT_VISIBLE_LINES) < aboutLineCount) {
        home_text(266, 202, "v", 0.48f, hintColor, shadowColor, true);
    }

    gfx_ps2_draw_home_menu_about_qr(306, 82);
    home_text(HOME_MENU_WIDTH / 2, 232, "Up/Down scroll  Circle, Cross, or Start: Back", 0.42f, hintColor,
              home_rgba(0, 0, 0, 160), true);
}

void gfx_ps2_render_home_menu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage,
                              uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue) {
    const bool oldTest = z_test;
    const bool oldBlend = do_blend;
    const bool oldWrite = z_write;
    const struct Clip oldClip = r_clip;

    z_test = false;
    z_write = false;
    set_blend(true);
    update_test();
    depth_mask(false);
    set_scissor_raw(0, 0, gs_global->Width - 1, gs_global->Height - 1);

    if (screen == 1) {
        gfx_ps2_render_controller_mapping(submenuSelectedIndex, statusMessage, highlightRed, highlightGreen,
                                          highlightBlue);
    } else if (screen == 2) {
        gfx_ps2_render_video_settings(submenuSelectedIndex, statusMessage, highlightRed, highlightGreen, highlightBlue);
    } else if (screen == 3) {
        gfx_ps2_render_about(submenuSelectedIndex);
    } else {
        gfx_ps2_render_home_menu_main(selectedIndex, highlightRed, highlightGreen, highlightBlue);
    }

    set_scissor_raw(oldClip.x0, oldClip.y0, oldClip.x1, oldClip.y1);
    z_test = oldTest;
    z_write = oldWrite;
    depth_mask(oldWrite);
    update_test();
    set_blend(oldBlend);
}

/*
essa tabela liga o fast3d nas funções deste arquivo, então não sai trocando assinatura e achando que ninguém usa
os gfx_ps2_* que ficaram públicos são chamados pelo gerenciador de janela, pelos menus e pela tradução do combiner
*/
struct GfxRenderingAPI gfx_ps2_rapi = {
    .z_is_from_0_to_1 = depth_range_is_zero_to_one,
    .unload_shader = unload_shader,
    .load_shader = load_shader,
    .create_and_load_new_shader = create_shader,
    .lookup_shader = lookup_shader,
    .shader_get_info = shader_info,
    .new_texture = new_texture,
    .select_texture = select_texture,
    .upload_texture = upload_texture,
    .set_sampler_parameters = sampler,
    .set_texture_env_color = env_color,
    .set_depth_test = depth_test,
    .set_depth_mask = depth_mask,
    .set_zmode_decal = zdecal,
    .set_viewport = viewport,
    .set_scissor = scissor,
    .set_use_alpha = set_alpha_enabled,
    .draw_triangles = draw_triangles,
    .draw_fog_triangles = draw_fog_triangles,
    .init = renderer_init,
    .on_resize = renderer_resize,
    .start_frame = start_frame,
    .end_frame = end_frame,
    .finish_render = finish_render,
};
