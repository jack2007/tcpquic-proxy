#include "admin_memory.h"

#include "memory_stats.h"

bool TqHandleMemoryAdmin(const TqHttpRequest& req, std::string& response) {
    if (req.Method != "POST" || req.Path != "/memory/allocator:dump") {
        return false;
    }

    const TqMemoryAllocatorStats stats = TqDumpMemoryAllocatorStatsToLog();
    response = TqJsonResponse(200, TqMemoryAllocatorStatsJson(stats));
    return true;
}
