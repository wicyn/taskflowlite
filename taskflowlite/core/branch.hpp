/// @file branch.hpp
/// @brief 提供条件分支控制器，通过 join_counter 差额机制实现 DAG 内的动态路由。
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

/// @brief 单目标分支选择器。
///
/// 在 BranchWork 的 callable 内部使用，为用户提供运行时的后继选择能力。
/// 能够在不修改图物理拓扑的前提下，实现动态的 if/else 条件调度。
///
/// @details
/// **调度原理（差额机制）**
///
/// BranchWork 的每条后继边初始权重为 2（普通边为 1），通过差额控制调度：
///
/// ```
///    选中:   join_counter = 2 - 1(allow) - 1(tear_down) = 0 → 被调度
///    未选中: join_counter = 2 - 0        - 1(tear_down) = 1 → 阻塞
/// ```
///
/// 这种机制完全复用了 DAG 现有的原子计数器，无需引入额外的状态分支或
/// 特殊的 tear_down 路径。
///
/// **选择语义（Last-write-wins）**
///
/// - 支持按索引、名称、谓词三种选择方式
/// - 多次 `allow()` 调用以最后一次为准（覆盖前值）
/// - 不调用任何 `allow()` 则所有后继均被安全阻塞
/// - `reset()` 可显式取消当前选择
///
/// @pre 必须由 BranchWork::invoke 在 Worker 线程的栈上临时构造。
/// @note 禁止将此对象的引用逃逸到外部线程，其生命周期严格限定在 callable 执行期间。
class Branch : public MoveOnly<Branch> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 分支操作 ====================

    /// @brief 按索引显式选择后继。O(1)。
    /// @param index 目标后继在后继数组中的绝对位置。
    /// @return `*this`，支持链式调用。
    /// @post 若 index 在合法范围内，则选中对应后继；若发生越界，则安全清除当前选择（等价于 reset）。
    Branch& allow(std::size_t index) noexcept;

    /// @brief 按名称标识选择后继。O(N) 线性搜索。
    /// @param name 目标后继的静态名称标识。
    /// @return `*this`，支持链式调用。
    /// @post 首个名称匹配的后继被选中；若全图无匹配，则清除当前选择。
    /// @note 提供与图插入顺序无关的稳定选择机制。
    Branch& allow(std::string_view name) noexcept;

    /// @brief 按自定义谓词动态评估并选择首个满足条件的后继。
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`，支持链式调用。
    /// @post 首个令谓词返回 true 的后继被选中；若无匹配则清除选择。
    template <predicate<TaskView> Pred>
    Branch& allow_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    /// @brief 显式清除当前的后继选择。
    /// @return `*this`，支持链式调用。
    /// @post 内部目标指针置空，本次任务流转将不激活任何下游后继。
    Branch& reset() noexcept;

    // ==================== 查询接口 ====================

    /// @brief 获取当前分支节点所连接的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 根据给定索引安全查询后继节点的名称。
    /// @param index 欲查询的后继索引。
    /// @return 若索引有效则返回包装好的名称视图；越界则返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::string_view> name(std::size_t index) const noexcept;

    /// @brief 根据给定名称安全反查后继节点的索引。
    /// @param name 欲查询的后继名称。
    /// @return 若命中则返回对应索引；否则返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::size_t> index(std::string_view name) const noexcept;

private:
    Work&  m_work;           ///< 绑定至宿主 BranchWork，借此访问底层的关联边数据
    Work* m_target{nullptr}; ///< 暂存被选中的后继指针，供 invoke 结束后 Executor 执行额外的 join_counter 递减

    explicit Branch(Work& work) noexcept : m_work{work} {}

    // 禁用拷贝构造和拷贝赋值
    Branch(const Branch&) = delete;
    Branch& operator=(const Branch&) = delete;

    [[nodiscard]] bool _empty() const noexcept { return m_target == nullptr; }
    [[nodiscard]] Work* _target() const noexcept { return m_target; }
};

// ============================================================
//  Branch 内联实现
// ============================================================

inline Branch& Branch::allow(std::size_t index) noexcept {
    auto succs = m_work._successors();
    // Why: 规避异常抛出，越界时平滑回退为未选中状态，保障调度引擎的鲁棒性。
    m_target = (index < succs.size()) ? succs[index] : nullptr;
    return *this;
}

inline Branch& Branch::allow(std::string_view name) noexcept {
    m_target = nullptr;
    for (auto* suc : m_work._successors()) {
        if (suc->m_name == name) { m_target = suc; return *this; }
    }
    return *this;
}

template <predicate<TaskView> Pred>
Branch& Branch::allow_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    m_target = nullptr;
    for (auto* suc : m_work._successors()) {
        // Why: 强制包装一层只读的 TaskView 传入谓词，严格隔离底层 Work 节点的写入权限，
        // 防范用户在谓词求值期间意外篡改图结构。
        if (std::invoke_r<bool>(pred, TaskView{*suc})) { m_target = suc; return *this; }
    }
    return *this;
}

inline Branch& Branch::reset() noexcept {
    m_target = nullptr;
    return *this;
}

inline std::size_t Branch::size() const noexcept {
    return m_work.m_num_successors;
}

inline std::optional<std::string_view> Branch::name(std::size_t index) const noexcept {
    auto succs = m_work._successors();
    return index < succs.size() ? std::optional{succs[index]->m_name} : std::nullopt;
}

inline std::optional<std::size_t> Branch::index(std::string_view name) const noexcept {
    auto succs = m_work._successors();
    for (std::size_t i = 0; i < succs.size(); ++i) {
        if (succs[i]->m_name == name) return i;
    }
    return std::nullopt;
}


/// @brief 多目标分支选择器。
///
/// 在 MultiBranchWork 的 callable 内部使用。
/// 与普通 Branch 的互斥选择不同，MultiBranch 允许同时点亮多个下游链路，
/// 适用于表达广播（Broadcast）、多条件并发路由等复杂流控模式。
///
/// @details
/// **选择语义（Accumulative）**
///
/// - 所有的 `allow()` 调用均为 **累积** 生效，而非覆盖
/// - 内部自动去重：哪怕通过名字和索引反复选中同一后继，也不会造成调度计数器的重复扣减
/// - `allow_all()` 一键实现无差别广播
/// - `reset()` 清空全部已累积的放行意图
///
/// **存储与性能**
///
/// 使用 SmallVector 存储选中集合，小数量后继时避免堆分配。
/// 去重采用线性扫描——DAG 节点的典型扇出数极低（通常 < 4），
/// 线性扫描优于 hash set 的常数开销。
///
/// @pre 必须由 MultiBranchWork::invoke 在 Worker 线程的栈上临时构造。
/// @post 所有存留于内部集合的后继节点，将在 invoke 返回后各获得一次额外 join_counter 递减。
/// @note 禁止将此对象的引用逃逸到外部线程。
class MultiBranch : public MoveOnly<MultiBranch> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 分支操作 ====================

    /// @brief 按索引集合批量点亮后继。
    /// @tparam Is 可转换为 `std::size_t` 的变参索引类型包。
    /// @param indices 一组需放行的后继索引值。
    /// @return `*this`，支持链式调用。
    /// @post 将所有合法的索引对应节点纳入放行集合，越界索引自动丢弃。
    template <typename... Is>
        requires (sizeof...(Is) > 0) && (std::convertible_to<Is, std::size_t> && ...)
    MultiBranch& allow(Is... indices);

    /// @brief 按名称集合批量点亮后继。
    /// @tparam Ns 可转换为 `std::string_view` 的变参名称类型包。
    /// @param names 一组需放行的后继名称标识。
    /// @return `*this`，支持链式调用。
    /// @post 将所有名称匹配上的节点纳入放行集合。
    template <typename... Ns>
        requires (sizeof...(Ns) > 0) && (std::convertible_to<Ns, std::string_view> && ...)
    MultiBranch& allow(Ns&&... names);

    /// @brief 一键选中该节点挂载的所有下游后继（启动全图广播模式）。
    /// @return `*this`，支持链式调用。
    /// @post 全部后继被加入选择集合。
    MultiBranch& allow_all();

    /// @brief 清仓拦截，抛弃此前积累的所有放行意图。
    /// @return `*this`，支持链式调用。
    /// @post 本次分支不调度任何后继。
    MultiBranch& reset() noexcept;

    /// @brief 基于谓词断言执行筛选，批量点亮所有符合条件的后继。
    ///
    /// 与 `Branch::allow_if` 不同：不是只选首个匹配，
    /// 而是选中所有满足谓词的后继。
    ///
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @post 凡是促使谓词评估为 true 的节点均并入放行大盘。
    template <predicate<TaskView> Pred>
    void allow_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 按索引查询后继名称，越界返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::string_view> name(std::size_t index) const noexcept;

    /// @brief 按名称查询后继索引，未找到返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::size_t> index(std::string_view name) const noexcept;

private:
    Work& m_work;                 ///< 关联宿主 MultiBranchWork，用以映射底层图结构
    SmallVector<Work*> m_targets; ///< 内置轻量缓冲区的去重集合，供 invoke 返回后 Executor 遍历做额外递减

    explicit MultiBranch(Work& work) noexcept : m_work{work} {}

    /// 去重插入。
    void _insert(Work* w) {
        // Why: DAG 节点的典型扇出数极低（普遍 < 4）。
        // 在这种微缩量级下，朴素的线性扫描防碰撞反而能规避哈希计算的常数开销，达到性能的最优解。
        for (auto* t : m_targets) {
            if (t == w) return;
        }
        m_targets.push_back(w);
    }

    [[nodiscard]] bool _empty() const noexcept { return m_targets.empty(); }
    [[nodiscard]] std::span<Work* const> _targets() const noexcept { return m_targets; }
};

// ============================================================
//  MultiBranch 内联实现
// ============================================================

template <typename... Is>
    requires (sizeof...(Is) > 0) && (std::convertible_to<Is, std::size_t> && ...)
MultiBranch& MultiBranch::allow(Is... indices) {
    auto succs = m_work._successors();
    const std::size_t sz = succs.size();

    // Why: 结合 IIFE (立即调用函数表达式) 与 C++17 折叠表达式。
    // 在编译期扁平化展开变参模板，同时在运行期实现零成本的越界屏蔽机制。
    ([&](std::size_t idx) {
        if (idx < sz) _insert(succs[idx]);
    }(static_cast<std::size_t>(indices)), ...);

    return *this;
}

template <typename... Ns>
    requires (sizeof...(Ns) > 0) && (std::convertible_to<Ns, std::string_view> && ...)
MultiBranch& MultiBranch::allow(Ns&&... names) {
    for (auto* suc : m_work._successors()) {
        // Why: 将多字符串比较下推至逻辑短路 OR 门（折叠表达式）。
        // 一旦匹配上参数包内的任一名字即终止本轮比对，急速并入目标队列。
        if (((suc->m_name == static_cast<std::string_view>(names)) || ...)) {
            _insert(suc);
        }
    }
    return *this;
}

inline MultiBranch& MultiBranch::allow_all() {
    for (auto* suc : m_work._successors()) {
        _insert(suc);
    }
    return *this;
}

inline MultiBranch& MultiBranch::reset() noexcept {
    m_targets.clear();
    return *this;
}

template <predicate<TaskView> Pred>
void MultiBranch::allow_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    for (auto* suc : m_work._successors()) {
        if (std::invoke_r<bool>(pred, TaskView{*suc})) {
            _insert(suc);
        }
    }
}

inline std::size_t MultiBranch::size() const noexcept {
    return m_work.m_num_successors;
}

inline std::optional<std::string_view> MultiBranch::name(std::size_t index) const noexcept {
    auto succs = m_work._successors();
    return index < succs.size() ? std::optional{succs[index]->m_name} : std::nullopt;
}

inline std::optional<std::size_t> MultiBranch::index(std::string_view name) const noexcept {
    auto succs = m_work._successors();
    for (std::size_t i = 0; i < succs.size(); ++i) {
        if (succs[i]->m_name == name) return i;
    }
    return std::nullopt;
}

}  // namespace tfl
