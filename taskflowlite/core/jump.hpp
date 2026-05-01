/// @file jump.hpp
/// @brief 无条件跳转控制器 Jump / MultiJump —— 绕过 DAG 计数器的强制路由
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

/// @brief 单目标跳转控制器 —— DAG 内"无条件 GOTO"权杖。
///
/// @details
/// `Jump` 与 `Branch` 是控制流双子星：同样在 worker 栈上由宿主 Work 构造、传给
/// 用户闭包决定后继路由，但 **路由协议截然相反**。
///
/// ============================================================================
///  Jump vs Branch —— 协作式 vs 抢占式
/// ============================================================================
///
/// | 维度        | `Branch`                          | `Jump`                            |
/// |-------------|-----------------------------------|-----------------------------------|
/// | 边权重      | 2（差额机制）                      | 0（不贡献依赖）                    |
/// | 路由方式    | join_counter -= 1（差额）         | join_counter = 0（强制清零）       |
/// | 未被选中    | 仍可被其他前驱触发                  | 完全绕过常规依赖                   |
/// | 自环        | 禁止                              | 唯一允许自环的节点类型             |
/// | 拓扑校验    | 严格 DAG 检测                      | jump 节点参与的边不计入环检测      |
/// | 适用语义    | "条件分支"（if/else）              | "强制跳转"（goto / state-machine） |
///
/// 关键差异在 **`_tear_down_jump_task` 直接把 target 的 join_counter 置零并立即
/// 入队**，绕过了"等所有前驱完成"的协议。这让 Jump 成为状态机循环、错误重试、
/// 强制覆盖等场景的合适工具 —— 用 Branch 表达这些会非常拐弯抹角。
///
/// ============================================================================
///  自环 / 死锁的边界处理
/// ============================================================================
/// `Work::_can_precede` 中只对**完全无 jump 节点参与**的环路做严格 DAG 校验：
/// jump 出/入的边可以构成环（这正是状态机循环的写法）。这一拓扑放宽是 Jump
/// 类型存在的关键 —— 没有它，循环图无法在编辑期通过校验。
///
/// ============================================================================
///  Last-Write-Wins 选择语义（同 Branch）
/// ============================================================================
/// - 多次 select / `operator[]=` 覆盖；不调用即不跳转，走常规 tear_down；
/// - `select_if(pred)` 首个匹配即停。
///
/// @pre 由 JumpWork::invoke 构造，传引用给闭包，不可逃逸。
/// @see Branch     协作式对偶
/// @see MultiJump  多目标版
/// @see JumpWork   宿主节点
class Jump : public Immovable<Jump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    /// @brief 下标赋值代理，支持 `jump[i] = true/false` 语法。
    class Proxy {
        friend class Jump;
        Jump& m_jmp;
        std::size_t m_idx;
        Proxy(Jump& jmp, std::size_t idx) noexcept : m_jmp{jmp}, m_idx{idx} {}
    public:
        /// @brief `jump[i] = true` 选中第 i 个后继；`= false` 若当前选中即为 i 则清除。
        Jump& operator=(bool on) noexcept {
            if (on) {
                m_jmp.select(m_idx);
            } else if (m_idx < m_jmp.m_work.m_num_successors
                       && m_jmp.m_target == m_jmp.m_work.m_edges[m_idx]) {
                m_jmp.m_target = nullptr;
            }
            return m_jmp;
        }
    };

    // ==================== 跳转操作 ====================

    /// @brief 按索引选择跳转目标。O(1)。
    /// @param index 目标后继在后继数组中的位置。
    /// @return `*this`，支持链式调用。
    /// @post 若 index 有效则选中对应后继；越界时清除选择（等价于 reset）。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Jump& select(I index) noexcept;

    /// @brief 按谓词选择首个满足条件的后继作为跳转目标。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`，支持链式调用。
    /// @post 首个满足谓词的后继被选中；无匹配时清除选择。
    template <predicate<TaskView> Pred>
    Jump& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    /// @brief 取消跳转。
    /// @return `*this`，支持链式调用。
    /// @post 内部目标指针置空，invoke 结束后走常规 tear_down 路径。
    Jump& reset() noexcept;

    /// @brief 下标赋值：`jump[i] = true` 选中，`jump[i] = false` 条件清除。
    [[nodiscard]] Proxy operator[](std::size_t index) noexcept;

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work& m_work;           ///< 绑定至宿主 JumpWork，借此访问后继列表
    Work* m_target{nullptr}; ///< 选中的跳转目标；invoke 结束后由 _tear_down_jump_task 直接置 counter=0 并调度

    explicit Jump(Work& work) noexcept : m_work{work} {}
};

// ============================================================
//  Jump 内联实现
// ============================================================
template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
Jump& Jump::select(I index) noexcept {
    const auto idx = static_cast<std::size_t>(index);
    m_target = (idx < m_work.m_num_successors) ? m_work.m_edges[idx] : nullptr;
    return *this;
}

template <predicate<TaskView> Pred>
Jump& Jump::select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
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

inline Jump& Jump::reset() noexcept {
    m_target = nullptr;
    return *this;
}

inline Jump::Proxy Jump::operator[](std::size_t index) noexcept {
    return {*this, index};
}

inline std::size_t Jump::size() const noexcept {
    return m_work.m_num_successors;
}

/// @brief 多目标跳转控制器 —— 同时强制激活 N 个后继的"广播 GOTO"。
///
/// @details
/// 与 `Jump` 互斥选择不同，`MultiJump` 允许同时跳转多个后继；
/// `_tear_down_multi_jump_task` 对每个 target 各执行一次"清零 + 入队"，
/// 等价于多次 `Jump` 的合并。
///
/// 与 `MultiBranch` 的关系，正如 `Jump` 与 `Branch`：累积语义、SmallVector 存储、
/// 单 / 多索引下标 Proxy —— 这部分文档与 `MultiBranch` 完全平行，仅差额机制
/// 替换为强制清零。
///
/// ============================================================================
///  典型用例 —— 状态机的"多分支跳转"
/// ============================================================================
/// 一个错误处理节点同时跳到"重试入口"和"日志记录"两个状态：
/// @code
///   error_handler.emplace([](MultiJump& mj){
///       if (transient_error)   mj[retry_state] = true;
///       if (need_log)          mj[log_state]   = true;
///   });
/// @endcode
///
/// @pre 由 MultiJumpWork::invoke 构造，传引用给闭包，不可逃逸。
/// @see Jump            单目标版
/// @see MultiBranch     协作式对偶
/// @see MultiJumpWork   宿主节点
class MultiJump : public Immovable<MultiJump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    /// @brief 单索引赋值代理：`mj[i] = true/false`。
    class Proxy {
        friend class MultiJump;
        MultiJump& m_mj;
        std::size_t m_idx;
        Proxy(MultiJump& mj, std::size_t idx) noexcept : m_mj{mj}, m_idx{idx} {}
    public:
        /// @brief `= true` 加入跳转集合；`= false` 移除。越界静默忽略。
        MultiJump& operator=(bool on) {
            if (m_idx >= m_mj.m_work.m_num_successors) return m_mj;
            Work* w = m_mj.m_work.m_edges[m_idx];
            if (on) m_mj._insert(w);
            else    m_mj._erase(w);
            return m_mj;
        }
    };

    /// @brief 多索引赋值代理：`mj[i, j, k] = {b0, b1, b2}`，编译期强制个数一致。
    /// @tparam N 索引个数，由 operator[] 变参包大小推导，编译期固定。
    template <std::size_t N>
    class MultiProxy {
        friend class MultiJump;
        MultiJump& m_mj;
        std::size_t m_indices[N]; ///< 原生 C 数组，N 编译期已知，零额外开销

        template <typename... Is>
        MultiProxy(MultiJump& mj, Is... indices) noexcept
            : m_mj{mj}, m_indices{static_cast<std::size_t>(indices)...} {}

    public:
        /// @brief `mj[1, 2, 3] = {false, true, false}` — 编译期强制个数一致。
        template <std::size_t M>
            requires (M == N)
        MultiJump& operator=(const bool (&mask)[M]) {
            const std::size_t sz = m_mj.m_work.m_num_successors;
            for (std::size_t i = 0; i < N; ++i) {
                if (m_indices[i] >= sz) continue;
                Work* w = m_mj.m_work.m_edges[m_indices[i]];
                if (mask[i]) m_mj._insert(w);
                else         m_mj._erase(w);
            }
            return m_mj;
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
    MultiJump& select(Is... indices);

    /// @brief 跳转到所有后继（无条件广播）。
    /// @return `*this`，支持链式调用。
    /// @post 全部后继被加入跳转集合。
    MultiJump& select_all();

    /// @brief 清空所有已累积的跳转目标。
    /// @return `*this`，支持链式调用。
    /// @post 本次跳转不激活任何后继。
    MultiJump& reset() noexcept;

    /// @brief 按谓词选择所有满足条件的后继作为跳转目标。
    ///
    /// 与 `Jump::select_if` 不同：不是只选首个匹配，
    /// 而是选中所有满足谓词的后继。
    ///
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @post 所有满足条件的后继被加入跳转集合。
    template <predicate<TaskView> Pred>
    MultiJump& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work&               m_work;                 ///< 绑定至宿主 MultiJumpWork
    SmallVector<Work*>  m_targets; ///< 去重后的跳转集合；invoke 返回后 Executor 遍历做强制调度

    explicit MultiJump(Work& work) noexcept : m_work{work} {}

    /// 去重插入。
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
//  MultiJump 内联实现
// ============================================================

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
MultiJump::Proxy MultiJump::operator[](I index) noexcept {
    return {*this, static_cast<std::size_t>(index)};
}
template <typename... Is>
    requires (sizeof...(Is) > 1)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
MultiJump::MultiProxy<sizeof...(Is)> MultiJump::operator[](Is... indices) noexcept {
    return {*this, indices...};
}

template <typename... Is>
    requires (sizeof...(Is) > 0)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
MultiJump& MultiJump::select(Is... indices) {
    const std::size_t sz = m_work.m_num_successors;

    ([&](std::size_t idx) {
        if (idx < sz) _insert(m_work.m_edges[idx]);
    }(static_cast<std::size_t>(indices)), ...);

    return *this;
}

inline MultiJump& MultiJump::select_all() {
    const std::size_t sz = m_work.m_num_successors;
    for (std::size_t i = 0; i < sz; ++i) {
        _insert(m_work.m_edges[i]);
    }
    return *this;
}

inline MultiJump& MultiJump::reset() noexcept {
    m_targets.clear();
    return *this;
}

template <predicate<TaskView> Pred>
MultiJump& MultiJump::select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    const std::size_t sz = m_work.m_num_successors;
    for (std::size_t i = 0; i < sz; ++i) {
        if (std::invoke_r<bool>(pred, TaskView{*m_work.m_edges[i]})) {
            _insert(m_work.m_edges[i]);
        }
    }
    return *this;
}

inline std::size_t MultiJump::size() const noexcept {
    return m_work.m_num_successors;
}

}  // namespace tfl
