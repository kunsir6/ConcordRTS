// ============================================================================
// compat/win32/src/RuntimeWin32CompatAtomic.cpp
//
// S8：Interlocked 族的**真实现**。
//
// 为什么这族不能只声明：两进程 smoke 用它们轮询共享内存里的状态字
// （`InterlockedCompareExchange(&shared->status, 0, 0)` 这种"原子读"），
// 在多核上这是唯一的可见性保证；声明一个空实现或者把它映射成普通读写，
// 表现是"偶发读到旧值"——在本机（x86_64，强内存模型）几乎测不出来，
// 换到 ARM 就是稳定复现的错，而且错得没有规律。
//
// 为什么是这四条签名（LONG 而不是 long）：`LONG` 在本项目里是 **int32_t**（T-01 的结论），
// 而 macOS 的 `long` 是 64 位。探针里的字段是 `volatile LONG`，写成 `volatile long*`
// 根本不匹配；真要用 reinterpret_cast 硬转过去，就是按 8 字节读写一个 4 字节字段，
// 把相邻字段踩掉。所以签名必须钉在 32 位整数上——Windows 上 LONG==long，等价。
//
// 内存序：Win32 的 Interlocked 是**完全屏障**（full barrier），这里一律用
// __ATOMIC_SEQ_CST，不用 acquire/release 去"优化"——省下来的那点性能换来的是
// "在某些 CPU 上偶尔失效"，而这层没有能力验证那种失效。
// ============================================================================

#include <windows.h>

#include <cstddef>

namespace {

// 空指针一律当作"没有可交换的目标"：返回 0 / 不写。真实 API 会直接崩（访问违例），
// 但在这层里崩溃会把"调用点传了空指针"这个真正的 bug 伪装成交付物的问题。
bool isNull(const volatile void* pointer) { return pointer == nullptr; }

} // namespace

LONG InterlockedIncrement(volatile LONG* lpAddend) {
    if (isNull(lpAddend)) return 0;
    return __atomic_add_fetch(lpAddend, 1, __ATOMIC_SEQ_CST);
}

LONG InterlockedDecrement(volatile LONG* lpAddend) {
    if (isNull(lpAddend)) return 0;
    return __atomic_sub_fetch(lpAddend, 1, __ATOMIC_SEQ_CST);
}

LONG InterlockedExchange(volatile LONG* Target, LONG Value) {
    if (isNull(Target)) return 0;
    return __atomic_exchange_n(Target, Value, __ATOMIC_SEQ_CST);
}

LONG InterlockedCompareExchange(volatile LONG* Destination, LONG Exchange, LONG Comperand) {
    if (isNull(Destination)) return 0;
    // 返回**原始值**：成功时它就是 Comperand，失败时 __atomic 会把当前值写回 expected。
    // 这两条合起来正是 Win32 的契约（调用点用 `== Comperand` 判断"是我改的"）。
    LONG expected = Comperand;
    __atomic_compare_exchange_n(Destination, &expected, Exchange, false, __ATOMIC_SEQ_CST,
                                __ATOMIC_SEQ_CST);
    return expected;
}

void* InterlockedExchangePointer(void* volatile* Target, void* Value) {
    if (isNull(Target)) return nullptr;
    return __atomic_exchange_n(Target, Value, __ATOMIC_SEQ_CST);
}

void* InterlockedCompareExchangePointer(void* volatile* Destination, void* Exchange,
                                        void* Comperand) {
    if (isNull(Destination)) return nullptr;
    void* expected = Comperand;
    __atomic_compare_exchange_n(Destination, &expected, Exchange, false, __ATOMIC_SEQ_CST,
                                __ATOMIC_SEQ_CST);
    return expected;
}
