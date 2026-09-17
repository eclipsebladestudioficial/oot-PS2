#ifndef OOT_PS2_VIDEO_H
#define OOT_PS2_VIDEO_H

#include <stddef.h>

#define OOT_PS2_VIDEO_INI_PATH "video.ini"
#define OOT_PS2_DVE_CABLE_COMPOSITE 0

typedef enum OotPs2VideoOutput {
    OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE = 0,
    OOT_PS2_VIDEO_OUTPUT_INTERLACED = 1,
} OotPs2VideoOutput;

typedef enum OotPs2VideoResolution {
    OOT_PS2_VIDEO_RESOLUTION_480I = 0,
    OOT_PS2_VIDEO_RESOLUTION_480P = 1,
    OOT_PS2_VIDEO_RESOLUTION_720P = 2,
    OOT_PS2_VIDEO_RESOLUTION_1080I = 3,
} OotPs2VideoResolution;

typedef enum OotPs2VideoAspect {
    OOT_PS2_VIDEO_ASPECT_4_3 = 0,
    OOT_PS2_VIDEO_ASPECT_16_9 = 1,
} OotPs2VideoAspect;

typedef struct OotPs2VideoMode {
    int field;
    int mode;
    int interlaced;
    int widescreen;
    int displayWidth;
    int displayHeight;
    int viewportWidth;
    int viewportHeight;
} OotPs2VideoMode;

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
void OotPs2Video_GetResolution(OotPs2VideoResolution* videoResolution);

#endif
