#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gsKit.h> 
#include "platform.h"
#include "renderer/video.h"

#define OOT_PS2_VIDEO_INI_BUFFER_SIZE 512
#define OOT_PS2_VIDEO_PATH_CAPACITY 384
#define OOT_PS2_VIDEO_SAVE_DIR "mc0:/OOTPS2"
#define OOT_PS2_VIDEO_SAVE_PATH "mc0:/OOTPS2/video.ini"

static VideoOutput sOotPs2VideoOutput;
static VideoResolution sOotPs2VideoResolution;
static VideoAspect sOotPs2VideoAspect;
static VideoOutput sOotPs2VideoActiveOutput;
static VideoResolution sOotPs2VideoActiveResolution;
static VideoAspect sOotPs2VideoActiveAspect;
static char sOotPs2VideoPath[OOT_PS2_VIDEO_PATH_CAPACITY];
static char sOotPs2VideoFallbackPath[OOT_PS2_VIDEO_PATH_CAPACITY];

int Dve_GetModel() { return -1; };
int Dve_IsAvailable() { return -1; };
int Dve_GetCable() { return -1; } ;

void Video_Init(const char* executablePath);
void Video_ResetDefaults(void);
void Video_ForceLcd(void);
void Video_CommitMode(void);
int Video_Save(void);
void Video_CycleOutput(int direction);
void Video_CycleResolution(int direction);
void Video_CycleAspect(int direction);
const char* Video_GetOutputName(void);
const char* Video_GetResolutionName(void);
const char* Video_GetAspectName(void);
void Video_GetMode(VideoMode* mode);
void Video_GetActiveMode(VideoMode* mode);

static int Video_StrIcmp(const char* a, const char* b) {
    unsigned char ca;
    unsigned char cb;

    while ((*a != '\0') && (*b != '\0')) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if ((ca >= 'A') && (ca <= 'Z')) {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if ((cb >= 'A') && (cb <= 'Z')) {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static char* Video_Trim(char* text) {
    char* end;

    while ((*text == ' ') || (*text == '\t') || (*text == '\r') || (*text == '\n')) {
        text++;
    }
    end = text + strlen(text);
    while ((end > text) &&
           ((end[-1] == ' ') || (end[-1] == '\t') || (end[-1] == '\r') || (end[-1] == '\n'))) {
        end--;
    }
    *end = '\0';
    return text;
}

static void Video_BuildPath(const char* executablePath) {
    const char* slash;
    const char* backslash;
    size_t rootLength;
    size_t suffixLength = sizeof(OOT_PS2_VIDEO_INI_PATH);

    memcpy(sOotPs2VideoPath, OOT_PS2_VIDEO_SAVE_PATH, sizeof(OOT_PS2_VIDEO_SAVE_PATH));
    sOotPs2VideoFallbackPath[0] = '\0';
    if ((executablePath == NULL) || (executablePath[0] == '\0')) {
        return;
    }

    slash = strrchr(executablePath, '/');
    backslash = strrchr(executablePath, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }

    rootLength = (slash != NULL) ? (size_t)(slash - executablePath) + 1 : 0;
    if ((rootLength + suffixLength) > sizeof(sOotPs2VideoFallbackPath)) {
        return;
    }

    if (rootLength != 0) {
        memcpy(sOotPs2VideoFallbackPath, executablePath, rootLength);
    }
    memcpy(sOotPs2VideoFallbackPath + rootLength, OOT_PS2_VIDEO_INI_PATH, suffixLength);
}

void Video_ResetDefaults(void) {
    sOotPs2VideoOutput = OOT_PS2_VIDEO_OUTPUT_INTERLACED;
    sOotPs2VideoResolution = OOT_PS2_VIDEO_RESOLUTION_480I; // 480P
    sOotPs2VideoAspect = OOT_PS2_VIDEO_ASPECT_4_3; // 16_9
}
void Video_ForceLcd(void) {
    sOotPs2VideoOutput = OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE;
}
void Video_CommitMode(void) {
    sOotPs2VideoActiveOutput = sOotPs2VideoOutput;
    sOotPs2VideoActiveResolution = sOotPs2VideoResolution;
    sOotPs2VideoActiveAspect = sOotPs2VideoAspect;
}

void Video_Init(const char* executablePath) {
    char buffer[OOT_PS2_VIDEO_INI_BUFFER_SIZE];
    Handle fd;
    int readSize;
    char* line;
    char* nextLine;

    Video_ResetDefaults();
    Video_CommitMode();
    Video_BuildPath(executablePath);
    if (sOotPs2VideoPath[0] == '\0') {
        return;
    }

    fd = File_Open(sOotPs2VideoPath, OOT_PS2_FILE_RDONLY, 0);
    if (fd < 0) {
        if (sOotPs2VideoFallbackPath[0] == '\0') {
            return;
        }
        fd = File_Open(sOotPs2VideoFallbackPath, OOT_PS2_FILE_RDONLY, 0);
        if (fd < 0) {
            return;
        }
    }
    readSize = File_Read(fd, buffer, sizeof(buffer) - 1);
    File_Close(fd);
    if (readSize < 0) {
        return;
    }
    buffer[readSize] = '\0';

    for (line = buffer; line != NULL; line = nextLine) {
        char* comment;
        char* equals;
        char* key;
        char* value;

        nextLine = strchr(line, '\n');
        if (nextLine != NULL) {
            *nextLine = '\0';
            nextLine++;
        }
        comment = strpbrk(line, "#;");
        if (comment != NULL) {
            *comment = '\0';
        }
        line = Video_Trim(line);
        if ((line[0] == '\0') || (line[0] == '[')) {
            continue;
        }
        equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        key = Video_Trim(line);
        value = Video_Trim(equals + 1);

        if (Video_StrIcmp(key, "output") == 0) {
            sOotPs2VideoOutput = (Video_StrIcmp(value, "progressive") == 0) ? OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE
                                                                          : OOT_PS2_VIDEO_OUTPUT_INTERLACED;
        } else if (Video_StrIcmp(key, "resolution") == 0) {
            if (Video_StrIcmp(value, "480i") == 0) {
                sOotPs2VideoResolution = OOT_PS2_VIDEO_RESOLUTION_480I;
            } else if (Video_StrIcmp(value, "480p") == 0) {
                sOotPs2VideoResolution = OOT_PS2_VIDEO_RESOLUTION_480P;
            }
        } else if (Video_StrIcmp(key, "aspect") == 0) {
            sOotPs2VideoAspect = (Video_StrIcmp(value, "4:3") == 0) ? OOT_PS2_VIDEO_ASPECT_4_3
                                                                          : OOT_PS2_VIDEO_ASPECT_16_9;
        }
    }

    // if (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_720P) {
    //     sOotPs2VideoAspect = OOT_PS2_VIDEO_ASPECT_4_3;
    // }
    Video_CommitMode();
}

int Video_Save(void) {
    char buffer[192];
    const char* resolutionName;
    int length;
    int written;
    Handle fd;

    if (sOotPs2VideoPath[0] == '\0') {
        return -1;
    }

    resolutionName = (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480I)
                         ? "480i"
                         : ((sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P) ? "480P" : "null");
    length = snprintf(buffer, sizeof(buffer), "[video]\noutput = %s\nresolution = %s\naspect = %s\n",
                      (sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE) ? "progressive" : "interlaced", resolutionName,
                      (sOotPs2VideoAspect == OOT_PS2_VIDEO_ASPECT_4_3) ? "4:3" : "16:9");
    if ((length < 0) || ((size_t)length >= sizeof(buffer))) {
        return -1;
    }

    File_Mkdir(OOT_PS2_VIDEO_SAVE_DIR, 0777);
    fd = File_Open(sOotPs2VideoPath, OOT_PS2_FILE_WRONLY | OOT_PS2_FILE_CREAT | OOT_PS2_FILE_TRUNC, 0777);
    if (fd >= 0) {
        written = File_Write(fd, buffer, length);
        File_Close(fd);
        if (written == length) {
            return 0;
        }
    }

    File_Remove(OOT_PS2_VIDEO_SAVE_PATH);
    if (sOotPs2VideoFallbackPath[0] == '\0') {
        return -1;
    }
    fd = File_Open(sOotPs2VideoFallbackPath, OOT_PS2_FILE_WRONLY | OOT_PS2_FILE_CREAT | OOT_PS2_FILE_TRUNC, 0777);
    if (fd < 0) {
        return fd;
    }
    written = File_Write(fd, buffer, length);
    File_Close(fd);
    return (written == length) ? 0 : -1;
}
void Video_CycleOutput(int direction) {
    (void)direction;

    sOotPs2VideoOutput = (sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_INTERLACED) ? OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE
                                                                              : OOT_PS2_VIDEO_OUTPUT_INTERLACED;
    if ((sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE) &&
        (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P)) {
        sOotPs2VideoResolution = OOT_PS2_VIDEO_RESOLUTION_480I;
    }
    if ((sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE) &&
        (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P)) {
        sOotPs2VideoAspect = OOT_PS2_VIDEO_ASPECT_4_3;
    }
}

void Video_CycleResolution(int direction) {
    int modeCount;
    int resolution;
    (void)direction;

    modeCount = 2;
    resolution = (int)sOotPs2VideoResolution;
    if ((resolution < 0) || (resolution >= modeCount)) {
        resolution = 0;
    }
    resolution = (resolution + ((direction < 0) ? modeCount - 1 : 1)) % modeCount;
    sOotPs2VideoResolution = (VideoResolution)resolution;
}
void Video_CycleAspect(int direction) {
    (void)direction;
    sOotPs2VideoAspect = (sOotPs2VideoAspect == OOT_PS2_VIDEO_ASPECT_4_3) ? OOT_PS2_VIDEO_ASPECT_16_9
                                                                          : OOT_PS2_VIDEO_ASPECT_4_3;
}
const char* Video_GetOutputName(void) {
    return (sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE) ? "Progressive" : "Interlaced";
}
const char* Video_GetResolutionName(void) {
    if (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480I) {
        return "480i";
    }
    if (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P) {
        return "480p";
    }
    return "480p";
}
const char* Video_GetAspectName(void) {
    if ((sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE) &&
        (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P)) {
        return "4:3 (fixed)";
    }
    return (sOotPs2VideoAspect == OOT_PS2_VIDEO_ASPECT_4_3) ? "4:3" : "16:9";
}
static void Video_ResolveMode(VideoOutput output, VideoResolution resolution,
                                    VideoAspect aspect, VideoMode* mode) {

    memset(mode, 0, sizeof(*mode));

    mode->displayWidth = 640;
    mode->displayHeight = 448;
    mode->viewportWidth = 640;
    mode->viewportHeight = 448;
    mode->widescreen = (sOotPs2VideoAspect);
    
    if(sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480I) {
        mode->field = GS_FIELD;
        mode->mode = GS_MODE_NTSC;
        mode->interlaced = GS_INTERLACED;
    } else if(sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P) {
        mode->field = GS_FRAME;
        mode->interlaced = GS_NONINTERLACED;
        mode->mode = GS_MODE_DTV_480P;
    }

}
void Video_GetMode(VideoMode* mode) {
    Video_ResolveMode(sOotPs2VideoOutput, sOotPs2VideoResolution, sOotPs2VideoAspect, mode);
}
void Video_GetActiveMode(VideoMode* mode) {
    Video_ResolveMode(sOotPs2VideoActiveOutput, sOotPs2VideoActiveResolution, sOotPs2VideoActiveAspect, mode);
}
