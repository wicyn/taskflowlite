#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "result_slot.hpp"
#include "topology.hpp"

namespace tfl {

/// @brief 为异步任务按值存储一个与任务生命周期一致的 `Topology`。
///
/// storage 拥有拓扑控制块，但不拥有传入的父 Topology 和 Executor。
/// Detached、Joinable 和 Attached 任务通过继承该类获得独立拓扑状态。
class TopologyStorage {
public:
    /// @brief 获取内部 Topology 的可修改地址。
    /// @return 在当前 storage 生命周期内有效的非空指针。
    [[nodiscard]] Topology* get_topology() noexcept {
        return std::addressof(m_topology);
    }

    /// @brief 获取内部 Topology 的只读地址。
    /// @return 在当前 storage 生命周期内有效的非空指针。
    [[nodiscard]] const Topology* get_topology() const noexcept {
        return std::addressof(m_topology);
    }

protected:
    /// @brief 构造绑定父拓扑和 Executor 的独立拓扑状态。
    /// @param parent_topology 父 Topology；根级任务允许为空。
    /// @param executor 非拥有 Executor 指针。
    explicit TopologyStorage(Topology* parent_topology, Executor* executor) noexcept
        : m_topology{parent_topology, executor} {}

    ~TopologyStorage() noexcept = default;

private:
    Topology m_topology;
};


/// @brief 为需要结果通道的异步任务按值存储 `Topology` 和 `ResultSlot<R>`。
///
/// Joinable 和 Attached 任务共享该存储层；两者的区别由各自生命周期和
/// tear-down 协议决定，而不是由结果存储方式决定。
///
/// @tparam R 任务返回类型。
template <typename R>
class ResultStorage : public TopologyStorage {
public:
    using result_type = R;

    /// @brief 获取内部结果槽的可修改地址。
    /// @return 在当前 storage 生命周期内有效的非空指针。
    [[nodiscard]] ResultSlot<R>* get_result_slot() noexcept {
        return std::addressof(m_result);
    }

    /// @brief 获取内部结果槽的只读地址。
    /// @return 在当前 storage 生命周期内有效的非空指针。
    [[nodiscard]] const ResultSlot<R>* get_result_slot() const noexcept {
        return std::addressof(m_result);
    }

protected:
    /// @brief 构造结果存储并初始化所属拓扑。
    /// @param parent_topology 父 Topology；根级任务允许为空。
    /// @param executor 非拥有 Executor 指针。
    explicit ResultStorage(Topology* parent_topology, Executor* executor) noexcept
        : TopologyStorage{parent_topology, executor} {}

    ~ResultStorage() noexcept = default;

    /// @brief 执行 callable，并将返回结果保存到内部结果槽。
    template <typename Callable>
        requires requires(ResultSlot<R>& result, Callable&& callable) {
            result.invoke(std::forward<Callable>(callable));
        }
    void set_result_from(Callable&& callable) noexcept(noexcept(m_result.invoke(std::forward<Callable>(callable)))) {
        m_result.invoke(std::forward<Callable>(callable));
    }

private:
    TFL_NO_UNIQUE_ADDRESS ResultSlot<R> m_result;
};

} // namespace tfl
