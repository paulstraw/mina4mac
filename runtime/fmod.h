// The part of the FMOD Studio/Core C API (2.01) the FMOD bridge (fmod.c) calls, as a table of function
// pointers filled from the native macOS dylibs (dlsym) or by the silent stub (fmod_stub.c). The types are
// declared here, so the launcher builds without the FMOD SDK. Compiled with FMOD_REAL_HEADERS and the SDK's
// inc/ on the include path (fmodtest in tools/check.sh), fmod.c instead checks every entry against the SDK's
// prototypes.
#pragma once
#include <stdint.h>

#ifdef FMOD_REAL_HEADERS
#include <fmod_studio.h>
#else
typedef int FMOD_RESULT, FMOD_BOOL, FMOD_OUTPUTTYPE, FMOD_STUDIO_STOP_MODE, FMOD_THREAD_TYPE, FMOD_THREAD_PRIORITY;
typedef int FMOD_STUDIO_USER_PROPERTY_TYPE;
typedef unsigned int FMOD_INITFLAGS, FMOD_STUDIO_INITFLAGS, FMOD_STUDIO_LOAD_BANK_FLAGS, FMOD_THREAD_STACK_SIZE;
typedef unsigned int FMOD_STUDIO_SYSTEM_CALLBACK_TYPE, FMOD_STUDIO_EVENT_CALLBACK_TYPE;
typedef unsigned long long FMOD_THREAD_AFFINITY;
typedef struct FMOD_SYSTEM FMOD_SYSTEM;
typedef struct FMOD_CHANNELGROUP FMOD_CHANNELGROUP;
typedef struct FMOD_STUDIO_SYSTEM FMOD_STUDIO_SYSTEM;
typedef struct FMOD_STUDIO_EVENTDESCRIPTION FMOD_STUDIO_EVENTDESCRIPTION;
typedef struct FMOD_STUDIO_EVENTINSTANCE FMOD_STUDIO_EVENTINSTANCE;
typedef struct FMOD_STUDIO_BUS FMOD_STUDIO_BUS;
typedef struct FMOD_STUDIO_BANK FMOD_STUDIO_BANK;
typedef struct { float x, y, z; } FMOD_VECTOR;
typedef struct { FMOD_VECTOR position, velocity, forward, up; } FMOD_3D_ATTRIBUTES;
typedef struct {
    const char *name;
    FMOD_STUDIO_USER_PROPERTY_TYPE type;
    union { int intvalue; FMOD_BOOL boolvalue; float floatvalue; const char *stringvalue; };
} FMOD_STUDIO_USER_PROPERTY;
typedef struct { const char *name; int position; } FMOD_STUDIO_TIMELINE_MARKER_PROPERTIES;
typedef FMOD_RESULT (*FMOD_STUDIO_SYSTEM_CALLBACK)(FMOD_STUDIO_SYSTEM *system, FMOD_STUDIO_SYSTEM_CALLBACK_TYPE type,
                                                   void *commanddata, void *userdata);
typedef FMOD_RESULT (*FMOD_STUDIO_EVENT_CALLBACK)(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE *event,
                                                  void *parameters);
#endif

// Values shared by the 32-bit Windows and the macOS builds (same enums, same header version scheme).
enum {
    FM_OK = 0, FM_ERR_INVALID_HANDLE = 30, FM_ERR_EVENT_NOTFOUND = 74,
    FM_USER_PROPERTY_STRING = 3, FM_OUTPUTTYPE_WAVWRITER = 3,
    FM_SYSTEM_CALLBACK_BANK_UNLOAD = 0x4,
    FM_EVENT_CALLBACK_DESTROYED = 0x2, FM_EVENT_CALLBACK_TIMELINE_MARKER = 0x800, FM_EVENT_CALLBACK_TIMELINE_BEAT = 0x1000,
    FM_EVENT_CALLBACK_NESTED_TIMELINE_BEAT = 0x40000,
    FM_THREAD_TYPE_STUDIO_UPDATE = 8, FM_THREAD_TYPE_STUDIO_LOAD_BANK = 9, FM_THREAD_TYPE_STUDIO_LOAD_SAMPLE = 10,
    FM_THREAD_PRIORITY_DEFAULT = -32 * 1024 - 1,
};
#define FM_THREAD_AFFINITY_GROUP_DEFAULT 0x8000000000000000ull

// X(return type, name without the FMOD_ prefix, parameter list)
#define FMOD_API(X)                                                                                                    \
    X(FMOD_RESULT, Thread_SetAttributes, (FMOD_THREAD_TYPE, FMOD_THREAD_AFFINITY, FMOD_THREAD_PRIORITY, FMOD_THREAD_STACK_SIZE)) \
    X(FMOD_RESULT, System_GetMasterChannelGroup, (FMOD_SYSTEM *, FMOD_CHANNELGROUP **))                             \
    X(FMOD_RESULT, System_GetVersion, (FMOD_SYSTEM *, unsigned int *))                                               \
    X(FMOD_RESULT, System_Set3DSettings, (FMOD_SYSTEM *, float, float, float))                                       \
    X(FMOD_RESULT, System_SetDSPBufferSize, (FMOD_SYSTEM *, unsigned int, int))                                      \
    X(FMOD_RESULT, System_SetOutput, (FMOD_SYSTEM *, FMOD_OUTPUTTYPE))                                               \
    X(FMOD_RESULT, System_SetSoftwareChannels, (FMOD_SYSTEM *, int))                                                 \
    X(FMOD_RESULT, ChannelGroup_SetVolume, (FMOD_CHANNELGROUP *, float))                                             \
    X(FMOD_RESULT, Studio_System_Create, (FMOD_STUDIO_SYSTEM **, unsigned int))                                      \
    X(FMOD_RESULT, Studio_System_Initialize, (FMOD_STUDIO_SYSTEM *, int, FMOD_STUDIO_INITFLAGS, FMOD_INITFLAGS, void *)) \
    X(FMOD_RESULT, Studio_System_Release, (FMOD_STUDIO_SYSTEM *))                                                    \
    X(FMOD_RESULT, Studio_System_Update, (FMOD_STUDIO_SYSTEM *))                                                     \
    X(FMOD_BOOL, Studio_System_IsValid, (FMOD_STUDIO_SYSTEM *))                                                      \
    X(FMOD_RESULT, Studio_System_GetCoreSystem, (FMOD_STUDIO_SYSTEM *, FMOD_SYSTEM **))                              \
    X(FMOD_RESULT, Studio_System_GetBus, (FMOD_STUDIO_SYSTEM *, const char *, FMOD_STUDIO_BUS **))                   \
    X(FMOD_RESULT, Studio_System_GetEvent, (FMOD_STUDIO_SYSTEM *, const char *, FMOD_STUDIO_EVENTDESCRIPTION **))    \
    X(FMOD_RESULT, Studio_System_LoadBankFile, (FMOD_STUDIO_SYSTEM *, const char *, FMOD_STUDIO_LOAD_BANK_FLAGS, FMOD_STUDIO_BANK **)) \
    X(FMOD_RESULT, Studio_System_SetCallback, (FMOD_STUDIO_SYSTEM *, FMOD_STUDIO_SYSTEM_CALLBACK, FMOD_STUDIO_SYSTEM_CALLBACK_TYPE)) \
    X(FMOD_RESULT, Studio_System_SetListenerAttributes, (FMOD_STUDIO_SYSTEM *, int, const FMOD_3D_ATTRIBUTES *, const FMOD_VECTOR *)) \
    X(FMOD_RESULT, Studio_Bank_GetEventCount, (FMOD_STUDIO_BANK *, int *))                                           \
    X(FMOD_RESULT, Studio_Bank_GetEventList, (FMOD_STUDIO_BANK *, FMOD_STUDIO_EVENTDESCRIPTION **, int, int *))      \
    X(FMOD_RESULT, Studio_EventDescription_CreateInstance, (FMOD_STUDIO_EVENTDESCRIPTION *, FMOD_STUDIO_EVENTINSTANCE **)) \
    X(FMOD_RESULT, Studio_EventDescription_GetLength, (FMOD_STUDIO_EVENTDESCRIPTION *, int *))                       \
    X(FMOD_RESULT, Studio_EventDescription_GetMinimumDistance, (FMOD_STUDIO_EVENTDESCRIPTION *, float *))            \
    X(FMOD_RESULT, Studio_EventDescription_GetMaximumDistance, (FMOD_STUDIO_EVENTDESCRIPTION *, float *))            \
    X(FMOD_RESULT, Studio_EventDescription_GetPath, (FMOD_STUDIO_EVENTDESCRIPTION *, char *, int, int *))            \
    X(FMOD_RESULT, Studio_EventDescription_GetUserProperty, (FMOD_STUDIO_EVENTDESCRIPTION *, const char *, FMOD_STUDIO_USER_PROPERTY *)) \
    X(FMOD_RESULT, Studio_EventDescription_IsSnapshot, (FMOD_STUDIO_EVENTDESCRIPTION *, FMOD_BOOL *))                \
    X(FMOD_RESULT, Studio_EventDescription_LoadSampleData, (FMOD_STUDIO_EVENTDESCRIPTION *))                         \
    X(FMOD_BOOL, Studio_EventInstance_IsValid, (FMOD_STUDIO_EVENTINSTANCE *))                                        \
    X(FMOD_RESULT, Studio_EventInstance_Release, (FMOD_STUDIO_EVENTINSTANCE *))                                      \
    X(FMOD_RESULT, Studio_EventInstance_GetUserData, (FMOD_STUDIO_EVENTINSTANCE *, void **))                         \
    X(FMOD_RESULT, Studio_EventInstance_SetUserData, (FMOD_STUDIO_EVENTINSTANCE *, void *))                          \
    X(FMOD_RESULT, Studio_EventInstance_GetTimelinePosition, (FMOD_STUDIO_EVENTINSTANCE *, int *))                   \
    X(FMOD_RESULT, Studio_EventInstance_Set3DAttributes, (FMOD_STUDIO_EVENTINSTANCE *, FMOD_3D_ATTRIBUTES *))        \
    X(FMOD_RESULT, Studio_EventInstance_SetCallback, (FMOD_STUDIO_EVENTINSTANCE *, FMOD_STUDIO_EVENT_CALLBACK, FMOD_STUDIO_EVENT_CALLBACK_TYPE)) \
    X(FMOD_RESULT, Studio_EventInstance_SetParameterByName, (FMOD_STUDIO_EVENTINSTANCE *, const char *, float, FMOD_BOOL)) \
    X(FMOD_RESULT, Studio_EventInstance_SetPaused, (FMOD_STUDIO_EVENTINSTANCE *, FMOD_BOOL))                        \
    X(FMOD_RESULT, Studio_EventInstance_SetVolume, (FMOD_STUDIO_EVENTINSTANCE *, float))                             \
    X(FMOD_RESULT, Studio_EventInstance_Start, (FMOD_STUDIO_EVENTINSTANCE *))                                        \
    X(FMOD_RESULT, Studio_EventInstance_Stop, (FMOD_STUDIO_EVENTINSTANCE *, FMOD_STUDIO_STOP_MODE))                  \
    X(FMOD_RESULT, Studio_Bus_SetPaused, (FMOD_STUDIO_BUS *, FMOD_BOOL))                                             \
    X(FMOD_RESULT, Studio_Bus_SetVolume, (FMOD_STUDIO_BUS *, float))

typedef struct {
#define FMOD_API_FIELD(ret, name, params) ret(*name) params;
    FMOD_API(FMOD_API_FIELD)
#undef FMOD_API_FIELD
} FmodApi;

// The silent backend (fmod_stub.c): every call succeeds, banks hold no events, the only callback it
// makes is DESTROYED when an instance is released.
extern const FmodApi FMOD_STUB;
