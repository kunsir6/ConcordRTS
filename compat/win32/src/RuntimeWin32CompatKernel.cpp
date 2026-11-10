// S8b 第一批：跨进程事件族（CreateEventW / OpenEventW / SetEvent / ResetEvent /
// WaitForSingleObject / WaitForMultipleObjects）+ strncpy_s。
//
// 为什么这一族必须先做：tests/multiplayer/RuntimeMultiplayerTwoProcessSmoke.cpp 是
// **唯一没有 #if 保护**的调用方（产品路径全在 #ifdef _WIN32 里），它用 5 个命名事件
// 做父子进程握手。这些符号在 windows.h 里只有声明，compat/src 下 0 实现——
// `-fsyntax-only` 能过（声明够了），但链接必失败。S8a 只兑现了"编译清零"，这一族
// 兑现的是"能链接"。
//
// 语义对齐的三条硬约束（不是"差不多"就行）：
//
// 1. **manual-reset 为主**。smoke 的 CreateEventW 传的是 bManualReset=TRUE，语义是
//    SetEvent 之后**保持** signaled 直到 ResetEvent，且要唤醒**所有**等待者。
//    POSIX 命名信号量做不到"唤醒所有"（sem 是计数），所以这里用
//    pthread_mutex/cond 放在**共享内存**里，用 PTHREAD_PROCESS_SHARED 实现真正的
//    跨进程条件变量。
//
// 2. **事件是命名的、跨进程的**。CreateEventW 在父进程建，子进程用 OpenEventW 打开
//    同一个名字。所以控制块必须落在 shm，不能是进程内堆。
//
// 3. **超时用 CLOCK_REALTIME**。macOS 的 pthread_condattr_setclock 不支持
//    CLOCK_MONOTONIC（返回 ENOTSUP），照 Linux 写法抄会静默退化成"永远不超时"。
//
// 已知边界（诚实写在这里，不假装完整）：
//   · WaitForSingleObject / WaitForMultipleObjects 目前只认 "event" 类句柄。
//     process / job / mapping 的等待要等进程族实现（S8b 后续），现在返回 WAIT_FAILED
//     而不是偷懒返回 WAIT_OBJECT_0 —— 后者会让 smoke 以为子进程已退出。
//   · shm 名字用 FNV-1a 压成 16 位十六进制：macOS 的 shm_open 名字上限约 31 字节，
//     而 objectName(token, suffix) 生成的名字可能远超这个长度。哈希碰撞概率在
//     smoke 的 5 个名字量级下可忽略，但它不是"绝对安全"的。
//   · 创建者/打开者之间的初始化竞态用 ready 标志 + 自旋处理。父子进程之间有
//     exec 的天然间隔，实测够用；没有做完全无锁的初始化协议。

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <mach-o/dyld.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char* kEventKind = "event";

// 放在共享内存里的控制块。全部字段都要是 POD 且不含指针——它是被两个进程
// 同时 mmap 的同一块物理页。
struct EventControl {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    std::int32_t signaled;
    std::int32_t manualReset;
    std::int32_t ready;  // 0 = 创建者还没初始化完
};

// 本进程侧的事件句柄。指向共享控制块，并记住"这块 shm 是不是我建的"
// （只有创建者负责 unlink，否则先关的那个进程会把别人正在用的名字删掉）。
struct CompatEvent {
    EventControl* control = nullptr;
    std::string shmName;
    bool heapBacked = false;  // 匿名事件：control 是本进程 calloc 的
    bool creator = false;     // 命名事件：本进程执行了 O_CREAT|O_EXCL
};

// macOS 的 shm_open 名字很短（PSHMNAMLEN 约 31），所以压成固定长度。
// FNV-1a over UTF-32 码点，取 64 位结果的十六进制。
std::string sharedNameFor(const wchar_t* wide) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t* p = wide; p != nullptr && *p != L'\0'; ++p) {
        const std::uint32_t code = static_cast<std::uint32_t>(*p);
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (code >> shift) & 0xFFu;
            hash *= 1099511628211ull;
        }
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "/cqe%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(buffer);
}

bool initialiseControl(EventControl* control, bool manualReset, bool initialState) {
    pthread_mutexattr_t mutexAttr;
    if (pthread_mutexattr_init(&mutexAttr) != 0) return false;
    pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_t mutex;
    const bool mutexOk = pthread_mutex_init(&mutex, &mutexAttr) == 0;
    pthread_mutexattr_destroy(&mutexAttr);

    pthread_condattr_t condAttr;
    if (pthread_condattr_init(&condAttr) != 0) return false;
    pthread_condattr_setpshared(&condAttr, PTHREAD_PROCESS_SHARED);
    pthread_cond_t cond;
    const bool condOk = pthread_cond_init(&cond, &condAttr) == 0;
    pthread_condattr_destroy(&condAttr);

    if (!mutexOk || !condOk) return false;

    control->mutex = mutex;
    control->cond = cond;
    control->manualReset = manualReset ? 1 : 0;
    control->signaled = initialState ? 1 : 0;
    // ready 最后写：打开者先看到它，再去碰 mutex/cond。
    __atomic_store_n(&control->ready, 1, __ATOMIC_RELEASE);
    return true;
}

// 打开者等创建者把控制块初始化完。父子进程之间有 exec 间隔，这里只是兜底。
bool waitUntilReady(const EventControl* control) {
    for (int spin = 0; spin < 500; ++spin) {  // 最多约 100ms
        if (__atomic_load_n(&control->ready, __ATOMIC_ACQUIRE) != 0) return true;
        ::usleep(200);
    }
    return false;
}

void destroyEvent(void* payload) {
    auto* event = static_cast<CompatEvent*>(payload);
    if (event == nullptr) return;
    if (event->control != nullptr) {
        if (event->heapBacked) {
            pthread_mutex_destroy(&event->control->mutex);
            pthread_cond_destroy(&event->control->cond);
            std::free(event->control);
        } else {
            ::munmap(event->control, sizeof(EventControl));
            // 只有创建者负责删名字，否则先退出的进程会拆掉另一个还在用的对象。
            if (event->creator) ::shm_unlink(event->shmName.c_str());
        }
    }
    delete event;
}

CompatEvent* asEvent(HANDLE handle) {
    if (!concord_compat::isRegisteredHandle(reinterpret_cast<void*>(handle), kEventKind)) {
        return nullptr;
    }
    return reinterpret_cast<CompatEvent*>(handle);
}

// CLOCK_REALTIME 上的绝对截止时刻。macOS 不支持 COND_CLOCK_MONOTONIC。
timespec deadlineAfter(DWORD milliseconds) {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += static_cast<time_t>(milliseconds / 1000u);
    ts.tv_nsec += static_cast<long>(milliseconds % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

// 单个事件是否 signaled（调用方负责持锁）。
bool isSignaledLocked(const EventControl* control) {
    return control->signaled != 0;
}

// ---------------------------------------------------------------------------
// 共享内存（S8b 第二批）：CreateFileMappingW / OpenFileMappingW / MapViewOfFile /
// UnmapViewOfFile
// ---------------------------------------------------------------------------
constexpr const char* kMappingKind = "mapping";
constexpr const char* kViewKind = "view";

// mapping 持有一个 fd；每次 MapViewOfFile 都从它 mmap 出**独立**的一段。
// 为什么不让 view 直接指向 mapping 的 base：Windows 上对同一 mapping 多次
// MapViewOfFile 得到的是**不同地址**且可独立解映射。若共用一个 base，第二个 view
// 会与第一个撞地址，UnmapViewOfFile 也会互相干扰。
struct CompatMapping {
    std::string shmName;
    int fd = -1;             // 保活：MapViewOfFile 需要它
    std::size_t size = 0;
    int prot = 0;            // 保护位，用于校验 view 的请求
    bool creator = false;    // 本进程执行了 O_CREAT|O_EXCL，负责 unlink
    bool anonymous = false;  // 无名字（建完立刻 unlink），不负责 unlink
};

// 每个 view 要单独记账：UnmapViewOfFile 只给一个基址，而 munmap 需要
// **页对齐的 raw 指针 + 长度**。Windows 允许任意字节偏移，mmap 不允许，
// 所以真实映射的起点和长度都要按基址存起来。
struct ViewRecord {
    void* raw = nullptr;
    std::size_t length = 0;
};

std::mutex& viewMutex() {
    static std::mutex value;
    return value;
}

std::unordered_map<void*, ViewRecord>& viewRecords() {
    static std::unordered_map<void*, ViewRecord> value;
    return value;
}

void destroyMapping(void* payload) {
    auto* mapping = static_cast<CompatMapping*>(payload);
    if (mapping == nullptr) return;
    if (mapping->fd >= 0) ::close(mapping->fd);
    if (!mapping->anonymous && mapping->creator) ::shm_unlink(mapping->shmName.c_str());
    delete mapping;
}

void destroyView(void* payload) {
    ViewRecord record;
    {
        std::lock_guard<std::mutex> lock(viewMutex());
        auto found = viewRecords().find(payload);
        if (found == viewRecords().end()) return;
        record = found->second;
        viewRecords().erase(found);
    }
    if (record.raw != nullptr && record.length != 0) ::munmap(record.raw, record.length);
}

CompatMapping* asMapping(HANDLE handle) {
    if (!concord_compat::isRegisteredHandle(reinterpret_cast<void*>(handle), kMappingKind)) {
        return nullptr;
    }
    return reinterpret_cast<CompatMapping*>(handle);
}

// flProtect（PAGE_*）与 dwDesiredAccess（FILE_MAP_*）是**两套数值域，而且会撞**：
// PAGE_READONLY 是 0x02，FILE_MAP_WRITE 也是 0x0002。塞进同一个判断，只读映射
// 会被误判成"可写"，MapViewOfFile 就返回一个 PROT_READ 的地址，调用点一写即
// SIGSEGV。所以必须分成两个函数。
int protectionForFlProtect(DWORD flProtect) {
    switch (flProtect) {
        case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
        case PAGE_READONLY:  return PROT_READ;
        default:             return -1;
    }
}

int protectionForViewAccess(DWORD access) {
    if ((access & FILE_MAP_WRITE) != 0) return PROT_READ | PROT_WRITE;
    if ((access & FILE_MAP_READ) != 0) return PROT_READ;
    return -1;
}

// 匿名 mapping 也需要 fd，才能让多个 view 共享同一块内存。做法：唯一名字建好后
// **立刻 unlink** —— 名字消失、fd 仍有效、内存随最后一个引用释放。
std::string anonymousShmName() {
    static std::atomic<unsigned long> counter{0};
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "/cqa%ld_%lu", static_cast<long>(::getpid()),
                  static_cast<unsigned long>(counter.fetch_add(1)));
    return std::string(buffer);
}


// ---------------------------------------------------------------------------
// 进程族（S8b 第三批）：CreateProcessW / TerminateProcess / GetExitCodeProcess /
// GetModuleFileNameW，以及 WaitForSingleObject 的 process 分支
// ---------------------------------------------------------------------------
constexpr const char* kProcessKind = "process";
constexpr const char* kThreadKind = "thread";

struct CompatProcess {
    pid_t pid = -1;
    bool reaped = false;               // 已经 waitpid 过
    DWORD exitCode = STILL_ACTIVE;     // TerminateProcess 可以预设
};

// macOS 没有"线程句柄"这种可独立关闭的对象。给 hThread 一个空壳只是为了
// CloseHandle(hThread) 有据可依——返回 nullptr 也能跑（smoke 会跳过），
// 但那会让"句柄非空"这条 Windows 契约悄悄失效。
struct CompatThread {};

void destroyProcess(void* payload) {
    auto* process = static_cast<CompatProcess*>(payload);
    if (process == nullptr) return;
    // Windows 上 CloseHandle 不杀进程，这里也不杀。只是顺手做一次非阻塞 reap，
    // 免得父进程长期运行时留下僵尸。若子进程还在跑，就留给系统（本层不接管
    // 生命周期——接管会违反"CloseHandle 不终止"的语义）。
    if (!process->reaped && process->pid > 0) {
        int status = 0;
        const pid_t result = ::waitpid(process->pid, &status, WNOHANG);
        if (result == process->pid) {
            process->reaped = true;
            if (process->exitCode == STILL_ACTIVE) {
                process->exitCode = WIFEXITED(status)
                    ? static_cast<DWORD>(WEXITSTATUS(status))
                    : static_cast<DWORD>(128 + WTERMSIG(status));
            }
        }
    }
    delete process;
}

void destroyThread(void* payload) { delete static_cast<CompatThread*>(payload); }

CompatProcess* asProcess(HANDLE handle) {
    if (!concord_compat::isRegisteredHandle(reinterpret_cast<void*>(handle), kProcessKind)) {
        return nullptr;
    }
    return reinterpret_cast<CompatProcess*>(handle);
}

// Windows 式命令行分词：双引号包住的整段算一个 token，`""` 是转义引号。
// smoke 的命令行形如 `"<path>" --child <role> <token>`，但也有不经引号的路径。
std::vector<std::string> splitCommandLine(LPCWSTR line) {
    std::vector<std::string> tokens;
    if (line == nullptr) return tokens;
    std::wstring current;
    bool inQuotes = false;
    for (const wchar_t* p = line; *p != L'\0'; ++p) {
        if (*p == L'"') {
            if (inQuotes && p[1] == L'"') {
                current.push_back(L'"');
                ++p;
            } else {
                inQuotes = !inQuotes;
            }
            continue;
        }
        if (!inQuotes && (*p == L' ' || *p == L'\t')) {
            if (!current.empty()) {
                tokens.push_back(concord_compat::utf8FromWideText(current.c_str()));
                current.clear();
            }
            continue;
        }
        current.push_back(*p);
    }
    if (!current.empty()) tokens.push_back(concord_compat::utf8FromWideText(current.c_str()));
    return tokens;
}

// 非阻塞收尸，并把退出码落进 process->exitCode（TerminateProcess 预设过就保留）。
void reapIfExited(CompatProcess* process) {
    if (process == nullptr || process->reaped || process->pid <= 0) return;
    int status = 0;
    const pid_t result = ::waitpid(process->pid, &status, WNOHANG);
    if (result == process->pid) {
        process->reaped = true;
        if (process->exitCode == STILL_ACTIVE) {
            process->exitCode = WIFEXITED(status)
                ? static_cast<DWORD>(WEXITSTATUS(status))
                : static_cast<DWORD>(128 + WTERMSIG(status));
        }
    } else if (result < 0 && errno == ECHILD) {
        // 已经被别人收走：当作已退出，但退出码不可知。
        process->reaped = true;
    }
}

DWORD waitForProcess(CompatProcess* process, DWORD dwMilliseconds) {
    if (process->reaped) return WAIT_OBJECT_0;
    const bool infinite = (dwMilliseconds == INFINITE);
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        reapIfExited(process);
        if (process->reaped) return WAIT_OBJECT_0;
        if (!infinite) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsed >= static_cast<long long>(dwMilliseconds)) return WAIT_TIMEOUT;
        }
        // 1ms 轮询而不是 kqueue(EVFILT_PROC)：EVFILT_PROC 只能在子进程上注册一次
        // 且可移植性差；smoke 的等待窗口是秒级，1ms 轮询的代价可以接受。
        ::usleep(1000);
    }
}

// ---------------------------------------------------------------------------
// Job 对象（S8b 第四批）：CreateJobObjectW / AssignProcessToJobObject /
// SetInformationJobObject / TerminateJobObject
// ---------------------------------------------------------------------------
// macOS 没有 Job 对象。Windows Job 提供的保证里，smoke 实际只用到一条：
// `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` —— Job 句柄全部关闭时，成员进程一起被杀。
// 所以这里用"成员 pid 表 + kill"对位，而不是进程组（setpgid）：
//   · posix_spawn 之后再 setpgid 会 EACCES（exec 之后不能改进程组）；
//   · 要 spawn 时就设组，得把 Job 信息塞进 CreateProcessW 的参数，而 Windows
//     的 AssignProcessToJobObject 是**事后**调用的——语义顺序对不上。
// 代价：这里不提供 CPU/内存上限、不做作业级记账、不支持命名 Job。
// 这些都是诚实缺口，不是"差不多实现了"。
constexpr const char* kJobKind = "job";

struct CompatJob {
    std::vector<pid_t> members;
    DWORD limitFlags = 0;
};

void destroyJob(void* payload) {
    auto* job = static_cast<CompatJob*>(payload);
    if (job == nullptr) return;
    // KILL_ON_JOB_CLOSE：最后一个句柄关闭时杀掉全部成员。这是 smoke 依赖的
    // 唯一语义（父进程异常退出时子进程不许成为孤儿继续跑）。
    if ((job->limitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0) {
        for (const pid_t member : job->members) {
            if (member > 0) ::kill(member, SIGKILL);
        }
    }
    delete job;
}

CompatJob* asJob(HANDLE handle) {
    if (!concord_compat::isRegisteredHandle(reinterpret_cast<void*>(handle), kJobKind)) {
        return nullptr;
    }
    return reinterpret_cast<CompatJob*>(handle);
}

}  // namespace

extern "C" {

HANDLE CreateEventW(SECURITY_ATTRIBUTES* /*lpEventAttributes*/,
                    BOOL bManualReset,
                    BOOL bInitialState,
                    LPCWSTR lpName) {
    const bool manualReset = bManualReset != FALSE;
    const bool initialState = bInitialState != FALSE;

    auto* event = new (std::nothrow) CompatEvent();
    if (event == nullptr) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    const bool anonymous = (lpName == nullptr || *lpName == L'\0');
    if (anonymous) {
        event->control = static_cast<EventControl*>(std::calloc(1, sizeof(EventControl)));
        if (event->control == nullptr
            || !initialiseControl(event->control, manualReset, initialState)) {
            std::free(event->control);
            delete event;
            concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        event->heapBacked = true;
    } else {
        event->shmName = sharedNameFor(lpName);
        int fd = ::shm_open(event->shmName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            if (::ftruncate(fd, static_cast<off_t>(sizeof(EventControl))) != 0) {
                const int saved = errno;
                ::close(fd);
                ::shm_unlink(event->shmName.c_str());
                delete event;
                concord_compat::setLastErrorFromErrno(saved);
                return nullptr;
            }
            void* mapped = ::mmap(nullptr, sizeof(EventControl),
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (mapped == MAP_FAILED) {
                ::shm_unlink(event->shmName.c_str());
                delete event;
                concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
                return nullptr;
            }
            event->control = static_cast<EventControl*>(mapped);
            if (!initialiseControl(event->control, manualReset, initialState)) {
                ::munmap(event->control, sizeof(EventControl));
                ::shm_unlink(event->shmName.c_str());
                delete event;
                concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
                return nullptr;
            }
            event->creator = true;
        } else if (errno == EEXIST) {
            // 同名对象已存在：Windows 上 CreateEventW 返回一个指向同一对象的新句柄。
            fd = ::shm_open(event->shmName.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                concord_compat::setLastErrorFromErrno(errno);
                delete event;
                return nullptr;
            }
            void* mapped = ::mmap(nullptr, sizeof(EventControl),
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (mapped == MAP_FAILED) {
                delete event;
                concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
                return nullptr;
            }
            event->control = static_cast<EventControl*>(mapped);
            if (!waitUntilReady(event->control)) {
                ::munmap(event->control, sizeof(EventControl));
                delete event;
                concord_compat::setLastError(ERROR_TIMEOUT);
                return nullptr;
            }
        } else {
            const int saved = errno;
            delete event;
            concord_compat::setLastErrorFromErrno(saved);
            return nullptr;
        }
    }

    if (!concord_compat::registerHandle(event, destroyEvent, kEventKind)) {
        destroyEvent(event);
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(event);
}

HANDLE OpenEventW(DWORD /*dwDesiredAccess*/, BOOL /*bInheritHandle*/, LPCWSTR lpName) {
    if (lpName == nullptr || *lpName == L'\0') {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto* event = new (std::nothrow) CompatEvent();
    if (event == nullptr) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    event->shmName = sharedNameFor(lpName);
    const int fd = ::shm_open(event->shmName.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        // 对象不存在时必须返回 nullptr。smoke 用它判"父进程还没建好"，
        // 返回一个假句柄会让它跳过重试、直接进到未初始化的握手里。
        concord_compat::setLastError(ERROR_FILE_NOT_FOUND);
        delete event;
        return nullptr;
    }
    void* mapped = ::mmap(nullptr, sizeof(EventControl),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        delete event;
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    event->control = static_cast<EventControl*>(mapped);
    if (!waitUntilReady(event->control)) {
        ::munmap(event->control, sizeof(EventControl));
        delete event;
        concord_compat::setLastError(ERROR_TIMEOUT);
        return nullptr;
    }
    if (!concord_compat::registerHandle(event, destroyEvent, kEventKind)) {
        destroyEvent(event);
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(event);
}

BOOL SetEvent(HANDLE hEvent) {
    CompatEvent* event = asEvent(hEvent);
    if (event == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    EventControl* control = event->control;
    pthread_mutex_lock(&control->mutex);
    control->signaled = 1;
    // manual-reset 要唤醒所有等待者；auto-reset 也广播，让其中一个抢到后清标志。
    pthread_cond_broadcast(&control->cond);
    pthread_mutex_unlock(&control->mutex);
    return TRUE;
}

BOOL ResetEvent(HANDLE hEvent) {
    CompatEvent* event = asEvent(hEvent);
    if (event == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    EventControl* control = event->control;
    pthread_mutex_lock(&control->mutex);
    control->signaled = 0;
    pthread_mutex_unlock(&control->mutex);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    CompatEvent* event = asEvent(hHandle);
    if (event == nullptr) {
        // process 句柄：等到子进程退出（这是 smoke 里 Join 的核心动作）。
        CompatProcess* process = asProcess(hHandle);
        if (process != nullptr) return waitForProcess(process, dwMilliseconds);
        // 其余类型（job / mapping 等）不冒充成功：返回 WAIT_OBJECT_0 会让
        // 调用方以为"已经 signaled"，从而跳过真正的握手。
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    EventControl* control = event->control;
    const bool infinite = (dwMilliseconds == INFINITE);
    timespec deadline{};
    if (!infinite) deadline = deadlineAfter(dwMilliseconds);

    pthread_mutex_lock(&control->mutex);
    while (!isSignaledLocked(control)) {
        if (infinite) {
            pthread_cond_wait(&control->cond, &control->mutex);
            continue;
        }
        const int rc = pthread_cond_timedwait(&control->cond, &control->mutex, &deadline);
        if (rc == ETIMEDOUT && !isSignaledLocked(control)) {
            pthread_mutex_unlock(&control->mutex);
            return WAIT_TIMEOUT;
        }
    }
    // auto-reset：被唤醒的这一个把标志消费掉；manual-reset 保持。
    if (control->manualReset == 0) control->signaled = 0;
    pthread_mutex_unlock(&control->mutex);
    return WAIT_OBJECT_0;
}

DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll,
                             DWORD dwMilliseconds) {
    if (nCount == 0 || lpHandles == nullptr || nCount > 64) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    // 数组里可以混着 event 与 process —— smoke 的 Join 就是"就绪事件 + 子进程
    // 句柄 + 完成事件"一起等（Windows 上这是标准做法）。只认 event 会让它拿到
    // WAIT_FAILED，进而 joined=false，整个 lifecycle 用例红。这才是 S8b 最后一
    // 个真正的语义缺口，而不是"少一个符号"。
    struct Entry {
        CompatEvent* event = nullptr;
        CompatProcess* process = nullptr;
    };
    std::vector<Entry> entries;
    entries.reserve(nCount);
    for (DWORD index = 0; index < nCount; ++index) {
        if (CompatEvent* event = asEvent(lpHandles[index])) {
            entries.push_back(Entry{event, nullptr});
            continue;
        }
        if (CompatProcess* process = asProcess(lpHandles[index])) {
            entries.push_back(Entry{nullptr, process});
            continue;
        }
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    const bool waitAll = bWaitAll != FALSE;
    const bool infinite = (dwMilliseconds == INFINITE);
    const auto started = std::chrono::steady_clock::now();

    for (;;) {
        DWORD signaledCount = 0;
        DWORD firstSignaled = 0;
        bool firstSet = false;
        for (DWORD index = 0; index < nCount; ++index) {
            bool signaled = false;
            if (entries[index].event != nullptr) {
                EventControl* control = entries[index].event->control;
                pthread_mutex_lock(&control->mutex);
                signaled = isSignaledLocked(control);
                pthread_mutex_unlock(&control->mutex);
            } else {
                // 进程句柄：非阻塞收尸，已退出即视为 signaled（Windows 语义上
                // 进程句柄在退出时变 signaled 并**保持** signaled）。
                reapIfExited(entries[index].process);
                signaled = entries[index].process->reaped;
            }
            if (signaled) {
                ++signaledCount;
                if (!firstSet) { firstSignaled = index; firstSet = true; }
            }
        }

        const bool satisfied = waitAll ? (signaledCount == nCount) : (signaledCount > 0);
        if (satisfied) {
            // 只消费 auto-reset event：process 的 signaled 状态不可消费，
            // manual-reset event 也要保持到 ResetEvent。
            const auto consume = [&](DWORD index) {
                if (entries[index].event == nullptr) return;
                EventControl* control = entries[index].event->control;
                pthread_mutex_lock(&control->mutex);
                if (control->manualReset == 0) control->signaled = 0;
                pthread_mutex_unlock(&control->mutex);
            };
            if (waitAll) {
                for (DWORD index = 0; index < nCount; ++index) consume(index);
                return WAIT_OBJECT_0;
            }
            consume(firstSignaled);
            return WAIT_OBJECT_0 + firstSignaled;
        }

        if (!infinite) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsed >= static_cast<long long>(dwMilliseconds)) return WAIT_TIMEOUT;
        }
        // 轮询而不是挂 cond：WAIT_ALL 要同时满足多个条件，单条件变量的唤醒
        // 表达不了"另一个也变了"。1ms 间隔对 smoke 的配合窗口足够。
        ::usleep(1000);
    }
}

HANDLE CreateFileMappingW(HANDLE hFile, SECURITY_ATTRIBUTES* /*lpAttributes*/,
                          DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
                          LPCWSTR lpName) {
    // 只支持 INVALID_HANDLE_VALUE（页面文件后备）。真实文件映射还要处理"文件长度
    // 为 0 时按 (SizeHigh,SizeLow) 扩展文件"，smoke 用不到，先诚实拒绝。
    if (hFile != INVALID_HANDLE_VALUE) {
        concord_compat::setLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(dwMaximumSizeHigh) << 32)
        | static_cast<std::uint64_t>(dwMaximumSizeLow);
    if (size == 0) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const int prot = protectionForFlProtect(flProtect);
    if (prot < 0) {
        concord_compat::setLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    auto* mapping = new (std::nothrow) CompatMapping();
    if (mapping == nullptr) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    mapping->size = static_cast<std::size_t>(size);
    mapping->prot = prot;

    const bool anonymous = (lpName == nullptr || *lpName == L'\0');
    bool alreadyExists = false;
    int fd = -1;
    if (anonymous) {
        mapping->shmName = anonymousShmName();
        fd = ::shm_open(mapping->shmName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            const int saved = errno;
            delete mapping;
            concord_compat::setLastErrorFromErrno(saved);
            return nullptr;
        }
        ::shm_unlink(mapping->shmName.c_str());  // 名字立刻消失，fd 仍然有效
        mapping->anonymous = true;
    } else {
        mapping->shmName = sharedNameFor(lpName);
        fd = ::shm_open(mapping->shmName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0 && errno == EEXIST) {
            // 已存在：返回指向同一对象的句柄，**并设 ERROR_ALREADY_EXISTS**。
            // smoke:741 正是用 `GetLastError() == ERROR_ALREADY_EXISTS` 判
            // "上一次运行的映射没清掉"，丢掉这个错误码它会重用脏数据。
            fd = ::shm_open(mapping->shmName.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                const int saved = errno;
                delete mapping;
                concord_compat::setLastErrorFromErrno(saved);
                return nullptr;
            }
            alreadyExists = true;
        } else if (fd < 0) {
            const int saved = errno;
            delete mapping;
            concord_compat::setLastErrorFromErrno(saved);
            return nullptr;
        } else {
            mapping->creator = true;
        }
    }

    if (mapping->creator || mapping->anonymous) {
        // 只在**新建**时 ftruncate。macOS 的 POSIX shm 对已存在对象再次
        // ftruncate（哪怕同尺寸）返回 EINVAL —— 实测：
        //   shm_open(O_CREAT|O_EXCL) + ftruncate(1024) → 0
        //   shm_open(O_RDWR)          + ftruncate(1024) → -1 / EINVAL
        // 若照 Linux 写法无条件 ftruncate，第二次 CreateFileMappingW 必失败，
        // 而 smoke:741 正是靠"第二次创建成功且报 ALREADY_EXISTS"判重。
        if (::ftruncate(fd, static_cast<off_t>(mapping->size)) != 0) {
            const int saved = errno;
            ::close(fd);
            if (mapping->creator) ::shm_unlink(mapping->shmName.c_str());
            delete mapping;
            concord_compat::setLastErrorFromErrno(saved);
            return nullptr;
        }
    } else {
        // 已存在：大小由创建者决定，请求值被忽略（Windows 同语义），
        // 所以用 fstat 取真实大小而不是沿用调用方传的 SizeLow。
        struct stat existing {};
        if (::fstat(fd, &existing) != 0 || existing.st_size <= 0) {
            ::close(fd);
            delete mapping;
            concord_compat::setLastError(ERROR_INVALID_HANDLE);
            return nullptr;
        }
        mapping->size = static_cast<std::size_t>(existing.st_size);
    }
    mapping->fd = fd;
    if (!concord_compat::registerHandle(mapping, destroyMapping, kMappingKind)) {
        destroyMapping(mapping);
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    // 这是"成功路径写 LastError"的**唯一例外**，而且是必须的：lastError 是线程
    // 局部的，上一条残留的 183 会让 smoke 把全新映射误判成"已存在"。Windows 的
    // 约定是"新建成功 → 不设 ERROR_ALREADY_EXISTS"，所以这里显式清零。
    concord_compat::setLastError(alreadyExists ? ERROR_ALREADY_EXISTS : 0);
    return reinterpret_cast<HANDLE>(mapping);
}

HANDLE OpenFileMappingW(DWORD dwDesiredAccess, BOOL /*bInheritHandle*/, LPCWSTR lpName) {
    if (lpName == nullptr || *lpName == L'\0') {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const int prot = protectionForViewAccess(dwDesiredAccess);
    if (prot < 0) {
        concord_compat::setLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    auto* mapping = new (std::nothrow) CompatMapping();
    if (mapping == nullptr) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    mapping->shmName = sharedNameFor(lpName);
    const int fd = ::shm_open(mapping->shmName.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        concord_compat::setLastError(ERROR_FILE_NOT_FOUND);
        delete mapping;
        return nullptr;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
        ::close(fd);
        delete mapping;
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    mapping->size = static_cast<std::size_t>(info.st_size);
    mapping->prot = prot;
    mapping->fd = fd;
    if (!concord_compat::registerHandle(mapping, destroyMapping, kMappingKind)) {
        destroyMapping(mapping);
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(mapping);
}

void* MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow,
                    SIZE_T dwNumberOfBytesToMap) {
    CompatMapping* mapping = asMapping(hFileMappingObject);
    if (mapping == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    const int viewProt = protectionForViewAccess(dwDesiredAccess);
    if (viewProt < 0) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // 请求写而 mapping 是只读时必须失败：否则拿到一个 PROT_READ 的地址去写，
    // 进程直接 SIGSEGV，调用点只会看到"数据没写进去"。
    if ((viewProt & PROT_WRITE) != 0 && (mapping->prot & PROT_WRITE) == 0) {
        concord_compat::setLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const std::uint64_t offset = (static_cast<std::uint64_t>(dwFileOffsetHigh) << 32)
        | static_cast<std::uint64_t>(dwFileOffsetLow);
    if (offset >= mapping->size) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const std::size_t available = mapping->size - static_cast<std::size_t>(offset);
    const std::size_t length = (dwNumberOfBytesToMap == 0) ? available
        : static_cast<std::size_t>(dwNumberOfBytesToMap);
    if (length > available) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // mmap 要求偏移页对齐，而 Windows 允许任意字节偏移。所以往后取整到页边，
    // 把多出来的那段从返回地址上加回去 —— 这就是 delta。
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        concord_compat::setLastError(ERROR_GEN_FAILURE);
        return nullptr;
    }
    const std::size_t pageMask = static_cast<std::size_t>(pageSize) - 1u;
    const std::size_t delta = static_cast<std::size_t>(offset) & pageMask;
    const std::size_t alignedOffset = static_cast<std::size_t>(offset) - delta;
    const std::size_t mapLength = length + delta;

    void* raw = ::mmap(nullptr, mapLength, viewProt, MAP_SHARED, mapping->fd,
                       static_cast<off_t>(alignedOffset));
    if (raw == MAP_FAILED) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    void* base = static_cast<char*>(raw) + delta;
    {
        std::lock_guard<std::mutex> lock(viewMutex());
        viewRecords()[base] = ViewRecord{raw, mapLength};
    }
    if (!concord_compat::registerHandle(base, destroyView, kViewKind)) {
        {
            std::lock_guard<std::mutex> lock(viewMutex());
            viewRecords().erase(base);
        }
        ::munmap(raw, mapLength);
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return base;
}

BOOL UnmapViewOfFile(const void* lpBaseAddress) {
    // 参数是视图基址（不是 HANDLE）。句柄表只比较指针值、不解引用，
    // 所以野指针在这里是安全的：查不到就返回 FALSE，不会崩。
    if (lpBaseAddress == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!concord_compat::isRegisteredHandle(const_cast<void*>(lpBaseAddress), kViewKind)) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    concord_compat::closeRegisteredHandle(const_cast<void*>(lpBaseAddress));
    return TRUE;
}


BOOL CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                    SECURITY_ATTRIBUTES* /*lpProcessAttributes*/,
                    SECURITY_ATTRIBUTES* /*lpThreadAttributes*/,
                    BOOL /*bInheritHandles*/, DWORD /*dwCreationFlags*/,
                    LPVOID /*lpEnvironment*/, LPCWSTR /*lpCurrentDirectory*/,
                    STARTUPINFOW* /*lpStartupInfo*/, PROCESS_INFORMATION* lpProcessInformation) {
    if (lpProcessInformation == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *lpProcessInformation = PROCESS_INFORMATION{};

    std::vector<std::string> arguments = splitCommandLine(lpCommandLine);
    std::string executable;
    if (lpApplicationName != nullptr && *lpApplicationName != L'\0') {
        executable = concord_compat::utf8FromWideText(lpApplicationName);
    } else if (!arguments.empty()) {
        executable = arguments.front();  // 没给 ApplicationName 时用命令行首个 token
    }
    if (executable.empty()) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (arguments.empty()) arguments.push_back(executable);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    extern char** environ;
    pid_t pid = -1;
    const int spawnResult = posix_spawn(&pid, executable.c_str(), &actions, &attributes,
                                        argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (spawnResult != 0) {
        // posix_spawn 返回的是 errno 值本身，不是 -1。直接映射能让
        // smoke 打印出"为什么起不来"而不是一个笼统的失败。
        concord_compat::setLastErrorFromErrno(spawnResult);
        return FALSE;
    }

    auto* process = new (std::nothrow) CompatProcess();
    auto* thread = new (std::nothrow) CompatThread();
    if (process == nullptr || thread == nullptr) {
        delete process;
        delete thread;
        ::kill(pid, SIGKILL);
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    process->pid = pid;
    if (!concord_compat::registerHandle(process, destroyProcess, kProcessKind)
        || !concord_compat::registerHandle(thread, destroyThread, kThreadKind)) {
        destroyProcess(process);
        destroyThread(thread);
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    lpProcessInformation->hProcess = reinterpret_cast<HANDLE>(process);
    lpProcessInformation->hThread = reinterpret_cast<HANDLE>(thread);
    lpProcessInformation->dwProcessId = static_cast<DWORD>(pid);
    lpProcessInformation->dwThreadId = 0;  // macOS 不暴露"主线程 id"这个概念
    return TRUE;
}

BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode) {
    CompatProcess* process = asProcess(hProcess);
    if (process == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (process->reaped) {
        // 已经退出的进程不能再被杀。返回 FALSE 而不是假装成功，
        // 否则调用方会以为"我杀了它"，而它其实早就自己结束了。
        concord_compat::setLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (::kill(process->pid, SIGKILL) != 0 && errno != ESRCH) {
        concord_compat::setLastErrorFromErrno(errno);
        return FALSE;
    }
    // Windows 语义：TerminateProcess 指定的 code 就是最终退出码。
    // 先记下来，后面 waitpid 拿到的 137(128+SIGKILL) 不能覆盖它。
    process->exitCode = static_cast<DWORD>(uExitCode);
    return TRUE;
}

BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode) {
    CompatProcess* process = asProcess(hProcess);
    if (process == nullptr || lpExitCode == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    reapIfExited(process);
    *lpExitCode = process->reaped ? process->exitCode : static_cast<DWORD>(STILL_ACTIVE);
    return TRUE;
}

DWORD GetModuleFileNameW(HMODULE /*hModule*/, LPWSTR lpFilename, DWORD nSize) {
    if (lpFilename == nullptr || nSize == 0) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::uint32_t required = 0;
    ::_NSGetExecutablePath(nullptr, &required);  // 先问长度
    std::vector<char> raw(required + 1, '\0');
    std::uint32_t capacity = static_cast<std::uint32_t>(raw.size());
    if (::_NSGetExecutablePath(raw.data(), &capacity) != 0) {
        concord_compat::setLastError(ERROR_GEN_FAILURE);
        return 0;
    }
    // _NSGetExecutablePath 可能给出含 `..` 或符号链接的相对路径，
    // 而 smoke 要拿它去 CreateProcessW —— 必须 realpath 成绝对真实路径。
    char resolved[PATH_MAX];
    if (::realpath(raw.data(), resolved) == nullptr) {
        concord_compat::setLastErrorFromErrno(errno);
        return 0;
    }
    const std::wstring wide = concord_compat::wideTextFromUtf8(resolved);
    if (wide.size() >= static_cast<std::size_t>(nSize)) {
        // Windows 契约：缓冲区不够时**截断写入**并返回 nSize，
        // 同时设 ERROR_INSUFFICIENT_BUFFER。smoke 正是用 `size >= buffer.size()`
        // 判失败，所以这里不能悄悄返回 true 的长度。
        const std::size_t copied = static_cast<std::size_t>(nSize) - 1;
        std::memcpy(lpFilename, wide.data(), copied * sizeof(wchar_t));
        lpFilename[copied] = L'\0';
        concord_compat::setLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }
    std::memcpy(lpFilename, wide.data(), wide.size() * sizeof(wchar_t));
    lpFilename[wide.size()] = L'\0';
    return static_cast<DWORD>(wide.size());
}

HANDLE CreateJobObjectW(SECURITY_ATTRIBUTES* /*lpJobAttributes*/, LPCWSTR /*lpName*/) {
    auto* job = new (std::nothrow) CompatJob();
    if (job == nullptr) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    if (!concord_compat::registerHandle(job, destroyJob, kJobKind)) {
        delete job;
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(job);
}

BOOL AssignProcessToJobObject(HANDLE hJob, HANDLE hProcess) {
    CompatJob* job = asJob(hJob);
    if (job == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    CompatProcess* process = asProcess(hProcess);
    if (process == nullptr || process->pid <= 0) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // 已在表里就不要再加：重复加入在 Windows 上也是成功的，但会在
    // TerminateJobObject 时对同一个 pid 发两次信号。
    for (const pid_t member : job->members) {
        if (member == process->pid) return TRUE;
    }
    job->members.push_back(process->pid);
    return TRUE;
}

BOOL SetInformationJobObject(HANDLE hJob, JOBOBJECTINFOCLASS JobObjectInformationClass,
                             LPVOID lpJobObjectInformation, DWORD cbJobObjectInformationLength) {
    CompatJob* job = asJob(hJob);
    if (job == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (JobObjectInformationClass != JobObjectExtendedLimitInformation) {
        // 其余信息类（基本记账、UI 限制、安全限制……）没有对位。
        // 返回 FALSE + NOT_SUPPORTED，而不是"收下但忽略"——后者会让调用方
        // 以为自己设上的限制真的生效了。
        concord_compat::setLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (lpJobObjectInformation == nullptr
        || cbJobObjectInformationLength < sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const auto* limits = static_cast<const JOBOBJECT_EXTENDED_LIMIT_INFORMATION*>(lpJobObjectInformation);
    job->limitFlags = limits->BasicLimitInformation.LimitFlags;
    return TRUE;
}

BOOL TerminateJobObject(HANDLE hJob, UINT uExitCode) {
    CompatJob* job = asJob(hJob);
    if (job == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    for (const pid_t member : job->members) {
        if (member > 0) ::kill(member, SIGKILL);
    }
    // 成员 pid 保留在表里：CloseHandle 时若还带着 KILL_ON_JOB_CLOSE 会再发一次
    // SIGKILL，对已退出的 pid 是 ESRCH，无害；而提前清表会让"杀完再关"路径
    // 悄悄失去兜底。
    (void)uExitCode;  // Windows 会把它写进成员退出码；smoke 丢弃返回值，不做假承诺
    return TRUE;
}

}  // extern "C"

// strncpy_s 在 windows.h 里是 **C++ linkage**（定义在 extern "C" 块之外），
// 所以这里也不能套 extern "C"——套了就是两个不同的符号，链接照样失败。
//
// 语义必须按 Secure-CRT 来，不能按 BSD strlcpy：
//   · count == _TRUNCATE：截断到 destSize-1，返回 STRUNCATE(80)，**不清空目标**
//   · count 是显式容量且 src 更长：目标置空并返回 ERANGE（这是"报错"路径）
//   · 成功：返回 0
// smoke:1050 的 `host.message[0] != '\0'` 断言直接依赖第一条——若按"清空目标"
// 实现，它在截断场景下会读到空串，断言红，而根因看起来像是"消息没写进去"。
errno_t strncpy_s(char* dest, std::size_t destSize, const char* src, std::size_t count) {
    if (dest == nullptr || destSize == 0) return EINVAL;
    if (src == nullptr) {
        dest[0] = '\0';
        return EINVAL;
    }
    if (count == static_cast<std::size_t>(-1)) {  // _TRUNCATE
        std::size_t length = std::strlen(src);
        if (length >= destSize) {
            std::memcpy(dest, src, destSize - 1);
            dest[destSize - 1] = '\0';
            return 80;  // STRUNCATE：截断但不报错
        }
        std::memcpy(dest, src, length);
        dest[length] = '\0';
        return 0;
    }
    std::size_t length = 0;
    while (length < count && src[length] != '\0') ++length;
    if (length >= destSize) {
        dest[0] = '\0';
        return ERANGE;
    }
    std::memcpy(dest, src, length);
    dest[length] = '\0';
    return 0;
}
