/// @file  jump.hpp
/// @brief 强制跳转控制器 Jump / MultiJump —— DAG 内的运行时抢占式路由。
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
//  Jump —— 单目标强制跳转选择器 (抢占式 / Last-Write-Wins)
// ============================================================================

/// @brief 单目标强制跳转选择器 —— DAG 节点内的"goto 路由权杖"。
///
/// 由 JumpWork::invoke 在 Worker 栈上构造，用户闭包通过 Jump& 形参拿到它。
/// 选中后继的 join_counter 被 store(0) 强制归零，绕过正常 fetch_sub 计数
/// 协议，无视其他前驱是否到齐。不 select 则所有后继被丢弃。走独立的
/// _tear_down_jump_task 路径。
///
/// @note 严格栈帧绑定：闭包返回即销毁，禁止逃逸。
class Jump : public Immovable<Jump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 下标赋值代理, 支持 jmp[i] = true/false 语法。
    ///
    /// - = true  : 等价于 select(i)
    /// - = false : 等价于 unselect(i) (仅当前选中正好是 i 时清除)
    ///
    /// 这种"非对称的反向操作"是 Jump 单选语义的自然结果:
    /// 用户不能"取消别人的选择", 只能取消自己刚做的选择。
    class Proxy {
        friend class Jump;
        Jump&       m_jmp;
        std::size_t m_idx;
        Proxy(Jump& jmp, std::size_t idx) noexcept : m_jmp{jmp}, m_idx{idx} {}

    public:
        /// @brief 赋值: true 选中, false 条件清除。
        Jump& operator=(bool on) noexcept {
            if (on) {
                m_jmp.select(m_idx);
            } else {
                m_jmp.unselect(m_idx);
            }
            return m_jmp;
        }
    };

    // ==================== 选择动作 ====================

    /// @brief 按索引显式选择跳转目标, O(1)。
    /// @tparam I 整数类型 (排除 bool)。
    /// @param index 目标后继在后继数组中的绝对位置 (0-based)。
    /// @return *this, 支持链式调用。
    /// @post 若 index 在合法范围内则选中对应后继 (将被强制激活); 越界则安全清除。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Jump& select(I index) noexcept;

    /// @brief 按谓词动态评估并选择首个满足条件的后继 (短路)。
    /// @tparam Pred 满足 predicate<TaskView> 概念的闭包类型。
    /// @param pred 接受只读 TaskView 并返回 bool 的可调用对象。
    /// @return *this, 支持链式调用。
    /// @post 首个令谓词返回 true 的后继被选中; 若无匹配则清除选择 (等价丢弃所有后继)。
    template <predicate<TaskView> Pred>
    Jump& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

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
    Jump& unselect(I index) noexcept;

    /// @brief 无条件清除当前选择。
    /// @return *this, 支持链式调用。
    /// @post m_target = nullptr, 本节点的所有后继被丢弃 (注意 Jump 语义)。
    ///
    /// @warning 不同于 Branch::reset: Jump 的 reset 后没有"常规路径" ——
    ///          所有后继都不会被调度, 这条分支在此节点终止。
    Jump& reset() noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法: jmp(i) —— 等价 jmp.select(i)。
    ///
    /// 设计动机:
    /// - 与 MultiJump::operator() 形成对偶, API 风格一致
    /// - 比 jmp.select(i) 更短, 便于在简单跳转分支里使用
    /// - 单参数: Jump 是互斥单选, 多参数没有语义
    ///
    /// @tparam I 整数类型 (排除 bool)。
    /// @param index 目标后继索引。
    /// @return *this, 支持链式调用。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Jump& operator()(I index) noexcept;

    // ==================== 下标语法 ====================

    /// @brief 下标赋值: jmp[i] = true 选中, jmp[i] = false 条件清除。
    /// @param index 后继索引 (0-based)。
    /// @return 赋值代理对象。
    [[nodiscard]] Proxy operator[](std::size_t index) noexcept;

    // ==================== 查询接口 ====================

    /// @brief 获取当前跳转节点所连接的后继总数。
    /// @return 出边数量。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work& m_work;
    /// @brief 暂存被选中的跳转目标指针, 供 invoke 结束后 Executor 强制激活。
    Work* m_target{nullptr};

    explicit Jump(Work& work) noexcept : m_work{work} {}
};

// ============================================================
//  Jump 内联实现
// ============================================================

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
inline Jump& Jump::select(I index) noexcept {
    const auto idx = static_cast<std::size_t>(index);
    m_target = (idx < m_work.m_num_successors) ? m_work.m_edges[idx] : nullptr;
    return *this;
}

template <predicate<TaskView> Pred>
inline Jump& Jump::select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
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
inline Jump& Jump::unselect(I index) noexcept {
    const auto idx = static_cast<std::size_t>(index);
    if (idx < m_work.m_num_successors && m_target == m_work.m_edges[idx]) {
        m_target = nullptr;
    }
    return *this;
}

inline Jump& Jump::reset() noexcept {
    m_target = nullptr;
    return *this;
}

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
inline Jump& Jump::operator()(I index) noexcept {
    return select(index);
}

inline Jump::Proxy Jump::operator[](std::size_t index) noexcept {
    return {*this, index};
}

inline std::size_t Jump::size() const noexcept {
    return m_work.m_num_successors;
}

// ============================================================================
//  MultiJump —— 多目标强制跳转选择器 (累积广播 / 抢占式)
// ============================================================================

/// @brief 多目标强制跳转选择器 —— "并行扇出 / 多路状态机"权杖。
///
/// 与 Jump 互斥选择不同，MultiJump 允许同时强制激活 N 条下游链路。
/// 每个被选中的后继执行 store(0) 强制清零。多次 select 累积生效，
/// 内部用 SmallVector<Work*> 去重集合。走独立的 _tear_down_multi_jump_task。
///
/// @note 严格栈帧绑定：闭包返回即销毁，禁止逃逸。
class MultiJump : public Immovable<MultiJump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 单索引赋值代理: mj[i] = true/false。
    ///
    /// - = true  : 等价于 select(i) (加入激活集合, 去重)
    /// - = false : 等价于 unselect(i) (从激活集合移除)
    /// - 越界索引: 静默忽略 (不抛异常, 符合"宽松边界"的实用主义)
    class Proxy {
        friend class MultiJump;
        MultiJump&  m_mj;
        std::size_t m_idx;
        Proxy(MultiJump& mj, std::size_t idx) noexcept : m_mj{mj}, m_idx{idx} {}

    public:
        /// @brief 赋值: true 加入, false 移除 (越界静默忽略)。
        MultiJump& operator=(bool on) noexcept {
            if (m_idx >= m_mj.m_work.m_num_successors) return m_mj;
            Work* w = m_mj.m_work.m_edges[m_idx];
            if (on) {
                m_mj._insert(w);
            } else {
                m_mj._erase(w);
            }
            return m_mj;
        }
    };

    /// @brief 单索引下标: mj[i] = true/false。
    /// @tparam I 整数类型 (排除 bool)。
    /// @param index 后继索引。
    /// @return 赋值代理对象。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    [[nodiscard]] Proxy operator[](I index) noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法: mj(i, j, k) —— 等价 mj.select(i, j, k)。
    ///
    /// 与 Jump::operator() 对偶, 但接受变参 (MultiJump 是累积式)。
    ///
    /// @note 仅累积式插入, 不会清除已有选择。需要重置请先 reset()。
    ///
    /// @tparam Is 整数索引类型 (至少一个, 排除 bool)。
    /// @param indices 要加入激活集合的索引列表。
    /// @return *this, 支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiJump& operator()(Is... indices);

    // ==================== 选择动作 ====================

    /// @brief 按索引集批量加入激活集合。
    /// @tparam Is 可转换为 std::size_t 的变参索引类型 (至少一个, 排除 bool)。
    /// @param indices 一组要强制激活的后继索引。
    /// @return *this, 支持链式调用。
    /// @post 所有有效索引对应的后继被加入激活集合, 越界索引自动忽略, 重复索引去重。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiJump& select(Is... indices);

    /// @brief 按索引集批量从激活集合移除 (select 的反向动作)。
    ///
    /// 与 select(...) 完全对偶:
    /// - select(0, 2)   -> 把 0/2 加入激活集合
    /// - unselect(0, 2) -> 把 0/2 从激活集合移除
    ///
    /// 不在集合中的索引、越界索引均静默忽略, 不抛异常。
    /// 典型用法: select_all() 后排除若干索引。
    ///
    /// @tparam Is 整数索引类型 (至少一个, 排除 bool)。
    /// @param indices 一组要从激活集合移除的索引。
    /// @return *this, 支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiJump& unselect(Is... indices) noexcept;

    /// @brief 一键选中所有下游后继 (广播跳转模式)。
    /// @return *this, 支持链式调用。
    /// @post 全部后继被加入激活集合。
    MultiJump& select_all();

    /// @brief 清空所有激活意图。
    /// @return *this, 支持链式调用。
    /// @post m_targets 清空, 本节点的所有后继被丢弃 (注意 Jump 语义)。
    ///
    /// @warning 不同于 MultiBranch::reset: MultiJump 的 reset 后没有"常规路径" ——
    ///          所有后继都不会被调度, 这条分支在此节点终止。
    MultiJump& unselect_all() noexcept;

    /// @brief 清空所有放行意图 —— unselect_all() 的语义别名。
    MultiJump& reset() noexcept;

    /// @brief 基于谓词批量点亮所有符合条件的后继。
    ///
    /// 与 Jump::select_if 不同: 不是只选首个匹配, 而是选中所有满足谓词的后继。
    ///
    /// @tparam Pred 满足 predicate<TaskView> 概念的闭包类型。
    /// @param pred 接受只读 TaskView 并返回 bool 的可调用对象。
    /// @return *this, 支持链式调用。
    /// @post 凡是促使谓词评估为 true 的节点均并入激活集合。
    template <predicate<TaskView> Pred>
    MultiJump& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    /// @return 出边数量。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work&               m_work;
    /// @brief 内置轻量缓冲区的去重集合, 供 invoke 返回后 Executor 遍历强制激活。
    SmallVector<Work*>  m_targets;

    explicit MultiJump(Work& work) noexcept : m_work{work} {}

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
//  MultiJump 内联实现
// ============================================================

template <std::integral I>
    requires (!std::same_as<std::remove_cvref_t<I>, bool>)
inline MultiJump::Proxy MultiJump::operator[](I index) noexcept {
    return {*this, static_cast<std::size_t>(index)};
}

template <typename... Is>
    requires (sizeof...(Is) >= 1)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
inline MultiJump& MultiJump::operator()(Is... indices) {
    return select(indices...);
}

template <typename... Is>
    requires (sizeof...(Is) >= 1)
            && (std::integral<Is> && ...)
            && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
inline MultiJump& MultiJump::select(Is... indices) {
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
inline MultiJump& MultiJump::unselect(Is... indices) noexcept {
    const std::size_t sz = m_work.m_num_successors;
    ([&](std::size_t idx) {
        if (idx < sz) _erase(m_work.m_edges[idx]);
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

inline MultiJump& MultiJump::unselect_all() noexcept {
    m_targets.clear();
    return *this;
}

inline MultiJump& MultiJump::reset() noexcept {
    unselect_all();
    return *this;
}

template <predicate<TaskView> Pred>
inline MultiJump& MultiJump::select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
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
