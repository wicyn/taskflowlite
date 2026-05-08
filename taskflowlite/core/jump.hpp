/// @file jump.hpp
/// @brief 强制跳转控制器 Jump / MultiJump —— DAG 内的运行时抢占式路由。
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
//  Jump —— 单目标强制跳转选择器(抢占式 / Last-Write-Wins)
// ============================================================================

/// @brief 单目标强制跳转选择器 —— DAG 节点内的"goto 路由权杖"。
///
/// @details
/// `Jump` 是 `JumpWork::invoke` 在 worker 栈上为用户构造的**临时控制凭据**:
/// 用户的闭包通过形参 `Jump&` 拿到它,调用 `select(i)` / `select_if(pred)` 决定
/// 本次执行后哪个后继被强制激活。它不持有数据、不分配内存,仅是 `Work` 后继边表
/// 的一个语义代理。
///
/// ============================================================================
///  调度原理 —— 抢占式强制清零(Force-Reset Trick)
/// ============================================================================
/// `JumpWork` 出边的初始 `join_weight` 是 **0**(普通边为 1,Branch 为 2),
/// 这意味着:
///
/// @code
///   Jump 出边对后继 join_counter 的常规递减贡献 = 0
///   → 单靠"常规 tear_down"路径,Jump 的所有后继都无法被激活
///   → 后继激活的唯一方式:被 select 选中的目标,join_counter 被强制 store(0)
/// @endcode
///
/// 这与 `Branch` 的"协作式差额"形成本质对比 —— Branch 的未选中后继仍然可以被
/// 其他前驱通过常规 join_counter 递减完成;**Jump 的所有后继激活完全由 `select`
/// 独占控制**,任何未被 select 命中的后继都会在本次分支中被丢弃。
///
/// 这种设计的工程意义:**赋予用户在节点内"强行改写控制流"的能力**,典型应用是
/// retry 循环、状态机回跳、跨层级 goto 等"非线性"控制结构。
///
/// ============================================================================
///  关键不变量 —— 不 select 就丢弃
/// ============================================================================
/// **必须强调:** 与 Branch 不同,Jump 节点若不调用 `select` / `select_if` 命中,
/// `target == nullptr`,**所有后继都不会被调度**(源码 `_tear_down_jump_task`
/// 中 `if (!exception && target)` 直接跳过激活逻辑)。
///
/// 这意味着:
/// - "走默认路径"的需求 → 用 `select(idx)` 显式跳过去,**不能依赖"什么都不做"**;
/// - retry 循环里"成功完成"的分支 → 也要 `select(success_idx)` 显式声明;
/// - `reset()` 等价于"放弃本节点的所有后继",而非"走常规路径"。
///
/// ============================================================================
///  选择语义(Last-Write-Wins / 互斥)
/// ============================================================================
/// - 多次 `select()` / `jmp[i] = true` / `jmp(i)` 以最后一次为准(覆盖前值);
/// - 不调用任何 select → 所有后继被丢弃(`m_target = nullptr`);
/// - `reset()` 显式清除,等价于"什么都没选";
/// - `unselect(i)` 条件清除:仅当前选中正好是 i 时才生效;
/// - `select_if(pred)` 短路:首个匹配即停。
///
/// ============================================================================
///  四种入口语法 —— 与 MultiJump 完全对偶
/// ============================================================================
/// @code
///   // 1) 单索引下标:动态状态机风格
///   jmp[i] = true;          // 跳到第 i 个
///   jmp[i] = false;         // 等价 unselect(i)
///
///   // 2) 函数调用:select 的简写
///   jmp(2);                 // 等价 jmp.select(2)
///
///   // 3) 显式动作:严肃代码风格
///   jmp.select(2);
///   jmp.unselect(3);        // 反向动作:仅当前选中正好是 3 时才清除
///   jmp.reset();            // 完全清空(等价丢弃所有后继)
///
///   // 4) 谓词驱动
///   jmp.select_if([](auto tv){ return tv.name() == "retry"; });
/// @endcode
///
/// 与 `MultiJump::operator()` 不同:这里只接受**单个**索引参数 ——
/// `Jump` 是互斥单选,变参没有意义。
///
/// ============================================================================
///  典型用法 —— Retry 循环
/// ============================================================================
/// @code
///   auto init    = flow.emplace([] {});
///   auto process = flow.emplace([&] { ++attempts; });
///   auto check   = flow.emplace([&](tfl::Jump& jmp) {
///       if (attempts < MAX) jmp.select(0);   // 跳回 process(索引 0)
///       else                jmp.select(1);   // 显式跳到 success(索引 1)
///   });
///   auto success = flow.emplace([&] { /* done */ });
///
///   init.precede(process);
///   process.precede(check);
///   check.precede(process, success);   // 0=process, 1=success
/// @endcode
///
/// 注意 `else` 分支必须 `select(1)` ——**不能依赖"什么都不做就走 success"**,
/// 因为 Jump 的 weight=0 出边没有"默认路径"机制。
///
/// ============================================================================
///  生命周期约束 —— 严格栈帧绑定
/// ============================================================================
/// `Jump` 由 `JumpWork::invoke` 在 worker 栈上构造,传引用给用户闭包。
/// 闭包返回即销毁。**禁止逃逸** 到外部线程或异步上下文 —— 一旦 invoke 返回,
/// 持有它的 `Work*` 进入 tear_down,行为未定义。这与 `Runtime` 的栈帧约束相同。
///
/// @pre 由 JumpWork::invoke 构造,传引用给闭包,不可逃逸。
/// @see Branch        协作式差额的"软选择"对偶
/// @see MultiJump     多目标广播跳转版
/// @see JumpWork      宿主节点类型
class Jump : public Immovable<Jump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 下标赋值代理,支持 `jmp[i] = true/false` 语法。
    ///
    /// @details
    /// - `= true`:等价于 `select(i)`;
    /// - `= false`:等价于 `unselect(i)`(仅当前选中正好是 i 时清除)。
    ///
    /// 这种"非对称的反向操作"是 Jump 单选语义的自然结果:
    /// 用户不能"取消别人的选择",只能取消自己刚做的选择。
    class Proxy {
        friend class Jump;
        Jump&       m_jmp;
        std::size_t m_idx;
        Proxy(Jump& jmp, std::size_t idx) noexcept : m_jmp{jmp}, m_idx{idx} {}

    public:
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

    /// @brief 按索引显式选择跳转目标。O(1)。
    /// @param index 目标后继在后继数组中的绝对位置。
    /// @return `*this`,支持链式调用。
    /// @post 若 index 在合法范围内,则选中对应后继(将被强制激活);越界则安全清除(等价于 reset)。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Jump& select(I index) noexcept;

    /// @brief 按谓词动态评估并选择**首个**满足条件的后继(短路)。
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`,支持链式调用。
    /// @post 首个令谓词返回 true 的后继被选中;若无匹配则清除选择(等价于丢弃所有后继)。
    template <predicate<TaskView> Pred>
    Jump& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

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
    Jump& unselect(I index) noexcept;

    /// @brief 无条件清除当前选择。
    /// @return `*this`,支持链式调用。
    /// @post `m_target = nullptr`,**本节点的所有后继被丢弃**(注意 Jump 语义)。
    ///
    /// @warning 不同于 Branch::reset:Jump 的 reset 后**没有"常规路径"** ——
    ///          所有后继都不会被调度,这条分支在此节点终止。
    Jump& reset() noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法:`jmp(i)` —— 等价 `jmp.select(i)`。
    ///
    /// @details
    /// 设计动机:
    /// - 与 `MultiJump::operator()` 形成对偶,API 风格一致;
    /// - 比 `jmp.select(i)` 更短,便于在简单跳转分支里使用;
    /// - 单参数:Jump 是互斥单选,多参数没有语义。
    ///
    /// @code
    ///   // 等价的三种写法
    ///   jmp(2);
    ///   jmp.select(2);
    ///   jmp[2] = true;
    /// @endcode
    ///
    /// @return `*this`,支持链式调用(尽管 last-write-wins 下链式意义不大)。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    Jump& operator()(I index) noexcept;

    // ==================== 下标语法 ====================

    /// @brief 下标赋值:`jmp[i] = true` 选中,`jmp[i] = false` 条件清除。
    /// @param index 后继索引。
    /// @return 赋值代理对象。
    [[nodiscard]] Proxy operator[](std::size_t index) noexcept;

    // ==================== 查询接口 ====================

    /// @brief 获取当前跳转节点所连接的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work& m_work;
    Work* m_target{nullptr};   ///< 暂存被选中的跳转目标指针,供 invoke 结束后 Executor 强制激活

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
//  MultiJump —— 多目标强制跳转选择器(累积广播 / 抢占式)
// ============================================================================

/// @brief 多目标强制跳转选择器 —— "并行扇出 / 多路状态机"权杖。
///
/// @details
/// 与 `Jump` 互斥选择不同,`MultiJump` 允许同时强制激活 N 条下游链路 ——
/// 每个被选中的后继都被独立 `join_counter.store(0)` 强制清零。底层调度协议复用
/// 同样的"边权 0 + 强制激活"机制,仅 select 集合从单个变为去重集合。
///
/// ============================================================================
///  累积语义(Accumulative,与 Jump 的覆盖语义对照)
/// ============================================================================
/// - 多次 `select(...)` / `mj(i, j, k)` / `mj[i] = true` **累积** 生效;
/// - 内部用 `SmallVector<Work*>` 存放选中集,`_insert` 线性查重;
/// - `select_all()` 一键全选(广播跳转);
/// - `unselect(...)` 从激活集合移除指定索引(`select` 的反向动作);
/// - `reset()` 清空全部累积(等价丢弃所有后继);
/// - `select_if(pred)` 选 **所有** 满足谓词的(不像 Jump::select_if 只选首个)。
///
/// ============================================================================
///  关键不变量 —— 不 select 就丢弃
/// ============================================================================
/// 与 Jump 一致:`MultiJump` 的所有后继都靠 select 显式激活。源码
/// `_tear_down_multi_jump_task` 中遍历的就是 `m_targets` 集合 —— 集合为空时
/// 整个 for 循环空跑,所有后继被静默丢弃。
///
/// 这意味着:
/// - 想"广播全部" → 用 `select_all()`,**不能省略**;
/// - 想"广播部分" → 显式 `select(i, j, k)`;
/// - 任何未被 select 的后继 → 永远不会激活。
///
/// ============================================================================
///  存储选型 —— 为什么用 SmallVector 不用 hash set?
/// ============================================================================
/// 与 MultiBranch 同理:DAG 节点扇出极低(普遍 < 4),线性扫描 + swap-and-pop
/// 删除的常数因子优于哈希。SmallVector 内置小缓冲区避免堆分配,O(1) 删除不碰
/// 内存分配器。**不要为渐近复杂度妥协常数因子**。
///
/// ============================================================================
///  四种入口语法 —— 与 Jump 对偶
/// ============================================================================
/// @code
///   // 1) 单索引下标:逐边动态决定状态(true/false 双向)
///   mj[0] = true;
///   mj[1] = false;
///
///   // 2) 函数调用:select 的简写,**变参累积**
///   mj(0, 1, 2);            // 等价 mj.select(0, 1, 2)
///   mj(0)(1)(2);            // 链式累积也合法
///
///   // 3) 显式动作:严肃代码风格
///   mj.select(0, 1, 2);
///   mj.unselect(1, 3);      // 从激活集合移除
///   mj.select_all();
///   mj.reset();
///
///   // 4) 谓词驱动
///   mj.select_if([](auto tv){ return tv.name().starts_with("loop_"); });
/// @endcode
///
/// 与 `Jump::operator()` 不同:此处接受**变参**,因为 MultiJump 是累积式 ——
/// 多个索引一次性加入激活集合。
///
/// ============================================================================
///  典型用法 —— 并行 Retry 循环
/// ============================================================================
/// @code
///   // 三条并行分支 + MultiJump 汇聚控制循环
///   auto branch_a = flow.emplace([] { /* 工作 a */ });
///   auto branch_b = flow.emplace([] { /* 工作 b */ });
///   auto branch_c = flow.emplace([] { /* 工作 c */ });
///   auto mj       = flow.emplace([&](tfl::MultiJump& jmp) {
///       if (count < ITERS) jmp.select(0, 1, 2);   // 同时跳回 a/b/c
///   });
///
///   branch_a.precede(mj);
///   branch_b.precede(mj);
///   branch_c.precede(mj);
///   mj.precede(branch_a, branch_b, branch_c);
///
///   auto init = flow.emplace([] {});
///   init.precede(branch_a, branch_b, branch_c);   // 必要的启动边
/// @endcode
///
/// @pre 由 MultiJumpWork::invoke 构造,传引用给闭包,不可逃逸。
/// @see Jump          单目标互斥版
/// @see MultiBranch   协作式差额的"软选择"对偶
/// @see MultiJumpWork 宿主节点类型
class MultiJump : public Immovable<MultiJump> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 单索引下标代理 ====================

    /// @brief 单索引赋值代理:`mj[i] = true/false`。
    ///
    /// @details
    /// - `= true`:等价于 `select(i)`(加入激活集合,去重);
    /// - `= false`:等价于 `unselect(i)`(从激活集合移除);
    /// - 越界索引:静默忽略(不抛异常,符合"宽松边界"的实用主义)。
    class Proxy {
        friend class MultiJump;
        MultiJump&  m_mj;
        std::size_t m_idx;
        Proxy(MultiJump& mj, std::size_t idx) noexcept : m_mj{mj}, m_idx{idx} {}

    public:
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

    /// @brief 单索引下标:`mj[i] = true/false`。
    /// @return 赋值代理对象。
    template <std::integral I>
        requires (!std::same_as<std::remove_cvref_t<I>, bool>)
    [[nodiscard]] Proxy operator[](I index) noexcept;

    // ==================== 函数调用语法 ====================

    /// @brief 函数调用语法:`mj(i, j, k)` —— 等价 `mj.select(i, j, k)`。
    ///
    /// @details
    /// 与 `Jump::operator()` 对偶,但接受**变参**(MultiJump 是累积式)。
    ///
    /// @code
    ///   mj(0, 2, 5);                       // 同时跳到索引 0/2/5(累积)
    ///   mj(0)(2)(5);                       // 链式调用,等价上面
    ///   mj.reset()(0, 2, 5);               // 先清空再选
    /// @endcode
    ///
    /// @note 仅累积式插入,不会清除已有选择。需要重置请先 `reset()`。
    ///
    /// @return `*this`,支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiJump& operator()(Is... indices);

    // ==================== 选择动作 ====================

    /// @brief 按索引集批量加入激活集合。
    /// @tparam Is 可转换为 `std::size_t` 的变参索引类型。
    /// @param indices 一组要强制激活的后继索引。
    /// @return `*this`,支持链式调用。
    /// @post 所有有效索引对应的后继被加入激活集合,越界索引自动忽略,重复索引去重。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiJump& select(Is... indices);

    /// @brief 按索引集批量从激活集合移除(`select` 的反向动作)。
    ///
    /// @details
    /// 与 `select(...)` 完全对偶:
    /// - `select(0, 2)` → 把 0/2 加入激活集合;
    /// - `unselect(0, 2)` → 把 0/2 从激活集合移除。
    ///
    /// 不在集合中的索引、越界索引均静默忽略,不抛异常。
    ///
    /// 典型用法:`select_all()` 后排除若干索引。
    /// @code
    ///   mj.select_all();        // 先广播全部
    ///   mj.unselect(2, 4);      // 然后排除 2 和 4
    /// @endcode
    ///
    /// @param indices 一组要从激活集合移除的索引。
    /// @return `*this`,支持链式调用。
    template <typename... Is>
        requires (sizeof...(Is) >= 1)
                && (std::integral<Is> && ...)
                && (!std::same_as<std::remove_cvref_t<Is>, bool> && ...)
    MultiJump& unselect(Is... indices) noexcept;

    /// @brief 一键选中所有下游后继(广播跳转模式)。
    /// @return `*this`,支持链式调用。
    /// @post 全部后继被加入激活集合。
    MultiJump& select_all();

    /// @brief 清空所有激活意图。
    /// @return `*this`,支持链式调用。
    /// @post `m_targets` 清空,**本节点的所有后继被丢弃**(注意 Jump 语义)。
    ///
    /// @warning 不同于 MultiBranch::reset:MultiJump 的 reset 后**没有"常规路径"** ——
    ///          所有后继都不会被调度,这条分支在此节点终止。
    MultiJump& reset() noexcept;

    /// @brief 基于谓词批量点亮所有符合条件的后继。
    ///
    /// @details
    /// 与 `Jump::select_if` 不同:不是只选首个匹配,而是选中所有满足谓词的后继。
    ///
    /// @tparam Pred 满足 predicate 概念的闭包类型。
    /// @param pred 接受只读 `TaskView` 并返回 `bool` 的可调用对象。
    /// @return `*this`,支持链式调用。
    /// @post 凡是促使谓词评估为 true 的节点均并入激活集合。
    template <predicate<TaskView> Pred>
    MultiJump& select_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    /// @brief 返回当前节点的后继总数。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Work&               m_work;
    SmallVector<Work*>  m_targets;   ///< 内置轻量缓冲区的去重集合,供 invoke 返回后 Executor 遍历强制激活

    explicit MultiJump(Work& work) noexcept : m_work{work} {}

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

inline MultiJump& MultiJump::reset() noexcept {
    m_targets.clear();
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

