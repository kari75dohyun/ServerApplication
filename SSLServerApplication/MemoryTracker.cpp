#include "MemoryTracker.h"
#include "Logger.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
std::string MemoryTracker::get_memory_usage() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        // WorkingSetSize = 실제 메모리 (bytes)
        return "WorkingSetSize=" + std::to_string(pmc.WorkingSetSize / 1024) + " KB";
    }
    return "Unable to get memory usage";
}
#else
#include <unistd.h>
#include <fstream>
std::string MemoryTracker::get_memory_usage() {
    std::ifstream statm("/proc/self/statm");
    long total_pages, resident_pages;
    if (statm >> total_pages >> resident_pages) {
        long page_size = sysconf(_SC_PAGESIZE) / 1024; // KB
        return "VmSize=" + std::to_string(total_pages * page_size) + " KB, VmRSS=" +
            std::to_string(resident_pages * page_size) + " KB";
    }
    return "Unable to get memory usage";
}
#endif

void MemoryTracker::log_memory_usage() {
    g_logger->info("[MEM] {}", get_memory_usage());
}
