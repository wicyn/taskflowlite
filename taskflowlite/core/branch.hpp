/// @file branch.hpp
/// @brief 条件分支控制器 Branch / MultiBranch —— DAG 内的运行时动态路由。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <concepts>
#include "task.hpp"
#include "small_vector.hpp"

namespace tfl {

// ============================================================================
//  Branch —— 单目标分支选择器(互斥 / Last-Write-Wins)
// ============================================================================

/// @brief 单目标分支选择器 —— DAG 节点内的"if/else 路由权杖"。
///
/// @details
/// `Branch` 是 `BranchWork::invoke` 在 worker 栈上为用户构造的**临时控制凭据**:
/// 用户的闭包通过形参 `Branch&` 拿到它,调用 `select(i)` / `select_if(pred)` 决定
/// 本次执行后哪个后继被放行。它不持有数据、不分配内存,仅是 `Work` 后继边表的
/// 一个语义代理。
///
/// ============================================================================
///  调度原理 —— 独立 tear_down 分流
/// ============================================================================
/// `BranchWork` 不走通用的 `_tear_down_task` 路径，而是由 `_tear_down_branch_task`
/// 单独处理 —— Executor 在 invoke 结束时拿到用户选中的 `m_target`，直接对其
/// `join_counter` 做一次 `fetch_sub(1)`（或 `store(0)` 快路径），不被选中的
/// 后继不接收本次递减，自然阻塞。
///
/// 与 `Jump` 的本质区别：
/// - Branch 的选中后继仍走 `fetch_sub` 计数协议，只是"谁被选中谁收到这一票"；
/// - Jump 的选中后继绕过计数协议，`store(0)` 强制归零，无视其他前驱是否到齐。
///
/// 这种设计完全复用 DAG 现有的原子计数器，不需要新的状态字段，只是由独立的
/// handler 实现"选择性传票"。
///
/// ============================================================================
///  选择语义(Last-Write-Wins / 互斥)
/// ============================================================================
/// - 多次 `select()` / `branch[i] = true` / `branch(i)` 以最后一次为准(覆盖前值);
/// - 不调用任何 select → 所有后继未被选中，`_tear_down_branch_task` 不触发递减，安全阻塞；
/// - `reset()` 显式清除,等价于"什么都没选";
/// - `unselect(i)` 条件清除:仅当前选中正好是 i 时才生效;
/// - `select_if(pred)` 短路:首个匹配即停。
///
/// ============================================================================
///  四种入口语法 —— 与 MultiBranch 完全对偶
/// ============================================================================
/// @code
///   // 1) 单索引下标:动态状态机风格
///   branch[i] = true;       // 选中第 i 个
///   branch[i] = false;      // 等价 unselect(i)
///
///   // 2) 函数调用:select 的简写
///   branch(2);              // 等价 branch.select(2)
///
///   // 3) 显式动作:严肃代码风格
///   branch.select(2);
///   branch.unselect(3);     // 反向动作:仅当前选中正好是 3 时才清除
///   branch.reset();         // 完全清空
///
///   // 4) 谓词驱动
///   branch.select_if([](auto tv){ return tv.name() == "fast"; });
/// @endcode
///
/// 与 `MultiBranch::operator()` 不同:这里只接受**单个**索引参数 ——
/// `Branch` 是互斥单选,变参没有意义。
///
/// ============================================================================
///  生命周期约束 —— 严格栈帧绑定
/// ============================================================================
/// `Branch` 由 `BranchWork::invoke` 在 worker 栈上构造,传引用给用户闭包。
/// 闭包返回即销毁。**禁止逃逸** 到外部线程或异步上下文 —— 一旦 invoke 返回,
/// 持有它的 `Work*` 进入 tear_down,行为未定义。这与 `Runtime` 的栈帧约束相同。
///
/// @pre 由 BranchWork::invoke 构造,传引用给闭包,不可逃逸。
/// @see Jump          强制清零的"抢占式"对偶
/// @see MultiBranch   多目标累积版
/// @see BranchWork    宿主节点类型
class Branch : public Immovable<Branch> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 下标赋值代理,支持 `branch[i] = true/false` 语法。
    ///
    /// @details
    /// - `= true`:等价于 `select(i)`;
    /// - `= false`:等价于 `unselect(i)`(仅当前选中正好是 i 时清除)。
    ///
    /// 这种"非对称的反向操作"是 Branch 单选语义的自然结果:
    /// 用户不能"取消别人的选择",只能取消自己刚做的选择。
    class Proxy {
        friend class Branch;
        Branch&     m_br;
        std::size_t m_idx;
        Proxy(Branch& br, std::size_t idx) noexcept : m_br{br}, m_idx{idx} {}

    public:
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

    /// @brief 按索引显式选择后继。O(1)。
    /// @param index 目标后继在后继数组中的绝对位置。
    /// @return `*this`,支持链式调用。
    /// @post 若 index 在合法范围内,则选中对应后继;越界则安全清除(等价于 reset)。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& select(I index) noexcept;

    /// @brief 按谓词动态评估并选择**首个**满足条件的后继(短路)。
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`,支持链式调用。
    /// @post 首个令谓词返回 true 的后继被选中;若无匹配则清除选择。
    template <predicate<TaskView> Pred>
    Branch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    /// @brief 条件清除当前选择 —— 仅当前选中正好是 i 时才生效。
    ///
    /// @details
    /// 与 `reset()` 的区别:`reset()` 无条件清空;`unselect(i)` 只在
    /// **当前选中的就是 i** 时才清空,其他情况静默无操作。
    ///
    /// 设计动机:
    /// 单选语义下,"取消第 i 项选择"只在你刚选了 i 时才有意义。如果你已经
    /// `select(2)` 之后又改成了 `select(5)`,再调 `unselect(2)` 不应该有任何
    /// 影响 —— 否则就破坏了 last-write-wins 语义。
    ///
    /// @param index 要条件清除的索引。
    /// @return `*this`,支持链式调用。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& unselect(I index) noexcept;

    /// @brief 无条件清除当前选择。
    /// @return `*this`,支持链式调用。
    /// @post `m_target = nullptr`,本次分支不调度任何后继。
    Branch& reset() noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法:`branch(i)` —— 等价 `branch.select(i)`。
    ///
    /// @details
    /// 设计动机:
    /// - 与 `MultiBranch::operator()` 形成对偶,API 风格一致;
    /// - 比 `branch.select(i)` 更短,便于在简单条件分支里使用;
    /// - 单参数:Branch 是互斥单选,多参数没有语义。
    ///
    /// @code
    ///   // 等价的三种写法
    ///   branch(2);
    ///   branch.select(2);
    ///   branch[2] = true;
    /// @endcode
    ///
    /// @return `*this`,支持链式调用(尽管 last-write-wins 下链式意义不大)。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& operator()(I index) noexcept;

    // ==================== 下标语法 ====================

    /// @brief 下标赋值:`branch[i] = true` 选中,`branch[i] = false` 条件清除。
    /// @param index 后继索引。
    /// @return 赋值代理对象。
    [[nodiscard]] Proxy operator[](std::size_t index) noexcept;

    // ==================== 查询接口 ====================

    /// @brief 获取当前分支节点所连接的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work& m_work;
    Work* m_target{nullptr};   ///< 暂存被选中的后继指针,供 invoke 结束后 Executor 执行额外的 join_counter 递减

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
//  MultiBranch —— 多目标分支选择器(累积 / 并发广播)
// ============================================================================

/// @brief 多目标分支选择器 —— "广播 / 多条件并发路由"权杖。
///
/// @details
/// 与 `Branch` 互斥选择不同,`MultiBranch` 允许同时点亮 N 条下游链路 ——
/// 走独立的 `_tear_down_multi_branch_task`，遍历 `m_targets` 集合，每个被选中
/// 的后继各获得一次 `fetch_sub(1)`。调度协议与 `Branch` 完全一致，仅 select
/// 集合从单个指针变为 `SmallVector<Work*>` 去重集合。
///
/// ============================================================================
///  累积语义(Accumulative,与 Branch 的覆盖语义对照)
/// ============================================================================
/// - 多次 `select(...)` / `mb(i, j, k)` / `mb[i] = true` **累积** 生效;
/// - 内部用 `SmallVector<Work*>` 存放选中集,`_insert` 线性查重;
/// - `select_all()` 一键全选(广播);
/// - `unselect(...)` 从放行集合移除指定索引(`select` 的反向动作);
/// - `reset()` 清空全部累积;
/// - `select_if(pred)` 选 **所有** 满足谓词的(不像 Branch::select_if 只选首个)。
///
/// ============================================================================
///  存储选型 —— 为什么用 SmallVector 不用 hash set?
/// ============================================================================
/// DAG 节点的典型扇出数极低(业内统计普遍 < 4,少数 < 10)。
/// 在这个规模上:
/// - hash set 的常数开销(哈希计算 + 桶访问 + 链表跳转)远大于 4 次 ptr 比较;
/// - SmallVector 内置小缓冲区避免堆分配;
/// - 删除走 swap-with-last,O(1) 且不碰内存分配器。
///
/// 这是典型的"小集合优化":**不要为渐近复杂度妥协常数因子**。
///
/// ============================================================================
///  四种入口语法 —— 与 Branch 对偶
/// ============================================================================
/// @code
///   // 1) 单索引下标:逐边动态决定状态(true/false 双向)
///   mb[0] = true;
///   mb[1] = false;
///
///   // 2) 函数调用:select 的简写,**变参累积**
///   mb(0, 1, 2);            // 等价 mb.select(0, 1, 2)
///   mb(0)(1)(2);            // 链式累积也合法
///
///   // 3) 显式动作:严肃代码风格
///   mb.select(0, 1, 2);
///   mb.unselect(1, 3);      // 从放行集合移除
///   mb.select_all();
///   mb.reset();
///
///   // 4) 谓词驱动
///   mb.select_if([](auto tv){ return tv.name().starts_with("ok_"); });
/// @endcode
///
/// 与 `Branch::operator()` 不同:此处接受**变参**,因为 MultiBranch 是累积式 ——
/// 多个索引一次性加入选中集合。
///
/// @pre 由 MultiBranchWork::invoke 构造,传引用给闭包,不可逃逸。
/// @see Branch       单目标互斥版
/// @see MultiJump    "强制清零"对偶版
/// @see MultiBranchWork  宿主节点类型
class MultiBranch : public Immovable<MultiBranch> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 单索引赋值代理:`mb[i] = true/false`。
    ///
    /// @details
    /// - `= true`:等价于 `select(i)`(加入放行集合,去重);
    /// - `= false`:等价于 `unselect(i)`(从放行集合移除);
    /// - 越界索引:静默忽略(不抛异常,符合"宽松边界"的实用主义)。
    class Proxy {
        friend class MultiBranch;
        MultiBranch& m_mb;
        std::size_t  m_idx;
        Proxy(MultiBranch& mb, std::size_t idx) noexcept : m_mb{mb}, m_idx{idx} {}

    public:
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

    /// @brief 单索引下标:`mb[i] = true/false`。
    /// @return 赋值代理对象。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    [[nodiscard]] Proxy operator[](I index) noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法:`mb(i, j, k)` —— 等价 `mb.select(i, j, k)`。
    ///
    /// @details
    /// 与 `Branch::operator()` 对偶,但接受**变参**(MultiBranch 是累积式)。
    ///
    /// @code
    ///   mb(0, 2, 5);                       // 选中索引 0/2/5(累积)
    ///   mb(0)(2)(5);                       // 链式调用,等价上面
    ///   mb.reset()(0, 2, 5);               // 先清空再选
    /// @endcode
    ///
    /// @note 仅累积式插入,不会清除已有选择。需要重置请先 `reset()`。
    ///
    /// @return `*this`,支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& operator()(Is... indices);

    // ==================== 选择动作 ====================

    /// @brief 按索引集批量加入放行集合。
    /// @tparam Is 可转换为 `std::size_t` 的变参索引类型。
    /// @param indices 一组要放行的后继索引。
    /// @return `*this`,支持链式调用。
    /// @post 所有有效索引对应的后继被加入放行集合,越界索引自动忽略,重复索引去重。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& select(Is... indices);

    /// @brief 按索引集批量从放行集合移除(`select` 的反向动作)。
    ///
    /// @details
    /// 与 `select(...)` 完全对偶:
    /// - `select(0, 2)` → 把 0/2 加入放行集合;
    /// - `unselect(0, 2)` → 把 0/2 从放行集合移除。
    ///
    /// 不在集合中的索引、越界索引均静默忽略,不抛异常。
    ///
    /// @param indices 一组要从放行集合移除的索引。
    /// @return `*this`,支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& unselect(Is... indices) noexcept;

    /// @brief 一键选中所有下游后继(广播模式)。
    /// @return `*this`,支持链式调用。
    /// @post 全部后继被加入选择集合。
    MultiBranch& select_all();

    /// @brief 清空所有放行意图。
    /// @return `*this`,支持链式调用。
    /// @post 本次分支不调度任何后继。
    MultiBranch& reset() noexcept;

    /// @brief 基于谓词批量点亮所有符合条件的后继。
    ///
    /// @details
    /// 与 `Branch::select_if` 不同:不是只选首个匹配,而是选中所有满足谓词的后继。
    ///
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`,支持链式调用。
    /// @post 凡是促使谓词评估为 true 的节点均并入放行集合。
    template <predicate<TaskView> Pred>
    MultiBranch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work&               m_work;
    SmallVector<Work*>  m_targets;   ///< 内置轻量缓冲区的去重集合,供 invoke 返回后 Executor 遍历做额外递减

    explicit MultiBranch(Work& work) noexcept : m_work{work} {}

    /// @brief 去重插入。
    /// @details DAG 节点典型扇出数极低(普遍 < 4),线性扫描优于哈希常数开销。
    void _insert(Work* w) {
        if (m_targets.size() >= m_work.m_num_successors) return;
        for (auto* t : m_targets) {
            if (t == w) return;
        }
        m_targets.push_back(w);
    }

    /// @brief 线性查找移除(swap-and-pop,O(1) 删除,顺序无关)。
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
    // Why: IIFE + 折叠表达式,编译期展开变参,运行期零成本越界屏蔽。
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

inline MultiBranch& MultiBranch::reset() noexcept {
    m_targets.clear();
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


