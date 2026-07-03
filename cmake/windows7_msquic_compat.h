#pragma once

#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0602
#if defined(_M_X64) && !defined(_AMD64_)
#define _AMD64_ 1
#elif defined(_M_IX86) && !defined(_X86_)
#define _X86_ 1
#elif defined(_M_ARM64) && !defined(_ARM64_)
#define _ARM64_ 1
#elif defined(_M_ARM) && !defined(_ARM_)
#define _ARM_ 1
#endif

#include <winnt.h>

typedef struct _SOCKET_PROCESSOR_AFFINITY {
    PROCESSOR_NUMBER Processor;
    USHORT NumaNodeId;
    USHORT Reserved;
} SOCKET_PROCESSOR_AFFINITY, *PSOCKET_PROCESSOR_AFFINITY;
#endif
