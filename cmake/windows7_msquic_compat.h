#pragma once

#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0602
#include <winnt.h>

typedef struct _SOCKET_PROCESSOR_AFFINITY {
    PROCESSOR_NUMBER Processor;
    USHORT NumaNodeId;
    USHORT Reserved;
} SOCKET_PROCESSOR_AFFINITY, *PSOCKET_PROCESSOR_AFFINITY;
#endif
