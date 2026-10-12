#include "controls.h"
#include "assets/loader.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controller.h"

#define OOT_PS2_CONTROLS_INI_BUFFER_SIZE 4096

typedef struct ButtonBinding {
    const char* iniName;
    const char* displayName;
    u32 buttonMask;
    u16 defaultN64Mask;
    u16 n64Mask;
} ButtonBinding;

typedef struct N64Action {
    const char* iniName;
    const char* displayName;
    u16 n64Mask;
} N64Action;

static ButtonBinding sButtonBindings[] = {
    { "cross", "Cross", OOT_PS2_BUTTON_CROSS, BTN_A, BTN_A },
    { "circle", "Circle", OOT_PS2_BUTTON_CIRCLE, BTN_B, BTN_B },
    { "triangle", "Triangle", OOT_PS2_BUTTON_TRIANGLE, BTN_CUP, BTN_CUP },
    { "square", "Square", OOT_PS2_BUTTON_SQUARE, BTN_CLEFT, BTN_CLEFT },
    { "ltrigger", "L Trigger", OOT_PS2_BUTTON_L1, BTN_Z, BTN_Z },
    { "rtrigger", "R Trigger", OOT_PS2_BUTTON_R1, BTN_R, BTN_R },
    { "start", "Start", OOT_PS2_BUTTON_START, BTN_START, BTN_START },
    { "select", "Select", OOT_PS2_BUTTON_SELECT, BTN_L, BTN_L },
    { "up", "D-Pad Up", OOT_PS2_BUTTON_UP, BTN_CUP, BTN_CUP },
    { "down", "D-Pad Down", OOT_PS2_BUTTON_DOWN, BTN_CDOWN, BTN_CDOWN },
    { "left", "D-Pad Left", OOT_PS2_BUTTON_LEFT, BTN_CLEFT, BTN_CLEFT },
    { "right", "D-Pad Right", OOT_PS2_BUTTON_RIGHT, BTN_CRIGHT, BTN_CRIGHT },
};

static const N64Action sN64Actions[] = {
    { "none", "None", 0 },
    { "A", "A", BTN_A },
    { "B", "B", BTN_B },
    { "Z", "Z", BTN_Z },
    { "Start", "Start", BTN_START },
    { "D-Up", "D-Up", BTN_DUP },
    { "D-Down", "D-Down", BTN_DDOWN },
    { "D-Left", "D-Left", BTN_DLEFT },
    { "D-Right", "D-Right", BTN_DRIGHT },
    { "L", "L", BTN_L },
    { "R", "R", BTN_R },
    { "C-Up", "C-Up", BTN_CUP },
    { "C-Down", "C-Down", BTN_CDOWN },
    { "C-Left", "C-Left", BTN_CLEFT },
    { "C-Right", "C-Right", BTN_CRIGHT },
};

static int sDeadzone;
static int sInitialized;

static int OotPortControls_StrIcmp(const char* a, const char* b) {
    unsigned char ca;
    unsigned char cb;

    while (*a != '\0' && *b != '\0') {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;

        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
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

static char* OotPortControls_Trim(char* text) {
    char* end;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }

    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';

    return text;
}

static int OotPortControls_ClampDeadzone(int deadzone) {
    if (deadzone < OOT_PORT_CONTROLS_DEADZONE_MIN) {
        return OOT_PORT_CONTROLS_DEADZONE_MIN;
    }
    if (deadzone > OOT_PORT_CONTROLS_DEADZONE_MAX) {
        return OOT_PORT_CONTROLS_DEADZONE_MAX;
    }
    return deadzone;
}

static const char* OotPortControls_Path(char* buffer, size_t bufferSize) {
    return OotPort_ResolveRootPath(OOT_PORT_CONTROLS_INI_PATH, buffer, bufferSize);
}

static int OotPortControls_FindBinding(const char* name) {
    int i;

    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        if (OotPortControls_StrIcmp(name, sButtonBindings[i].iniName) == 0) {
            return i;
        }
    }

    return -1;
}

static const N64Action* OotPortControls_FindActionByName(const char* name) {
    int i;

    for (i = 0; i < (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0])); i++) {
        if (OotPortControls_StrIcmp(name, sN64Actions[i].iniName) == 0 ||
            OotPortControls_StrIcmp(name, sN64Actions[i].displayName) == 0) {
            return &sN64Actions[i];
        }
    }

    return NULL;
}

static int OotPortControls_FindActionIndexByMask(u16 n64Mask) {
    int i;

    for (i = 0; i < (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0])); i++) {
        if (sN64Actions[i].n64Mask == n64Mask) {
            return i;
        }
    }

    return 0;
}

static u16 OotPortControls_ParseActionList(char* value) {
    u16 n64Mask = 0;
    char* token;

    for (token = strtok(value, ",|+"); token != NULL; token = strtok(NULL, ",|+")) {
        const N64Action* action = OotPortControls_FindActionByName(OotPortControls_Trim(token));

        if (action != NULL) {
            n64Mask |= action->n64Mask;
        }
    }

    return n64Mask;
}

static void OotPortControls_EnsureInitialized(void) {
    if (!sInitialized) {
        OotPortControls_InitDefaults();
    }
}

void OotPortControls_InitDefaults(void) {
    int i;

    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        sButtonBindings[i].n64Mask = sButtonBindings[i].defaultN64Mask;
    }

    sDeadzone = 0;
    sInitialized = true;
}

void OotPortControls_ResetDefaults(void) {
    OotPortControls_InitDefaults();
}

s32 OotPortControls_Load(void) {
    char pathBuffer[384];
    char iniBuffer[OOT_PS2_CONTROLS_INI_BUFFER_SIZE];
    const char* path;
    Handle fd;
    s32 readSize;
    char* line;
    char* nextLine;
    int section = 0;

    OotPortControls_InitDefaults();

    path = OotPortControls_Path(pathBuffer, sizeof(pathBuffer));
    fd = File_Open(path, OOT_PS2_FILE_RDONLY, 0);
    if (fd < 0) {
        if (OotPort_IsDiscBoot()) {
            return 0;
        }
        return OotPortControls_Save();
    }

    readSize = File_Read(fd, iniBuffer, sizeof(iniBuffer) - 1);
    File_Close(fd);
    if (readSize < 0) {
        return readSize;
    }

    iniBuffer[readSize] = '\0';

    for (line = iniBuffer; line != NULL; line = nextLine) {
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

        line = OotPortControls_Trim(line);
        if (line[0] == '\0') {
            continue;
        }

        if (line[0] == '[') {
            char* sectionEnd = strchr(line, ']');

            if (sectionEnd != NULL) {
                *sectionEnd = '\0';
                if (OotPortControls_StrIcmp(line + 1, "buttons") == 0) {
                    section = 1;
                } else if (OotPortControls_StrIcmp(line + 1, "analog") == 0) {
                    section = 2;
                } else {
                    section = 0;
                }
            }
            continue;
        }

        equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        key = OotPortControls_Trim(line);
        value = OotPortControls_Trim(equals + 1);

        if (section == 1) {
            int bindingIndex = OotPortControls_FindBinding(key);

            if (bindingIndex >= 0) {
                sButtonBindings[bindingIndex].n64Mask = OotPortControls_ParseActionList(value);
            }
        } else if (section == 2 && OotPortControls_StrIcmp(key, "deadzone") == 0) {
            OotPortControls_SetDeadzone(atoi(value));
        }
    }

    return 0;
}

s32 OotPortControls_Save(void) {
    char pathBuffer[384];
    char iniBuffer[OOT_PS2_CONTROLS_INI_BUFFER_SIZE];
    char valueBuffer[96];
    const char* path;
    int written;
    int offset = 0;
    int i;
    Handle fd;

    OotPortControls_EnsureInitialized();

#define OOT_PS2_CONTROLS_APPEND(...)                                                             \
    do {                                                                                          \
        written = snprintf(iniBuffer + offset, sizeof(iniBuffer) - (size_t)offset, __VA_ARGS__); \
        if (written < 0 || (size_t)written >= sizeof(iniBuffer) - (size_t)offset) {               \
            return -1;                                                                            \
        }                                                                                         \
        offset += written;                                                                        \
    } while (0)

    OOT_PS2_CONTROLS_APPEND("[buttons]\n");
    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        OotPortControls_GetBindingValueText(i, valueBuffer, sizeof(valueBuffer));
        OOT_PS2_CONTROLS_APPEND("%s = %s\n", sButtonBindings[i].iniName, valueBuffer);
    }

    OOT_PS2_CONTROLS_APPEND("\n[analog]\n");
    OOT_PS2_CONTROLS_APPEND("deadzone = %d\n", sDeadzone);

#undef OOT_PS2_CONTROLS_APPEND

    path = OotPortControls_Path(pathBuffer, sizeof(pathBuffer));
    fd = File_Open(path, OOT_PS2_FILE_WRONLY | OOT_PS2_FILE_CREAT | OOT_PS2_FILE_TRUNC, 0777);
    if (fd < 0) {
        return fd;
    }

    written = File_Write(fd, iniBuffer, offset);
    File_Close(fd);
    return written == offset ? 0 : -1;
}

u16 OotPortControls_MapButtons(u32 buttons) {
    u16 n64Buttons = 0;
    int i;

    OotPortControls_EnsureInitialized();

    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        if (buttons & sButtonBindings[i].buttonMask) {
            n64Buttons |= sButtonBindings[i].n64Mask;
        }
    }

    return n64Buttons;
}

s8 OotPortControls_MapStick(u8 raw) {
    s32 centered = (s32)raw - 128;

    OotPortControls_EnsureInitialized();

    if (centered >= -sDeadzone && centered <= sDeadzone) {
        centered = 0;
    }

    if (centered < -80) {
        centered = -80;
    }
    if (centered > 80) {
        centered = 80;
    }

    return (s8)centered;
}

int OotPortControls_GetBindingCount(void) {
    return (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0]));
}

const char* OotPortControls_GetBindingName(int index) {
    if (index < 0 || index >= OotPortControls_GetBindingCount()) {
        return "";
    }

    return sButtonBindings[index].displayName;
}

void OotPortControls_GetBindingValueText(int index, char* buffer, size_t bufferSize) {
    u16 mask;
    size_t used = 0;
    int i;

    if (buffer == NULL || bufferSize == 0) {
        return;
    }

    buffer[0] = '\0';
    if (index < 0 || index >= OotPortControls_GetBindingCount()) {
        return;
    }

    mask = sButtonBindings[index].n64Mask;
    if (mask == 0) {
        snprintf(buffer, bufferSize, "None");
        return;
    }

    for (i = 1; i < (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0])); i++) {
        if (mask & sN64Actions[i].n64Mask) {
            int written = snprintf(buffer + used, bufferSize - used, "%s%s", used > 0 ? "+" : "",
                                   sN64Actions[i].displayName);

            if (written < 0 || (size_t)written >= bufferSize - used) {
                buffer[bufferSize - 1] = '\0';
                return;
            }
            used += (size_t)written;
        }
    }
}

void OotPortControls_CycleBinding(int index, int direction) {
    int actionIndex;
    int actionCount = (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0]));

    if (index < 0 || index >= OotPortControls_GetBindingCount()) {
        return;
    }

    actionIndex = OotPortControls_FindActionIndexByMask(sButtonBindings[index].n64Mask);
    actionIndex += direction >= 0 ? 1 : -1;

    if (actionIndex < 0) {
        actionIndex = actionCount - 1;
    } else if (actionIndex >= actionCount) {
        actionIndex = 0;
    }

    sButtonBindings[index].n64Mask = sN64Actions[actionIndex].n64Mask;
}

int OotPortControls_GetDeadzone(void) {
    OotPortControls_EnsureInitialized();
    return sDeadzone;
}

void OotPortControls_SetDeadzone(int deadzone) {
    sDeadzone = OotPortControls_ClampDeadzone(deadzone);
}

void OotPortControls_AdjustDeadzone(int delta) {
    OotPortControls_SetDeadzone(sDeadzone + delta);
}

int OotPortControls_IsN64StickScalingEnabled(void) {
    //return sN64StickScalingEnabled;
    return 0;
}

void OotPortControls_SetN64StickScalingEnabled(int enabled) {
    //sN64StickScalingEnabled = enabled != 0;
}
