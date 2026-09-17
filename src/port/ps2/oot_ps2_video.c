#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gsKit.h> 
#include "oot_ps2_platform.h"
#include "oot_ps2_video.h"

#define OOT_PS2_VIDEO_INI_BUFFER_SIZE 512
#define OOT_PS2_VIDEO_PATH_CAPACITY 384

static OotPs2VideoOutput sOotPs2VideoOutput;
static OotPs2VideoResolution sOotPs2VideoResolution;
static OotPs2VideoAspect sOotPs2VideoAspect;
static OotPs2VideoOutput sOotPs2VideoActiveOutput;
static OotPs2VideoResolution sOotPs2VideoActiveResolution;
static OotPs2VideoAspect sOotPs2VideoActiveAspect;
static char sOotPs2VideoPath[OOT_PS2_VIDEO_PATH_CAPACITY];

int OotPs2Dve_GetModel() { return -1; };
int OotPs2Dve_IsAvailable() { return -1; };
int OotPs2Dve_GetCable() { return -1; } ;

void OotPs2Video_Init(const char* executablePath);
void OotPs2Video_ResetDefaults(void);
void OotPs2Video_ForceLcd(void);
void OotPs2Video_CommitMode(void);
int OotPs2Video_Save(void);
void OotPs2Video_CycleOutput(int direction);
void OotPs2Video_CycleResolution(int direction);
void OotPs2Video_CycleAspect(int direction);
const char* OotPs2Video_GetOutputName(void);
const char* OotPs2Video_GetResolutionName(void);
const char* OotPs2Video_GetAspectName(void);
void OotPs2Video_GetMode(OotPs2VideoMode* mode);
void OotPs2Video_GetActiveMode(OotPs2VideoMode* mode);

static int OotPs2Video_StrIcmp(const char* a, const char* b) {
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

static char* OotPs2Video_Trim(char* text) {
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

static void OotPs2Video_BuildPath(const char* executablePath) {
    const char* slash;
    const char* backslash;
    size_t rootLength;
    size_t suffixLength = sizeof(OOT_PS2_VIDEO_INI_PATH);

    sOotPs2VideoPath[0] = '\0';
    if ((executablePath == NULL) || (executablePath[0] == '\0')) {
        return;
    }

    slash = strrchr(executablePath, '/');
    backslash = strrchr(executablePath, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }

    rootLength = (slash != NULL) ? (size_t)(slash - executablePath) + 1 : 0;
    if ((rootLength + suffixLength) > sizeof(sOotPs2VideoPath)) {
        return;
    }

    if (rootLength != 0) {
        memcpy(sOotPs2VideoPath, executablePath, rootLength);
    }
    memcpy(sOotPs2VideoPath + rootLength, OOT_PS2_VIDEO_INI_PATH, suffixLength);
}

void OotPs2Video_ResetDefaults(void) {
    sOotPs2VideoOutput = OOT_PS2_VIDEO_OUTPUT_INTERLACED;
    sOotPs2VideoResolution = OOT_PS2_VIDEO_RESOLUTION_480I; // 480P
    sOotPs2VideoAspect = OOT_PS2_VIDEO_ASPECT_4_3; // 16_9
}
void OotPs2Video_ForceLcd(void) {
    sOotPs2VideoOutput = OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE;
}
void OotPs2Video_CommitMode(void) {
    sOotPs2VideoActiveOutput = sOotPs2VideoOutput;
    sOotPs2VideoActiveResolution = sOotPs2VideoResolution;
    sOotPs2VideoActiveAspect = sOotPs2VideoAspect;
}

void OotPs2Video_Init(const char* executablePath) {
    char buffer[OOT_PS2_VIDEO_INI_BUFFER_SIZE];
    OotPs2Handle fd;
    int readSize;
    char* line;
    char* nextLine;

    OotPs2Video_ResetDefaults();
    OotPs2Video_CommitMode();
    OotPs2Video_BuildPath(executablePath);
    if (sOotPs2VideoPath[0] == '\0') {
        return;
    }

    fd = OotPs2File_Open(sOotPs2VideoPath, OOT_PS2_FILE_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    readSize = OotPs2File_Read(fd, buffer, sizeof(buffer) - 1);
    OotPs2File_Close(fd);
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
        line = OotPs2Video_Trim(line);
        if ((line[0] == '\0') || (line[0] == '[')) {
            continue;
        }
        equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        key = OotPs2Video_Trim(line);
        value = OotPs2Video_Trim(equals + 1);

        if (OotPs2Video_StrIcmp(key, "output") == 0) {
            sOotPs2VideoOutput = (OotPs2Video_StrIcmp(value, "progressive") == 0) ? OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE
                                                                          : OOT_PS2_VIDEO_OUTPUT_INTERLACED;
        } else if (OotPs2Video_StrIcmp(key, "resolution") == 0) {
            if (OotPs2Video_StrIcmp(value, "480i") == 0) {
                sOotPs2VideoResolution = OOT_PS2_VIDEO_RESOLUTION_480I;
            } else if (OotPs2Video_StrIcmp(value, "480p") == 0) {
                sOotPs2VideoResolution = OOT_PS2_VIDEO_RESOLUTION_480P;
            }
        } else if (OotPs2Video_StrIcmp(key, "aspect") == 0) {
            sOotPs2VideoAspect = (OotPs2Video_StrIcmp(value, "4:3") == 0) ? OOT_PS2_VIDEO_ASPECT_4_3
                                                                          : OOT_PS2_VIDEO_ASPECT_16_9;
        }
    }

    // if (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_720P) {
    //     sOotPs2VideoAspect = OOT_PS2_VIDEO_ASPECT_4_3;
    // }
    OotPs2Video_CommitMode();
}

int OotPs2Video_Save(void) {
    char buffer[192];
    const char* resolutionName;
    int length;
    int written;
    OotPs2Handle fd;

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

    fd = OotPs2File_Open(sOotPs2VideoPath, OOT_PS2_FILE_WRONLY | OOT_PS2_FILE_CREAT | OOT_PS2_FILE_TRUNC, 0777);
    if (fd < 0) {
        return fd;
    }
    written = OotPs2File_Write(fd, buffer, length);
    OotPs2File_Close(fd);
    return (written == length) ? 0 : -1;
}
void OotPs2Video_CycleOutput(int direction) {
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

void OotPs2Video_CycleResolution(int direction) {
    int modeCount;
    int resolution;
    (void)direction;

    modeCount = 2;
    resolution = (int)sOotPs2VideoResolution;
    if ((resolution < 0) || (resolution >= modeCount)) {
        resolution = 0;
    }
    resolution = (resolution + ((direction < 0) ? modeCount - 1 : 1)) % modeCount;
    sOotPs2VideoResolution = (OotPs2VideoResolution)resolution;
}
void OotPs2Video_CycleAspect(int direction) {
    (void)direction;
    sOotPs2VideoAspect = (sOotPs2VideoAspect == OOT_PS2_VIDEO_ASPECT_4_3) ? OOT_PS2_VIDEO_ASPECT_16_9
                                                                          : OOT_PS2_VIDEO_ASPECT_4_3;
}
const char* OotPs2Video_GetOutputName(void) {
    return (sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE) ? "Progressive" : "Interlaced";
}
const char* OotPs2Video_GetResolutionName(void) {
    if (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480I) {
        return "480i";
    }
    if (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P) {
        return "480p";
    }
    return "480p";
}
const char* OotPs2Video_GetAspectName(void) {
    if ((sOotPs2VideoOutput == OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE) &&
        (sOotPs2VideoResolution == OOT_PS2_VIDEO_RESOLUTION_480P)) {
        return "4:3 (fixed)";
    }
    return (sOotPs2VideoAspect == OOT_PS2_VIDEO_ASPECT_4_3) ? "4:3" : "16:9";
}
static void OotPs2Video_ResolveMode(OotPs2VideoOutput output, OotPs2VideoResolution resolution,
                                    OotPs2VideoAspect aspect, OotPs2VideoMode* mode) {

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
void OotPs2Video_GetMode(OotPs2VideoMode* mode) {
    OotPs2Video_ResolveMode(sOotPs2VideoOutput, sOotPs2VideoResolution, sOotPs2VideoAspect, mode);
}
void OotPs2Video_GetActiveMode(OotPs2VideoMode* mode) {
    OotPs2Video_ResolveMode(sOotPs2VideoActiveOutput, sOotPs2VideoActiveResolution, sOotPs2VideoActiveAspect, mode);
}