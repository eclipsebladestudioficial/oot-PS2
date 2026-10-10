#include "alignment.h"
#include "buffers.h"
#include "ultra64/ultratypes.h"

#if defined(TARGET_PS2)
ALIGNED(64) u16 gZBuffer[1][32];
#else
ALIGNED(64) u16 gZBuffer[SCREEN_HEIGHT][SCREEN_WIDTH];
#endif
