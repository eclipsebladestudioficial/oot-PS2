#ifndef OOT_PS2_VIDEO_H
#define OOT_PS2_VIDEO_H

#include <stddef.h>

#define OOT_PS2_VIDEO_INI_PATH "video.ini"
#define OOT_PS2_DVE_CABLE_COMPOSITE 0

typedef enum VideoOutput {
    OOT_PS2_VIDEO_OUTPUT_PROGRESSIVE = 0,
    OOT_PS2_VIDEO_OUTPUT_INTERLACED = 1,
} VideoOutput;

typedef enum VideoResolution {
    OOT_PS2_VIDEO_RESOLUTION_480I = 0,
    OOT_PS2_VIDEO_RESOLUTION_480P = 1,
    OOT_PS2_VIDEO_RESOLUTION_720P = 2,
    OOT_PS2_VIDEO_RESOLUTION_1080I = 3,
} VideoResolution;

typedef enum VideoAspect {
    OOT_PS2_VIDEO_ASPECT_4_3 = 0,
    OOT_PS2_VIDEO_ASPECT_16_9 = 1,
} VideoAspect;

typedef struct VideoMode {
    int field;
    int mode;
    int interlaced;
    int widescreen;
    int displayWidth;
    int displayHeight;
    int viewportWidth;
    int viewportHeight;
} VideoMode;

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
void Video_GetResolution(VideoResolution* videoResolution);

#endif
