/// @file semaphore.hpp
/// @brief 任务级信号量 Semaphore —— 通过配额控制任务并发度且不阻塞 Worker 线程。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <vector>
#include <algorithm>
#include <cassert>
#include <mutex>
#include <string>
#include <string_view>
#include <functional>

#include "small_vector.hpp"
#include "exception.hpp"

namespace tfl {

/// @brief 以可用配额限制任务并发度，而不阻塞执行任务的 Worker 线程。
///
/// Semaphore 维护最大配额 `m_max_value`、当前可用配额 `m_value` 以及等待任务队列。
/// 任务获取配额成功后继续执行；配额不足时，当前 `Work` 被登记到等待队列并停止本次
/// 执行尝试，使 Worker 可以继续执行其他就绪任务，而不是阻塞操作系统线程。
///
/// 配额释放时不会直接在 Semaphore 内执行等待任务，而是将当前等待者批量移出并交给
/// Executor 重新调度。任务被再次调度后会重新调用获取流程，因此一次 release 唤醒的
/// waiter 并不意味着已经获得配额，只表示它获得了重新竞争配额的机会。
///
/// Semaphore 只保存等待任务的非拥有 `Work*`，不负责这些 Work 的生命周期。
///
/// @note `m_value` 表示“当前可用配额”，而不是当前正在运行的任务数量；
///       已占用配额数量在通常状态下等于 `m_max_value - m_value`。
/// @note 运行期配额获取、释放以及等待队列修改均由 `m_lock` 串行化。
/// @note `max_value()`、名称访问以及 `reset()` 的并发使用限制由各接口契约约束。
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

    /// @brief 信号量包含互斥同步状态，因此禁止复制构造。
    Semaphore(const Semaphore&) = delete;

    /// @brief 禁止复制赋值。
    Semaphore& operator=(const Semaphore&) = delete;

    /// @brief 获取当前可用配额的线程安全快照。
    /// @return 在持有内部互斥锁期间观察到的当前可用配额。
    /// @note 返回值离开本函数后可能立即因其他任务获取或释放配额而失效。
    [[nodiscard]] std::size_t value() const noexcept;

    /// @brief 获取配置的最大配额。
    /// @return 当前最大可用配额。
    /// @note 本函数不获取 `m_lock`，因此不得与 `reset()` 并发调用。
    [[nodiscard]] std::size_t max_value() const noexcept;

    /// @brief 重置信号量最大配额，并使全部新配额恢复为可用状态。
    /// @param max_value 新的最大配额，同时成为新的当前可用配额。
    /// @pre 不得存在等待任务或尚未归还的已占用配额，也不得与任务获取、释放或其他配置操作并发调用。
    /// @throws Exception 当前内部等待队列非空时抛出异常。
    /// @note 本实现只显式检测等待队列是否为空；其他前置条件由调用方保证。
    void reset(std::size_t max_value);

    /// @brief 重置信号量最大配额和当前可用配额。
    /// @param max_value 新的最大配额。
    /// @param current_value 新的当前可用配额；超过 @p max_value 时自动裁剪为最大值。
    /// @pre 不得存在等待任务或尚未归还的已占用配额，也不得与任务获取、释放或其他配置操作并发调用。
    /// @throws Exception 当前内部等待队列非空时抛出异常。
    /// @note 本实现只显式检测等待队列是否为空；其他前置条件由调用方保证。
    void reset(std::size_t max_value, std::size_t current_value);

    /// @brief 获取信号量名称。
    /// @return 指向内部名称存储的字符串视图。
    /// @note 返回视图在下次修改名称或销毁本对象之前有效。
    /// @note 本函数不加锁，不得与 `name(S&&)` 或其他可能修改名称的操作并发调用。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 设置信号量名称。
    /// @tparam S 可用于构造 `std::string` 的名称类型。
    /// @param name 新名称。
    /// @return `*this`，用于链式配置。
    /// @note 名称不参与任务调度和配额同步，仅用于诊断或可视化。
    /// @note 本函数不加锁，不得与其他名称读取或修改操作并发调用。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Semaphore& name(S&& name);

private:
    std::string m_name;
    /// @brief 串行保护 `m_value` 和 `m_waiters` 的运行期访问。
    mutable std::mutex m_lock;
    /// @brief 信号量允许持有的最大配额总量。
    std::size_t m_max_value{0};
    /// @brief 当前尚未被任务占用的可用配额数量。
    std::size_t m_value{0};
    /// @brief 因配额不足而等待重新调度并再次尝试获取的非拥有 Work 指针集合。
    SmallVector<Work*> m_waiters;


    /// @brief 尝试为指定任务一次性获取 @p count 个配额。
    ///
    /// 配额充足时在持锁状态下直接从 `m_value` 扣除请求数量并返回 true。
    /// 配额不足时不阻塞当前 Worker，而是把 @p w 登记到 `m_waiters`，由后续
    /// `_release()` 将其交回 Executor 重新调度并再次尝试获取。
    ///
    /// @param w 发起获取请求的任务；Semaphore 不取得其所有权。
    /// @param count 本次需要原子获取的配额数量。
    /// @return 获取全部请求配额时返回 true；配额不足并进入等待队列时返回 false。
    /// @note 本接口采用 all-or-nothing 语义，不会部分扣减配额。
    [[nodiscard]] bool _try_acquire(Work* w, std::size_t count);

    /// @brief 归还配额，并收集需要重新参与调度竞争的等待任务。
    ///
    /// 首先在 `m_lock` 保护下归还最多 @p count 个配额，并将当前全部等待者
    /// 从 `m_waiters` 批量移出。随后在锁外把这些 Work 合并到 @p out，
    /// 由 Executor 负责重新发布；被重新调度的任务仍需要再次执行获取操作。
    ///
    /// @param count 本次归还的配额数量；内部计数最多恢复到 `m_max_value`。
    /// @param out 接收需要由 Executor 重新调度的非拥有 Work 指针。
    /// @note 一次释放会取出当前全部 waiter，而不是只选择当前配额能够满足的任务。
    void _release(std::size_t count, SmallVector<Work*>& out);
};


// ============================================================================
// 实现
// ============================================================================

inline Semaphore::Semaphore(std::size_t max_value, std::string name)
    : m_name{std::move(name)}
    , m_max_value{max_value}
    , m_value{max_value} {}

inline Semaphore::Semaphore(std::size_t max_value, std::size_t current_value, std::string name)
    : m_name{std::move(name)}
    , m_max_value{max_value}
    , m_value{(std::min)(current_value, max_value)} {}

inline std::size_t Semaphore::value() const noexcept {
    std::lock_guard lk{m_lock};
    return m_value;
}

inline std::size_t Semaphore::max_value() const noexcept {
    std::lock_guard lk{m_lock};
    return m_max_value;
}
inline void Semaphore::reset(std::size_t max_value) {
    std::lock_guard lk{m_lock};

    if (!m_waiters.empty()) {
        throw Exception("cannot reset semaphore while waiters exist.");
    }

    m_max_value = max_value;
    m_value = max_value;
}

inline void Semaphore::reset(std::size_t max_value, std::size_t current_value) {
    std::lock_guard lk{m_lock};

    if (!m_waiters.empty()) {
        throw Exception("cannot reset semaphore while waiters exist.");
    }

    m_max_value = max_value;
    m_value = (std::min)(current_value, max_value);
}

inline std::string_view Semaphore::name() const noexcept {
    return m_name;
}

template <typename S>
    requires std::constructible_from<std::string, S>
inline Semaphore& Semaphore::name(S&& name) {
    m_name = std::forward<S>(name);
    return *this;
}

inline bool Semaphore::_try_acquire(Work* w, std::size_t count) {
    std::lock_guard lk{m_lock};

    // 当前可用配额能够一次性满足整个请求时直接扣减并放行，不发生部分获取。
    if (m_value >= count) {
        m_value -= count;
        return true;
    }

    // 配额不足时仅登记当前 Work，不阻塞执行它的 Worker。
    // 后续 release 会把等待者重新交回调度器；任务再次执行时重新竞争所需全部配额。
    // 因此错误的配额依赖关系仍然可能在任务层形成逻辑死锁。
    m_waiters.push_back(w);
    return false;
}

inline void Semaphore::_release(std::size_t count, SmallVector<Work*>& out) {
    std::lock_guard lk{m_lock};

    // 当前可用配额必须始终位于 [0, m_max_value]。
    TFL_ASSERT(m_value <= m_max_value && "semaphore invariant broken");

    // release 不允许使可用配额超过配置上限。
    // 使用差值判断避免直接计算 m_value + count 时发生无符号溢出。
    if (count > m_max_value - m_value) [[unlikely]] {
        throw Exception("semaphore release exceeds max_value.");
    }

    if (!m_waiters.empty()) {
        if (out.empty()) {
            // 最常见路径：直接交换底层存储。
            // 不需要复制 waiter，也不会发生额外内存分配。
            out.swap(m_waiters);
        } else {
            // 先完成唯一可能发生内存分配的容量准备。
            // reserve 失败时，Semaphore 的配额和 waiter 状态均保持不变。
            out.reserve(out.size() + m_waiters.size());

            // 容量已经准备完成，元素类型为 Work*；
            // 追加成功后再清空内部 waiter，完成所有权意义上的转移。
            out.insert(out.end(), m_waiters.begin(), m_waiters.end());
            m_waiters.clear();
        }
    }

    // waiter 转移完成后提交配额变化。
    m_value += count;

}

}  // namespace tfl
