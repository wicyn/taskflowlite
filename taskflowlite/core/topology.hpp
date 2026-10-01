/// @file topology.hpp
/// @brief 执行拓扑 Topology —— 任务图运行实例的生命周期与状态机。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <limits>

#include "forward.hpp"
#include "utility.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief 表示一次独立任务提交或异步任务的运行控制块。
///
/// `Topology` 集中保存启动状态、强引用计数、协作式停止请求标志及可选父拓扑，
/// 并借用负责调度的 `Executor`。它不拥有任务图或 Executor，由所属 Work 状态管理生命周期。
///
/// @note 原子状态允许执行、等待和句柄引用跨线程协作；对象本身不可复制或移动。
/// @note Executor 在构造时绑定，绑定后不可更换。
class Topology : public Immovable<Topology> {

    friend class Work;
    friend class Task;
    friend class Context;
    friend class Runtime;
    friend class Executor;
    friend class TaskGroup;
    template <typename> friend class AsyncFuture;
    template <typename> friend class AsyncTask;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 构造 Topology 并绑定到 Executor。
    /// @param parent 非拥有指针；父级运行拓扑，无父级时为空。
    /// @param executor 非拥有引用；执行期间及通过本对象访问它时必须保持有效。
    /// @pre parent 必须在本对象仍可能访问其父拓扑链期间保持有效。
    /// @note 初始状态为 Idle，强引用计数为零，所有控制标志均未设置。
    explicit Topology(Executor& executor, Topology* parent = nullptr) noexcept
        : m_executor{executor}
        , m_parent{parent}{}


    /// @brief 销毁拓扑状态；调用方必须已完成全部引用计数协议且不存在并发访问。
    ~Topology() = default;

private:

    /// @brief Topology 生命周期与并发控制字。
    ///
    /// 高位保存独立控制标志，中间保存生命周期状态，低位保存强引用计数。
    ///
    /// 布局：
    ///
    ///     STOP_REQUESTED | LOCKED | Status | use_count
    ///
    struct Control {
        using type = std::size_t;

        /// @brief Topology 生命周期状态。
        ///
        /// Finished 保持编码为 3，使 Running -> Finished 可以直接通过 fetch_or
        /// 设置完成位，而不需要清除已有状态位；完成发布仍须遵守动态依赖锁协议。
        enum class Status : type {
            Idle     = 0, ///< 尚未启动。
            Running  = 1, ///< 已启动，正在执行。
            Finished = 3  ///< 已完成。
        };

        static constexpr unsigned BITS = std::numeric_limits<type>::digits;
        static constexpr type NONE = 0;

        /// @brief 当前 Topology 已收到协作式停止请求。
        static constexpr type STOP_REQUESTED = type{1} << (BITS - 1);

        /// @brief 动态依赖边表正在被独占访问。
        static constexpr type LOCKED = type{1} << (BITS - 2);

        /// @brief 所有独立控制标志。
        static constexpr type FLAG_MASK = STOP_REQUESTED | LOCKED;

        /// @brief 控制标志占用的位数。
        static constexpr unsigned FLAG_BITS = std::popcount(FLAG_MASK);

        /// @brief Status 字段所需的位数。
        static constexpr unsigned STATE_BITS = std::bit_width(static_cast<type>(Status::Finished));

        /// @brief 引用计数字段占用的位数。
        static constexpr unsigned USE_COUNT_BITS = BITS - FLAG_BITS - STATE_BITS;

        static_assert(USE_COUNT_BITS > 16);

        /// @brief Status 字段起始 bit。
        static constexpr unsigned STATE_SHIFT = USE_COUNT_BITS;

        /// @brief Status 字段掩码。
        static constexpr type STATE_MASK = ((type{1} << STATE_BITS) - 1) << STATE_SHIFT;

        /// @brief 引用计数字段掩码。
        static constexpr type USE_COUNT_MASK = (type{1} << USE_COUNT_BITS) - 1;

        /// @brief 引用计数最大值。
        static constexpr type USE_COUNT_MAX = USE_COUNT_MASK;

        /// @brief 引用计数增量步长。
        static constexpr type USE_COUNT_INC = type{1};

        /// @brief 从完整控制值中提取生命周期状态。
        [[nodiscard]] static constexpr Status status(type value) noexcept {
            return static_cast<Status>((value & STATE_MASK) >> STATE_SHIFT);
        }

        /// @brief 将生命周期状态转换为对应位域。
        [[nodiscard]] static constexpr type state_bits(Status status) noexcept {
            return static_cast<type>(status) << STATE_SHIFT;
        }

        /// @brief 替换完整控制值中的生命周期状态，并保留其它字段。
        [[nodiscard]] static constexpr type set_status(type value, Status status) noexcept {
            return (value & ~STATE_MASK) | state_bits(status);
        }

        /// @brief 当前控制值是否持有动态依赖锁。
        [[nodiscard]] static constexpr bool locked(type value) noexcept {
            return value & LOCKED;
        }

        /// @brief 从完整控制值中提取引用计数。
        [[nodiscard]] static constexpr type use_count(type value) noexcept {
            return value & USE_COUNT_MASK;
        }
    };

    /// @brief 阻塞当前线程，直到 Topology 进入 Finished。
    ///
    /// 通过 `atomic::wait` 等待控制字发生变化，并仅以 Status::Finished
    /// 作为完成条件；STOP_REQUESTED、LOCKED、引用计数等其他位变化只会触发重新检查。
    ///
    /// @note 发布 Finished 的完成路径必须对同一控制字执行 `notify_all()`。
    TFL_FORCE_INLINE void _wait() const noexcept {
        auto value = m_control.load(std::memory_order_acquire);

        while (Control::status(value) != Control::Status::Finished) {
            m_control.wait(value, std::memory_order_acquire);
            value = m_control.load(std::memory_order_acquire);
        }
    }

    /// @brief 增加一份 Topology 强引用。
    TFL_FORCE_INLINE void _increment_ref() noexcept {
        auto prev = m_control.fetch_add(Control::USE_COUNT_INC, std::memory_order_relaxed);
        TFL_ASSERT(Control::use_count(prev) < Control::USE_COUNT_MAX);
    }

    /// @brief 释放一份 Topology 强引用。
    ///
    /// @return 释放前引用计数为 1、即本次释放最后一份强引用时返回 true。
    TFL_FORCE_INLINE bool _decrement_ref() noexcept {
        auto prev = m_control.fetch_sub(Control::USE_COUNT_INC, std::memory_order_acq_rel);
        TFL_ASSERT(Control::use_count(prev) != 0);
        return Control::use_count(prev) == 1;
    }

    /// @brief 查询当前 Topology 的状态是否为 Running。
    [[nodiscard]] TFL_FORCE_INLINE bool _is_running() const noexcept {
        return Control::status(m_control.load(std::memory_order_relaxed)) == Control::Status::Running;
    }

    /// @brief 查询当前 Topology 的状态是否为 Finished。
    [[nodiscard]] TFL_FORCE_INLINE bool _is_finished() const noexcept {
        return Control::status(m_control.load(std::memory_order_relaxed)) == Control::Status::Finished;
    }

    /// @brief 返回当前 Topology 强引用计数的瞬时快照。
    [[nodiscard]] TFL_FORCE_INLINE std::size_t _use_count() const noexcept {
        return static_cast<std::size_t>(Control::use_count(m_control.load(std::memory_order_relaxed)));
    }

    /// @brief 将当前 Topology 状态设置为 Running。
    /// @pre 当前不存在需要通过动态依赖锁协议协调的并发状态迁移。
    TFL_FORCE_INLINE void _set_running() noexcept {
        auto value = m_control.load(std::memory_order_relaxed);
        m_control.store(Control::set_status(value, Control::Status::Running), std::memory_order_relaxed);
    }

    /// @brief 将当前 Topology 状态发布为 Finished 并唤醒全部等待线程。
    /// @pre 当前不存在需要通过动态依赖锁协议协调的并发状态迁移。
    TFL_FORCE_INLINE void _set_finished() noexcept {
        auto value = m_control.load(std::memory_order_relaxed);
        m_control.store(Control::set_status(value, Control::Status::Finished), std::memory_order_release);
        m_control.notify_all();
    }

    /// @brief 向当前 Topology 原子设置停止请求。
    ///
    /// 这里只设置当前 Topology 的 STOP_REQUESTED，不遍历或主动修改子 Topology；
    /// 后代 Topology 通过 `_stop_requested()` 沿 `m_parent` 向上观察停止链。
    ///
    /// @return 本次调用把 STOP_REQUESTED 从未设置变为已设置时返回 true；
    ///         该位此前已经存在时返回 false。
    [[nodiscard]] TFL_FORCE_INLINE bool _request_stop() noexcept {
        auto prev = m_control.fetch_or(Control::STOP_REQUESTED, std::memory_order_relaxed);
        return !(prev & Control::STOP_REQUESTED);
    }

    /// @brief 查询当前 Topology 及其祖先 Topology 链是否存在停止请求。
    ///
    /// 从当前 Topology 开始逐级检查 STOP_REQUESTED。若祖先 Topology 已请求停止，
    /// 则将 STOP_REQUESTED 惰性缓存到当前 Topology，使后续查询无需再次遍历父链。
    ///
    /// @return 当前或任一祖先 Topology 已请求停止时返回 true。
    /// @warning 当前 Topology 及其全部祖先 Topology 必须在调用期间保持有效。
    [[nodiscard]] TFL_FORCE_INLINE bool _stop_requested() const noexcept {
        const Topology* topology = this;

        do {
            if (topology->m_control.load(std::memory_order_relaxed) & Control::STOP_REQUESTED) {
                if (topology != this) {
                    m_control.fetch_or(Control::STOP_REQUESTED, std::memory_order_relaxed);
                }
                return true;
            }
        } while ((topology = topology->m_parent) != nullptr);

        return false;
    }

    mutable std::atomic<Control::type>  m_control{Control::NONE}; ///< 原子控制字：停止请求、依赖锁、生命周期状态和引用计数。
    Executor&                           m_executor;               ///< 非拥有引用；构造时绑定，访问期间必须保持有效。
    Topology*                           m_parent{nullptr};        ///< 父级运行拓扑；非空时持有一份强引用。
};

}  // namespace tfl
