/// @file topology.hpp
/// @brief 执行拓扑 Topology —— 任务图运行实例的生命周期与状态机。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <stop_token>
#include <cstddef>
#include <cstdint>

#include "forward.hpp"
#include "utility.hpp"

namespace tfl {

/// @brief 表示一次独立任务提交或异步任务的运行控制块。
///
/// `Topology` 集中保存启动状态、强引用计数、协作式停止源及可选父停止转发，
/// 并借用负责调度的 `Executor`。它不拥有任务图或 Executor，由所属 Work 状态管理生命周期。
///
/// @note 原子状态允许执行、等待和句柄引用跨线程协作；对象本身不可复制或移动。
class Topology : public Immovable<Topology> {

    friend class Work;
    friend class Task;
    friend class Runtime;
    friend class Executor;
    friend class TaskGroup;
    template <typename> friend class AsyncFuture;
    template <typename> friend class AsyncTask;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 构造 Topology 并绑定到 Executor。
    /// @param executor 非拥有指针；执行期间对应 Executor 必须保持存活。
    explicit Topology(Topology* parent, Executor* executor) noexcept
        : m_parent{parent}
        , m_executor{executor} {}


    /// @brief 销毁停止回调和拓扑状态；调用方必须已完成全部引用计数协议。
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
        /// 设置完成位，而不需要清除已有状态位。
        enum class Status : type {
            Idle     = 0, ///< 尚未启动。
            Running  = 1, ///< 正在执行。
            Finished = 3  ///< 已完成。
        };

        static constexpr unsigned BITS = std::numeric_limits<type>::digits;
        static constexpr type NONE = 0;

        /// @brief 当前 Topology 已收到协作式停止请求。
        static constexpr type STOP_REQUESTED = type{1} << (BITS - 1);

        /// @brief 动态依赖后继表正在被独占修改。
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



    Topology*                  m_parent{nullptr};              ///< 非拥有指针；父级运行拓扑，无父级时为空。
    std::atomic<Control::type> m_control{Control::NONE};       ///< 原子控制字：停止请求、生命周期状态和引用计数。
    Executor*                  m_executor{nullptr};            ///< 非拥有指针；执行期间必须保持有效。

};

}  // namespace tfl
