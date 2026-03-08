/// @file jump.hpp
/// @brief 提供无条件跳转控制器，绕过 DAG 计数器直接将目标置为就绪并调度。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <optional>
#include <string_view>
#include <cstddef>
#include <concepts>
#include <span>
#include "task.hpp"
#include "small_vector.hpp"

namespace tfl {

/// @brief 单目标跳转控制器。
///
/// 在 JumpWork 的 callable 内部使用，为用户提供运行时的无条件跳转能力。
/// 可将执行流直接转移到指定后继，不受常规 DAG 依赖关系的约束。
///
/// @details
/// **调度原理（与 Branch 的核心区别）**
///
/// Branch 在现有 DAG 计数器上做差额（边权重 2，选中多减一次），
/// 未选中的后继仍可被其他前驱触发。
///
/// Jump 则完全绕过计数器机制——`_tear_down_jump_task` 直接将
/// target 的 `join_counter` 置 0 并立即调度：
///
/// ```
///   Branch:  join_counter -= 1 (差额)   → 可能仍 > 0 → 等待其他前驱
///   Jump:    join_counter  = 0 (强制)    → 必定就绪    → 立即调度
/// ```
///
/// 这意味着 Jump 目标不会等待任何其他前驱完成，实现真正的
/// "goto" 语义。适用于错误恢复、状态机跳转、提前终止等场景。
///
/// **选择语义（Last-write-wins）**
///
/// - 支持按索引、名称、谓词三种选择方式
/// - 多次 `to()` 调用以最后一次为准
/// - 不调用任何 `to()` 则不执行跳转，走常规 tear_down 路径
/// - `reset()` 可显式取消当前选择
///
/// @pre 必须由 JumpWork::invoke 在 Worker 线程的栈上临时构造。
/// @note 禁止将此对象的引用逃逸到外部线程，其生命周期严格限定在 callable 执行期间。
class Jump : public MoveOnly<Jump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 跳转操作 ====================

    /// @brief 按索引选择跳转目标。O(1)。
    /// @param index 目标后继在后继数组中的位置。
    /// @return `*this`，支持链式调用。
    /// @post 若 index 有效则选中对应后继；越界时清除选择（等价于 reset）。
    Jump& to(std::size_t index) noexcept;

    /// @brief 按名称选择跳转目标。O(N) 线性搜索。
    /// @param name 目标后继的名称标识。
    /// @return `*this`，支持链式调用。
    /// @post 首个名称匹配的后继被选中；无匹配时清除选择。
    /// @note 提供与插入顺序无关的稳定选择方式。
    Jump& to(std::string_view name) noexcept;

    /// @brief 按谓词选择首个满足条件的后继作为跳转目标。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`，支持链式调用。
    /// @post 首个满足谓词的后继被选中；无匹配时清除选择。
    template <predicate<TaskView> Pred>
    Jump& to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    /// @brief 取消跳转。
    /// @return `*this`，支持链式调用。
    /// @post 内部目标指针置空，invoke 结束后走常规 tear_down 路径。
    Jump& reset() noexcept;

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 按索引查询后继名称。
    /// @param index 要查询的后继索引。
    /// @return 索引有效时返回名称视图，越界返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::string_view> name(std::size_t index) const noexcept;

    /// @brief 按名称查询后继索引。
    /// @param name 要查询的后继名称。
    /// @return 匹配时返回对应索引，未找到返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::size_t> index(std::string_view name) const noexcept;

private:
    Work&  m_work;           ///< 绑定至宿主 JumpWork，借此访问后继列表
    Work* m_target{nullptr}; ///< 选中的跳转目标；invoke 结束后由 _tear_down_jump_task 直接置 counter=0 并调度

    explicit Jump(Work& work) noexcept : m_work{work} {}

    [[nodiscard]] bool _empty() const noexcept { return m_target == nullptr; }
    [[nodiscard]] Work* _target() const noexcept { return m_target; }
};

// ============================================================
//  Jump 内联实现
// ============================================================

inline Jump& Jump::to(std::size_t index) noexcept {
    auto succs = m_work._successors();
    m_target = (index < succs.size()) ? succs[index] : nullptr;
    return *this;
}

inline Jump& Jump::to(std::string_view name) noexcept {
    m_target = nullptr;
    for (auto* suc : m_work._successors()) {
        if (suc->m_name == name) { m_target = suc; return *this; }
    }
    return *this;
}

template <predicate<TaskView> Pred>
Jump& Jump::to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    m_target = nullptr;
    for (auto* suc : m_work._successors()) {
        // Why: 包装为只读 TaskView 传入谓词，隔离底层 Work 的写入权限，
        // 防止用户在谓词求值期间篡改图结构。
        if (std::invoke_r<bool>(pred, TaskView{*suc})) { m_target = suc; return *this; }
    }
    return *this;
}

inline Jump& Jump::reset() noexcept {
    m_target = nullptr;
    return *this;
}

inline std::size_t Jump::size() const noexcept {
    return m_work.m_num_successors;
}

inline std::optional<std::string_view> Jump::name(std::size_t index) const noexcept {
    auto succs = m_work._successors();
    return index < succs.size() ? std::optional{succs[index]->m_name} : std::nullopt;
}

inline std::optional<std::size_t> Jump::index(std::string_view name) const noexcept {
    auto succs = m_work._successors();
    for (std::size_t i = 0; i < succs.size(); ++i) {
        if (succs[i]->m_name == name) return i;
    }
    return std::nullopt;
}


/// @brief 多目标跳转控制器。
///
/// 在 MultiJumpWork 的 callable 内部使用。
/// 与 Jump 的互斥选择不同，MultiJump 允许同时选中多个后继，
/// `_tear_down_multi_jump_task` 对每个 target 执行与单目标相同的
/// 强制置零 + 立即调度逻辑。
///
/// @details
/// **选择语义（Accumulative）**
///
/// - 所有 `to()` 调用均为 **累积** 生效，而非覆盖
/// - 内部自动去重：同一后继不会被重复跳转，避免重复调度
/// - `to_all()` 实现无条件广播
/// - `reset()` 清空全部已累积的选择
///
/// **存储与性能**
///
/// 使用 SmallVector 存储选中集合，小数量后继时避免堆分配。
/// 去重采用线性扫描——DAG 节点的典型扇出数极低（通常 < 10），
/// 线性扫描优于 hash set 的常数开销。
///
/// @pre 必须由 MultiJumpWork::invoke 在 Worker 线程的栈上临时构造。
/// @post 所有选中的 target 在 invoke 返回后被强制置为就绪并调度。
/// @note 禁止将此对象的引用逃逸到外部线程，其生命周期严格限定在 callable 执行期间。
class MultiJump : public MoveOnly<MultiJump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 跳转操作 ====================

    /// @brief 按索引集批量选择跳转目标。
    /// @tparam Is 可转换为 `std::size_t` 的变参索引类型。
    /// @param indices 一组要跳转的后继索引。
    /// @return `*this`，支持链式调用。
    /// @post 所有有效索引对应的后继被加入跳转集合，越界索引自动忽略。
    template <typename... Is>
        requires (sizeof...(Is) > 0) && (std::convertible_to<Is, std::size_t> && ...)
    MultiJump& to(Is... indices);

    /// @brief 按名称集批量选择跳转目标。
    /// @tparam Ns 可转换为 `std::string_view` 的变参名称类型。
    /// @param names 一组要跳转的后继名称。
    /// @return `*this`，支持链式调用。
    /// @post 名称匹配的所有后继被加入跳转集合。
    template <typename... Ns>
        requires (sizeof...(Ns) > 0) && (std::convertible_to<Ns, std::string_view> && ...)
    MultiJump& to(Ns&&... names);

    /// @brief 跳转到所有后继（无条件广播）。
    /// @return `*this`，支持链式调用。
    /// @post 全部后继被加入跳转集合。
    MultiJump& to_all();

    /// @brief 清空所有已累积的跳转目标。
    /// @return `*this`，支持链式调用。
    /// @post 本次跳转不激活任何后继。
    MultiJump& reset() noexcept;

    /// @brief 按谓词选择所有满足条件的后继作为跳转目标。
    ///
    /// 与 `Jump::to_if` 不同：不是只选首个匹配，
    /// 而是选中所有满足谓词的后继。
    ///
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @post 所有满足条件的后继被加入跳转集合。
    template <predicate<TaskView> Pred>
    void to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 按索引查询后继名称，越界返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::string_view> name(std::size_t index) const noexcept;

    /// @brief 按名称查询后继索引，未找到返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::size_t> index(std::string_view name) const noexcept;

private:
    Work& m_work;                 ///< 绑定至宿主 MultiJumpWork
    SmallVector<Work*> m_targets; ///< 去重后的跳转集合；invoke 返回后 Executor 遍历做强制调度

    explicit MultiJump(Work& work) noexcept : m_work{work} {}

    /// 去重插入。
    void _insert(Work* w) {
        // Why: 典型扇出数 < 10，线性扫描去重优于 hash set 的常数开销。
        for (auto* t : m_targets) {
            if (t == w) return;
        }
        m_targets.push_back(w);
    }
};

// ============================================================
//  MultiJump 内联实现
// ============================================================

template <typename... Is>
    requires (sizeof...(Is) > 0) && (std::convertible_to<Is, std::size_t> && ...)
MultiJump& MultiJump::to(Is... indices) {
    auto succs = m_work._successors();
    const std::size_t sz = succs.size();

    // Why: IIFE + 折叠表达式在编译期展开变参，运行期零开销越界过滤。
    ([&](std::size_t idx) {
        if (idx < sz) _insert(succs[idx]);
    }(static_cast<std::size_t>(indices)), ...);

    return *this;
}

template <typename... Ns>
    requires (sizeof...(Ns) > 0) && (std::convertible_to<Ns, std::string_view> && ...)
MultiJump& MultiJump::to(Ns&&... names) {
    for (auto* suc : m_work._successors()) {
        // Why: 折叠表达式短路 OR——任一名称命中即终止比对，直接并入跳转集合。
        if (((suc->m_name == static_cast<std::string_view>(names)) || ...)) {
            _insert(suc);
        }
    }
    return *this;
}

inline MultiJump& MultiJump::to_all() {
    auto succs = m_work._successors();
    for (auto* suc : succs) {
        _insert(suc);
    }
    return *this;
}

inline MultiJump& MultiJump::reset() noexcept {
    m_targets.clear();
    return *this;
}

template <predicate<TaskView> Pred>
void MultiJump::to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    for (auto* suc : m_work._successors()) {
        if (std::invoke_r<bool>(pred, TaskView{*suc})) {
            _insert(suc);
        }
    }
}

inline std::size_t MultiJump::size() const noexcept {
    return m_work.m_num_successors;
}

inline std::optional<std::string_view> MultiJump::name(std::size_t index) const noexcept {
    auto succs = m_work._successors();
    return index < succs.size() ? std::optional{succs[index]->m_name} : std::nullopt;
}

inline std::optional<std::size_t> MultiJump::index(std::string_view name) const noexcept {
    auto succs = m_work._successors();
    for (std::size_t i = 0; i < succs.size(); ++i) {
        if (succs[i]->m_name == name) return i;
    }
    return std::nullopt;
}

}  // namespace tfl
