/// @file work.hpp
/// @brief DAG 任务节点基类 Work —— 框架最复杂、最核心的内部数据结构
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <expected>
#include <utility>
#include <span>
#include <algorithm>
#include <memory>
#include <stack>
#include <future>
#include <cmath>
#include <climits>

#include "enums.hpp"
#include "traits.hpp"
#include "utility.hpp"
#include "exception.hpp"
#include "observer.hpp"
#include "topology.hpp"
#include "semaphore.hpp"
#include "small_vector.hpp"
#include "unordered_dense.hpp"
#include "object_pool.hpp"

namespace tfl {

/// @brief DAG 任务节点基类 Work —— 框架内部的统一执行单元。
///
/// @details
/// `Work` 是 taskflow-lite 的核心内部抽象：普通任务、运行时任务、条件分支、
/// 跳转、嵌套 Flow、异步任务等，最终都会落到某个 `Work` 派生类上。
///
/// 用户层不会直接操作 `Work`，而是通过 `Task` / `TaskView` / `AsyncTask` /
/// `DeferredAsyncTask` 这些句柄访问受限能力。`Work` 本身只暴露给框架内部友元，
/// 用来承载调度、依赖计数、异常归档、信号量、观察者以及 D2 可视化等运行时状态。
///
/// ============================================================================
///  类层次（多态分派）
/// ============================================================================
/// @code
///   Work
///    ├── BasicWork            普通同步任务
///    ├── RuntimeWork          注入 Runtime& 的动态任务
///    ├── BranchWork           单目标条件分支
///    ├── MultiBranchWork      多目标条件分支
///    ├── JumpWork             单目标强制跳转
///    ├── MultiJumpWork        多目标强制跳转
///    ├── GraphWork<FlowStore> 嵌套 Flow / 动态 Flow 容器节点
///    └── AnchorWork           内部锚点节点，用于 cowait / dependent_async 的归档与计数
/// @endcode
///
/// 具体 Invoker 子类负责保存用户 callable 与参数，并在 `invoke()` 中执行用户逻辑。
/// `Work` 基类只定义公共运行时协议，不关心 callable 的具体类型。
///
/// ============================================================================
///  状态字段：Implicit + Explicit
/// ============================================================================
/// `Implicit` 是构造期确定的节点属性,运行期非原子读写：
/// - `ANCHORED`：节点自带异常归档能力(如 Subflow/Runtime 在构造期置位)；
/// - `PREEMPTED`：节点已挂起,等待子任务完成后由 `_schedule_parent` 重入；
/// - `JOIN`：本节点作为前驱时对后继 join_counter 贡献 1;Jump/MultiJump 不置位。
///
/// `Explicit` 是运行期原子状态(多 Worker 并发读写)：
/// - `ANCHORED`：`ExplicitAnchorGuard`/`corun` 在运行期动态开启的异常归档会话；
/// - `EXCEPTION`：本节点或其子链路发生异常,用于 tear_down 识别异常路径；
/// - `CAUGHT`：首个成功 fetch_or 该位的线程获胜,负责写入 `m_exception_ptr`。
///
/// ============================================================================
///  统一边表 m_edges + m_num_successors
/// ============================================================================
/// 前驱与后继共用一个连续数组：
/// @code
///   m_edges = [ succ_0 ... succ_N-1 | pred_0 ... pred_M-1 ]
///                              ↑
///                       m_num_successors
/// @endcode
///
/// 这样可以减少一次 vector 分配，并让邻接关系在内存中连续存放。
/// 插入、删除边时必须维护“后继区间在前，前驱区间在后”的不变式。
///
/// ============================================================================
///  Join Weight / Join Counter
/// ============================================================================
/// 每个节点的入度是前驱 `_join_counted()` 的权重和(非简单边数)：
/// - 普通/Branch/MultiBranch 任务:JOIN 位置 1,权重 = 1,走 fetch_sub 计数协议；
/// - Jump/MultiJump:JOIN 位为 0,权重 = 0,不走计数协议,通过 store(0) 强制触发目标。
///
/// `_set_up_graph()` 会在每轮执行前重新计算 `m_join_weight` 并初始化
/// `m_join_counter`。前驱完成时递减后继计数，计数归零则后继进入就绪队列。
///
/// ============================================================================
///  异常传播与归档
/// ============================================================================
/// `_process_exception()` 会沿 `m_parent` 链向上寻找归档点：
/// 1. 优先找运行期显式锚点 `Explicit::ANCHORED`；
/// 2. 其次找节点自带的隐式锚点 `Implicit::ANCHORED`；
/// 3. 都没有时，异常归档到当前节点。
///
/// 归档通过原子状态位避免并发重复写入：第一个成功设置 `CAUGHT` 的线程保存
/// `m_exception_ptr`，后续线程只传播状态，不覆盖异常对象。
///
/// ============================================================================
///  PREEMPTED 重入协议
/// ============================================================================
/// `RuntimeWork` / `GraphWork` 这类节点可能在执行期间派生子任务或子图，不能在
/// 用户 body 返回后立即 tear_down。它们采用两段式执行：
///
/// 1. 首次进入：设置 `PREEMPTED`，给自身 `join_counter` 增加一个 self-pin，
///    执行用户 body 或启动子图；
/// 2. 如果仍有子任务未完成，当前 worker 直接返回；
/// 3. 最后一个子任务完成时通过 `_schedule_parent` 重新调度父节点；
/// 4. 第二次进入：清除 `PREEMPTED`，执行真正的 tear_down。
///
/// 这使等待子任务完成的过程不阻塞 worker 线程，而是通过调度器协作完成。
///
/// ============================================================================
///  线程安全边界
/// ============================================================================
/// - 构建期：由 `Flow` 单线程编辑，邻接表和名称等字段可写；
/// - 执行期：图结构只读，调度状态通过原子字段维护；
/// - 销毁期：静态图节点由 `Graph` 释放，动态任务由 `Topology` 引用计数归零后释放。
///
/// @see Task              用户层弱句柄
/// @see AsyncTask         动态任务强句柄
/// @see Topology          执行实例与引用计数
/// @see Executor          调度与 tear_down 协议
/// @see works.hpp         Work 派生类与 Invoker 实现


class Work : public Immovable<Work> {

    friend class Graph;
    friend class Flow;
    friend class Task;
    friend class TaskView;
    friend class AsyncTask;
    friend class DeferredAsyncTask;
    friend class Topology;
    friend class Worker;
    friend class Executor;
    friend class Runtime;
    friend class Semaphore;
    friend class Branch;
    friend class MultiBranch;
    friend class Jump;
    friend class MultiJump;
    friend class ExplicitAnchorGuard;
    friend class D2Renderer;

    TFL_WORK_SUBCLASS_FRIENDS;
public:
    /// @brief 隐式属性位域 —— 节点构造期确定，运行期不变，非原子。
    struct Implicit {
        using type = std::uint32_t;
        static constexpr unsigned BITS = sizeof(type) * char_bits;
        static constexpr type NONE      = 0;

        /// @brief 隐式锚点：节点类型自带的异常汇聚点
        ///        （Subflow / Runtime 等抢占式节点在构造期置位）。
        static constexpr type ANCHORED  = type{1} << (BITS - 1);

        /// @brief 可抢占：body 执行期间可能派生 child，
        ///        需挂起节点等待 child 完成后恢复。
        static constexpr type PREEMPTED = type{1} << (BITS - 2);

        /// @brief 是否计入后继 join_counter。
        ///        置位表示本节点作为前驱时，对后继 join_counter 贡献 1；
        ///        Jump / MultiJump 不置位。
        static constexpr type JOIN = type{1} << (BITS - 3);
    };

    /// @brief 显式状态位域 —— 运行期动态设置，多 Worker 并发读写，必须原子访问。
    struct Explicit {
        using type = std::uint32_t;
        static constexpr unsigned BITS = sizeof(type) * char_bits;
        static constexpr type NONE      = 0;

        /// @brief 显式锚点：由 @c AnchorGuard / @c corun 在运行期动态开启的
        ///        异常汇聚会话。置位方与读取方分处不同 Worker，故必须原子。
        static constexpr type ANCHORED  = type{1} << (BITS - 1);

        /// @brief 本节点闭包执行期间抛出了异常（由抛出线程 release 置位）。
        static constexpr type EXCEPTION = type{1} << (BITS - 2);

        /// @brief 异常已被本节点或上游锚点归档，停止继续向下传播。
        static constexpr type CAUGHT    = type{1} << (BITS - 3);
    };


    /// @brief 信号量请求描述符
    struct SemaphoreReq {
        Semaphore* sem;
        std::size_t count;
    };

    /// @brief 节点绑定的信号量集合
    struct SemaphoreData {
        std::vector<SemaphoreReq> acquires; ///< 执行前必须获取的约束及配额
        std::vector<SemaphoreReq> releases; ///< 执行后应当释放的约束及配额

        [[nodiscard]] bool empty() const noexcept {
            return acquires.empty() && releases.empty();
        }
    };


    /// @brief 节点挂载的生命周期观察者集合（按需延迟分配）。
    struct ObserverData {
        std::vector<std::shared_ptr<TaskObserver>> observers;

        [[nodiscard]] bool empty() const noexcept {
            return observers.empty();
        }
    };

    /// @brief 默认构造函数
    explicit Work() = default;

    /// @brief 静态图内节点构造函数
    /// @param type 节点类型 tag,运行期常量
    /// @param implicit 初始选项配置
    /// @param graph 节点归属的任务物理图指针
    explicit Work(const TaskType type, Implicit::type it, Explicit::type et, const Graph* graph) noexcept
        : m_type{type}
        , m_implicit{it}
        , m_explicit{et}
        , m_graph{graph} {}

    /// @brief 独立异步任务节点构造函数 (挂接到外部 Topology)
    /// @param type 节点类型 tag
    /// @param implicit 初始选项配置
    /// @param topo 该任务绑定的独立执行拓扑上下文
    /// @param parent 父节点 (用于异常传播与生命周期追踪)
    explicit Work(const TaskType type, Implicit::type it, Explicit::type et, Topology* topo, Work* parent) noexcept
        : m_type{type}
        , m_implicit{it}
        , m_explicit{et}
        , m_topology{topo}
        , m_parent{parent} {}

    /// @brief 默认对齐对象走 Work 对象池。
    static void* operator new(std::size_t bytes) {
#if defined(TFL_ENABLE_WORK_POOL)
        return ObjectPool<Work>::allocate(bytes);
#else
        return ::operator new(bytes);
#endif
    }

    /// @brief sized delete：根据实际对象大小归还到对应 size class。
    static void operator delete(void* p, std::size_t bytes) noexcept { 
#if defined(TFL_ENABLE_WORK_POOL)
        ObjectPool<Work>::deallocate(p, bytes);
#else
        ::operator delete(p, bytes);
#endif
    }

    /// @brief over-aligned 对象绕过 WorkPool，避免普通池返回低对齐地址。
    static void* operator new(std::size_t bytes, std::align_val_t al) {
        return ::operator new(bytes, al);
    }

    /// @brief over-aligned 对象使用匹配的全局 aligned delete。
    static void operator delete(void* p, std::size_t bytes, std::align_val_t al) noexcept {
        ::operator delete(p, bytes, al);
    }

    /// @brief Work 节点不支持数组分配。
    static void* operator new[](std::size_t) = delete;
    static void operator delete[](void*) noexcept = delete;

    /// @brief Work 节点不支持 nothrow new。
    static void* operator new(std::size_t, const std::nothrow_t&) noexcept = delete;
    static void operator delete(void*, const std::nothrow_t&) noexcept = delete;

    /// @brief 虚析构 —— 允许通过基类指针安全删除派生 Invoker。
    virtual ~Work() noexcept = default;

    /// @brief 任务执行入口 —— 由 Executor 在工作线程上调用。
    /// @param exec   所属 Executor。
    /// @param wr     执行当前任务的 Worker。
    /// @param cache  本地缓存指针，用于批量调度优化。
    virtual void invoke(Executor& exec, Worker& wr, Work*& cache) = 0;

    /// @brief 将当前节点导出为 D2 可视化描述。
    virtual void dump(std::ostream& ostream) const = 0;

protected:
    std::string                     m_name;                     ///< 节点名称（调试/D2 可视化用）
    const Graph*                    m_graph{nullptr};           ///< 归属的任务物理图指针
    const TaskType                  m_type{TaskType::None};     ///< 类型 tag (原虚函数 m_type 的字段化)
    std::exception_ptr              m_exception_ptr{nullptr};   ///< 捕获的异常（冷路径）

    Topology*                       m_topology{nullptr};        ///< 执行拓扑上下文（invoke 开头读）
    Work*                           m_parent{nullptr};          ///< 父容器（tear_down 循环内高频读）
    Implicit::type                  m_implicit{Implicit::NONE};    ///< 静态选项
    std::atomic<Explicit::type>     m_explicit{Explicit::NONE};       ///< 运行期状态 (4B)
    std::size_t                     m_join_weight{0};           // 静态缓存：依赖权重总和
    std::atomic<std::size_t>        m_join_counter{0};          // 运行期动态计数
    std::size_t                     m_num_successors{0};        ///< edges 分割点
    std::vector<Work*>              m_edges;                    ///< [后继|前驱] 统一存储
    std::unique_ptr<SemaphoreData>  m_semaphores;               ///< 信号量约束
    std::unique_ptr<ObserverData>   m_observers;                ///< 生命周期观察者（按需延迟分配）


    [[nodiscard]] bool _join_counted() const noexcept {
        return (m_implicit & Implicit::JOIN) != 0;
    }

    // 本节点的 join 计数 = 所有前驱权重之和
    [[nodiscard]] std::size_t _join_weight() const noexcept {
        std::size_t sum = 0;
        for (std::size_t i = m_num_successors; i < m_edges.size(); ++i) {
            sum += m_edges[i]->_join_counted();
        }
        return sum;
    }

    // Why: 只取本节点直属 topology 的 stop_source。父链路 stop 通过
    //      _stop_requested() 的 traversal 在 invoke 入口处判定；
    //      用户 callable 拿到的 stop_token 是直属 topology 的，
    //      若需感知父链停止，由 topology 构造时 std::stop_callback 链接。
    [[nodiscard]] std::stop_token _stop_token() const noexcept {
        return m_topology ? m_topology->m_stop_source.get_token() : std::stop_token{};   // 无所属 topology：返回空 token，永不 stopped
    }

    /// @brief 查询节点所属 Topology 是否已被外部请求停止。
    [[nodiscard]] bool _stop_requested() const noexcept {
        return m_topology && m_topology->m_stop_source.stop_requested();
    }

    [[nodiscard]] bool _has_exception() const noexcept {
        return m_explicit.load(std::memory_order_relaxed) & Explicit::EXCEPTION;
    }

    [[nodiscard]] bool _should_abort() const noexcept {
        return (m_explicit.load(std::memory_order_relaxed) & Explicit::EXCEPTION) || (m_topology && m_topology->m_stop_source.stop_requested());
    }

    void _rethrow_exception() {
        if (m_exception_ptr) {
            auto e = m_exception_ptr;
            m_exception_ptr = nullptr;
            m_explicit.fetch_and(~(Explicit::EXCEPTION | Explicit::CAUGHT), std::memory_order_relaxed);
            std::rethrow_exception(e);
        }
    }

    /// @brief 异常传播与归档。
    ///
    /// 沿 m_parent 链向上:对中间节点标记 EXCEPTION(供 tear_down 级联取消),
    /// 遇到 Explicit::ANCHORED 或 Implicit::ANCHORED 时停止,将 exception_ptr
    /// 写入该锚点。首个通过 fetch_or 将 CAUGHT 位从 0 翻至 1 的线程获胜,
    /// 其余并发异常的 exception_ptr 被丢弃(与 std::async 的单异常语义一致)。
    ///
    /// 全程使用 relaxed 内存序:EXCEPTION 标记仅作状态查询,CAUGHT+m_exception_ptr
    /// 的可见性由读取端(future::get/tear_down)的 acquire 操作保证。
    void _process_exception() noexcept {
        // ── 阶段 1：沿父链向上传播 EXCEPTION，寻找锚点 ─────────────
        // 循环不变式：
        //   - explicit_anchor 始终为 nullptr（未找到显式锚点时继续向上）
        //   - implicit_anchor 记录沿途首个隐式锚点（后续不再覆盖）
        //   - cur 指向当前检查的节点，沿 m_parent 向上推进
        Work* explicit_anchor = nullptr;
        Work* implicit_anchor = nullptr;
        for (Work* cur = this; cur; cur = cur->m_parent) {
            // 显式锚点优先级最高：一旦遇到立即停止传播，由阶段 2 归档
            if (cur->m_explicit.load(std::memory_order_relaxed) & Explicit::ANCHORED) {
                explicit_anchor = cur;
                break;
            }
            // 非锚点节点打 EXCEPTION 位，供 tear_down 识别异常路径
            cur->m_explicit.fetch_or(Explicit::EXCEPTION, std::memory_order_relaxed);
            // 沿途捕获首个隐式锚点（非原子读 —— Implicit 构造期确定后不变）
            if (!implicit_anchor && (cur->m_implicit & Implicit::ANCHORED)) {
                implicit_anchor = cur;
            }
        }

        // ── 阶段 2：按优先级抢占归档权 ──────────────────────────────
        // archive_mask 同时置 EXCEPTION 和 CAUGHT：
        //   - EXCEPTION 标识锚点自身处于异常路径上（与阶段 1 中间节点一致）
        //   - CAUGHT    作为 CAS 胜出标志，首个将该位从 0 翻至 1 的线程赢
        constexpr auto archive_mask = Explicit::EXCEPTION | Explicit::CAUGHT;

        // 优先级 1：显式锚点
        if (explicit_anchor) {
            auto prev = explicit_anchor->m_explicit.fetch_or(archive_mask, std::memory_order_relaxed);
            if ((prev & Explicit::CAUGHT) == 0) {
                explicit_anchor->m_exception_ptr = std::current_exception();
                return;
            }
            // CAS 失败：已有并发异常先行归档于此，当前异常丢弃 —— 跳至兜底
        }
        // 优先级 2：隐式锚点（仅在显式锚点完全不存在时触发）
        else if (implicit_anchor) {
            auto prev = implicit_anchor->m_explicit.fetch_or(archive_mask, std::memory_order_relaxed);
            if ((prev & Explicit::CAUGHT) == 0) {
                implicit_anchor->m_exception_ptr = std::current_exception();
                return;
            }
            // CAS 失败：理由同上
        }

        // ── 阶段 3：兜底 —— 存于本节点 ─────────────────────────────
        // 适用场景：
        //   - silent_async 顶级任务，无任何父锚点
        //   - 前两级优先级归档时 CAUGHT 竞争失败（异常被丢弃，但本地仍留底）
        //
        // 同样使用 CAS（fetch_or + 检查旧 CAUGHT 位）：
        // 防御 _process_exception 在同一节点被重入触发时的多次覆盖。
        auto prev = m_explicit.fetch_or(archive_mask, std::memory_order_relaxed);
        if ((prev & Explicit::CAUGHT) == 0) {
            m_exception_ptr = std::current_exception();
        }
    }

    [[nodiscard]] std::span<Work*> _successors() noexcept {
        return {m_edges.data(), m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _successors() const noexcept {
        return {m_edges.data(), m_num_successors};
    }
    [[nodiscard]] std::span<Work*> _predecessors() noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _predecessors() const noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }
    [[nodiscard]] std::size_t _num_predecessors() const noexcept {
        return m_edges.size() - m_num_successors;
    }

    [[nodiscard]] SemaphoreData& _ensure_semaphores() {
        if (!m_semaphores) {
            m_semaphores = std::make_unique<SemaphoreData>();
        }
        return *m_semaphores;
    }

    void _try_release_semaphores() noexcept {
        if (m_semaphores && m_semaphores->empty()) {
            m_semaphores.reset();
        }
    }

    [[nodiscard]] std::span<SemaphoreReq> _acquires() noexcept {
        return m_semaphores ? std::span<SemaphoreReq>{m_semaphores->acquires} : std::span<SemaphoreReq>{};
    }
    [[nodiscard]] std::span<SemaphoreReq const> _acquires() const noexcept {
        return m_semaphores ? std::span<SemaphoreReq const>{m_semaphores->acquires} : std::span<SemaphoreReq const>{};
    }
    [[nodiscard]] std::span<SemaphoreReq> _releases() noexcept {
        return m_semaphores ? std::span<SemaphoreReq>{m_semaphores->releases} : std::span<SemaphoreReq>{};
    }
    [[nodiscard]] std::span<SemaphoreReq const> _releases() const noexcept {
        return m_semaphores ? std::span<SemaphoreReq const>{m_semaphores->releases} : std::span<SemaphoreReq const>{};
    }
    [[nodiscard]] std::size_t _num_acquires() const noexcept {
        return m_semaphores ? m_semaphores->acquires.size() : 0;
    }
    [[nodiscard]] std::size_t _num_releases() const noexcept {
        return m_semaphores ? m_semaphores->releases.size() : 0;
    }
    [[nodiscard]] std::size_t _num_observers() const noexcept {
        return m_observers ? m_observers->observers.size() : 0;
    }

    void _acquire(Semaphore* sem, std::size_t count);
    void _release(Semaphore* sem, std::size_t count);
    void _remove_acquire(Semaphore* sem) noexcept;
    void _remove_release(Semaphore* sem) noexcept;
    void _clear_acquires() noexcept;
    void _clear_releases() noexcept;

    [[nodiscard]] bool _try_acquire_semaphores(SmallVector<Work*>& out);
    void _release_semaphores(SmallVector<Work*>& out);

    void _notify_before(Worker& wr) const;
    void _notify_after(Worker& wr) const;

    void _erase_successor_at(std::size_t idx) noexcept;
    void _erase_predecessor_at(std::size_t idx) noexcept;
    void _precede(Work* target);
    void _remove_successor(Work* target) noexcept;
    void _clear_predecessors() noexcept;
    void _clear_successors() noexcept;

    [[nodiscard]] bool _has_path_without_jump(const Work* from, const Work* to) const;
    [[nodiscard]] std::expected<void, std::string_view> _can_precede(Work* target) const;
};


inline void Work::_erase_successor_at(std::size_t idx) noexcept {
    TFL_ASSERT(idx < m_num_successors);
    const std::size_t last_succ = m_num_successors - 1;
    const std::size_t num_preds = _num_predecessors();

    if (idx != last_succ) {
        m_edges[idx] = m_edges[last_succ];
    }
    if (num_preds > 0) {
        m_edges[last_succ] = m_edges.back();
    }
    m_edges.pop_back();
    --m_num_successors;
}

inline void Work::_erase_predecessor_at(std::size_t idx) noexcept {
    TFL_ASSERT(idx < _num_predecessors());
    const std::size_t abs_idx = m_num_successors + idx;
    m_edges[abs_idx] = m_edges.back();
    m_edges.pop_back();
}

inline void Work::_precede(Work* const target) {
    if (auto result = _can_precede(target); !result) {
        throw Exception("cannot precede: {}.", result.error());
    }

    // Why: 双向压入 —— 后继区间在前、前驱区间在后，统一存入 m_edges。
    // 构造期静态建立邻接关系，运行期 join_counter 计算直接复用，零额外开销。
    m_edges.push_back(target);
    if (m_num_successors < m_edges.size() - 1) {
        std::swap(m_edges[m_num_successors], m_edges.back());
    }
    ++m_num_successors;
    target->m_edges.push_back(this);
}

inline void Work::_remove_successor(Work* const target) noexcept {
    if (!target) return;

    auto succ = _successors();
    auto it = std::ranges::find(succ, target);
    if (it == succ.end()) return;
    _erase_successor_at(static_cast<std::size_t>(it - succ.begin()));

    auto pred = target->_predecessors();
    auto pit = std::ranges::find(pred, this);
    TFL_ASSERT(pit != pred.end() && "predecessor must exist");
    target->_erase_predecessor_at(static_cast<std::size_t>(pit - pred.begin()));
}

inline void Work::_clear_predecessors() noexcept {
    for (Work* pred : _predecessors()) {
        auto succ = pred->_successors();
        auto it = std::ranges::find(succ, this);
        TFL_ASSERT(it != succ.end() && "successor must exist");
        pred->_erase_successor_at(static_cast<std::size_t>(it - succ.begin()));
    }

    m_edges.erase(m_edges.begin() + m_num_successors, m_edges.end());
}

inline void Work::_clear_successors() noexcept {
    for (Work* succ : _successors()) {
        auto pred = succ->_predecessors();
        auto it = std::ranges::find(pred, this);
        TFL_ASSERT(it != pred.end() && "predecessor must exist");
        succ->_erase_predecessor_at(static_cast<std::size_t>(it - pred.begin()));
    }

    // 擦除前半段后利用底层 memmove 将前驱节点整体前移
    m_edges.erase(m_edges.begin(), m_edges.begin() + m_num_successors);
    m_num_successors = 0;
}

inline std::expected<void, std::string_view> Work::_can_precede(Work* const target) const {
    if (!target) return std::unexpected{"target is null"};
    if (!m_graph) return std::unexpected{"work not attached to graph"};
    if (m_graph != target->m_graph) return std::unexpected{"works belong to different graphs"};
    if (std::ranges::contains(_successors(), target)) return std::unexpected{"edge already exists"};
    const auto this_type = m_type;
    const bool this_is_jump = (this_type == TaskType::Jump || this_type == TaskType::MultiJump);

    // 1. 自环检测
    if (target == this) {
        if (!this_is_jump) {
            return std::unexpected{"invalid topology: self-loops are exclusively allowed for jump-type nodes"};
        }
        return {};
    }
    if (this_is_jump) return {};
    const auto target_type = target->m_type;
    const bool target_is_jump = (target_type == TaskType::Jump || target_type == TaskType::MultiJump);
    if (target_is_jump) return {};

    // 4. 冷路径：无跳转闭环的 BFS 检测
    if (_has_path_without_jump(target, this)) {
        return std::unexpected{"invalid topology: strict cycle detected without any jump-type node"};
    }

    return {};
}
inline bool Work::_has_path_without_jump(const Work* from, const Work* to) const {
    if (!from || !to) return false;

    // 显式指定底层使用 vector，保留连续内存的 L1 缓存预取优势
    std::stack<const Work*, std::vector<const Work*>> dfs_stack;
    unordered_dense::set<const Work*> visited;

    // 预分配容量，避免扩容开销
    visited.reserve(64);

    dfs_stack.push(from);
    visited.insert(from);

    // 标准的 DFS 迭代搜索
    while (!dfs_stack.empty()) {
        const Work* curr = dfs_stack.top();
        dfs_stack.pop();

        for (const auto* succ : curr->_successors()) {
            if (succ == to) return true; // 发现死锁回环

            const auto st = succ->m_type;
            if (st == TaskType::Jump || st == TaskType::MultiJump) continue; // 遇跳转截断

            // O(1) 查重，如果是新节点则压栈，准备深搜
            if (visited.insert(succ).second) {
                dfs_stack.push(succ);
            }
        }
    }

    return false;
}

inline void Work::_acquire(Semaphore* sem, std::size_t count) {
    if (!sem) throw Exception("cannot acquire null semaphore.");
    if (count == 0) return; // 忽略空请求,避免语义歧义

    auto& sd = _ensure_semaphores();

    // 闭区间线性查重,小 N 下优于哈希
    for (std::size_t i = 0; i < sd.acquires.size(); ++i) {
        if (sd.acquires[i].sem == sem) {
            throw Exception("semaphore already in acquire list.");
        }
    }

    sd.acquires.emplace_back(sem, count);
}

inline void Work::_release(Semaphore* sem, std::size_t count) {
    if (!sem) throw Exception("cannot release null semaphore.");
    if (count == 0) return;

    auto& sd = _ensure_semaphores();

    // 闭区间线性查重,小 N 下优于哈希
    for (std::size_t i = 0; i < sd.releases.size(); ++i) {
        if (sd.releases[i].sem == sem) {
            throw Exception("semaphore already in release list.");
        }
    }

    sd.releases.emplace_back(sem, count);
}


inline void Work::_remove_acquire(Semaphore* sem) noexcept {
    if (!m_semaphores) return;
    auto& acqs = m_semaphores->acquires;
    for (std::size_t i = 0; i < acqs.size(); ++i) {
        if (acqs[i].sem == sem) {
            acqs[i] = acqs.back();
            acqs.pop_back();
            _try_release_semaphores();
            return;
        }
    }
}

inline void Work::_remove_release(Semaphore* sem) noexcept {
    if (!m_semaphores) return;
    auto& rels = m_semaphores->releases;

    for (std::size_t i = 0; i < rels.size(); ++i) {
        if (rels[i].sem == sem) {
            rels[i] = rels.back();
            rels.pop_back();
            _try_release_semaphores();
            return;
        }
    }
}

inline void Work::_clear_acquires() noexcept {
    if (!m_semaphores) return;
    m_semaphores->acquires.clear();
    _try_release_semaphores();
}

inline void Work::_clear_releases() noexcept {
    if (!m_semaphores) return;
    m_semaphores->releases.clear();
    _try_release_semaphores();
}

/// @brief 尝试获取本节点声明的所有信号量配额
/// @param out 回滚时释放的 waiters 汇总到此;调用方负责消费并调度
/// @return 成功获取全部配额返回 true;部分失败则已完成回滚返回 false
///
/// @note 返回 false 时 out 可能非空——这些是回滚过程中解冻的其他等待者,
///       调用方必须将它们推回调度器,否则会造成饥饿
TFL_FORCE_INLINE bool Work::_try_acquire_semaphores(SmallVector<Work*>& out) {
    auto& acqs = m_semaphores->acquires;
    for (std::size_t i = 0; i < acqs.size(); ++i) {
        if (!acqs[i].sem->_try_acquire(this, acqs[i].count)) {
            for (std::size_t j = i; j > 0; --j) {
                acqs[j - 1].sem->_release(acqs[j - 1].count, out);
            }
            return false;
        }
    }
    return true;
}

/// @brief 释放本节点声明的所有信号量配额
/// @param out 解冻的 waiters 汇总到此;调用方负责调度
TFL_FORCE_INLINE void Work::_release_semaphores(SmallVector<Work*>& out) {
    for (const auto& req : m_semaphores->releases) {
        req.sem->_release(req.count, out);
    }
}


TFL_FORCE_INLINE void Work::_notify_before(Worker& wr) const {
    if (!m_observers) [[likely]] return;
    for (auto& aspect : m_observers->observers) {
        aspect->on_before(WorkerView{wr});
    }
}

TFL_FORCE_INLINE void Work::_notify_after(Worker& wr) const {
    if (!m_observers) [[likely]] return;
    for (auto& aspect : m_observers->observers) {
        aspect->on_after(WorkerView{wr});
    }
}


/// @brief RAII 守卫 —— 在作用域内动态置 / 清节点的显式锚点位。
///
/// @details
/// 用于 `Runtime::cowait` / `corun` 等阻塞调用点 ——
/// 进入阻塞窗口时把节点临时标为显式锚点（让作用域内派生的 child 异常归档
/// 到此），退出时清除。
///
/// ============================================================================
///  幂等性 —— 防破坏外层语义
/// ============================================================================
/// 若构造时节点 **已是** 显式锚点（嵌套场景或工厂期已设定），守卫感知后既不
/// 重置也不清除：通过 `fetch_or` 返回旧值检查 ANCHORED 位，仅在自己"真正
/// 拥有"该置位时才负责复位。
///
/// 典型嵌套场景：
/// @code
///   {
///       ExplicitAnchorGuard g1{w};   // 外层置位，m_owned = true
///       {
///           ExplicitAnchorGuard g2{w};  // 已置位，m_owned = false
///       }   // g2 析构：不清位（被外层占据）
///   }   // g1 析构：清位
/// @endcode
///
/// 这种"幂等 + 嵌套安全"是 RAII 守卫的标准设计要求。
///
/// @see Runtime::cowait / Executor::_cowait_until  使用点
class ExplicitAnchorGuard {
public:
    explicit ExplicitAnchorGuard(Work* w) noexcept : m_work{w} {
        // fetch_or 返回旧值；旧值已含 ANCHORED 说明此前就是锚点
        const auto prev = m_work->m_explicit.fetch_or(
            Work::Explicit::ANCHORED, std::memory_order_relaxed);
        m_owned = (prev & Work::Explicit::ANCHORED) == 0;
    }

    ~ExplicitAnchorGuard() {
        if (m_owned) {
            m_work->m_explicit.fetch_and(~Work::Explicit::ANCHORED,
                                         std::memory_order_relaxed);
        }
    }

    ExplicitAnchorGuard(const ExplicitAnchorGuard&) = delete;
    ExplicitAnchorGuard& operator=(const ExplicitAnchorGuard&) = delete;

private:
    Work* const m_work;
    bool        m_owned;   ///< 本守卫是否真的"拥有"锚点位的置/清责任
};


}  // namespace tfl
