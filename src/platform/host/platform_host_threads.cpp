// SPDX-License-Identifier: GPL-3.0-or-later
/// A computer's answer to the task table and the memory sheet: the process's threads with their CPU time, its resident memory and the machine's available memory, read from each OS's own interface.
///
/// @technical
///
/// ## The threads
///
/// Linux and Android list a process's threads under `/proc/self/task`, each with its name and its user and system time in clock ticks. macOS answers `task_threads`, and `thread_info` gives each thread's name and times. Windows walks a thread snapshot of the process and asks each thread its times.
///
/// The thread mm_main runs on reads "render": `recordSystemHeap`, the first call of mm_main, marks it. A worker carries the name it was spawned with, which `nameCurrentThread` gives it as it starts.
///
/// A thread's CPU share is its CPU time since the last snapshot over the wall time between them, per mille of one core, as FreeRTOS's run-time stats give a task's. The first snapshot has no earlier one, so its shares read unmeasured.
///
/// ## The memory
///
/// The process's resident memory is what it holds in RAM, and the machine's available memory is what it can still give before it swaps: `/proc/self/statm` and `MemAvailable` on Linux, `task_info` and the VM statistics on macOS, the working set and `GlobalMemoryStatusEx` on Windows.

#include "platform/platform_heap.h"
#include "platform/platform_tasks.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif

namespace mm::platform {

namespace {

/// One thread's CPU time at the last snapshot, so the next one gives its share.
struct CpuMark { uint64_t id = 0; uint64_t cpuNs = 0; };
constexpr size_t kMarks = 64;
uint64_t renderThread_ = 0;   ///< the thread mm_main runs on, which the table names "render"
CpuMark marks_[kMarks];
CpuMark prevMarks_[kMarks];   ///< the last snapshot's marks while the next is taken, beside it rather than on the stack
size_t markCount_ = 0;
uint64_t lastWallNs_ = 0;

uint64_t wallNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// One snapshot being taken: the rows it fills, and the marks of the one before for the shares.
struct Snapshot {
    TaskInfo* out;
    size_t max;
    uint64_t dWall;
    const CpuMark* prev;
    size_t prevCount = 0;
    size_t n = 0;
    size_t nextCount = 0;

    /// The share since the last snapshot, per mille of one core, unmeasured on the first.
    uint32_t shareOf(uint64_t id, uint64_t cpuNs) const {
        if (dWall == 0) return kTaskCpuUnmeasured;
        for (size_t i = 0; i < prevCount; i++) {
            if (prev[i].id != id) continue;
            const uint64_t permille = (cpuNs > prev[i].cpuNs ? cpuNs - prev[i].cpuNs : 0) * 1000u / dWall;
            return static_cast<uint32_t>(permille > 1000u ? 1000u : permille);
        }
        return kTaskCpuUnmeasured;
    }

    /// Fill one row and keep its mark for the next snapshot.
    void add(uint64_t id, const char* name, TaskState state, uint64_t cpuNs) {
        if (n >= max) return;
        TaskInfo& t = out[n++];
        t = TaskInfo{};
        std::snprintf(t.name, sizeof(t.name), "%s", renderThread_ != 0 && id == renderThread_ ? "render" : name);
        t.state = state;
        t.cpuPermille = shareOf(id, cpuNs);
        if (nextCount < kMarks) marks_[nextCount++] = {id, cpuNs};
    }
};

#if defined(__linux__)
/// The first line of a small file, its newline cut.
void readLine(const char* path, char* buf, size_t cap) {
    buf[0] = '\0';
    if (FILE* f = std::fopen(path, "r")) {
        if (std::fgets(buf, static_cast<int>(cap), f)) buf[std::strcspn(buf, "\n")] = '\0';
        std::fclose(f);
    }
}

/// One thread of `/proc/self/task`: its name, its state and its user plus system time.
void addThread(Snapshot& s, long tid) {
    char path[64], name[32], stat[512];
    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid);
    readLine(path, name, sizeof(name));
    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
    readLine(path, stat, sizeof(stat));
    // After the parenthesized name: the state, then user time as the 12th field and system time as the 13th.
    const char* rest = std::strrchr(stat, ')');
    char st = '?';
    unsigned long long utime = 0, stime = 0;
    if (rest) std::sscanf(rest + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &st, &utime, &stime);
    const long tick = sysconf(_SC_CLK_TCK);
    const uint64_t cpuNs = tick > 0 ? (utime + stime) * 1000000000ull / static_cast<uint64_t>(tick) : 0;
    const TaskState state = st == 'R' ? TaskState::Running : (st == 'S' || st == 'D') ? TaskState::Blocked : TaskState::Unknown;
    s.add(static_cast<uint64_t>(tid), tid == getpid() ? "main" : name, state, cpuNs);
}

void listThreads(Snapshot& s) {
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return;
    while (dirent* e = readdir(dir))
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') addThread(s, std::atol(e->d_name));
    closedir(dir);
}
#elif defined(__APPLE__)
/// One Mach thread: its name, run state and user plus system time, keyed by its system-wide id.
void addThread(Snapshot& s, thread_act_t th, bool first) {
    thread_extended_info_data_t info{};
    mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(th, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &size) != KERN_SUCCESS) return;
    thread_identifier_info_data_t ident{};
    mach_msg_type_number_t isize = THREAD_IDENTIFIER_INFO_COUNT;
    const bool known = thread_info(th, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident), &isize) == KERN_SUCCESS;
    const TaskState state = info.pth_run_state == TH_STATE_RUNNING ? TaskState::Running
                          : info.pth_run_state == TH_STATE_WAITING ? TaskState::Blocked : TaskState::Unknown;
    const char* name = info.pth_name[0] ? info.pth_name : (first ? "main" : "thread");
    s.add(known ? ident.thread_id : static_cast<uint64_t>(th), name, state, info.pth_user_time + info.pth_system_time);
}

void listThreads(Snapshot& s) {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        addThread(s, threads[i], i == 0);   // the kernel lists the main thread first
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), sizeof(thread_act_t) * count);
}
#elif defined(_WIN32)
/// A thread's kernel plus user time, in nanoseconds.
uint64_t threadCpuNs(DWORD id) {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!h) return 0;
    FILETIME c, x, k, u;
    uint64_t ns = 0;
    if (GetThreadTimes(h, &c, &x, &k, &u))
        ns = ((static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime)) * 100u;
    CloseHandle(h);
    return ns;
}

void listThreads(Snapshot& s) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    bool first = true;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        char name[16];
        std::snprintf(name, sizeof(name), first ? "main" : "thread %lu", static_cast<unsigned long>(te.th32ThreadID));
        first = false;
        s.add(te.th32ThreadID, name, TaskState::Unknown, threadCpuNs(te.th32ThreadID));
    }
    CloseHandle(snap);
}
#else
void listThreads(Snapshot&) {}
#endif

/// The process's resident bytes.
size_t residentBytes() {
#if defined(__linux__)
    unsigned long pages = 0, rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%lu %lu", &pages, &rss) != 2) rss = 0;
        std::fclose(f);
    }
    return static_cast<size_t>(rss) * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
        ? static_cast<size_t>(info.resident_size) : 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.WorkingSetSize : 0;
#else
    return 0;
#endif
}

/// The machine's available bytes, what it gives before it swaps.
size_t availableBytes() {
#if defined(__linux__)
    size_t available = 0;
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        unsigned long kb = 0;
        while (available == 0 && std::fgets(line, sizeof(line), f))
            if (std::sscanf(line, "MemAvailable: %lu kB", &kb) == 1) available = static_cast<size_t>(kb) * 1024u;
        std::fclose(f);
    }
    return available;
#elif defined(__APPLE__)
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    return host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS
        ? static_cast<size_t>(vm.free_count + vm.inactive_count) * page : 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) ? static_cast<size_t>(ms.ullAvailPhys) : 0;
#else
    return 0;
#endif
}

/// The calling thread's id, as the thread list keys it: the kernel's thread id on Linux, the system-wide one on macOS, the thread id on Windows.
uint64_t currentNativeThread() {
#if defined(__linux__)
    return static_cast<uint64_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#elif defined(_WIN32)
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

}  // namespace

void markRenderThread() { renderThread_ = currentNativeThread(); }

void nameCurrentThread(const char* name) {
    if (!name || !name[0]) return;
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    char cut[16];   // the kernel keeps 15 characters and the terminator
    std::snprintf(cut, sizeof(cut), "%s", name);
    pthread_setname_np(pthread_self(), cut);
#endif
}

/// The process's threads, the host's task table; MoonShine reads it when someone looks.
size_t hostThreads(TaskInfo* out, size_t maxTasks) {
    if (!out || maxTasks == 0) return 0;
    const uint64_t now = wallNs();
    std::memcpy(prevMarks_, marks_, sizeof(CpuMark) * markCount_);
    Snapshot s{out, maxTasks, lastWallNs_ ? now - lastWallNs_ : 0, prevMarks_};
    s.prevCount = markCount_;
    listThreads(s);
    markCount_ = s.nextCount;
    lastWallNs_ = now;
    return s.n;
}

void processMemory(size_t& resident, size_t& available) {
    resident = residentBytes();
    available = availableBytes();
}

}  // namespace mm::platform
