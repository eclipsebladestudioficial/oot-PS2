#ifndef OOT_PORT_AUDIO_BACKEND_H
#define OOT_PORT_AUDIO_BACKEND_H

#include "ultra64.h"

s32 OotPortAudioBackend_Init(void);
s32 OotPortAudioBackend_Queue(const void* buf, u32 size);
s32 OotPortAudioBackend_SetFrequency(u32 frequency);
u32 OotPortAudioBackend_GetLength(void);
s32 OotPortAudioBackend_NeedsRefillUrgently(void);
s32 OotPortAudioBackend_NeedsRefillDuringIo(void);
void OotPortAudioBackend_SubmitCommands(const Acmd* cmdList, s32 cmdCount);
void OotPortAudioBackend_SubmitCommandsAndQueue(const Acmd* cmdList, s32 cmdCount, const void* buf, u32 size);
void OotPortAudioBackend_SubmitSynthesis(Acmd* cmdList, s16* aiBuffer, s32 aiFrames);
void OotPortAudioBackend_ExecuteCommands(const Acmd* cmdList, s32 cmdCount);

void OotPortAudio_Init(void);
void OotPortAudio_Update(void);

#endif
