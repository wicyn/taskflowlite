/// @file  branch.hpp
/// @brief 条件分支控制器 Branch / MultiBranch —— DAG 内的运行时动态路由。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <concepts>
#include "task.hpp"
#include "small_vector.hpp"

namespace tfl {

// ============================================================================
//  Branch —— 单目标分支选择器 (互斥 / 末次写入胜出)
// ============================================================================

/// @brief 单目标分支选择器 —— DAG 节点内的"if/else 路由权杖"。
///
/// 由 BranchWork::invoke 在 Worker 栈上构造，用户闭包通过 Branch& 形参拿
/// 到它。调用 select(i) / select_if(pred) 决定本次执行后哪个后继被放行。
/// 语义为 Last-Write-Wins（互斥单选），不被选中的后继不接收递减、自然阻塞。
/// 走独立的 _tear_down_branch_task 而非通用 _tear_down_task 路径。
///
/// @note 严格栈帧绑定：闭包返回即销毁，禁止逃逸到外部线程或异步上下文。
class Branch : public Immovable<Branch> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 下标赋值代理, 支持 branch[i] = true/false 语法。
    ///
    /// - = true  : 等价于 select(i)
    /// - = false : 等价于 unselect(i) (仅当前选中正好是 i 时清除)
    ///
    /// 这种"非对称的反向操作"是 Branch 单选语义的自然结果:
    /// 用户不能"取消别人的选择", 只能取消自己刚做的选择。
    class Proxy {
        friend class Branch;
        Branch&     m_br;
        std::size_t m_idx;
        Proxy(Branch& br, std::size_t idx) noexcept : m_br{br}, m_idx{idx} {}

    public:
        /// @brief 赋值: true 选中, false 条件清除。
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

    /// @brief 按索引显式选择后继, O(1)。
    /// @tparam I 整数类型 (排除 bool, 避免与隐式转换混淆)。
    /// @param index 目标后继在后继数组中的绝对位置 (0-based)。
    /// @return *this, 支持链式调用。
    /// @post 若 index 在合法范围内则选中对应后继; 越界则安全清除 (等价于 reset)。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& select(I index) noexcept;

    /// @brief 按谓词动态评估并选择首个满足条件的后继 (短路)。
    /// @tparam Pred 满足 predicate<TaskView> 概念的闭包类型。
    /// @param pred 接受只读 TaskView 并返回 bool 的可调用对象。
    /// @return *this, 支持链式调用。
    /// @post 首个令谓词返回 true 的后继被选中; 若无匹配则清除选择。
    template <predicate<TaskView> Pred>
    Branch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    /// @brief 条件清除当前选择 —— 仅当前选中正好是 i 时才生效。
    ///
    /// 与 reset() 的区别: reset() 无条件清空; unselect(i) 只在
    /// 当前选中的就是 i 时才清空, 其他情况静默无操作。
    ///
    /// 设计动机: 单选语义下, "取消第 i 项选择"只在你刚选了 i 时才有意义。
    /// 如果你已经 select(2) 之后又改成了 select(5), 再调 unselect(2)
    /// 不应该有任何影响 —— 否则就破坏了 last-write-wins 语义。
    ///
    /// @tparam I 整数类型 (排除 bool)。
    /// @param index 要条件清除的索引。
    /// @return *this, 支持链式调用。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& unselect(I index) noexcept;

    /// @brief 无条件清除当前选择。
    /// @return *this, 支持链式调用。
    /// @post m_target = nullptr, 本次分支不调度任何后继。
    Branch& reset() noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法: branch(i) —— 等价 branch.select(i)。
    ///
    /// 设计动机:
    /// - 与 MultiBranch::operator() 形成对偶, API 风格一致
    /// - 比 branch.select(i) 更短, 便于在简单条件分支里使用
    /// - 单参数: Branch 是互斥单选, 多参数没有语义
    ///
    /// @tparam I 整数类型 (排除 bool)。
    /// @param index 目标后继索引。
    /// @return *this, 支持链式调用。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& operator()(I index) noexcept;

    // ==================== 下标语法 ====================

    /// @brief 下标赋值: branch[i] = true 选中, branch[i] = false 条件清除。
    /// @param index 后继索引 (0-based)。
    /// @return 赋值代理对象 Proxy。
    [[nodiscard]] Proxy operator[](std::size_t index) noexcept;

    // ==================== 查询接口 ====================

    /// @brief 获取当前分支节点所连接的后继总数。
    /// @return 出边数量 (即 precede 调用次数)。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work& m_work;
    /// @brief 暂存被选中的后继指针, 供 invoke 结束后 Executor 执行额外的 join_counter 递减。
    Work* m_target{nullptr};

    explicit Branch(Work& work) noexcept : m_work{work} {}
};

// ============================================================
//  Branch 内联实现
// ============================================================

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
        if (std::invoke_r<bool>(pred, TaskView{*m_work.m_edges[i]})) {
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
//  MultiBranch —— 多目标分支选择器 (累积 / 并发广播)
// ============================================================================

/// @brief 多目标分支选择器 —— "广播 / 多条件并发路由"权杖。
///
/// 与 Branch 互斥选择不同，MultiBranch 允许同时点亮 N 条下游链路。
/// 多次 select 累积生效，内部用 SmallVector<Work*> 去重集合（扇出低时
/// 线性扫描优于哈希）。走独立的 _tear_down_multi_branch_task 路径。
///
/// @note 严格栈帧绑定：闭包返回即销毁，禁止逃逸。
class MultiBranch : public Immovable<MultiBranch> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 单索引赋值代理: mb[i] = true/false。
    ///
    /// - = true  : 等价于 select(i) (加入放行集合, 去重)
    /// - = false : 等价于 unselect(i) (从放行集合移除)
    /// - 越界索引: 静默忽略 (不抛异常, 符合"宽松边界"的实用主义)
    class Proxy {
        friend class MultiBranch;
        MultiBranch& m_mb;
        std::size_t  m_idx;
        Proxy(MultiBranch& mb, std::size_t idx) noexcept : m_mb{mb}, m_idx{idx} {}

    public:
        /// @brief 赋值: true 加入, false 移除 (越界静默忽略)。
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

    /// @brief 单索引下标: mb[i] = true/false。
    /// @tparam I 整数类型 (排除 bool)。
    /// @param index 后继索引。
    /// @return 赋值代理对象。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    [[nodiscard]] Proxy operator[](I index) noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法: mb(i, j, k) —— 等价 mb.select(i, j, k)。
    ///
    /// 与 Branch::operator() 对偶, 但接受变参 (MultiBranch 是累积式)。
    ///
    /// @note 仅累积式插入, 不会清除已有选择。需要重置请先 reset()。
    ///
    /// @tparam Is 整数索引类型 (至少一个, 排除 bool)。
    /// @param indices 要加入放行集合的索引列表。
    /// @return *this, 支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& operator()(Is... indices);

    // ==================== 选择动作 ====================

    /// @brief 按索引集批量加入放行集合。
    /// @tparam Is 可转换为 std::size_t 的变参索引类型 (至少一个, 排除 bool)。
    /// @param indices 一组要放行的后继索引。
    /// @return *this, 支持链式调用。
    /// @post 所有有效索引对应的后继被加入放行集合, 越界索引自动忽略, 重复索引去重。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& select(Is... indices);

    /// @brief 按索引集批量从放行集合移除 (select 的反向动作)。
    ///
    /// 与 select(...) 完全对偶:
    /// - select(0, 2)   -> 把 0/2 加入放行集合
    /// - unselect(0, 2) -> 把 0/2 从放行集合移除
    ///
    /// 不在集合中的索引、越界索引均静默忽略, 不抛异常。
    ///
    /// @tparam Is 整数索引类型 (至少一个, 排除 bool)。
    /// @param indices 一组要从放行集合移除的索引。
    /// @return *this, 支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& unselect(Is... indices) noexcept;

    /// @brief 一键选中所有下游后继 (广播模式)。
    /// @return *this, 支持链式调用。
    /// @post 全部后继被加入选择集合。
    MultiBranch& select_all();

    /// @brief 一键移除所有下游后继 (select_all 的反向动作)。
    /// 语义上等价于 reset(): 后者作为本函数的别名保留, 用于跨 Branch/
    /// MultiBranch 两类提供统一的"清空"动词 (单选的 Branch 无 all 概念)。
    ///
    /// @return *this, 支持链式调用。
    /// @post m_targets 为空, 本次分支不调度任何后继。
    MultiBranch& unselect_all() noexcept;

    /// @brief 清空所有放行意图 —— unselect_all() 的语义别名。
    MultiBranch& reset() noexcept;

    /// @brief 基于谓词批量点亮所有符合条件的后继。
    ///
    /// 与 Branch::select_if 不同: 不是只选首个匹配, 而是选中所有满足谓词的后继。
    ///
    /// @tparam Pred 满足 predicate<TaskView> 概念的闭包类型。
    /// @param pred 接受只读 TaskView 并返回 bool 的可调用对象。
    /// @return *this, 支持链式调用。
    /// @post 凡是促使谓词评估为 true 的节点均并入放行集合。
    template <predicate<TaskView> Pred>
    MultiBranch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    /// @return 出边数量。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work&               m_work;
    /// @brief 内置轻量缓冲区的去重集合, 供 invoke 返回后 Executor 遍历做额外递减。
    SmallVector<Work*>  m_targets;

    explicit MultiBranch(Work& work) noexcept : m_work{work} {}

    /// @brief 去重插入 —— 线性扫描优于哈希常数开销 (扇出极低时)。
    /// @param w 待插入的后继 Work 指针。
    void _insert(Work* w) {
        if (m_targets.size() >= m_work.m_num_successors) return;
        for (auto* t : m_targets) {
            if (t == w) return;
        }
        m_targets.push_back(w);
    }

    /// @brief 线性查找移除 —— swap-and-pop, O(1) 删除, 顺序无关。
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

// ============================================================
//  MultiBranch 内联实现
// ============================================================

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
    /// @brief IIFE + 折叠表达式, 编译期展开变参, 运行期零成本越界屏蔽。
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
        if (std::invoke_r<bool>(pred, TaskView{*m_work.m_edges[i]})) {
            _insert(m_work.m_edges[i]);
        }
    }
    return *this;
}

inline std::size_t MultiBranch::size() const noexcept {
    return m_work.m_num_successors;
}

}  // namespace tfl
