/// @file branch.hpp
/// @brief 条件分支控制器 Branch / MultiBranch —— DAG 内的运行时动态路由
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <concepts>
#include <initializer_list>
#include <span>
#include "task.hpp"
#include "small_vector.hpp"

namespace tfl {

/// @brief 单目标分支选择器 —— DAG 节点内的"if/else 路由权杖"。
///
/// @details
/// `Branch` 是 `BranchWork::invoke` 在 worker 栈上为用户构造的 **临时控制凭据**：
/// 用户的闭包通过形参 `Branch&` 拿到它，调用 `select(i)` / `select_if(pred)` 决定
/// 本次执行后哪个后继被放行。它不持有数据、不分配内存，仅是 `Work` 后继边表的
/// 一个语义代理。
///
/// ============================================================================
///  调度原理 —— 差额机制（Weight-Difference Trick）
/// ============================================================================
/// `BranchWork` 出边的初始 `join_weight` 是 **2**（普通边为 1），分支语义靠这一字
/// 之差表达：
///
/// @code
///    被选中:   后继 join_counter = 2 - 1(select)  - 1(tear_down) = 0  → 被调度
///    未选中:   后继 join_counter = 2 -    0       - 1(tear_down) = 1  → 等待其他前驱
/// @endcode
///
/// 即"未选中"的后继并非永久阻塞，仍有其他前驱来填那 1 的差额；这与 `Jump`
/// 的"强制清零"形成本质对比 —— Branch 走的是**协作式语义**，Jump 是**抢占式**。
///
/// 这种设计的工程美感在于：**完全复用 DAG 现有的原子计数器**，不需要新的状态
/// 字段、不需要分支特化的 tear_down 路径，只是把边权调成 2 而已。
///
/// ============================================================================
///  选择语义（Last-Write-Wins）
/// ============================================================================
/// - 多次 `select()` / `operator[]=` 以最后一次为准（覆盖前值）；
/// - 不调用任何 select → 所有后继被安全阻塞（差额永远未填平）；
/// - `reset()` 显式清除，等价于"什么都没选"；
/// - `select_if(pred)` 短路：首个匹配即停。
///
/// ============================================================================
///  下标代理 Proxy —— 让 select 长成 `branch[i] = true`
/// ============================================================================
/// `Proxy` 是 `operator[]` 返回的零成本包装，仅持 `Branch&` + 索引。
/// `branch[i] = true` 等价 `select(i)`，`branch[i] = false` 在"当前选中即为 i"
/// 时取消（语义对称）。这是 Modern C++ 里"赋值即调用"的常见手法。
///
/// ============================================================================
///  生命周期约束 —— 严格栈帧绑定
/// ============================================================================
/// `Branch` 由 `BranchWork::invoke` 在 worker 栈上构造，传引用给用户闭包。
/// 闭包返回即销毁。**禁止逃逸**到外部线程或异步上下文 —— 一旦 invoke 返回，
/// 持有它的 `Work*` 进入 tear_down，行为未定义。这与 `Runtime` 的栈帧约束相同。
///
/// @pre 由 BranchWork::invoke 构造，传引用给闭包，不可逃逸。
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

    /// @brief 下标赋值代理，支持 `branch[i] = true/false` 语法。
    class Proxy {
        friend class Branch;
        Branch& m_br;
        std::size_t m_idx;
        Proxy(Branch& br, std::size_t idx) noexcept : m_br{br}, m_idx{idx} {}
    public:
        /// @brief `branch[i] = true` 选中第 i 个后继；`= false` 若当前选中即为 i 则清除。
        Branch& operator=(bool on) noexcept {
            if (on) {
                m_br.select(m_idx);
            } else if (m_idx < m_br.m_work.m_num_successors
                       && m_br.m_target == m_br.m_work.m_edges[m_idx]) {
                m_br.m_target = nullptr;
            }
            return m_br;
        }
    };

    // ==================== 分支操作 ====================

    /// @brief 按索引显式选择后继。O(1)。
    /// @param index 目标后继在后继数组中的绝对位置。
    /// @return `*this`，支持链式调用。
    /// @post 若 index 在合法范围内，则选中对应后继；越界则安全清除（等价于 reset）。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Branch& select(I index) noexcept;

    /// @brief 按自定义谓词动态评估并选择首个满足条件的后继。
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`，支持链式调用。
    /// @post 首个令谓词返回 true 的后继被选中；若无匹配则清除选择。
    template <predicate<TaskView> Pred>
    Branch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    /// @brief 显式清除当前的后继选择。
    /// @return `*this`，支持链式调用。
    Branch& reset() noexcept;

    /// @brief 下标赋值：`branch[i] = true` 选中，`branch[i] = false` 条件清除。
    /// @param index 后继索引。
    /// @return 赋值代理对象。
    [[nodiscard]] Proxy operator[](std::size_t index) noexcept;

    // ==================== 查询接口 ====================

    /// @brief 获取当前分支节点所连接的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work& m_work;           ///< 绑定至宿主 BranchWork，借此访问底层的关联边数据
    Work* m_target{nullptr}; ///< 暂存被选中的后继指针，供 invoke 结束后 Executor 执行额外的 join_counter 递减

    explicit Branch(Work& work) noexcept : m_work{work} {}

    Branch(const Branch&) = delete;
    Branch& operator=(const Branch&) = delete;

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

inline Branch& Branch::reset() noexcept {
    m_target = nullptr;
    return *this;
}

inline Branch::Proxy Branch::operator[](std::size_t index) noexcept {
    return {*this, index};
}

inline std::size_t Branch::size() const noexcept {
    return m_work.m_num_successors;
}

/// @brief 多目标分支选择器 —— "广播 / 多条件并发路由"权杖。
///
/// @details
/// 与 `Branch` 互斥选择不同，`MultiBranch` 允许同时点亮 N 条下游链路 ——
/// 每个被选中的后继都获得一次额外 `join_counter -= 1`。底层调度协议复用
/// 同样的差额机制（边权 2），仅 select 集合从单个变为去重集合。
///
/// ============================================================================
///  累积语义（Accumulative，与 Branch 的覆盖语义对照）
/// ============================================================================
/// - 多次 `select(...)` / `operator[]=true` **累积** 生效；
/// - 内部用 `SmallVector<Work*>` 存放选中集，`_insert` 线性查重；
/// - `select_all()` 一键全选（广播）；
/// - `reset()` 清空全部累积；
/// - `select_if(pred)` 选 **所有** 满足谓词的（不像 Branch::select_if 只选首个）。
///
/// ============================================================================
///  存储选型 —— 为什么用 SmallVector 不用 hash set？
/// ============================================================================
/// DAG 节点的典型扇出数极低（业内统计普遍 < 4，少数 < 10）。
/// 在这个规模上：
/// - hash set 的常数开销（哈希计算 + 桶访问 + 链表跳转）远大于 4 次 ptr 比较；
/// - SmallVector 内置小缓冲区避免堆分配；
/// - 删除走 swap-with-last，O(1) 且不碰内存分配器。
///
/// 这是典型的"小集合优化"：**不要为渐近复杂度妥协常数因子**。
///
/// ============================================================================
///  下标语法 —— 单 / 多索引双形态
/// ============================================================================
/// @code
///   mb[5]       = true;                  // 单索引：Proxy
///   mb[1, 2, 3] = {false, true, false};  // 多索引：MultiProxy<3>
/// @endcode
/// `MultiProxy<N>` 把索引存进 `std::size_t[N]` 原生数组（N 编译期已知，
/// 零额外开销），赋值时编译期强制 `bool[N]` 等长 —— 长度不一致直接编译错。
///
/// @pre 由 MultiBranchWork::invoke 构造，传引用给闭包，不可逃逸。
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

    /// @brief 单索引赋值代理：`mb[i] = true/false`。
    class Proxy {
        friend class MultiBranch;
        MultiBranch& m_mb;
        std::size_t m_idx;
        Proxy(MultiBranch& mb, std::size_t idx) noexcept : m_mb{mb}, m_idx{idx} {}
    public:
        /// @brief `= true` 加入放行集合；`= false` 移除。越界静默忽略。
        MultiBranch& operator=(bool on) {
            if (m_idx >= m_mb.m_work.m_num_successors) return m_mb;
            Work* w = m_mb.m_work.m_edges[m_idx];
            if (on) m_mb._insert(w);
            else    m_mb._erase(w);
            return m_mb;
        }
    };

    /// @brief 多索引赋值代理：`mb[i, j, k] = {b0, b1, b2}`，编译期强制个数一致。
    /// @tparam N 索引个数，由 operator[] 变参包大小推导，编译期固定。
    template <std::size_t N>
    class MultiProxy {
        friend class MultiBranch;
        MultiBranch& m_mb;
        std::size_t m_indices[N]; ///< 原生 C 数组，N 编译期已知，零额外开销

        template <typename... Is>
        MultiProxy(MultiBranch& mb, Is... indices) noexcept
            : m_mb{mb}, m_indices{static_cast<std::size_t>(indices)...} {}

    public:
        /// @brief `mb[1, 2, 3] = {false, true, false}` — 编译期强制个数一致。
        template <std::size_t M>
            requires (M == N)
        MultiBranch& operator=(const bool (&mask)[M]) {
            const std::size_t sz = m_mb.m_work.m_num_successors;
            for (std::size_t i = 0; i < N; ++i) {
                if (m_indices[i] >= sz) continue;
                Work* w = m_mb.m_work.m_edges[m_indices[i]];
                if (mask[i]) m_mb._insert(w);
                else         m_mb._erase(w);
            }
            return m_mb;
        }
    };

    // ==================== 下标操作 ====================
    /// @brief 单索引：`mj[i] = true/false`。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    [[nodiscard]] Proxy operator[](I index) noexcept;

    /// @brief 多索引（≥2）：`mj[i, j, k] = {b0, b1, b2}`。
    template <typename... Is>
        requires (sizeof...(Is) > 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    [[nodiscard]] MultiProxy<sizeof...(Is)> operator[](Is... indices) noexcept;

    // ==================== 跳转操作 ====================

    /// @brief 按索引集批量选择跳转目标。
    /// @tparam Is 可转换为 `std::size_t` 的变参索引类型。
    /// @param indices 一组要跳转的后继索引。
    /// @return `*this`，支持链式调用。
    /// @post 所有有效索引对应的后继被加入跳转集合，越界索引自动忽略。
    template <typename... Is>
        requires (sizeof...(Is) > 0)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiBranch& select(Is... indices);


    /// @brief 一键选中所有下游后继（广播模式）。
    /// @return `*this`，支持链式调用。
    /// @post 全部后继被加入选择集合。
    MultiBranch& select_all();

    /// @brief 清空所有放行意图。
    /// @return `*this`，支持链式调用。
    /// @post 本次分支不调度任何后继。
    MultiBranch& reset() noexcept;

    /// @brief 基于谓词批量点亮所有符合条件的后继。
    ///
    /// 与 `Branch::select_if` 不同：不是只选首个匹配，
    /// 而是选中所有满足谓词的后继。
    ///
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @post 凡是促使谓词评估为 true 的节点均并入放行集合。
    template <predicate<TaskView> Pred>
    MultiBranch& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work&               m_work;                 ///< 关联宿主 MultiBranchWork，用以映射底层图结构
    SmallVector<Work*>  m_targets; ///< 内置轻量缓冲区的去重集合，供 invoke 返回后 Executor 遍历做额外递减

    explicit MultiBranch(Work& work) noexcept : m_work{work} {}

    /// 去重插入。
    /// Why: DAG 节点的典型扇出数极低（普遍 < 4），线性扫描优于哈希的常数开销。
    void _insert(Work* w) {
        if (m_targets.size() >= m_work.m_num_successors) return;
        for (auto* t : m_targets) {
            if (t == w) return;
        }
        m_targets.push_back(w);
    }

    /// 线性查找移除（swap-and-pop，O(1) 删除，顺序无关）。
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
    requires (sizeof...(Is) > 1)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
inline MultiBranch::MultiProxy<sizeof...(Is)> MultiBranch::operator[](Is... indices) noexcept {
    return {*this, indices...};
}

template <typename... Is>
    requires (sizeof...(Is) > 0)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
inline MultiBranch& MultiBranch::select(Is... indices) {
    const std::size_t sz = m_work.m_num_successors;
    // Why: IIFE + 折叠表达式，编译期展开变参，运行期零成本越界屏蔽。
    ([&](std::size_t idx) {
        if (idx < sz) _insert(m_work.m_edges[idx]);
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
MultiBranch& MultiBranch::select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
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
