#include "oot_ps2_asset_setup.h"

#include "oot_port_asset_loader.h"
#include "oot_ps2_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <zlib.h>

#define OOT_PS2_INPUT_ROM_PATH "baserom.z64"
#define OOT_PS2_PACKED_ASSET_PATH "oot_ps2_assets.bin"
#define OOT_PS2_PACKED_ASSET_TEMP_PATH "oot_ps2_assets.tmp"
#define OOT_PS2_ROM_SIZE 0x02000000U
#define OOT_PS2_ROM_CRC32 0xCD16C529U
#define OOT_PS2_DMADATA_OFFSET 0x7430U
#define OOT_PS2_DMADATA_COUNT 1510U
#define OOT_PS2_TRANSFORM_ZERO_SELECTOR 56U
#define OOT_PS2_TRANSFORM_MAPPED_SELECTOR 57U
#define OOT_PS2_IO_CHUNK_SIZE 0x4000U
#define OOT_PS2_IO_ZERO_RETRY_COUNT 16
#define OOT_PS2_IO_ZERO_RETRY_USEC 1000
#define OOT_PS2_PRERENDER_WIDTH 320U
#define OOT_PS2_PRERENDER_HEIGHT 240U
#define OOT_PS2_PRERENDER_RGBA16_SIZE (OOT_PS2_PRERENDER_WIDTH * OOT_PS2_PRERENDER_HEIGHT * 2U)

typedef struct AssetSetupJpegError {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} AssetSetupJpegError;

static void AssetSetup_JpegErrorExit(j_common_ptr common) {
    AssetSetupJpegError* error = (AssetSetupJpegError*)common->err;
    longjmp(error->jump, 1);
}

static u8 AssetSetup_ReadTextureWordByte(const u8* data, size_t base, size_t offset) {
    return data[base + (offset ^ 7U)];
}

static s32 AssetSetup_ExtractTextureWordJpeg(const u8* data, size_t size, size_t base, u8* output,
                                                   size_t outputCapacity, size_t* outputSize) {
    size_t capacity;
    size_t offset;
    u8 previous = 0;

    if ((base & 7U) != 0 || base + 8U > size ||
        AssetSetup_ReadTextureWordByte(data, base, 0) != 0xFF ||
        AssetSetup_ReadTextureWordByte(data, base, 1) != 0xD8 ||
        AssetSetup_ReadTextureWordByte(data, base, 2) != 0xFF) {
        return false;
    }

    capacity = size - base;
    if (capacity > OOT_PS2_PRERENDER_RGBA16_SIZE) {
        capacity = OOT_PS2_PRERENDER_RGBA16_SIZE;
    }
    if (capacity > outputCapacity) {
        capacity = outputCapacity;
    }

    for (offset = 0; offset < capacity; offset++) {
        u8 current = AssetSetup_ReadTextureWordByte(data, base, offset);
        output[offset] = current;
        if (previous == 0xFF && current == 0xD9) {
            *outputSize = offset + 1U;
            return true;
        }
        previous = current;
    }

    return false;
}

static void AssetSetup_WriteTextureWordRgba16(u8* data, size_t base, size_t pixelIndex, u8 r, u8 g, u8 b) {
    size_t offset = pixelIndex * 2U;
    u16 value = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);

    data[base + (offset ^ 7U)] = (u8)(value >> 8);
    data[base + ((offset + 1U) ^ 7U)] = (u8)value;
}

static s32 AssetSetup_DecodePrerenderJpeg(u8* data, size_t size, size_t base, u8* jpegInput,
                                                size_t jpegInputCapacity) {
    struct jpeg_decompress_struct decoder;
    AssetSetupJpegError error;
    JSAMPROW rows[1];
    u8 row[OOT_PS2_PRERENDER_WIDTH * 3U];
    size_t jpegSize = 0;
    size_t y;

    if (!AssetSetup_ExtractTextureWordJpeg(data, size, base, jpegInput, jpegInputCapacity, &jpegSize)) {
        return 0;
    }

    memset(&decoder, 0, sizeof(decoder));
    memset(&error, 0, sizeof(error));
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = AssetSetup_JpegErrorExit;
    if (setjmp(error.jump) != 0) {
        if (decoder.mem != NULL) {
            jpeg_destroy_decompress(&decoder);
        }
        return 0;
    }

    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, jpegInput, jpegSize);
    jpeg_read_header(&decoder, TRUE);
    if (decoder.image_width != OOT_PS2_PRERENDER_WIDTH || decoder.image_height != OOT_PS2_PRERENDER_HEIGHT) {
        jpeg_destroy_decompress(&decoder);
        return 0;
    }

    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    if (decoder.output_width != OOT_PS2_PRERENDER_WIDTH || decoder.output_height != OOT_PS2_PRERENDER_HEIGHT ||
        decoder.output_components != 3) {
        jpeg_abort_decompress(&decoder);
        jpeg_destroy_decompress(&decoder);
        return 0;
    }

    for (y = 0; y < OOT_PS2_PRERENDER_HEIGHT; y++) {
        size_t x;
        rows[0] = row;
        if (jpeg_read_scanlines(&decoder, rows, 1) != 1) {
            jpeg_abort_decompress(&decoder);
            jpeg_destroy_decompress(&decoder);
            return 0;
        }
        for (x = 0; x < OOT_PS2_PRERENDER_WIDTH; x++) {
            size_t rgb = x * 3U;
            AssetSetup_WriteTextureWordRgba16(data, base, y * OOT_PS2_PRERENDER_WIDTH + x,
                                                     row[rgb], row[rgb + 1U], row[rgb + 2U]);
        }
    }

    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return 1;
}

static s32 AssetSetup_PredecodePrerenderJpegs(u8* data, size_t size, const OotPortExternalAsset* asset,
                                                    u8* jpegInput, size_t jpegInputCapacity) {
    size_t offset = 0;
    s32 decoded = 0;

    if ((asset->flags & OOT_PORT_EXTERNAL_ASSET_TEXTURE_WORDS) == 0 || strstr(asset->name, "_room_") == NULL ||
        size < OOT_PS2_PRERENDER_RGBA16_SIZE) {
        return 0;
    }

    while (offset + OOT_PS2_PRERENDER_RGBA16_SIZE <= size) {
        if (AssetSetup_DecodePrerenderJpeg(data, size, offset, jpegInput, jpegInputCapacity)) {
            decoded++;
            offset += OOT_PS2_PRERENDER_RGBA16_SIZE;
        } else {
            offset += 8U;
        }
    }

    return decoded;
}

typedef struct DmaEntry {
    u32 vromStart;
    u32 vromEnd;
    u32 romStart;
    u32 romEnd;
} DmaEntry;

extern const u8 gOotPortAssetTransformCompressed[];
extern const u8 gOotPortAssetTransformCompressedEnd[];

static u32 sOotPs2AssetSetupProgress;
static s32 sOotPs2AssetSetupErrorShown;

static void AssetSetup_ShowProgress(u32 progressPermille, const char* status) {
    sOotPs2AssetSetupProgress = progressPermille;
    printf("oot-ps2 asset setup %u/1000: %s\n", (unsigned)progressPermille, status);
}

static void AssetSetup_ShowError(const char* status) {
    sOotPs2AssetSetupErrorShown = true;
    printf("oot-ps2 asset setup failed at %u/1000: %s\n", (unsigned)sOotPs2AssetSetupProgress, status);
}

static u32 AssetSetup_ReadLe32(const u8* data) {
    return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u32 AssetSetup_ReadBe32(const u8* data) {
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | (u32)data[3];
}

static s32 AssetSetup_ReadAt(Handle fd, u32 offset, void* output, size_t size) {
    u8* cursor = output;
    s32 zeroReads = 0;

    if (File_Seek(fd, (Offset)offset, OOT_PS2_SEEK_SET) < 0) {
        return false;
    }

    while (size != 0) {
        size_t chunk = size > OOT_PS2_IO_CHUNK_SIZE ? OOT_PS2_IO_CHUNK_SIZE : size;
        s32 read = File_Read(fd, cursor, (u32)chunk);

        if (read < 0) {
            return false;
        }
        if (read == 0) {
            if (zeroReads++ >= OOT_PS2_IO_ZERO_RETRY_COUNT) {
                return false;
            }
            Thread_Delay(OOT_PS2_IO_ZERO_RETRY_USEC);
            continue;
        }
        zeroReads = 0;
        cursor += read;
        size -= (size_t)read;
    }

    return true;
}

static s32 AssetSetup_WriteAt(Handle fd, u32 offset, const void* input, size_t size) {
    const u8* cursor = input;
    s32 zeroWrites = 0;

    if (File_Seek(fd, (Offset)offset, OOT_PS2_SEEK_SET) < 0) {
        return false;
    }

    while (size != 0) {
        size_t chunk = size > OOT_PS2_IO_CHUNK_SIZE ? OOT_PS2_IO_CHUNK_SIZE : size;
        s32 written = File_Write(fd, cursor, (u32)chunk);

        if (written < 0) {
            return false;
        }
        if (written == 0) {
            if (zeroWrites++ >= OOT_PS2_IO_ZERO_RETRY_COUNT) {
                return false;
            }
            Thread_Delay(OOT_PS2_IO_ZERO_RETRY_USEC);
            continue;
        }
        zeroWrites = 0;
        cursor += written;
        size -= (size_t)written;
    }

    return true;
}

static s32 AssetSetup_CopyUncompressedAsset(Handle romFd, Handle outputFd, u32 romOffset,
                                                   u32 outputOffset, size_t size) {
    u8 buffer[OOT_PS2_IO_CHUNK_SIZE];

    while (size != 0) {
        size_t chunk = size > sizeof(buffer) ? sizeof(buffer) : size;

        if (!AssetSetup_ReadAt(romFd, romOffset, buffer, chunk) ||
            !AssetSetup_WriteAt(outputFd, outputOffset, buffer, chunk)) {
            return false;
        }
        romOffset += (u32)chunk;
        outputOffset += (u32)chunk;
        size -= chunk;
    }

    return true;
}

static s32 AssetSetup_FileHasSize(const char* path, size_t expectedSize) {
    Handle fd = File_Open(path, OOT_PS2_FILE_RDONLY, 0);
    Offset size;

    if (fd < 0) {
        return false;
    }

    size = File_Seek(fd, 0, OOT_PS2_SEEK_END);
    File_Close(fd);
    return (size >= 0) && ((size_t)size == expectedSize);
}

static s32 AssetSetup_PackHasCompressedPrerenderJpeg(const char* path) {
    const OotPortExternalAsset* asset = NULL;
    Handle fd;
    u8 buffer[OOT_PS2_IO_CHUNK_SIZE];
    size_t i;
    size_t offset;
    size_t size;

    for (i = 0; i < gOotPortExternalAssetCount; i++) {
        if (strcmp(gOotPortExternalAssets[i].name, "link_home_room_0") == 0) {
            asset = &gOotPortExternalAssets[i];
            break;
        }
    }
    if (asset == NULL) {
        return false;
    }

    fd = File_Open(path, OOT_PS2_FILE_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    size = asset->vromEnd - asset->vromStart;
    for (offset = 0; offset < size; offset += sizeof(buffer)) {
        size_t chunk = size - offset;
        size_t j;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        if (!AssetSetup_ReadAt(fd, (u32)(asset->fileOffset + offset), buffer, chunk)) {
            File_Close(fd);
            return false;
        }
        for (j = 0; j + 8U <= chunk; j += 8U) {
            if ((buffer[j] == 0xFF && buffer[j + 1U] == 0xD8 && buffer[j + 2U] == 0xFF) ||
                (buffer[j + 7U] == 0xFF && buffer[j + 6U] == 0xD8 && buffer[j + 5U] == 0xFF)) {
                File_Close(fd);
                return true;
            }
        }
    }

    File_Close(fd);
    return false;
}

static s32 AssetSetup_CopyFile(const char* sourcePath, const char* destinationPath) {
    Handle sourceFd = -1;
    Handle destinationFd = -1;
    Offset sourceSize;
    Offset destinationSize;
    u8 buffer[OOT_PS2_IO_CHUNK_SIZE];
    u32 offset = 0;
    s32 ok = false;

    sourceFd = File_Open(sourcePath, OOT_PS2_FILE_RDONLY, 0);
    if (sourceFd < 0) {
        goto cleanup;
    }

    sourceSize = File_Seek(sourceFd, 0, OOT_PS2_SEEK_END);
    if ((sourceSize < 0) || ((u64)sourceSize > 0xFFFFFFFFULL)) {
        goto cleanup;
    }

    destinationFd = File_Open(destinationPath,
                                    OOT_PS2_FILE_WRONLY | OOT_PS2_FILE_CREAT | OOT_PS2_FILE_TRUNC, 0777);
    if (destinationFd < 0) {
        goto cleanup;
    }

    while (offset < (u32)sourceSize) {
        u32 remaining = (u32)sourceSize - offset;
        u32 chunk = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;

        if (!AssetSetup_ReadAt(sourceFd, offset, buffer, chunk) ||
            !AssetSetup_WriteAt(destinationFd, offset, buffer, chunk)) {
            goto cleanup;
        }
        offset += chunk;
    }

    destinationSize = File_Seek(destinationFd, 0, OOT_PS2_SEEK_END);
    ok = destinationSize == sourceSize;

cleanup:
    if (destinationFd >= 0) {
        File_Close(destinationFd);
    }
    if (sourceFd >= 0) {
        File_Close(sourceFd);
    }
    return ok;
}

static Handle AssetSetup_OpenRom(char* pathBuffer, size_t pathBufferSize) {
    const char* path = OotPort_ResolveRootPath(OOT_PS2_INPUT_ROM_PATH, pathBuffer, pathBufferSize);

    return File_Open(path, OOT_PS2_FILE_RDONLY, 0);
}

static s32 AssetSetup_ValidateRom(Handle fd) {
    static const u8 expectedHeader[16] = {
        0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F,
        0x80, 0x00, 0x04, 0x00, 0x00, 0x00, 0x14, 0x49,
    };
    u8* buffer;
    u8 header[16];
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t remaining = OOT_PS2_ROM_SIZE;
    size_t processed = 0;
    Offset size = File_Seek(fd, 0, OOT_PS2_SEEK_END);

    if ((size < 0) || ((u32)size != OOT_PS2_ROM_SIZE) ||
        !AssetSetup_ReadAt(fd, 0, header, sizeof(header)) ||
        (memcmp(header, expectedHeader, sizeof(header)) != 0)) {
        AssetSetup_ShowError("NTSC 1.0 z64 ROM required");
        return false;
    }

    buffer = malloc(OOT_PS2_IO_CHUNK_SIZE);
    if (buffer == NULL) {
        AssetSetup_ShowError("Not enough memory to validate ROM");
        return false;
    }

    if (File_Seek(fd, 0, OOT_PS2_SEEK_SET) < 0) {
        free(buffer);
        AssetSetup_ShowError("Could not read the ROM");
        return false;
    }

    while (remaining != 0) {
        size_t chunk = remaining > OOT_PS2_IO_CHUNK_SIZE ? OOT_PS2_IO_CHUNK_SIZE : remaining;
        s32 read = File_Read(fd, buffer, (u32)chunk);

        if (read <= 0) {
            free(buffer);
            AssetSetup_ShowError("Could not read the ROM");
            return false;
        }

        crc = crc32(crc, buffer, read);
        remaining -= (size_t)read;
        processed += (size_t)read;

        if (((processed & 0xFFFFF) == 0) || (remaining == 0)) {
            char status[64];

            snprintf(status, sizeof(status), "Validating ROM: %lu / 32 MB", (unsigned long)(processed >> 20));
            AssetSetup_ShowProgress((u32)((processed * 100) / OOT_PS2_ROM_SIZE), status);
        }
    }

    free(buffer);

    if ((u32)crc != OOT_PS2_ROM_CRC32) {
        AssetSetup_ShowError("ROM checksum failed - NTSC 1.0 required");
        return false;
    }

    return true;
}

static s32 AssetSetup_LoadDmaTable(Handle fd, DmaEntry* entries) {
    u8 raw[OOT_PS2_DMADATA_COUNT * 16];
    size_t i;

    if (!AssetSetup_ReadAt(fd, OOT_PS2_DMADATA_OFFSET, raw, sizeof(raw))) {
        return false;
    }

    for (i = 0; i < OOT_PS2_DMADATA_COUNT; i++) {
        const u8* input = &raw[i * 16];

        entries[i].vromStart = AssetSetup_ReadBe32(input + 0);
        entries[i].vromEnd = AssetSetup_ReadBe32(input + 4);
        entries[i].romStart = AssetSetup_ReadBe32(input + 8);
        entries[i].romEnd = AssetSetup_ReadBe32(input + 12);
    }

    return true;
}

static const DmaEntry* AssetSetup_FindDmaEntry(const DmaEntry* entries,
                                                           const OotPortExternalAsset* asset) {
    size_t i;

    for (i = 0; i < OOT_PS2_DMADATA_COUNT; i++) {
        if ((entries[i].vromStart == asset->originalVromStart) && (entries[i].vromEnd == asset->originalVromEnd)) {
            return &entries[i];
        }
    }

    for (i = 0; i < OOT_PS2_DMADATA_COUNT; i++) {
        if ((entries[i].vromStart <= asset->originalVromStart) && (entries[i].vromEnd >= asset->originalVromEnd)) {
            return &entries[i];
        }
    }

    return NULL;
}

static s32 AssetSetup_DecompressYaz0(const u8* input, size_t inputSize, u8* output, size_t outputSize) {
    const u8* inputEnd = input + inputSize;
    u8* outputStart = output;
    u8* outputEnd = output + outputSize;
    u8 code = 0;
    u32 validBits = 0;

    if ((inputSize < 16) || (memcmp(input, "Yaz0", 4) != 0) ||
        (AssetSetup_ReadBe32(input + 4) != outputSize)) {
        return false;
    }

    input += 16;

    while (output < outputEnd) {
        if (validBits == 0) {
            if (input >= inputEnd) {
                return false;
            }
            code = *input++;
            validBits = 8;
        }

        if ((code & 0x80) != 0) {
            if (input >= inputEnd) {
                return false;
            }
            *output++ = *input++;
        } else {
            size_t distance;
            size_t length;
            u8* copy;

            if ((size_t)(inputEnd - input) < 2) {
                return false;
            }

            distance = (((size_t)input[0] & 0x0F) << 8) | input[1];
            length = input[0] >> 4;
            input += 2;

            if (length == 0) {
                if (input >= inputEnd) {
                    return false;
                }
                length = (size_t)*input++ + 0x12;
            } else {
                length += 2;
            }

            if ((distance + 1 > (size_t)(output - outputStart)) || (length > (size_t)(outputEnd - output))) {
                return false;
            }

            copy = output - distance - 1;
            while (length-- != 0) {
                *output++ = *copy++;
            }
        }

        code <<= 1;
        validBits--;
    }

    return true;
}

static s32 AssetSetup_LoadAsset(Handle romFd, const DmaEntry* dma,
                                      const OotPortExternalAsset* asset, u8** output, size_t outputSize) {
    size_t entrySize = dma->vromEnd - dma->vromStart;
    size_t storedSize = dma->romEnd != 0 ? dma->romEnd - dma->romStart : entrySize;
    size_t assetOffset = asset->originalVromStart - dma->vromStart;
    u8* stored;
    u8* decoded;
    u8* result;

    if ((dma->romStart == 0xFFFFFFFFU) || (storedSize == 0) || (assetOffset + outputSize > entrySize)) {
        return false;
    }

    stored = malloc(storedSize);
    if ((stored == NULL) || !AssetSetup_ReadAt(romFd, dma->romStart, stored, storedSize)) {
        free(stored);
        return false;
    }

    if (dma->romEnd == 0) {
        if ((assetOffset == 0) && (outputSize == entrySize)) {
            *output = stored;
            return true;
        }

        result = malloc(outputSize);
        if (result == NULL) {
            free(stored);
            return false;
        }

        memcpy(result, stored + assetOffset, outputSize);
        free(stored);
        *output = result;
        return true;
    }

    decoded = malloc(entrySize);
    if ((decoded == NULL) || !AssetSetup_DecompressYaz0(stored, storedSize, decoded, entrySize)) {
        free(decoded);
        free(stored);
        return false;
    }

    free(stored);

    if ((assetOffset == 0) && (outputSize == entrySize)) {
        *output = decoded;
        return true;
    }

    result = malloc(outputSize);
    if (result == NULL) {
        free(decoded);
        return false;
    }

    memcpy(result, decoded + assetOffset, outputSize);
    free(decoded);
    *output = result;
    return true;
}

static s32 AssetSetup_TransformAsset(u8* data, size_t size, size_t assetIndex, const u8** manifestCursor,
                                           const u8* manifestEnd, const u8* permutations,
                                           size_t permutationCount) {
    const u8* cursor = *manifestCursor;
    u32 manifestIndex;
    u32 manifestSize;
    u32 payloadSize;
    u32 compressedSize;
    uLongf inflatedSize;
    u8* payload = NULL;
    const u8* selectors;
    const u8* mappings;
    const u8* mappingEnd;
    u8* mappedBytes = NULL;
    size_t mappedSize = 0;
    size_t mappedCursor = 0;
    size_t selectorCount = (size + 7) / 8;
    size_t blockIndex;
    s32 ok = false;

    if ((size_t)(manifestEnd - cursor) < 16) {
        return false;
    }

    manifestIndex = AssetSetup_ReadLe32(cursor + 0);
    manifestSize = AssetSetup_ReadLe32(cursor + 4);
    payloadSize = AssetSetup_ReadLe32(cursor + 8);
    compressedSize = AssetSetup_ReadLe32(cursor + 12);
    cursor += 16;

    if ((manifestIndex != assetIndex) || (manifestSize != size) || (payloadSize < selectorCount) ||
        ((size_t)(manifestEnd - cursor) < compressedSize)) {
        return false;
    }

    payload = malloc(payloadSize);
    if (payload == NULL) {
        return false;
    }

    inflatedSize = payloadSize;
    if ((uncompress(payload, &inflatedSize, cursor, compressedSize) != Z_OK) || (inflatedSize != payloadSize)) {
        goto cleanup;
    }

    cursor += compressedSize;
    selectors = payload;
    mappings = selectors + selectorCount;
    mappingEnd = payload + payloadSize;

    for (blockIndex = 0; blockIndex < selectorCount; blockIndex++) {
        if (selectors[blockIndex] == OOT_PS2_TRANSFORM_MAPPED_SELECTOR) {
            size_t offset = blockIndex * 8;

            mappedSize += (size - offset) < 8 ? size - offset : 8;
        }
    }

    if ((size_t)(mappingEnd - mappings) != mappedSize * 5) {
        goto cleanup;
    }

    if (mappedSize != 0) {
        size_t i;

        mappedBytes = malloc(mappedSize);
        if (mappedBytes == NULL) {
            goto cleanup;
        }

        for (i = 0; i < mappedSize; i++) {
            u32 sourceOffset = AssetSetup_ReadLe32(mappings);
            u8 delta = mappings[4];

            mappings += 5;
            if (sourceOffset >= size) {
                goto cleanup;
            }
            mappedBytes[i] = data[sourceOffset] + delta;
        }
    }

    for (blockIndex = 0; blockIndex < selectorCount; blockIndex++) {
        size_t offset = blockIndex * 8;
        size_t blockSize = (size - offset) < 8 ? size - offset : 8;
        u8 selector = selectors[blockIndex];
        u8 source[8];
        size_t i;

        memcpy(source, data + offset, blockSize);

        if (selector < permutationCount) {
            const u8* mapping = permutations + (selector * 8);

            if (blockSize != 8) {
                goto cleanup;
            }

            for (i = 0; i < 8; i++) {
                data[offset + i] = source[mapping[i]];
            }
        } else if (selector == OOT_PS2_TRANSFORM_ZERO_SELECTOR) {
            memset(data + offset, 0, blockSize);
        } else if (selector == OOT_PS2_TRANSFORM_MAPPED_SELECTOR) {
            if ((mappedSize - mappedCursor) < blockSize) {
                goto cleanup;
            }
            memcpy(data + offset, mappedBytes + mappedCursor, blockSize);
            mappedCursor += blockSize;
        } else {
            goto cleanup;
        }
    }

    if ((mappings != mappingEnd) || (mappedCursor != mappedSize)) {
        goto cleanup;
    }

    *manifestCursor = cursor;
    ok = true;

cleanup:
    free(mappedBytes);
    free(payload);
    return ok;
}

static s32 AssetSetup_Build(Handle romFd, const char* outputPath, const char* tempPath) {
    const size_t compressedSize = gOotPortAssetTransformCompressedEnd - gOotPortAssetTransformCompressed;
    const u8* compressed = gOotPortAssetTransformCompressed;
    const u8* manifestCursor;
    const u8* manifestEnd;
    const u8* permutations;
    u32 manifestEntryCount;
    u32 permutationCount;
    u32 nativeSeen = 0;
    DmaEntry* dmaEntries = NULL;
    Handle outputFd = -1;
    u8* jpegInput = NULL;
    size_t assetIndex;
    u32 jpegCount = 0;
    s32 ok = false;

    if ((compressedSize < 12) || (memcmp(compressed, "OPZ4", 4) != 0)) {
        AssetSetup_ShowError("Conversion data is missing");
        return false;
    }

    AssetSetup_ShowProgress(100, "Preparing conversion data");
    manifestEntryCount = AssetSetup_ReadLe32(compressed + 4);
    permutationCount = AssetSetup_ReadLe32(compressed + 8);

    if ((permutationCount != OOT_PS2_TRANSFORM_ZERO_SELECTOR) ||
        (compressedSize < 12 + (permutationCount * 8))) {
        AssetSetup_ShowError("Conversion data is incompatible");
        return false;
    }

    permutations = compressed + 12;
    manifestCursor = permutations + (permutationCount * 8);
    manifestEnd = compressed + compressedSize;

    dmaEntries = malloc(sizeof(*dmaEntries) * OOT_PS2_DMADATA_COUNT);
    if (dmaEntries == NULL) {
        AssetSetup_ShowError("Not enough memory for asset setup");
        goto cleanup;
    }

    if (!AssetSetup_LoadDmaTable(romFd, dmaEntries)) {
        AssetSetup_ShowError("Could not read the ROM file table");
        goto cleanup;
    }

    jpegInput = malloc(OOT_PS2_PRERENDER_RGBA16_SIZE);
    if (jpegInput == NULL) {
        AssetSetup_ShowError("Not enough memory for JPEG conversion");
        goto cleanup;
    }

    outputFd = File_Open(tempPath, OOT_PS2_FILE_WRONLY | OOT_PS2_FILE_CREAT | OOT_PS2_FILE_TRUNC, 0777);
    if (outputFd < 0) {
        AssetSetup_ShowError("Could not create the asset file");
        goto cleanup;
    }

    for (assetIndex = 0; assetIndex < gOotPortExternalAssetCount; assetIndex++) {
        const OotPortExternalAsset* asset = &gOotPortExternalAssets[assetIndex];
        const DmaEntry* dma = AssetSetup_FindDmaEntry(dmaEntries, asset);
        size_t assetSize = asset->vromEnd - asset->vromStart;
        u8* data = NULL;
        char status[64];

        if ((assetIndex & 7) == 0) {
            snprintf(status, sizeof(status), "Converting assets: %lu / %lu", (unsigned long)assetIndex,
                     (unsigned long)gOotPortExternalAssetCount);
            AssetSetup_ShowProgress(100 + (u32)((assetIndex * 890) / gOotPortExternalAssetCount), status);
        }

        if (dma == NULL) {
            if ((strcmp(asset->name, "bump_texture_static") == 0) && (assetSize == 0x400)) {
                data = calloc(1, assetSize);
                if (data == NULL) {
                    AssetSetup_ShowError("Not enough memory for asset setup");
                    goto cleanup;
                }
            } else {
                snprintf(status, sizeof(status), "Could not extract %.36s", asset->name);
                AssetSetup_ShowError(status);
                goto cleanup;
            }
        }

        if ((data == NULL) && (asset->flags == 0) && (dma->romEnd == 0)) {
            u32 assetRomOffset = dma->romStart + (u32)(asset->originalVromStart - dma->vromStart);

            if (!AssetSetup_CopyUncompressedAsset(romFd, outputFd, assetRomOffset, asset->fileOffset,
                                                         assetSize)) {
                AssetSetup_ShowError("Could not copy data from the ROM");
                goto cleanup;
            }
        } else {
            if ((data == NULL) && !AssetSetup_LoadAsset(romFd, dma, asset, &data, assetSize)) {
                snprintf(status, sizeof(status), "Could not extract %.36s", asset->name);
                AssetSetup_ShowError(status);
                goto cleanup;
            }

            if ((asset->flags != 0) &&
                !AssetSetup_TransformAsset(data, assetSize, assetIndex, &manifestCursor, manifestEnd,
                                                 permutations, permutationCount)) {
                snprintf(status, sizeof(status), "Could not convert %.36s", asset->name);
                AssetSetup_ShowError(status);
                free(data);
                goto cleanup;
            }

            jpegCount += AssetSetup_PredecodePrerenderJpegs(data, assetSize, asset, jpegInput,
                                                                  OOT_PS2_PRERENDER_RGBA16_SIZE);

            if (!AssetSetup_WriteAt(outputFd, asset->fileOffset, data, assetSize)) {
                AssetSetup_ShowError("Could not write the asset file");
                free(data);
                goto cleanup;
            }

            free(data);
        }

        nativeSeen += asset->flags != 0;

        if ((assetIndex + 1) == gOotPortExternalAssetCount) {
            snprintf(status, sizeof(status), "Converting assets: %lu / %lu", (unsigned long)(assetIndex + 1),
                     (unsigned long)gOotPortExternalAssetCount);
            AssetSetup_ShowProgress(990, status);
        }
    }

    if ((nativeSeen != manifestEntryCount) || (manifestCursor != manifestEnd)) {
        AssetSetup_ShowError("Conversion data did not match the ROM");
        goto cleanup;
    }
    if (jpegCount == 0) {
        AssetSetup_ShowError("Could not predecode room JPEGs");
        goto cleanup;
    }

    File_Close(outputFd);
    outputFd = -1;

    AssetSetup_ShowProgress(995, "Finishing asset file");
    if (!AssetSetup_CopyFile(tempPath, outputPath)) {
        AssetSetup_ShowError("Could not finish the asset file");
        goto cleanup;
    }

    outputFd = File_Open(tempPath, OOT_PS2_FILE_WRONLY | OOT_PS2_FILE_TRUNC, 0);
    if (outputFd >= 0) {
        File_Close(outputFd);
        outputFd = -1;
    }

    ok = true;

cleanup:
    if (outputFd >= 0) {
        File_Close(outputFd);
    }
    free(jpegInput);
    free(dmaEntries);
    return ok;
}

s32 AssetSetup_Ensure(void) {
    char outputBuffer[384];
    char tempBuffer[384];
    char romBuffer[384];
    const char* outputPath = OotPort_ResolveRootPath(OOT_PS2_PACKED_ASSET_PATH, outputBuffer, sizeof(outputBuffer));
    const char* tempPath = OotPort_ResolveRootPath(OOT_PS2_PACKED_ASSET_TEMP_PATH, tempBuffer, sizeof(tempBuffer));
    size_t expectedSize;
    Handle romFd;
    s32 ok;

    if (gOotPortExternalAssetCount == 0) {
        return false;
    }

    expectedSize = gOotPortExternalAssets[gOotPortExternalAssetCount - 1].fileOffset +
                   (gOotPortExternalAssets[gOotPortExternalAssetCount - 1].vromEnd -
                    gOotPortExternalAssets[gOotPortExternalAssetCount - 1].vromStart);

    if (AssetSetup_FileHasSize(outputPath, expectedSize) &&
        !AssetSetup_PackHasCompressedPrerenderJpeg(outputPath)) {
        return true;
    }

    sOotPs2AssetSetupProgress = 0;
    sOotPs2AssetSetupErrorShown = false;
    AssetSetup_ShowProgress(0, "Checking NTSC 1.0 ROM");

    romFd = AssetSetup_OpenRom(romBuffer, sizeof(romBuffer));
    if (romFd < 0) {
        AssetSetup_ShowError("baserom.z64 not found");
        return false;
    }

    ok = AssetSetup_ValidateRom(romFd) && AssetSetup_Build(romFd, outputPath, tempPath);
    File_Close(romFd);

    if (ok) {
        AssetSetup_ShowProgress(1000, "Asset setup complete");
    } else if (!sOotPs2AssetSetupErrorShown) {
        AssetSetup_ShowError("Asset setup failed");
    }

    return ok;
}
