/// @file  work.hpp
/// @brief DAG 任务节点基类 Work —— 框架最核心的内部数据结构。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
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

/// @brief DAG 任务节点基类 —— 框架内部的统一执行单元。
///
/// 承载调度、依赖计数（m_join_counter）、异常归档、信号量和观察者等运行时状态。
/// 边表统一布局为 [succs | preds]，单次分配双倍 cache 友好。
///
/// Implicit 标志（构造期确定，非原子）：ANCHORED / PREEMPTED / JOIN。
/// Explicit 标志（运行期原子）：ANCHORED / EXCEPTION / CAUGHT。
///
/// @note 构建期单线程编辑邻接表；执行期图结构只读，调度状态通过原子字段维护。
class Work : public Immovable<Work> {

    friend class Graph;
    friend class Flow;
    friend class Task;
    friend class TaskView;
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
    template <typename> friend class AsyncTask;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 隐式属性位域 —— 构造期确定，运行期非原子只读。
    ///
    /// 这些位描述节点类型固有的、不可变的行为属性。因为运行时不会改变，
    /// 使用普通 uint32_t 而非 atomic，避免为常量属性付出原子操作开销。
    struct Implicit {
        using type = std::uint32_t;
        static constexpr unsigned BITS = sizeof(type) * char_bits;
        static constexpr type NONE      = 0;

        /// @brief 隐式锚点: 节点类型自带的异常汇聚点。
        ///
        /// Subflow / Runtime 等可抢占节点在构造期置位，表示该节点可以承接
        /// 子链路抛出的异常。
        static constexpr type ANCHORED  = type{1} << (BITS - 1);

        /// @brief 可抢占: body 执行期间可能派生 child，需挂起等待恢复。
        static constexpr type PREEMPTED = type{1} << (BITS - 2);

        /// @brief 是否计入后继 join_counter。
        ///
        /// 置位 = 本节点作为前驱时对后继贡献 1；Jump/MultiJump 不置位（贡献 0）。
        static constexpr type JOIN = type{1} << (BITS - 3);
    };

    /// @brief 显式状态位域 —— 运行期动态设置，多 Worker 并发读写，必须原子访问。
    ///
    /// 这些位描述节点在单次执行中的瞬时状态，由不同 Worker 并发修改。
    /// 使用 atomic<uint32_t> 保证原子性，配合 relaxed/acq_rel 内存序。
    struct Explicit {
        using type = std::uint32_t;
        static constexpr unsigned BITS = sizeof(type) * char_bits;
        static constexpr type NONE      = 0;

        /// @brief 显式锚点: ExplicitAnchorGuard / corun 在运行期动态开启的异常汇聚会话。
        ///
        /// @note 置位方与读取方分处不同 Worker，必须原子。
        static constexpr type ANCHORED  = type{1} << (BITS - 1);

        /// @brief 本节点闭包执行期间抛出了异常（由抛出线程 release 置位）。
        static constexpr type EXCEPTION = type{1} << (BITS - 2);

        /// @brief 异常已被本节点或上游锚点归档，停止继续向上传播。
        static constexpr type CAUGHT    = type{1} << (BITS - 3);
    };


    /// @brief 信号量请求描述符。
    struct SemaphoreReq {
        Semaphore* sem;     ///< 目标信号量
        std::size_t count;  ///< 请求配额
    };

    /// @brief 节点绑定的信号量集合。
    ///
    /// acquire = 执行前必须获取的约束（阻塞型）；release = 执行后应当释放的约束。
    struct SemaphoreData {
        std::vector<SemaphoreReq> acquires; ///< 执行前必须获取
        std::vector<SemaphoreReq> releases; ///< 执行后应当释放

        [[nodiscard]] bool empty() const noexcept {
            return acquires.empty() && releases.empty();
        }
    };


    /// @brief 节点挂载的生命周期观察者集合（按需延迟分配）。
    ///
    /// 观察者在任务执行前后被回调（on_before / on_after），用于日志、性能采样等。
    struct ObserverData {
        std::vector<std::shared_ptr<TaskObserver>> observers;

        [[nodiscard]] bool empty() const noexcept {
            return observers.empty();
        }
    };

    /// @brief 默认构造空节点，所有字段零/空初始化（m_type=TaskType::None, m_join_counter=0, m_implicit/m_explicit=NONE）。
    explicit Work() = default;

    /// @brief 静态图内节点构造函数。
    ///
    /// @param type     节点类型 tag（运行期常量）。
    /// @param implicit 初始隐式属性配置。
    /// @param explicit 初始显式状态配置。
    /// @param graph    节点归属的任务物理图指针。
    explicit Work(const TaskType type, Implicit::type it, Explicit::type et, const Graph* graph) noexcept
        : m_type{type}
        , m_implicit{it}
        , m_explicit{et}
        , m_graph{graph} {}

    /// @brief 独立异步任务节点构造函数（挂接到外部 Topology）。
    ///
    /// @param type     节点类型 tag。
    /// @param implicit 初始隐式属性配置。
    /// @param explicit 初始显式状态配置。
    /// @param topo     该任务绑定的独立执行拓扑上下文。
    /// @param parent   父节点（异常传播与生命周期追踪用）。
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

    /// @brief sized delete: 根据实际对象大小归还到对应 size class。
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
    ///
    /// @param exec   所属 Executor。
    /// @param wr     执行当前任务的 Worker。
    /// @param cache  本地缓存指针，用于批量调度优化。
    virtual void invoke(Executor& exec, Worker& wr, Work*& cache) = 0;

    /// @brief 将当前节点导出为 D2 可视化描述。
    virtual void dump(std::ostream& ostream) const = 0;

protected:
    std::string                     m_name;                     ///< 节点名称（调试/D2 可视化用）
    const Graph*                    m_graph{nullptr};           ///< 归属的任务物理图指针
    const TaskType                  m_type{TaskType::None};     ///< 类型 tag (原虚函数的字段化替代)
    std::exception_ptr              m_exception_ptr{nullptr};   ///< 捕获的异常（冷路径）

    Topology*                       m_topology{nullptr};        ///< 执行拓扑上下文（invoke 开头读取）
    Work*                           m_parent{nullptr};          ///< 父容器（tear_down 循环内高频读取）
    Implicit::type                  m_implicit{Implicit::NONE}; ///< 静态隐式属性（构造期确定，运行期只读）
    std::atomic<Explicit::type>     m_explicit{Explicit::NONE}; ///< 运行期显式状态 (4B, 多 Worker 原子访问)
    std::size_t                     m_join_weight{0};           ///< 静态缓存: 依赖权重总和（_set_up_graph 计算）
    std::atomic<std::size_t>        m_join_counter{0};          ///< 运行期动态递减计数（归零 = 就绪）
    std::size_t                     m_num_successors{0};        ///< edges 分割点: 后继数量
    std::vector<Work*>              m_edges;                    ///< [后继 | 前驱] 统一存储
    std::unique_ptr<SemaphoreData>  m_semaphores;               ///< 信号量约束（按需分配）
    std::unique_ptr<ObserverData>   m_observers;                ///< 生命周期观察者（按需分配）


    /// @brief 查询本节点作为前驱时是否计入后继的 join_counter。
    ///
    /// @return JOIN 位置位返回 true; Jump/MultiJump 返回 false。
    ///
    /// @note 此法在邻接表遍历时高频调用。JOIN 位是 Implicit 属性，
    ///       构造期固定，运行期非原子读取无数据竞争风险。
    [[nodiscard]] bool _join_counted() const noexcept {
        return (m_implicit & Implicit::JOIN) != 0;
    }

    /// @brief 计算本节点的 join 权重 = 所有"计入计数"的前驱权重之和。
    ///
    /// @return 所有权重之和。非计数的前驱（Jump/MultiJump）贡献 0。
    [[nodiscard]] std::size_t _join_weight() const noexcept {
        std::size_t sum = 0;
        for (std::size_t i = m_num_successors; i < m_edges.size(); ++i) {
            sum += m_edges[i]->_join_counted();
        }
        return sum;
    }

    /// @brief 获取本节点直属 topology 的 stop_token。
    ///
        ///
    /// @note 只取直属 topology。父链路 stop 通过 _stop_requested() 的
    ///       traversal 在 invoke 入口处判定。用户 callable 拿到的 stop_token
    ///       是直属 topology 的；若需感知父链停止，由 topology 构造时
    ///       std::stop_callback 链接。
    [[nodiscard]] std::stop_token _stop_token() const noexcept {
        return m_topology ? m_topology->m_stop_source.get_token() : std::stop_token{};
    }

    /// @brief 查询节点所属 Topology 是否已被外部请求停止。
    [[nodiscard]] bool _stop_requested() const noexcept {
        return m_topology && m_topology->m_stop_source.stop_requested();
    }

    /// @brief 本节点是否处于异常路径（查询 EXCEPTION 位）。
    [[nodiscard]] bool _has_exception() const noexcept {
        return m_explicit.load(std::memory_order_relaxed) & Explicit::EXCEPTION;
    }

    /// @brief 是否应该中止执行 —— 异常或外部停止。
    ///
    /// @return 本节点标记了异常 OR 所属 Topology 已请求停止。
    ///
    /// @note 用于 invoke 入口快速跳过 body。relaxed 即可——EXCEPTION 位仅作
    ///       状态查询，stop_requested 的可见性由 stop_source 内部保证。
    [[nodiscard]] bool _should_abort() const noexcept {
        return (m_explicit.load(std::memory_order_relaxed) & Explicit::EXCEPTION)
            || (m_topology && m_topology->m_stop_source.stop_requested());
    }

    /// @brief 重新抛出已捕获的异常，并清除异常状态。
    ///
    /// @pre m_exception_ptr 非空。
    /// @post m_exception_ptr 置空，EXCEPTION 和 CAUGHT 位均清除。
    void _rethrow_exception() {
        if (m_exception_ptr) {
            auto e = m_exception_ptr;
            m_exception_ptr = nullptr;
            m_explicit.fetch_and(~(Explicit::EXCEPTION | Explicit::CAUGHT), std::memory_order_relaxed);
            std::rethrow_exception(e);
        }
    }

    /// @brief 异常传播与归档 —— 沿 m_parent 链向上寻找锚点并写入 exception_ptr。
    ///
    /// @details
    /// 三阶段协议:
    ///
    /// 阶段 1: 沿父链向上传播 EXCEPTION 标记，寻找锚点
    /// - 遇到 Explicit::ANCHORED 立即停止（显式锚点优先级最高）
    /// - 非锚点节点打 EXCEPTION 位（供 tear_down 识别异常路径并级联取消）
    /// - 沿途记录首个 Implicit::ANCHORED 节点
    ///
    /// 阶段 2: 按优先级竞争归档权（CAS 胜者写入）
    /// - 优先级 1: 显式锚点（ExplicitAnchorGuard/corun 设置）
    /// - 优先级 2: 隐式锚点（节点类型自带）
    /// - 同时置 EXCEPTION | CAUGHT，检查旧 CAUGHT 位判断是否首个
    ///
    /// 阶段 3: 兜底 —— 存入当前节点
    /// - 适用: detach 顶级任务无父锚点，或前两级 CAS 竞争失败
    ///
        ///            m_exception_ptr 的可见性由读取端 (future::get / tear_down)
    ///            的 acquire 操作保证。
    ///
    /// @note 若所有锚点均已有并发异常先行归档（CAUGHT 竞争失败），当前异常被丢弃
    ///       —— 与 std::async 的单异常语义一致。
    void _process_exception() noexcept {
        // 阶段 1: 沿父链向上传播 EXCEPTION，寻找锚点
        // 循环不变式:
        //   - explicit_anchor = nullptr until an explicit anchor is found
        //   - implicit_anchor 记录沿途首个隐式锚点
        //   - cur 沿 m_parent 向上推进
        Work* explicit_anchor = nullptr;
        Work* implicit_anchor = nullptr;
        for (Work* cur = this; cur; cur = cur->m_parent) {
            // 显式锚点优先级最高: 遇到立即停止, 由阶段 2 归档
            if (cur->m_explicit.load(std::memory_order_relaxed) & Explicit::ANCHORED) {
                explicit_anchor = cur;
                break;
            }
            // 非锚点节点打 EXCEPTION 位, 供 tear_down 识别异常路径
            cur->m_explicit.fetch_or(Explicit::EXCEPTION, std::memory_order_relaxed);
            // 沿途捕获首个隐式锚点（非原子读 —— Implicit 构造期确定后不变）
            if (!implicit_anchor && (cur->m_implicit & Implicit::ANCHORED)) {
                implicit_anchor = cur;
            }
        }

        // 阶段 2: 按优先级抢占归档权
        // archive_mask 同时置 EXCEPTION 和 CAUGHT:
        //   - EXCEPTION: 标识锚点自身处于异常路径（与阶段 1 中间节点一致）
        //   - CAUGHT   : CAS 胜出标志, 首个将该位从 0 翻至 1 的线程赢
        constexpr auto archive_mask = Explicit::EXCEPTION | Explicit::CAUGHT;

        // 优先级 1: 显式锚点
        if (explicit_anchor) {
            auto prev = explicit_anchor->m_explicit.fetch_or(archive_mask, std::memory_order_relaxed);
            if ((prev & Explicit::CAUGHT) == 0) {
                explicit_anchor->m_exception_ptr = std::current_exception();
                return;
            }
            // CAS 失败: 已有并发异常先行归档于此，当前异常丢弃
        }
        // 优先级 2: 隐式锚点（仅在显式锚点完全不存在时触发）
        else if (implicit_anchor) {
            auto prev = implicit_anchor->m_explicit.fetch_or(archive_mask, std::memory_order_relaxed);
            if ((prev & Explicit::CAUGHT) == 0) {
                implicit_anchor->m_exception_ptr = std::current_exception();
                return;
            }
        }

        // 阶段 3: 兜底 —— 存于本节点
        // 适用场景:
        //   - detach 顶级任务, 无任何父锚点
        //   - 前两级 CAS 竞争失败（异常被丢弃, 但本地仍留底）
        //
        // 同样使用 CAS (fetch_or + 检查旧 CAUGHT 位):
        // 防御 _process_exception 在同一节点被重入触发时的多次覆盖
        auto prev = m_explicit.fetch_or(archive_mask, std::memory_order_relaxed);
        if ((prev & Explicit::CAUGHT) == 0) {
            m_exception_ptr = std::current_exception();
        }
    }

    /// @brief 获取后继节点的 span（只读视图）。
    [[nodiscard]] std::span<Work*> _successors() noexcept {
        return {m_edges.data(), m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _successors() const noexcept {
        return {m_edges.data(), m_num_successors};
    }

    /// @brief 获取前驱节点的 span（只读视图）。
    [[nodiscard]] std::span<Work*> _predecessors() noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _predecessors() const noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }

    /// @brief 前驱节点数量。
    [[nodiscard]] std::size_t _num_predecessors() const noexcept {
        return m_edges.size() - m_num_successors;
    }

    /// @brief 按需创建信号量数据。
    [[nodiscard]] SemaphoreData& _ensure_semaphores() {
        if (!m_semaphores) {
            m_semaphores = std::make_unique<SemaphoreData>();
        }
        return *m_semaphores;
    }

    /// @brief 若信号量数据为空则释放内存。
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

    // ---- 信号量管理 ----
    void _acquire(Semaphore* sem, std::size_t count);
    void _release(Semaphore* sem, std::size_t count);
    void _remove_acquire(Semaphore* sem) noexcept;
    void _remove_release(Semaphore* sem) noexcept;
    void _clear_acquires() noexcept;
    void _clear_releases() noexcept;

    [[nodiscard]] bool _try_acquire_semaphores(SmallVector<Work*>& out);
    void _release_semaphores(SmallVector<Work*>& out);

    // ---- 观察者通知 ----
    void _notify_before(Worker& wr) const;
    void _notify_after(Worker& wr) const;

    // ---- 边管理 ----
    void _erase_successor_at(std::size_t idx) noexcept;
    void _erase_predecessor_at(std::size_t idx) noexcept;
    void _precede(Work* target);
    void _remove_successor(Work* target) noexcept;
    void _clear_predecessors() noexcept;
    void _clear_successors() noexcept;

    // ---- 图校验 ----
    [[nodiscard]] bool _has_path_without_jump(const Work* from, const Work* to) const;
    [[nodiscard]] std::expected<void, std::string_view> _can_precede(Work* target) const;
};


/// @brief 在 m_edges 中删除指定下标的后继节点，维护"后继在前、前驱在后"不变式。
///
/// 算法:
/// 1. 若目标不是最后一个后继，用最后一个后继覆盖之
/// 2. 若有前驱，将最后一个后继位置用最后一个前驱覆盖
/// 3. pop_back + 递减 m_num_successors
///
/// @pre idx < m_num_successors
/// @post m_edges 大小减少 1，m_num_successors 减少 1
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

/// @brief 在 m_edges 中删除指定下标的前驱节点。
///
/// 前驱区间 [m_num_successors, end)，直接用最后一个元素覆盖目标位置再 pop_back。
///
/// @pre idx < _num_predecessors()
/// @post m_edges 大小减少 1，m_num_successors 不变
inline void Work::_erase_predecessor_at(std::size_t idx) noexcept {
    TFL_ASSERT(idx < _num_predecessors());
    const std::size_t abs_idx = m_num_successors + idx;
    m_edges[abs_idx] = m_edges.back();
    m_edges.pop_back();
}

/// @brief 添加一条单向边: this -> target（并确保双向对称）。
///
/// @param target 目标节点。
///
/// @throws Exception 若建边不合法（自环、跨图、闭环等）。
///
/// @note 构造期静态建立邻接关系。将 target 插入后继区间末尾，
///       在 target 侧将 this 插入前驱区间末尾。运行期 join_counter 计算
///       直接复用此数据结构，零额外开销。
inline void Work::_precede(Work* const target) {
    if (auto result = _can_precede(target); !result) {
        throw Exception("cannot precede: {}.", result.error());
    }

    // 双向压入: 后继区间在前, 前驱区间在后, 统一存入 m_edges
    m_edges.push_back(target);
    if (m_num_successors < m_edges.size() - 1) {
        std::swap(m_edges[m_num_successors], m_edges.back());
    }
    ++m_num_successors;
    target->m_edges.push_back(this);
}

/// @brief 删除到 target 的出边，同时在 target 侧删除对应入边。
///
/// @param target 目标节点（nullptr 时无操作）。
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

/// @brief 清除所有入边（前驱），同时在每个前驱侧清除对应的出边。
///
/// @post 所有前驱的 successor 列表中不再包含 this。
inline void Work::_clear_predecessors() noexcept {
    for (Work* pred : _predecessors()) {
        auto succ = pred->_successors();
        auto it = std::ranges::find(succ, this);
        TFL_ASSERT(it != succ.end() && "successor must exist");
        pred->_erase_successor_at(static_cast<std::size_t>(it - succ.begin()));
    }

    m_edges.erase(m_edges.begin() + m_num_successors, m_edges.end());
}

/// @brief 清除所有出边（后继），同时在每个后继侧清除对应的入边。
///
/// @details
/// 擦除前半段后利用底层 memmove 将前驱区间整体前移，保持后继区间在前的布局。
///
/// @post m_num_successors = 0, m_edges 仅含前驱。
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

/// @brief 校验 this -> target 建边是否合法。
///
/// 检查项:
/// 1. target 非空
/// 2. 同图（跨图建边非法）
/// 3. 不重复建边
/// 4. 无非法闭环（Jump 类型允许自环；闭环中存在 Jump 节点 = 合法）
///
/// @param target 目标节点。
/// @return 合法返回 void; 非法返回带错误描述的 std::unexpected。
///
/// @note Jump/MultiJump 可形成闭环（用于实现循环控制流），所以仅对不含 Jump
///       的严格闭环报错。
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

    // 4. 冷路径: 无跳转闭环的 BFS 检测
    if (_has_path_without_jump(target, this)) {
        return std::unexpected{"invalid topology: strict cycle detected without any jump-type node"};
    }

    return {};
}

/// @brief 从 from 到 to 是否存在不含 Jump 节点的路径（DFS 搜索）。
///
/// @param from 起点。
/// @param to   终点。
/// @return 存在不含 Jump 的路径返回 true。
///
/// @note 使用 std::stack + std::vector 底层容器，保留连续内存的 L1 缓存预取优势。
///       预分配 visited 哈希表容量 (64)，避免扩容开销。
inline bool Work::_has_path_without_jump(const Work* from, const Work* to) const {
    if (!from || !to) return false;

    // 显式指定底层使用 vector, 保留连续内存的 L1 缓存预取优势
    std::stack<const Work*, std::vector<const Work*>> dfs_stack;
    unordered_dense::set<const Work*> visited;

    // 预分配容量, 避免扩容开销
    visited.reserve(64);

    dfs_stack.push(from);
    visited.insert(from);

    // 标准 DFS 迭代搜索
    while (!dfs_stack.empty()) {
        const Work* curr = dfs_stack.top();
        dfs_stack.pop();

        for (const auto* succ : curr->_successors()) {
            if (succ == to) return true; // 发现目标

            const auto st = succ->m_type;
            if (st == TaskType::Jump || st == TaskType::MultiJump) continue; // 遇跳转截断

            // O(1) 查重, 新节点则压栈
            if (visited.insert(succ).second) {
                dfs_stack.push(succ);
            }
        }
    }

    return false;
}

/// @brief 向节点添加信号量获取约束。
///
/// @param sem   信号量。
/// @param count 需要获取的配额。
///
/// @throws Exception 若 sem 为空或已存在。
///
/// @note count=0 时忽略（避免语义歧义）。
/// @note 小 N 线性查重，优于哈希（acquire 列表通常很小）。
inline void Work::_acquire(Semaphore* sem, std::size_t count) {
    if (!sem) throw Exception("cannot acquire null semaphore.");
    if (count == 0) return; // 忽略空请求, 避免语义歧义

    auto& sd = _ensure_semaphores();

    // 闭区间线性查重, 小 N 下优于哈希
    for (std::size_t i = 0; i < sd.acquires.size(); ++i) {
        if (sd.acquires[i].sem == sem) {
            throw Exception("semaphore already in acquire list.");
        }
    }

    sd.acquires.emplace_back(sem, count);
}

/// @brief 向节点添加信号量释放约束。
///
/// @param sem   信号量。
/// @param count 需要释放的配额。
///
/// @throws Exception 若 sem 为空或已存在。
inline void Work::_release(Semaphore* sem, std::size_t count) {
    if (!sem) throw Exception("cannot release null semaphore.");
    if (count == 0) return;

    auto& sd = _ensure_semaphores();

    // 闭区间线性查重, 小 N 下优于哈希
    for (std::size_t i = 0; i < sd.releases.size(); ++i) {
        if (sd.releases[i].sem == sem) {
            throw Exception("semaphore already in release list.");
        }
    }

    sd.releases.emplace_back(sem, count);
}

/// @brief 移除指定信号量的获取约束（swap-with-last 删除）。
///
/// @param sem 要移除的信号量。
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

/// @brief 移除指定信号量的释放约束（swap-with-last 删除）。
///
/// @param sem 要移除的信号量。
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

/// @brief 清空所有获取约束。
inline void Work::_clear_acquires() noexcept {
    if (!m_semaphores) return;
    m_semaphores->acquires.clear();
    _try_release_semaphores();
}

/// @brief 清空所有释放约束。
inline void Work::_clear_releases() noexcept {
    if (!m_semaphores) return;
    m_semaphores->releases.clear();
    _try_release_semaphores();
}

/// @brief 尝试获取本节点声明的所有信号量配额。
///
/// @param out 回滚时释放的 waiters 汇总到此处；调用方负责消费并调度。
/// @return 成功获取全部配额返回 true；部分失败则已完成回滚返回 false。
///
/// @warning 返回 false 时 out 可能非空 —— 这些是回滚过程中解冻的其他等待者，
///          调用方必须将它们推回调度器，否则会造成饥饿。
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

/// @brief 释放本节点声明的所有信号量配额。
///
/// @param out 解冻的 waiters 汇总到此处；调用方负责调度。
TFL_FORCE_INLINE void Work::_release_semaphores(SmallVector<Work*>& out) {
    for (const auto& req : m_semaphores->releases) {
        req.sem->_release(req.count, out);
    }
}

/// @brief 执行前通知所有注册的观察者（on_before 回调）。
///
/// @note [[likely]] 标记空集合快速路径。
TFL_FORCE_INLINE void Work::_notify_before(Worker& wr) const {
    if (!m_observers) [[likely]] return;
    for (auto& aspect : m_observers->observers) {
        aspect->on_before(WorkerView{wr});
    }
}

/// @brief 执行后通知所有注册的观察者（on_after 回调）。
///
/// @note [[likely]] 标记空集合快速路径。
TFL_FORCE_INLINE void Work::_notify_after(Worker& wr) const {
    if (!m_observers) [[likely]] return;
    for (auto& aspect : m_observers->observers) {
        aspect->on_after(WorkerView{wr});
    }
}


/// @brief RAII 守卫 —— 在作用域内动态置/清节点的显式锚点位。
///
/// 用于 Runtime::cowait / corun 等阻塞调用点，嵌套安全（幂等性）。
/// 通过 fetch_or 返回旧值检查 ANCHORED 位，仅在自己"真正拥有"该置位时才负责复位。
class ExplicitAnchorGuard {
public:
    /// @brief 构造守卫，在目标节点上置显式锚点位。
    ///
    /// @param w 目标 Work 节点。
    ///
    /// @post 若此前 ANCHORED 未置位，则置位且 m_owned=true；
    ///       若已置位（嵌套），则 m_owned=false（不拥有该位）。
    explicit ExplicitAnchorGuard(Work* w) noexcept : m_work{w} {
        // fetch_or 返回旧值; 旧值已含 ANCHORED 说明此前就是锚点
        const auto prev = m_work->m_explicit.fetch_or(
            Work::Explicit::ANCHORED, std::memory_order_relaxed);
        m_owned = (prev & Work::Explicit::ANCHORED) == 0;
    }

    /// @brief 析构守卫，仅当本实例拥有该位时才清除。
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
    bool        m_owned;   ///< 本守卫是否真正"拥有"锚点位的置/清责任
};


}  // namespace tfl
