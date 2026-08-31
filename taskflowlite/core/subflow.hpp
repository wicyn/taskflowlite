/// @file subflow.hpp
/// @brief 运行时动态子图构建器。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include "flow_builder.hpp"
#include "context.hpp"

namespace tfl {

/// @brief 为当前任务构建、提交并等待一个运行时动态子图。
///
/// `SubFlow` 同时借用动态 `Graph` 和当前执行上下文；通过 `FlowBuilder` 创建的
/// 节点归该动态图管理，并作为当前父任务的动态子任务参与完成与异常传播。
///
/// @warning 对象只在当前 callable 和 Worker 线程内有效，不得保存、跨线程传递或在子图执行时修改图结构。
class SubFlow final : public FlowBuilder, public Context {
    friend class ScopedExceptionAnchor;
    friend class FlowBuilder;
    TFL_WORK_SUBCLASS_FRIENDS;

    using Builder = FlowBuilder;

public:
    /// @brief 提交当前动态子图的源节点并立即返回，不等待子图完成。
    /// @pre 不得在上一次 `run()` 提交的同一子图仍在执行时再次提交或修改它。
    void run();

    /// @brief 协作式等待当前 SubFlow 父节点挂接的全部动态子任务完成。
    ///
    /// 本函数只负责等待并重新抛出已经归档到当前父节点的异常，
    /// 不会自动启用显式异常锚点。若需要在当前 SubFlow 内拦截子任务异常，
    /// 必须在启动子图之前创建 ScopedExceptionAnchor，并保证守卫存活到
    /// wait() 完成。
    ///
    /// @code
    /// ScopedExceptionAnchor guard{subflow};
    ///
    /// subflow.run();
    /// subflow.wait();
    /// @endcode
    ///
    /// @note 等待期间当前 Worker 会继续执行其他就绪任务。
    /// @note 没有 ScopedExceptionAnchor 时，异常按照默认父链继续向外传播。
    /// @note 可以重复调用，每次调用都会等待当前已经挂接的全部子任务。
    /// @throws 重新抛出显式异常锚点归档的首个异常。
    void wait();

private:
    /// @brief 由框架构造绑定当前动态图和执行上下文的 SubFlow。
    /// @param graph 动态节点的物理存储。
    /// @param work 当前父任务。
    /// @param worker 当前 Worker。
    /// @param executor 当前 Executor。
    explicit SubFlow(Graph& graph, Work& work, Worker& worker, Executor& executor) noexcept
        : Builder{graph}
        , Context{work, worker, executor} {
        graph.clear();
    }
};

inline void SubFlow::run() {
    auto num_sources = m_executor._set_up_graph(graph(), m_work);
    if (num_sources == 0) {
        return;
    }

    m_work.m_join_counter.fetch_add(num_sources, std::memory_order_relaxed);
    m_executor._schedule(m_worker, graph().begin(), num_sources);
}

inline void SubFlow::wait() {
    m_executor._corun_until(m_worker, [this]() noexcept {
        return m_work.m_join_counter.load(std::memory_order_acquire) == 1;
    });

    m_work._rethrow_exception();
}

} // namespace tfl
