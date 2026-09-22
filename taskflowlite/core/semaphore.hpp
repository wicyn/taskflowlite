/// @file semaphore.hpp
/// @brief 任务级信号量 Semaphore —— 通过配额控制任务并发度且不阻塞 Worker 线程。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-09-18
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "exception.hpp"
#include "macros.hpp"
#include "spin_mutex.hpp"
#include "utility.hpp"
#include "forward.hpp"

namespace tfl {

/// @brief 以可用配额限制任务并发度，而不阻塞执行任务的 Worker 线程。
///
/// Semaphore 维护最大配额 `m_max_value`、当前可用配额 `m_value` 以及等待任务链。
/// Work 在执行前由统一的多 Semaphore acquire 协议锁定全部目标 Semaphore，
/// 在所有请求都能够同时满足时一次性扣减配额；任一请求不满足时不修改任何配额，
/// 并将当前 Work 加入对应 Semaphore 的 waiter 链。
///
/// 配额释放时不会直接执行等待任务，而是整体摘除当前 waiter 链并交由 Executor
/// 重新发布。被唤醒的 Work 仍需要重新竞争其配置的全部 acquire 请求，因此唤醒
/// 只表示重新获得竞争机会，并不表示已经取得所需配额。
///
/// Semaphore 只保存等待任务的非拥有 Work 指针，不负责这些 Work 的生命周期。
/// 等待链复用 Work 的 `m_next`，因此等待中的 Work 不得同时属于其他使用
/// `m_next` 的 intrusive 链。
///
/// @note `m_value` 表示当前可用配额，而不是当前正在运行的任务数量。
/// @note 单个 Semaphore 的配额和 waiter 链访问均由 `m_lock` 保护。
/// @note 多 Semaphore acquire 由 Work 按全局固定顺序同时持锁，避免循环等待。
/// @note waiter 按进入 Semaphore 的顺序连接，单个 Semaphore 内保持 FIFO 链接顺序。
/// @warning 所有引用本对象的任务完成并归还其已获取配额之前，本对象必须保持存活。
class Semaphore : public Immovable<Semaphore> {
    friend class Work;
    friend class Executor;
    friend class Task;

public:
    /// @brief 以指定最大容量构造信号量，并使全部配额初始可用。
    /// @param max_value 最大可用配额，同时作为初始可用配额。
    /// @param name 用于诊断、调试和可视化的可选名称。
    explicit Semaphore(std::size_t max_value, std::string name = "");

    /// @brief 分别指定最大配额和初始可用配额构造信号量。
    /// @param max_value 最大可用配额。
    /// @param current_value 初始可用配额；超过 @p max_value 时自动裁剪为最大值。
    /// @param name 用于诊断、调试和可视化的可选名称。
    Semaphore(std::size_t max_value, std::size_t current_value, std::string name = "");

    /// @brief 获取当前可用配额的线程安全快照。
    /// @return 当前观察到的可用配额数量。
    /// @note 返回值离开本函数后可能立即因其他任务获取或释放配额而失效。
    [[nodiscard]] std::size_t value() const noexcept;

    /// @brief 获取当前最大配额的线程安全快照。
    /// @return 当前配置的最大可用配额。
    /// @note 返回值离开本函数后可能立即因 reset 修改配置而失效。
    [[nodiscard]] std::size_t max_value() const noexcept;

    /// @brief 重置信号量最大配额，并使全部新配额恢复为可用状态。
    /// @param max_value 新的最大配额，同时成为新的当前可用配额。
    /// @pre 不得存在尚未归还的已占用配额，也不得与其他配置操作并发调用。
    /// @throws Exception 当前存在等待任务时抛出异常。
    void reset(std::size_t max_value);

    /// @brief 重置信号量最大配额和当前可用配额。
    /// @param max_value 新的最大配额。
    /// @param current_value 新的当前可用配额；超过 @p max_value 时自动裁剪为最大值。
    /// @pre 不得存在尚未归还的已占用配额，也不得与其他配置操作并发调用。
    /// @throws Exception 当前存在等待任务时抛出异常。
    void reset(std::size_t max_value, std::size_t current_value);

    /// @brief 获取信号量名称。
    /// @return 指向内部名称存储的字符串视图。
    /// @note 返回视图在下次修改名称或销毁本对象之前有效。
    /// @note 本函数不加锁，不得与名称修改操作并发调用。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 设置信号量名称。
    /// @tparam S 可用于构造 `std::string` 的名称类型。
    /// @param name 新名称。
    /// @return `*this`，用于链式配置。
    /// @note 名称不参与任务调度和配额同步，仅用于诊断或可视化。
    /// @note 本函数不加锁，不得与其他名称读取或修改操作并发调用。
    template <typename S>
        requires std::constructible_from<std::string, S&&>
    Semaphore& name(S&& name);

private:

    /// @brief 归还指定配额，并将当前全部 waiter 合并到调用方待调度链。
    ///
    /// 在当前 Semaphore 的内部锁保护下增加 `m_value`，随后整体摘除当前 waiter
    /// intrusive 链，并通过 O(1) 指针操作追加到 `[out_first, out_last]`。
    ///
    /// 被摘出的 Work 并未直接取得配额，只获得重新执行 acquire 流程的机会；
    /// 后续仍由 Work::_try_acquire_semaphores() 重新竞争全部 acquire 请求。
    ///
    /// @param out_first 调用方待调度链首节点；空链时为 nullptr。
    /// @param out_last 调用方待调度链尾节点；空链时为 nullptr。
    /// @param count 本次归还的配额数量。
    /// @pre out_first 和 out_last 必须同时为空或同时非空。
    /// @pre count > 0。
    /// @pre count <= m_max_value - m_value。
    /// @note 一次 release 会摘出当前全部 waiter，而不是只摘出当前配额能够满足的任务。
    TFL_FORCE_INLINE void _release(Work*& out_first, Work*& out_last, std::size_t count) noexcept;

    std::string m_name;
    mutable SpinMutex m_lock;
    std::size_t m_max_value{0};
    std::size_t m_value{0};
    Work* m_waiter_head{nullptr};
    Work* m_waiter_tail{nullptr};
};


// ============================================================================
// 实现
// ============================================================================

inline Semaphore::Semaphore(std::size_t max_value, std::string name)
    : m_name{std::move(name)}
    , m_max_value{max_value}
    , m_value{max_value} {
}

inline Semaphore::Semaphore(std::size_t max_value, std::size_t current_value, std::string name)
    : m_name{std::move(name)}
    , m_max_value{max_value}
    , m_value{(std::min)(current_value, max_value)} {
}

inline std::size_t Semaphore::value() const noexcept {
    std::lock_guard lock{m_lock};
    return m_value;
}

inline std::size_t Semaphore::max_value() const noexcept {
    std::lock_guard lock{m_lock};
    return m_max_value;
}

inline void Semaphore::reset(std::size_t max_value) {
    std::lock_guard lock{m_lock};

    if (m_waiter_head) {
        throw Exception("cannot reset semaphore while waiters exist.");
    }

    TFL_ASSERT(m_waiter_tail == nullptr);

    m_max_value = max_value;
    m_value = max_value;
}

inline void Semaphore::reset(std::size_t max_value, std::size_t current_value) {
    std::lock_guard lock{m_lock};

    if (m_waiter_head) {
        throw Exception("cannot reset semaphore while waiters exist.");
    }

    TFL_ASSERT(m_waiter_tail == nullptr);

    m_max_value = max_value;
    m_value = (std::min)(current_value, max_value);
}

inline std::string_view Semaphore::name() const noexcept {
    return m_name;
}

template <typename S>
    requires std::constructible_from<std::string, S&&>
inline Semaphore& Semaphore::name(S&& name) {
    m_name = std::forward<S>(name);
    return *this;
}

}  // namespace tfl

