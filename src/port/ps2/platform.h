#ifndef OOT_PS2_PLATFORM_H
#define OOT_PS2_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "ultra64/ultratypes.h"

typedef s32 Handle;
typedef s64 Offset;
typedef u32 ThreadArgSize;
typedef s32 (*ThreadEntry)(ThreadArgSize args, void* argp);

typedef struct ThreadInfo {
    void* stack;
    s32 stackSize;
    s32 currentPriority;
} ThreadInfo;

typedef struct PadState {
    u32 buttons;
    u8 lx;
    u8 ly;
} PadState;

enum {
    OOT_PS2_BUTTON_SELECT   = 1u << 0,
    OOT_PS2_BUTTON_START    = 1u << 1,
    OOT_PS2_BUTTON_UP       = 1u << 2,
    OOT_PS2_BUTTON_RIGHT    = 1u << 3,
    OOT_PS2_BUTTON_DOWN     = 1u << 4,
    OOT_PS2_BUTTON_LEFT     = 1u << 5,
    OOT_PS2_BUTTON_L1       = 1u << 6,
    OOT_PS2_BUTTON_R1       = 1u << 7,
    OOT_PS2_BUTTON_TRIANGLE = 1u << 8,
    OOT_PS2_BUTTON_CIRCLE   = 1u << 9,
    OOT_PS2_BUTTON_CROSS    = 1u << 10,
    OOT_PS2_BUTTON_SQUARE   = 1u << 11,
    OOT_PS2_BUTTON_R2       = 1u << 12,
    OOT_PS2_BUTTON_L3       = 1u << 13,
    OOT_PS2_BUTTON_R3       = 1u << 14,
};

enum {
    OOT_PS2_FILE_RDONLY = 0,
    OOT_PS2_FILE_WRONLY = 1,
    OOT_PS2_FILE_RDWR = 2,
    OOT_PS2_FILE_CREAT = 0x0200,
    OOT_PS2_FILE_TRUNC = 0x0400,
};

enum {
    OOT_PS2_SEEK_SET = 0,
    OOT_PS2_SEEK_CUR = 1,
    OOT_PS2_SEEK_END = 2,
};

Handle Thread_Create(const char* name, ThreadEntry entry, s32 priority, s32 stackSize);
s32 Thread_Start(Handle id, ThreadArgSize args, void* argp);
s32 Thread_Delete(Handle id);
s32 Thread_TerminateDelete(Handle id);
void Thread_ExitDelete(void);
s32 Thread_ChangePriority(Handle id, s32 priority);
Handle Thread_GetId(void);
s32 Thread_GetInfo(Handle id, ThreadInfo* out);
void Thread_Delay(u32 usec);
void Thread_ExitGame(void);

Handle Sema_Create(s32 initialCount, s32 maxCount);
s32 Sema_Wait(Handle id);
s32 Sema_Signal(Handle id);
s32 Sema_Delete(Handle id);

u64 Time_GetUsec(void);
u32 Time_GetUsecLow(void);

Handle File_Open(const char* path, s32 flags, s32 mode);
s32 File_Close(Handle fd);
s32 File_Read(Handle fd, void* buffer, u32 size);
s32 File_Write(Handle fd, const void* buffer, u32 size);
Offset File_Seek(Handle fd, Offset offset, s32 whence);
s32 File_Mkdir(const char* path, s32 mode);
s32 File_Remove(const char* path);
s32 File_Rename(const char* from, const char* to);

void Platform_SetDiscBoot(s32 discBoot);
s32 Platform_IsDiscBoot(void);

void Cache_WritebackInvalidateAll(void);
void Cache_WritebackRange(const void* address, size_t size);
void Cache_InvalidateRange(void* address, size_t size);
void Cache_InvalidateInstruction(void);

void Pad_Init(void);
s32 Pad_Read(PadState* out);
void _GetRightStick(int* x, int* y);
void Input_SetPlayState(void* playState);

#endif
