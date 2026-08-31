/// @file branch.hpp
/// @brief 条件分支控制器 Branch / MultiBranch —— DAG 内的运行时动态路由。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <concepts>
#include <functional>

#include "task.hpp"
#include "small_vector.hpp"
#include "context.hpp"

namespace tfl {

// ============================================================================
// Branch 单目标分支
// ============================================================================

/// @brief 在分支 callable 执行期间记录唯一一个待调度后继。
///
/// 选择目标来自当前 `Work` 的后继列表；后续选择会覆盖先前结果，未选择或取消
/// 选择时本次分支不调度后继。对象及其执行上下文均由框架临时注入。
///
/// @warning 仅可在当前 callable 和 Worker 线程内使用，不得保存或跨线程传递。
class Branch final : public Context {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 将一次下标布尔赋值转发为指定后继的选择或取消选择。
    ///
    /// 代理不保存独立选择状态，只借用所属 `Branch` 和索引；true 选择该位置，
    /// false 仅在该位置当前被选中时取消选择。
    class Proxy {
        friend class Branch;
        Branch&     m_br;
        std::size_t m_idx;
        Proxy(Branch& br, std::size_t idx) noexcept : m_br{br}, m_idx{idx} {}

    public:
        /// @brief 根据布尔值选择或取消选择当前索引。
        Branch& operator=(bool on) noexcept {
            if (on) {
                m_br.select(m_idx);
            } else {
                m_br.unselect(m_idx);
            }
            return m_br;
        }
    };

    // ==================== 选择动作 ====================

    /// @brief 按索引选择一个后继。
    /// @tparam I 除 bool 外的整数类型。
    /// @param index 后继列表中的零基索引。
    /// @return `*this`，用于链式调用。
    /// @post 索引有效时选择对应后继；否则清除当前选择。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& select(I index) noexcept;

    /// @brief 选择首个使谓词返回 true 的后继。
    /// @tparam Pred 接受 `TaskView` 并返回 bool 的谓词类型。
    /// @param pred 用于测试后继的谓词。
    /// @return `*this`，用于链式调用。
    /// @post 没有后继匹配时清除当前选择。
    template <predicate<TaskView> Pred>
    Branch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    /// @brief 仅当当前选择等于指定索引时清除选择。
    /// @tparam I 除 bool 外的整数类型。
    /// @param index 要取消选择的后继索引。
    /// @return `*this`，用于链式调用。
    /// @post 索引无效或未被选中时不改变当前选择。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& unselect(I index) noexcept;

    /// @brief 无条件清除当前选择。
    /// @return `*this`，用于链式调用。
    /// @post 本次分支不调度任何后继。
    Branch& reset() noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 选择指定索引，等价于 `select(index)`。
    /// @tparam I 除 bool 外的整数类型。
    /// @param index 后继列表中的零基索引。
    /// @return `*this`，用于链式调用。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& operator()(I index) noexcept;

    // ==================== 下标语法 ====================

    /// @brief 返回支持 `branch[index] = true/false` 的赋值代理。
    /// @param index 后继列表中的零基索引。
    /// @return 绑定该索引的 Proxy。
    [[nodiscard]] Proxy operator[](std::size_t index) noexcept;

    // ==================== 查询接口 ====================

    /// @brief 获取当前分支节点的后继数量。
    /// @return 出边数量。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    /// @brief 当前选中的后继；nullptr 表示未选择。
    Work* m_target{nullptr};

    explicit Branch(Work& work, Worker& worker, Executor& executor) noexcept
        : Context{work, worker, executor} {}
};

// ============================================================================
// Branch 内联实现
// ============================================================================

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
inline Branch& Branch::select(I index) noexcept {
    const auto idx = static_cast<std::size_t>(index);
    m_target = (idx < m_work.m_num_successors) ? m_work.m_edges[idx] : nullptr;
    return *this;
}

template <predicate<TaskView> Pred>
inline Branch& Branch::select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    m_target = nullptr;
    const std::size_t sz = m_work.m_num_successors;
    for (std::size_t i = 0; i < sz; ++i) {
        if (std::invoke(pred, TaskView{*m_work.m_edges[i]})) {
            m_target = m_work.m_edges[i];
            return *this;
        }
    }
    return *this;
}

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
inline Branch& Branch::unselect(I index) noexcept {
    const auto idx = static_cast<std::size_t>(index);
    if (idx < m_work.m_num_successors && m_target == m_work.m_edges[idx]) {
        m_target = nullptr;
    }
    return *this;
}

inline Branch& Branch::reset() noexcept {
    m_target = nullptr;
    return *this;
}

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
inline Branch& Branch::operator()(I index) noexcept {
    return select(index);
}

inline Branch::Proxy Branch::operator[](std::size_t index) noexcept {
    return {*this, index};
}

inline std::size_t Branch::size() const noexcept {
    return m_work.m_num_successors;
}

// ============================================================================
// MultiBranch 多目标分支
// ============================================================================

/// @brief 在多分支 callable 执行期间记录零个或多个待调度后继。
///
/// 每个目标来自当前 `Work` 的后继列表，可独立选择或取消；callable 返回后，
/// 框架仅调度仍处于选中状态的目标。对象及其执行上下文均为临时借用。
///
/// @warning 仅可在当前 callable 和 Worker 线程内使用，不得保存或跨线程传递。
class MultiBranch final : public Context {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 将一次下标布尔赋值转发为指定后继的选择或取消选择。
    ///
    /// 代理只借用所属 `MultiBranch` 和索引，不拥有上下文或目标节点。
    class Proxy {
        friend class MultiBranch;
        MultiBranch& m_mb;
        std::size_t  m_idx;
        Proxy(MultiBranch& mb, std::size_t idx) noexcept : m_mb{mb}, m_idx{idx} {}

    public:
        /// @brief 根据布尔值添加或移除当前索引。
        /// @note 越界索引不会改变选择集合。
        MultiBranch& operator=(bool on) noexcept {
            if (m_idx >= m_mb.m_work.m_num_successors) return m_mb;
            Work* w = m_mb.m_work.m_edges[m_idx];
            if (on) {
                m_mb._insert(w);
            } else {
                m_mb._erase(w);
            }
            return m_mb;
        }
    };

    /// @brief 返回支持 `mb[index] = true/false` 的赋值代理。
    /// @tparam I 除 bool 外的整数类型。
    /// @param index 后继列表中的零基索引。
    /// @return 绑定该索引的 Proxy。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    [[nodiscard]] Proxy operator[](I index) noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 将一个或多个索引加入选择集合。
    /// @tparam Is 除 bool 外的整数类型。
    /// @param indices 要选择的后继索引。
    /// @return `*this`，用于链式调用。
    /// @note 本操作不清除既有选择。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& operator()(Is... indices);

    // ==================== 选择动作 ====================

    /// @brief 将一个或多个索引加入选择集合。
    /// @tparam Is 除 bool 外的整数类型。
    /// @param indices 要选择的后继索引。
    /// @return `*this`，用于链式调用。
    /// @post 忽略越界索引，并对重复索引去重。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& select(Is... indices);

    /// @brief 从选择集合移除一个或多个索引。
    /// @tparam Is 除 bool 外的整数类型。
    /// @param indices 要取消选择的后继索引。
    /// @return `*this`，用于链式调用。
    /// @post 忽略越界索引和未被选择的索引。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& unselect(Is... indices) noexcept;

    /// @brief 将全部后继加入选择集合。
    /// @return `*this`，用于链式调用。
    MultiBranch& select_all();

    /// @brief 清空选择集合。
    /// @return `*this`，用于链式调用。
    MultiBranch& unselect_all() noexcept;

    /// @brief 清空选择集合，等价于 `unselect_all()`。
    /// @return `*this`，用于链式调用。
    MultiBranch& reset() noexcept;

    /// @brief 将所有使谓词返回 true 的后继加入选择集合。
    /// @tparam Pred 接受 `TaskView` 并返回 bool 的谓词类型。
    /// @param pred 用于测试后继的谓词。
    /// @return `*this`，用于链式调用。
    template <predicate<TaskView> Pred>
    MultiBranch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    /// @return 出边数量。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    /// @brief 保存已选择后继的去重集合。
    SmallVector<Work*>  m_targets;

    explicit MultiBranch(Work& work, Worker& worker, Executor& executor) noexcept
        : Context{work, worker, executor} {}

    /// @brief 以线性扫描去重后插入目标；适用于预期较小的后继集合。
    /// @param w 待插入的后继 Work 指针。
    void _insert(Work* w) {
        if (m_targets.size() >= m_work.m_num_successors) return;
        for (auto* t : m_targets) {
            if (t == w) return;
        }
        m_targets.push_back(w);
    }

    /// @brief 线性查找目标，找到后使用 swap-and-pop 常数时间移除；整体复杂度为 O(n)。
    /// @param w 待移除的后继 Work 指针。
    void _erase(Work* w) noexcept {
        for (auto it = m_targets.begin(); it != m_targets.end(); ++it) {
            if (*it == w) {
                *it = m_targets.back();
                m_targets.pop_back();
                return;
            }
        }
    }
};

// ============================================================================
// MultiBranch 内联实现
// ============================================================================

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
inline MultiBranch::Proxy MultiBranch::operator[](I index) noexcept {
    return {*this, static_cast<std::size_t>(index)};
}

template <typename... Is>
    requires (sizeof...(Is) >= 1)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
inline MultiBranch& MultiBranch::operator()(Is... indices) {
    return select(indices...);
}

template <typename... Is>
    requires (sizeof...(Is) >= 1)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
inline MultiBranch& MultiBranch::select(Is... indices) {
    const std::size_t sz = m_work.m_num_successors;
    // 使用折叠表达式依次处理索引，越界项被忽略。
    ([&](std::size_t idx) {
        if (idx < sz) _insert(m_work.m_edges[idx]);
    }(static_cast<std::size_t>(indices)), ...);
    return *this;
}

template <typename... Is>
    requires (sizeof...(Is) >= 1)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
inline MultiBranch& MultiBranch::unselect(Is... indices) noexcept {
    const std::size_t sz = m_work.m_num_successors;
    ([&](std::size_t idx) {
        if (idx < sz) _erase(m_work.m_edges[idx]);
    }(static_cast<std::size_t>(indices)), ...);
    return *this;
}

inline MultiBranch& MultiBranch::select_all() {
    const std::size_t sz = m_work.m_num_successors;
    for (std::size_t i = 0; i < sz; ++i) {
        _insert(m_work.m_edges[i]);
    }
    return *this;
}

inline MultiBranch& MultiBranch::unselect_all() noexcept {
    m_targets.clear();
    return *this;
}

inline MultiBranch& MultiBranch::reset() noexcept {
    unselect_all();
    return *this;
}

template <predicate<TaskView> Pred>
inline MultiBranch& MultiBranch::select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    const std::size_t sz = m_work.m_num_successors;
    for (std::size_t i = 0; i < sz; ++i) {
        if (std::invoke(pred, TaskView{*m_work.m_edges[i]})) {
            _insert(m_work.m_edges[i]);
        }
    }
    return *this;
}

inline std::size_t MultiBranch::size() const noexcept {
    return m_work.m_num_successors;
}

}  // namespace tfl
