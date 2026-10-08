#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::sysinfo {

bool cpuName(char* out, std::size_t cap);
bool cpuThreads(int& out);
struct Memory {
    std::uint64_t privateBytes;
    std::uint64_t workingSet;
    std::uint64_t totalPhysical;
};
bool memoryUsage(Memory& out);

struct Display {
    int width;
    int height;
    int refreshHz;
    double refreshRate;
    char driver[32];
    char adapter[128];
    char vendor[32];
};
bool displayInfo(void* hwnd, Display& out);
void* mainWindow();

bool gpuUtilization(int& percent);
void closeGpuUtilization();

}
