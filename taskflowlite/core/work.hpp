/// @file work.hpp
/// @brief DAG 任务节点、执行状态、依赖关系及运行期辅助逻辑。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <span>
#include <stack>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "enums.hpp"
#include "topology.hpp"
#include "utility.hpp"
#include "exception.hpp"
#include "observer.hpp"
#include "semaphore.hpp"
#include "small_vector.hpp"
#include "work_storage.hpp"
#include "object_pool.hpp"
#include "macros.hpp"
namespace tfl {

/// @brief 表示调度器执行的一个任务节点及其静态关系、运行状态和执行载荷。
///
/// `Work` 按值拥有 Payload、边表、名称以及按需创建的信号量/观察者描述，
/// 非拥有地引用相邻 Work、所属 Graph、父 Work 和 Topology。具体 callable 与执行语义由 Payload 中的 Invoker 提供。
///
/// @note 类型不可复制或移动；静态图关系在构建期修改，执行期间仅允许按调度协议更新运行期状态。
/// @warning Work 的存储来源必须与销毁路径匹配；栈上 AnchorWork 不得交给 `destroy_work()` 回收。
class Work : public Immovable<Work> {

    friend class Graph;
    friend class Task;
    friend class TaskView;
    friend class Worker;
    friend class Executor;
    friend class Runtime;
    friend class SubFlow;
    friend class Semaphore;
    friend class Branch;
    friend class MultiBranch;
    friend class Jump;
    friend class MultiJump;
    friend class ScopedExceptionAnchor;
    friend class D2Renderer;
    friend class TaskGroup;
    friend class FlowBuilder;
    template <typename> friend class AsyncFuture;
    template <typename> friend class AsyncTask;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief Work 的非原子属性位、独占执行状态和静态 join weight。
    ///
    /// 这些位仅在不存在并发写入的阶段修改；运行期需要修改的状态由当前执行该 Work 的 Worker 独占访问。
    /// 高位保存行为/独占状态标志，剩余低位保存由强前驱数量编码得到的静态 join weight。
    struct Properties {
        using type = std::uint32_t;

        static constexpr unsigned BITS = std::numeric_limits<type>::digits;
        static constexpr type NONE = 0;

        /// @brief 当前节点具备隐式异常归档能力。
        ///
        /// SubFlow / Runtime 等能够承接内部子链异常的节点可置位；异常传播时，
        /// 在不存在显式异常锚点的情况下可选择沿父链遇到的首个隐式锚点归档。
        static constexpr type IMPLICIT_ANCHOR = type{1} << (BITS - 1);

        /// @brief 当前节点执行可因派生子任务而进入挂起/恢复状态。
        ///
        /// body 派生需要等待的 child 后置位；父节点完成等待条件后由调度器恢复执行。
        static constexpr type PREEMPTED = type{1} << (BITS - 2);

        /// @brief 当前节点作为前驱时是否参与普通后继的 strong join。
        ///
        /// 置位表示当前节点完成时会向普通后继的 `join_counter` 贡献一次 strong 到达；
        /// Jump / MultiJump 等跳转型前驱通过专用激活路径处理，因此通常不置位。
        static constexpr type STRONG = type{1} << (BITS - 3);

        /// @brief 所有占用高位的属性与独占运行状态标志。
        static constexpr type FLAG_MASK = IMPLICIT_ANCHOR | PREEMPTED | STRONG;

        /// @brief 静态 join weight 可使用的低位掩码。
        static constexpr type JOIN_WEIGHT_MASK = ~FLAG_MASK;

        /// @brief 静态 join weight 在当前位布局下可表示的最大值。
        static constexpr type JOIN_WEIGHT_MAX = JOIN_WEIGHT_MASK;
    };


    /// @brief Work 自身需要原子并发访问的控制标志位。
    ///
    /// 保存可能由多个 Worker 并发读取或修改的异常锚点、异常传播/归档状态，
    /// 以及可选的重复执行检查位；对应实例存储在 `m_control` 中并通过原子操作访问。
    struct Control {
        using type = std::uint32_t;

        static constexpr unsigned BITS = std::numeric_limits<type>::digits;
        static constexpr type NONE = 0;

        /// @brief 当前 Work 被用作显式异常归档锚点。
        static constexpr type EXPLICIT_ANCHOR = type{1} << (BITS - 1);

        /// @brief 当前 Work 自身或其向上传播的执行链处于异常路径。
        static constexpr type EXCEPTION = type{1} << (BITS - 2);

        /// @brief 当前 Work 已取得一次异常归档权并保存对应 exception_ptr。
        static constexpr type EXCEPTION_CAUGHT = type{1} << (BITS - 3);

#if TFL_ENABLE_WORK_EXECUTION_CHECK
        /// @brief 当前 Work 已开始一轮尚未最终 tear-down 的执行。
        ///
        /// 仅用于可选的执行一致性检查；首次进入一轮执行时置位，Runtime
        /// 挂起/恢复期间保持不变，并在该轮真正完成、进入最终 tear-down 前清除。
        /// 新一轮开始时若已经置位，表示同一 Work 被非法并发或重复激活。
        static constexpr type EXECUTION = type{1} << (BITS - 4);

        /// @brief 当前配置下所有原子控制标志位。
        static constexpr type FLAG_MASK = EXPLICIT_ANCHOR | EXCEPTION | EXCEPTION_CAUGHT | EXECUTION;
#else \
        /// @brief 当前配置下所有原子控制标志位。
        static constexpr type FLAG_MASK = EXPLICIT_ANCHOR | EXCEPTION | EXCEPTION_CAUGHT;
#endif
    };

    /// @brief 描述当前 Work 对一个外部 Semaphore 的非拥有配额请求。
    ///
    /// `sem` 的生命周期必须覆盖相关 Work 执行期；`count` 表示一次完整 acquire/release 的配额数量。
    struct SemaphoreReq {
        Semaphore* sem;     ///< 非拥有的目标信号量。
        std::size_t count;  ///< 单次请求或归还的配额数量。
    };

    /// @brief 保存当前 Work 配置的执行前 acquire 与执行后 release 请求。
    ///
    /// 容器拥有请求描述符本身，但其中的 Semaphore 指针均为非拥有引用。
    struct SemaphoreData {
        std::vector<SemaphoreReq> acquires; ///< invoke body 前必须全部获取的请求。
        std::vector<SemaphoreReq> releases; ///< invoke body 完成后执行的归还请求。

        [[nodiscard]] bool empty() const noexcept {
            return acquires.empty() && releases.empty();
        }
    };


    /// @brief 以共享所有权保存当前 Work 执行前后需要通知的观察者。
    ///
    /// shared_ptr 延长观察者生命周期；观察者本身不拥有 Work，也不参与依赖与调度状态管理。
    struct ObserverData {
        std::vector<std::shared_ptr<TaskObserver>> observers;

        [[nodiscard]] bool empty() const noexcept {
            return observers.empty();
        }
    };


    class Payload : public Immovable<Payload> {
        using Invoke = void (*)(Payload&, Work&, Worker&, Executor&, Work*&);

        struct Operations {
            void (*destroy)(Payload&) noexcept;
            void (*dump)(const Payload&, const Work&, std::ostream&);
            TaskType type;
        };

        static constexpr std::size_t k_size = TFL_WORK_PAYLOAD_SIZE;
        static constexpr std::size_t k_buffer_size = k_size - sizeof(Invoke) - sizeof(const Operations*);
        static_assert(k_buffer_size >= sizeof(void*));
    public:
        Payload() noexcept = default;

        template <typename T>
        static constexpr bool uses_heap = sizeof(T) > k_buffer_size || alignof(T) > alignof(std::max_align_t);

        template <typename T>
        static constexpr bool valid_type = std::is_nothrow_destructible_v<T> &&
                                           requires(T& payload, const T& const_payload, Work& work, Worker& worker, Executor& executor, Work*& cache, std::ostream& os) {
                                               { payload.invoke(work, worker, executor, cache) } -> std::same_as<void>;
                                               { const_payload.dump(std::as_const(work), os) } -> std::same_as<void>;
                                               { T::TYPE } -> std::convertible_to<TaskType>;
                                               { T::PROPERTIES } -> std::convertible_to<Properties::type>;
                                               { T::CONTROL } -> std::convertible_to<Control::type>;
                                           };
        template <typename T, typename... Args>
            requires (valid_type<T> && std::constructible_from<T, Args&&...>)
        explicit Payload(std::in_place_type_t<T>, Args&&... args) {
            _construct<T>(std::forward<Args>(args)...);
        }

        ~Payload() noexcept {
            reset();
        }

        template <typename T, typename... Args>
            requires (valid_type<T> && std::constructible_from<T, Args&&...>)
        T* emplace(Args&&... args) {
            reset();
            _construct<T>(std::forward<Args>(args)...);
            return _target<T>();
        }

        template <typename T>
            requires valid_type<T>
        [[nodiscard]] T* target() noexcept {
            TFL_ASSERT(m_operations == std::addressof(operations<T>));
            return _target<T>();
        }

        template <typename T>
            requires valid_type<T>
        [[nodiscard]] const T* target() const noexcept {
            TFL_ASSERT(m_operations == std::addressof(operations<T>));
            return _target<T>();
        }

        void invoke(Work& work, Worker& worker, Executor& executor, Work*& cache) {
            TFL_ASSERT(m_invoke);
            m_invoke(*this, work, worker, executor, cache);
        }

        void dump(const Work& work, std::ostream& os) const {
            TFL_ASSERT(m_operations);
            m_operations->dump(*this, work, os);
        }

        [[nodiscard]] TaskType type() const noexcept {
            TFL_ASSERT(m_operations);
            return m_operations->type;
        }

        void reset() noexcept {
            const Operations* operations = m_operations;

            if (operations) {
                m_invoke = nullptr;
                operations->destroy(*this);
                m_operations = nullptr;
            }
        }

        [[nodiscard]] bool empty() const noexcept {
            return m_invoke == nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return m_invoke != nullptr;
        }

    private:
        template <typename T, typename... Args>
            requires (valid_type<T> && std::constructible_from<T, Args&&...>)
        void _construct(Args&&... args) {
            if constexpr (uses_heap<T>) {
                T* pointer = ::new T(std::forward<Args>(args)...);
                _set_large_ptr(pointer);
            } else {
                std::construct_at(reinterpret_cast<T*>(m_buffer), std::forward<Args>(args)...);
            }

            m_operations = std::addressof(operations<T>);
            m_invoke = &_invoke<T>;
        }

        template <typename T>
        [[nodiscard]] T* _small_ptr() noexcept {
            return std::launder(reinterpret_cast<T*>(m_buffer));
        }

        template <typename T>
        [[nodiscard]] const T* _small_ptr() const noexcept {
            return std::launder(reinterpret_cast<const T*>(m_buffer));
        }

        template <typename T>
        void _set_large_ptr(T* pointer) noexcept {
            std::memcpy(m_buffer, std::addressof(pointer), sizeof(pointer));
        }

        template <typename T>
        [[nodiscard]] T* _large_ptr() noexcept {
            T* pointer{nullptr};
            std::memcpy(std::addressof(pointer), m_buffer, sizeof(pointer));
            return pointer;
        }

        template <typename T>
        [[nodiscard]] const T* _large_ptr() const noexcept {
            T* pointer{nullptr};
            std::memcpy(std::addressof(pointer), m_buffer, sizeof(pointer));
            return pointer;
        }

        template <typename T>
        [[nodiscard]] T* _target() noexcept {
            if constexpr (uses_heap<T>) {
                return _large_ptr<T>();
            } else {
                return _small_ptr<T>();
            }
        }

        template <typename T>
        [[nodiscard]] const T* _target() const noexcept {
            if constexpr (uses_heap<T>) {
                return _large_ptr<T>();
            } else {
                return _small_ptr<T>();
            }
        }

        template <typename T>
        static void _invoke(Payload& payload, Work& work, Worker& worker, Executor& executor, Work*& cache) {
            payload.template _target<T>()->invoke(work, worker, executor, cache);
        }

        template <typename T>
        static void _destroy(Payload& payload) noexcept {
            if constexpr (uses_heap<T>) {
                ::delete payload.template _target<T>();
            } else {
                std::destroy_at(payload.template _target<T>());
            }
        }

        template <typename T>
        static void _dump(const Payload& payload, const Work& work, std::ostream& os) {
            payload.template _target<T>()->dump(work, os);
        }

        template <typename T>
        inline static constexpr Operations operations{
            &_destroy<T>,
            &_dump<T>,
            static_cast<TaskType>(T::TYPE)
        };

        Invoke m_invoke{nullptr};
        const Operations* m_operations{nullptr};
        alignas(std::max_align_t) std::byte m_buffer[k_buffer_size];
    };

public:

    /// @brief 默认构造一个尚未安装 Payload、未绑定 Graph/Topology 的空 Work。
    Work() = default;


    template <typename Invoker, typename... Args>
        requires (Payload::template valid_type<Invoker> && std::constructible_from<Invoker, Args&&...>)
    explicit Work(std::in_place_type_t<Invoker>, const Graph* graph, Args&&... args)
        : m_payload{std::in_place_type<Invoker>, std::forward<Args>(args)...}
        , m_properties{Invoker::PROPERTIES}
        , m_control{Invoker::CONTROL}
        , m_graph{graph} {
        TFL_ASSERT(graph);
    }

    template <typename Invoker, typename... Args>
        requires (Payload::template valid_type<Invoker> &&
                 std::constructible_from<Invoker, Args&&...> &&
                 requires(Invoker& invoker) {
                     { invoker.get_topology() } noexcept -> std::same_as<Topology*>;
                 })
    explicit Work(std::in_place_type_t<Invoker>, Work* parent, Args&&... args)
        : m_payload{std::in_place_type<Invoker>, std::forward<Args>(args)...}
        , m_parent{parent}
        , m_properties{Invoker::PROPERTIES}
        , m_control{Invoker::CONTROL} {
        m_topology = target<Invoker>().get_topology();
        TFL_ASSERT(m_topology && "async Invoker must provide a valid Topology");
    }

    ~Work() noexcept = default;

    /// @brief 替换当前 Payload 为指定 Invoker，并同步该 Invoker 声明的节点语义。
    ///
    /// Payload 构造成功后，以 `T::PROPERTIES` 和 `T::CONTROL`
    /// 重置节点属性和原子控制初值；若 T 提供 `get_topology()`，
    /// 同时把 `m_topology` 重新绑定到该 Invoker 暴露的 Topology。
    ///
    /// @tparam T 要存入 Payload 的具体 Invoker 类型。
    /// @param args 完美转发给 Invoker 构造函数的参数。
    /// @return 新构造并安装到 Payload 中的 Invoker 引用。
    template <typename T, typename... Args>
        requires (Payload::template valid_type<T> && std::constructible_from<T, Args&&...>)
    T& emplace(Args&&... args) {
        T& payload = *m_payload.template emplace<T>(std::forward<Args>(args)...);

        if constexpr (requires(T& value) {
                          { value.get_topology() } noexcept -> std::same_as<Topology*>;
                      }) {
            m_topology = payload.get_topology();
        }

        m_properties = T::PROPERTIES;
        m_control.store(T::CONTROL, std::memory_order_relaxed);
        m_exception_ptr = nullptr;

        return payload;
    }

    template <typename T>
        requires Payload::template valid_type<T>
        [[nodiscard]] T& target() noexcept {
        return *m_payload.template target<T>();
    }

    template <typename T>
        requires Payload::template valid_type<T>
        [[nodiscard]] const T& target() const noexcept {
        return *m_payload.template target<T>();
    }

    void invoke(Worker& worker, Executor& executor, Work*& cache) {
        m_payload.invoke(*this, worker, executor, cache);
    }

    void dump(std::ostream& os) const {
        m_payload.dump(*this, os);
    }

    [[nodiscard]] TaskType type() const noexcept {
        return m_payload.type();
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_payload.empty();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(m_payload);
    }

private:
    Payload m_payload;

    Topology*                           m_topology{nullptr};                ///< 非拥有的执行拓扑上下文；提供停止域、完成状态和强引用计数。
    Work*                               m_parent{nullptr};                  ///< 非拥有的运行期父 Work；当前节点完成时向其归还或转移一个 join slot。
    Properties::type                    m_properties{Properties::NONE};     ///< 非原子属性、独占运行状态及静态 join weight 的位编码。
    std::atomic<Control::type>          m_control{Control::NONE};           ///< 并发控制位；保存异常锚点、异常传播/归档及可选执行检查状态。
    std::atomic<std::uint32_t>          m_join_counter{0};                  ///< 运行期 join 计数；用于 strong predecessor 到达、动态子任务等待及父节点恢复。
    std::uint32_t                       m_num_successors{0};                ///< `m_edges` 的后继数量，同时作为 `[后继 | 前驱]` 两个逻辑区间的分割点。
    std::vector<Work*>                  m_edges;                            ///< 非拥有的统一邻接表；布局固定为 `[后继 | 前驱]`，同时服务调度传播和 join weight 计算。
    std::unique_ptr<SemaphoreData>      m_semaphores;                       ///< 按需分配的信号量约束；保存执行前 acquire 与执行后 release 请求。
    std::unique_ptr<ObserverData>       m_observers;                        ///< 按需分配的观察者集合；在节点执行前后触发生命周期通知。

    std::exception_ptr                  m_exception_ptr{nullptr};           ///< 当前 Work 获得异常归档权后保存的异常；由完成同步保证读取可见性。
    const Graph*                        m_graph{nullptr};                   ///< 非拥有的静态物理 Graph；仅静态图节点绑定，独立异步 Work 通常为空。
    std::string                         m_name;                             ///< 节点名称；仅用于调试、诊断和 D2 可视化，不参与调度语义。

    /// @brief 阻塞当前线程，直到本 Work 所属 Topology 进入 Finished。
    ///
    /// 通过 `atomic::wait` 等待 Topology 控制字发生变化，并仅以 Status::Finished
    /// 作为完成条件；STOP_REQUESTED、LOCKED、引用计数等其他位变化只会触发重新检查。
    ///
    /// @note 发布 Finished 的完成路径必须对同一控制字执行 `notify_all()`。
    void _wait() const noexcept {
        auto value = m_topology->m_control.load(std::memory_order_acquire);

        while (Topology::Control::status(value) != Topology::Control::Status::Finished) {
            m_topology->m_control.wait(value, std::memory_order_acquire);
            value = m_topology->m_control.load(std::memory_order_acquire);
        }
    }

    /// @brief 为当前 Work 所属 Topology 增加一份强引用。
    void _increment_ref() noexcept {
        auto prev = m_topology->m_control.fetch_add(Topology::Control::USE_COUNT_INC, std::memory_order_relaxed);
        TFL_ASSERT(Topology::Control::use_count(prev) < Topology::Control::USE_COUNT_MAX);
    }

    /// @brief 释放当前 Work 所属 Topology 的一份强引用。
    ///
    /// @return 释放前引用计数为 1、即本次释放最后一份强引用时返回 true。
    bool _decrement_ref() noexcept {
        auto prev = m_topology->m_control.fetch_sub(Topology::Control::USE_COUNT_INC, std::memory_order_acq_rel);
        TFL_ASSERT(Topology::Control::use_count(prev) != 0);
        return Topology::Control::use_count(prev) == 1;
    }

    /// @brief 查询当前 Work 所属 Topology 的状态是否为 Running。
    [[nodiscard]] bool _is_running() const noexcept {
        return Topology::Control::status(m_topology->m_control.load(std::memory_order_relaxed)) == Topology::Control::Status::Running;
    }

    /// @brief 查询当前 Work 所属 Topology 的状态是否为 Finished。
    [[nodiscard]] bool _is_finished() const noexcept {
        return Topology::Control::status(m_topology->m_control.load(std::memory_order_relaxed)) == Topology::Control::Status::Finished;
    }

    /// @brief 返回当前 Work 所属 Topology 强引用计数的瞬时快照。
    [[nodiscard]] std::size_t _use_count() const noexcept {
        return static_cast<std::size_t>(Topology::Control::use_count(m_topology->m_control.load(std::memory_order_relaxed)));
    }


    /// @brief 查询当前 Work 作为前驱时是否参与普通后继的 strong join。
    ///
    /// @return `Properties::STRONG` 置位时返回 true；Jump / MultiJump 等弱前驱返回 false。
    [[nodiscard]] TFL_FORCE_INLINE bool _is_strong() const noexcept {
        return m_properties & Properties::STRONG;
    }

    /// @brief 根据当前前驱区间重新计算静态 join weight。
    ///
    /// 遍历 `[m_num_successors, m_edges.size())` 的全部物理前驱，仅累计
    /// `_is_strong()` 为 true 的前驱。
    /// @return 当前节点普通执行路径需要等待的 strong predecessor 数量。
    [[nodiscard]] TFL_FORCE_INLINE std::size_t _compute_join_weight() const noexcept {
        std::size_t sum = 0;
        for (std::size_t i = m_num_successors; i < m_edges.size(); ++i) {
            sum += m_edges[i]->_is_strong();
        }
        return sum;
    }

    /// @brief 从 `m_properties` 低位读取已经编码的静态 join weight。
    ///
    /// 高位 FLAG_MASK 与 join weight 共用一个整数；该函数只保留低位计数区域。
    ///
    /// @return 编码在 Properties 低位中的 strong predecessor 数量。
    [[nodiscard]] TFL_FORCE_INLINE Properties::type _join_weight() const noexcept {
        return m_properties & Properties::JOIN_WEIGHT_MASK;
    }


    /// @brief 向当前 Work 所属 Topology 原子设置停止请求。
    ///
    /// 这里只设置当前 Topology 的 STOP_REQUESTED，不遍历或主动修改子 Topology；
    /// 后代任务通过 `_stop_requested()` 沿自身 `Topology::m_parent` 向上观察停止链。
    ///
    /// @return 本次调用把 STOP_REQUESTED 从未设置变为已设置时返回 true；
    ///         该位此前已经存在时返回 false。
    [[nodiscard]] TFL_FORCE_INLINE bool _request_stop() noexcept {
        auto prev = m_topology->m_control.fetch_or(Topology::Control::STOP_REQUESTED, std::memory_order_relaxed);
        return !(prev & Topology::Control::STOP_REQUESTED);
    }

    /// @brief 查询当前 Topology 及其祖先 Topology 链是否存在停止请求。
    ///
    /// 从当前 `m_topology` 开始逐级检查 STOP_REQUESTED；当前层未停止时
    /// 沿 `Topology::m_parent` 向上继续，直到发现停止请求或父链为空。
    /// 当前 Work 的 `m_topology` 必须在调用期间保持有效。
    ///
    /// @return 当前或任一祖先 Topology 已请求停止时返回 true。
    [[nodiscard]] TFL_FORCE_INLINE bool _stop_requested() const noexcept {
        const Topology* topology = m_topology;

        do {
            if (topology->m_control.load(std::memory_order_relaxed) & Topology::Control::STOP_REQUESTED) {
                return true;
            }
        } while ((topology = topology->m_parent) != nullptr);

        return false;
    }

    /// @brief 查询当前 Work 是否已被标记为异常传播路径。
    ///
    /// 仅读取本 Work 的 `Control::EXCEPTION`，不沿 parent 链查询。
    [[nodiscard]] TFL_FORCE_INLINE bool _has_exception() const noexcept {
        return m_control.load(std::memory_order_relaxed) & Control::EXCEPTION;
    }

    /// @brief 查询当前执行是否因异常或继承停止请求而应终止后续工作。
    ///
    /// 先检查当前 Work 的异常位；不存在异常时再检查当前 Topology
    /// 及其祖先停止链，以避免无必要的父链遍历。
    ///
    /// @return 当前 Work 处于异常路径或停止链存在请求时返回 true。
    [[nodiscard]] TFL_FORCE_INLINE bool _should_abort() const noexcept {
        return _has_exception() || _stop_requested();
    }

    /// @brief 若当前 Work 已归档异常，则取出并重新抛出，同时清除本地归档状态。
    ///
    /// @note `m_exception_ptr` 为空时直接返回，不修改控制位。
    /// @post 实际发生重抛时，先清空 `m_exception_ptr` 及 EXCEPTION/EXCEPTION_CAUGHT 位。
    TFL_FORCE_INLINE void _rethrow_exception() {
        if (m_exception_ptr) {
            auto e = m_exception_ptr;
            m_exception_ptr = nullptr;
            m_control.fetch_and(~(Control::EXCEPTION | Control::EXCEPTION_CAUGHT), std::memory_order_relaxed);
            std::rethrow_exception(e);
        }
    }

    /// @brief 若存在已归档异常则重新抛出，但保留异常指针和控制状态供共享观察者继续读取。
    TFL_FORCE_INLINE void _rethrow_shared_exception() const {
        if (m_exception_ptr) {
            std::rethrow_exception(m_exception_ptr);
        }
    }

    /// @brief 捕获当前 `catch (...)` 正在处理的异常并进入统一传播/归档流程。
    /// @note 必须在活动异常处理上下文内调用，否则 `std::current_exception()` 可能为空。
    TFL_FORCE_INLINE void _process_exception() noexcept {
        _process_exception(std::current_exception());
    }


    /// @brief 沿 `m_parent` 链传播异常标记，并按显式/隐式锚点优先级竞争异常归档位置。
    ///
    /// @param eptr 待传播和归档的 `std::exception_ptr`。
    ///
    /// 本重载可直接接收已经捕获的 exception_ptr。实现先沿父 Work 链设置
    /// EXCEPTION 并寻找显式/隐式锚点，再使用 `fetch_or` 的旧值竞争 EXCEPTION_CAUGHT；
    /// 同一归档位置只允许首个观察到 CAUGHT 未置位的调用写入 `m_exception_ptr`。
    /// 选定锚点竞争失败时继续尝试当前 Work 作为兜底归档位置。
    /// @note `m_exception_ptr` 是非原子对象，只能在框架约定的完成同步之后由观察者读取。
    /// 归档流程分为三阶段：
    ///
    /// 阶段 1：沿父 Work 链传播 EXCEPTION 并寻找异常锚点
    ///   - 遇到 `Control::EXPLICIT_ANCHOR` 后立即停止继续向上搜索；
    ///   - 在显式锚点之前经过的节点置 EXCEPTION，供 tear-down 识别异常路径；
    ///   - 同时记录沿途遇到的首个 `Properties::IMPLICIT_ANCHOR`。
    ///
    /// 阶段 2：按优先级竞争锚点归档权
    ///   - 优先级 1：显式锚点；
    ///   - 优先级 2：仅在不存在显式锚点时使用首个隐式锚点；
    ///   - `fetch_or(EXCEPTION | EXCEPTION_CAUGHT)` 的旧 CAUGHT 位决定本次是否取得写入权。
    ///
    /// 阶段 3：当前 Work 兜底归档
    ///   - 无可用锚点，或选定锚点已被其他异常先行占用时，在当前 Work 再竞争一次归档权。
    ///
    /// @note CAUGHT 位的原子竞争只负责选择写入者；`m_exception_ptr` 的读取可见性
    ///       必须由任务完成/等待协议提供，不能仅依赖这里的 relaxed `fetch_or`。
    ///
    /// @note 若锚点和当前 Work 的 CAUGHT 均已被其他异常占用，本次 eptr 不再覆盖已有异常，
    ///       因而该归档位置保持“首个异常优先”的语义。
    void _process_exception(std::exception_ptr eptr) noexcept {
        // 阶段 1：沿父 Work 链传播 EXCEPTION，并寻找可用异常锚点。
        // 循环不变式：
        //   - explicit_anchor 只在遇到首个显式锚点时赋值；
        //   - implicit_anchor 记录显式锚点之前遇到的首个隐式锚点；
        //   - cur 每轮沿 m_parent 向上推进。
        Work* explicit_anchor = nullptr;
        Work* implicit_anchor = nullptr;
        for (Work* cur = this; cur; cur = cur->m_parent) {
            // 显式锚点优先级最高：遇到后停止向上搜索，交由阶段 2 竞争归档权。
            if (cur->m_control.load(std::memory_order_relaxed) & Control::EXPLICIT_ANCHOR) {
                explicit_anchor = cur;
                break;
            }
            // 显式锚点之前经过的节点全部置 EXCEPTION，供后续 tear-down 识别异常路径。
            cur->m_control.fetch_or(Control::EXCEPTION, std::memory_order_relaxed);
            // Properties 在该执行阶段无并发写入，因此可直接读取并记录首个隐式锚点。
            if (!implicit_anchor && (cur->m_properties & Properties::IMPLICIT_ANCHOR)) {
                implicit_anchor = cur;
            }
        }

        // 阶段 2：优先尝试显式锚点，否则尝试首个隐式锚点。
        // archive_mask 同时设置异常路径位和异常归档权位：
        //   - EXCEPTION       表示归档 Work 本身也处于异常路径；
        //   - EXCEPTION_CAUGHT 的旧值用于判断本次调用是否取得首次归档权。
        constexpr auto archive_mask = Control::EXCEPTION | Control::EXCEPTION_CAUGHT;

        // 优先级 1：显式锚点。
        if (explicit_anchor) {
            auto prev = explicit_anchor->m_control.fetch_or(archive_mask, std::memory_order_relaxed);
            if ((prev & Control::EXCEPTION_CAUGHT) == 0) {
                explicit_anchor->m_exception_ptr = eptr;
                return;
            }
            // 显式锚点已有异常占用归档权；继续尝试当前 Work 的兜底位置。
        }
        // 优先级 2：仅在没有显式锚点时尝试首个隐式锚点。
        else if (implicit_anchor) {
            auto prev = implicit_anchor->m_control.fetch_or(archive_mask, std::memory_order_relaxed);
            if ((prev & Control::EXCEPTION_CAUGHT) == 0) {
                implicit_anchor->m_exception_ptr = eptr;
                return;
            }
        }

        // 阶段 3：当前 Work 兜底归档。
        // 适用场景：
        //   - 沿父链没有找到任何异常锚点；
        //   - 选定锚点的归档权已被其他异常占用。
        //
        // 当前 Work 仍使用同一 CAUGHT 竞争协议；
        // 若当前 Work 也已有归档异常，则保留原异常，不覆盖 m_exception_ptr。
        auto prev = m_control.fetch_or(archive_mask, std::memory_order_relaxed);
        if ((prev & Control::EXCEPTION_CAUGHT) == 0) {
            m_exception_ptr = eptr;
        }
    }

    /// @brief 获取 `m_edges` 中后继前缀的 span 视图。
    [[nodiscard]] std::span<Work*> _successors() noexcept {
        return {m_edges.data(), m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _successors() const noexcept {
        return {m_edges.data(), m_num_successors};
    }

    /// @brief 获取 `m_edges` 中前驱后缀的 span 视图。
    [[nodiscard]] std::span<Work*> _predecessors() noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _predecessors() const noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }

    /// @brief 返回当前物理前驱数量。
    [[nodiscard]] std::size_t _num_predecessors() const noexcept {
        return m_edges.size() - m_num_successors;
    }

    /// @brief 按需创建 SemaphoreData，并返回其可修改引用。
    [[nodiscard]] SemaphoreData& _ensure_semaphores() {
        if (!m_semaphores) {
            m_semaphores = std::make_unique<SemaphoreData>();
        }
        return *m_semaphores;
    }

    /// @brief 当 acquire/release 列表均为空时释放按需分配的 SemaphoreData。
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

    // ---- Semaphore 配置与运行期 acquire/release ----
    void _acquire(Semaphore* sem, std::size_t count);
    void _release(Semaphore* sem, std::size_t count);
    void _remove_acquire(Semaphore* sem) noexcept;
    void _remove_release(Semaphore* sem) noexcept;
    void _clear_acquires() noexcept;
    void _clear_releases() noexcept;

    [[nodiscard]] bool _try_acquire_semaphores(SmallVector<Work*>& out);
    void _release_semaphores(SmallVector<Work*>& out);

    // ---- 观察者执行前/后通知 ----
    void _notify_before(Worker& wr) const;
    void _notify_after(Worker& wr) const;

    // ---- 静态图双向边表维护 ----
    void _erase_successor_at(std::size_t idx) noexcept;
    void _erase_predecessor_at(std::size_t idx) noexcept;
    void _precede(Work* target);
    void _remove_successor(Work* target) noexcept;
    void _clear_predecessors() noexcept;
    void _clear_successors() noexcept;

    // ---- 静态图建边合法性与无 Jump 路径检测 ----
    [[nodiscard]] bool _has_path_without_jump(const Work* from, const Work* to) const;
    [[nodiscard]] std::optional<std::string_view> _can_precede(Work* target) const;
};


/// @brief 删除指定后继并维持 `m_edges == [后继 | 前驱]` 的分区不变式。
///
/// 实现使用 swap-with-last 风格的常数次指针搬移：
/// 1. 目标不是最后一个后继时，用最后一个后继覆盖目标；
/// 2. 存在前驱时，用边表最后一个前驱填补原最后后继位置；
/// 3. `pop_back()` 并递减 `m_num_successors`。
///
/// @pre `idx < m_num_successors`。
/// @post 边表总长度和后继数量各减少 1，前驱数量保持不变。
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

/// @brief 删除前驱区间中的指定元素，后继分区保持不变。
///
/// 前驱区间为 `[m_num_successors, m_edges.size())`；用整个边表最后一个前驱覆盖目标后 `pop_back()`。
///
/// @pre `idx < _num_predecessors()`。
/// @post 边表总长度减少 1，`m_num_successors` 保持不变。
inline void Work::_erase_predecessor_at(std::size_t idx) noexcept {
    TFL_ASSERT(idx < _num_predecessors());
    const std::size_t abs_idx = m_num_successors + idx;
    m_edges[abs_idx] = m_edges.back();
    m_edges.pop_back();
}

/// @brief 建立逻辑有向边 `this -> target`，并在两端维护对称的邻接记录。
///
/// @param target 新后继节点。
///
/// @throws Exception target 为空、跨 Graph、重复边或形成不允许的严格闭环时抛出。
///
/// @note 当前 Work 的 `m_edges` 前缀保存 target，target 的前驱后缀保存 this；
///       两侧记录共同表示一条逻辑有向边，运行期 join weight 和后继传播直接
///       复用这套静态边表。
inline void Work::_precede(Work* const target) {
    if (auto error = _can_precede(target)) {
        throw Exception("cannot precede: {}.", *error);
    }

    // 先在 this 侧插入后继，并通过交换维持“后继在前、前驱在后”的统一布局。
    m_edges.push_back(target);
    if (m_num_successors < m_edges.size() - 1) {
        std::swap(m_edges[m_num_successors], m_edges.back());
    }
    ++m_num_successors;
    target->m_edges.push_back(this);
}

/// @brief 删除逻辑边 `this -> target`，并同步删除 target 侧对应的前驱记录。
///
/// @param target 要解除连接的后继；nullptr 或当前不存在该边时无操作。
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

/// @brief 清除当前 Work 的全部前驱，并同步删除每个前驱侧对应的后继记录。
///
/// @post 当前 Work 的前驱区间为空，原前驱的 successor 列表均不再包含 this。
inline void Work::_clear_predecessors() noexcept {
    for (Work* pred : _predecessors()) {
        auto succ = pred->_successors();
        auto it = std::ranges::find(succ, this);
        TFL_ASSERT(it != succ.end() && "successor must exist");
        pred->_erase_successor_at(static_cast<std::size_t>(it - succ.begin()));
    }

    m_edges.erase(m_edges.begin() + m_num_successors, m_edges.end());
}

/// @brief 清除当前 Work 的全部后继，并同步删除每个后继侧对应的前驱记录。
/// @note 删除前缀后由 vector 将现有前驱区间整体前移，其相对顺序保持不变。
///
/// @post `m_num_successors == 0`，`m_edges` 仅保留原前驱。
inline void Work::_clear_successors() noexcept {
    for (Work* succ : _successors()) {
        auto pred = succ->_predecessors();
        auto it = std::ranges::find(pred, this);
        TFL_ASSERT(it != pred.end() && "predecessor must exist");
        succ->_erase_predecessor_at(static_cast<std::size_t>(it - pred.begin()));
    }

    // 擦除后继前缀，由 vector 将前驱区间整体前移到 m_edges 起始位置。
    m_edges.erase(m_edges.begin(), m_edges.begin() + m_num_successors);
    m_num_successors = 0;
}

/// @brief 校验新增逻辑边 `this -> target` 是否符合当前图的建边规则。
///
/// 检查规则：
/// 1. target 必须非空并与 this 属于同一 Graph；
/// 2. 不允许重复建立同一条逻辑边；
/// 3. 非 Jump 节点不允许自环；
/// 4. 不含 Jump/MultiJump 的严格闭环非法，跳转型节点参与的循环由专用控制流语义允许。
///
/// @param target 待连接的后继节点。
/// @note 若 this 或 target 为 Jump/MultiJump，当前实现直接允许该连接；其他情况
///       通过 `_has_path_without_jump(target, this)` 检测新增边是否形成严格闭环。
inline std::optional<std::string_view> Work::_can_precede(Work* const target) const {
    if (!target) return std::string_view{"target is null"};
    if (!m_graph) return std::string_view{"work not attached to graph"};
    if (m_graph != target->m_graph) return std::string_view{"works belong to different graphs"};

    const auto successors = _successors();
    if (std::ranges::find(successors, target) != successors.end()) {
        return std::string_view{"edge already exists"};
    }

    const auto this_type = type();
    const bool this_is_jump = (this_type == TaskType::Jump || this_type == TaskType::MultiJump);

    // 自环只对 Jump / MultiJump 类型开放。
    if (target == this) {
        if (!this_is_jump) {
            return std::string_view{"invalid topology: self-loops are propertiesly allowed for jump-type nodes"};
        }
        return std::nullopt;
    }
    if (this_is_jump) return std::nullopt;
    const auto target_type = target->type();
    const bool target_is_jump = (target_type == TaskType::Jump || target_type == TaskType::MultiJump);
    if (target_is_jump) return std::nullopt;

    // 非 Jump 两端使用 DFS 检测新增边是否闭合一条不经过 Jump 节点的路径。
    if (_has_path_without_jump(target, this)) {
        return std::string_view{"invalid topology: strict cycle detected without any jump-type node"};
    }

    return std::nullopt;
}

/// @brief 查询 `from` 到 `to` 是否存在一条不穿过 Jump/MultiJump 节点的有向路径。
///
/// @param from 搜索起点。
/// @param to 目标节点。
/// @return 找到符合约束的路径时返回 true。
///
/// @note DFS 使用 `std::stack<const Work*, std::vector<const Work*>>` 保存待访问节点，
///       visited 预留常见小图容量；更大图仍可能触发动态扩容。
inline bool Work::_has_path_without_jump(const Work* from, const Work* to) const {
    if (!from || !to) return false;

    // 显式指定 vector 作为 DFS 栈的底层顺序容器。
    std::stack<const Work*, std::vector<const Work*>> dfs_stack;
    std::unordered_set<const Work*> visited;

    // 为常见小图预留访问集合容量；超出后按 unordered_set 规则扩容。
    visited.reserve(64);

    dfs_stack.push(from);
    visited.insert(from);


    while (!dfs_stack.empty()) {
        const Work* curr = dfs_stack.top();
        dfs_stack.pop();

        for (const auto* succ : curr->_successors()) {
            if (succ == to) return true;

            const auto st = succ->type();
            if (st == TaskType::Jump || st == TaskType::MultiJump) continue;

            // 首次访问的节点才进入 DFS；Jump/MultiJump 节点不会继续向后展开。
            if (visited.insert(succ).second) {
                dfs_stack.push(succ);
            }
        }
    }

    return false;
}

/// @brief 为当前 Work 添加一条执行前 Semaphore acquire 请求。
///
/// @param sem 非拥有的目标 Semaphore。
/// @param count 每次执行需要一次性获取的配额。
///
/// @throws Exception sem 为空或同一 Semaphore 已存在于 acquire 列表时抛出。
///
/// @note `count == 0` 时忽略该配置。
/// @note acquire 列表预期较小，因此使用线性扫描去重。
inline void Work::_acquire(Semaphore* sem, std::size_t count) {
    if (!sem) throw Exception("cannot acquire null semaphore.");
    if (count == 0) return; // 空 acquire 请求不建立配置项。

    auto& sd = _ensure_semaphores();

    // acquire 列表通常较小，直接线性检查同一 Semaphore 是否已存在。
    for (std::size_t i = 0; i < sd.acquires.size(); ++i) {
        if (sd.acquires[i].sem == sem) {
            throw Exception("semaphore already in acquire list.");
        }
    }

    sd.acquires.emplace_back(sem, count);
}

/// @brief 为当前 Work 添加一条执行后 Semaphore release 请求。
///
/// @param sem 非拥有的目标 Semaphore。
/// @param count 每次执行需要归还的配额。
///
/// @throws Exception sem 为空或同一 Semaphore 已存在于 release 列表时抛出。
inline void Work::_release(Semaphore* sem, std::size_t count) {
    if (!sem) throw Exception("cannot release null semaphore.");
    if (count == 0) return;

    auto& sd = _ensure_semaphores();

    // release 列表通常较小，直接线性检查同一 Semaphore 是否已存在。
    for (std::size_t i = 0; i < sd.releases.size(); ++i) {
        if (sd.releases[i].sem == sem) {
            throw Exception("semaphore already in release list.");
        }
    }

    sd.releases.emplace_back(sem, count);
}

/// @brief 以 swap-with-last 方式移除指定 Semaphore 的 acquire 配置。
///
/// @param sem 要移除的 Semaphore；不存在时无操作。
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

/// @brief 以 swap-with-last 方式移除指定 Semaphore 的 release 配置。
///
/// @param sem 要移除的 Semaphore；不存在时无操作。
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

/// @brief 清空全部 acquire 配置，并在两张列表都为空时释放 SemaphoreData。
inline void Work::_clear_acquires() noexcept {
    if (!m_semaphores) return;
    m_semaphores->acquires.clear();
    _try_release_semaphores();
}

/// @brief 清空全部 release 配置，并在两张列表都为空时释放 SemaphoreData。
inline void Work::_clear_releases() noexcept {
    if (!m_semaphores) return;
    m_semaphores->releases.clear();
    _try_release_semaphores();
}

/// @brief 按配置顺序尝试一次性取得当前 Work 的全部 acquire 配额。
///
/// @param out 接收回滚此前已获取配额时被解冻的其他 waiter；调用方负责重新调度。
/// @return 全部 acquire 成功返回 true；任一请求失败时释放此前已成功获取的配额并返回 false。
///
/// @warning 返回 false 时当前 Work 已登记在失败 Semaphore 的 waiter 中；`out` 还可能包含
///          回滚其他 Semaphore 时被解冻的 Work，调用方必须将这些 Work 重新发布。
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

/// @brief 执行当前 Work 配置的全部 Semaphore release 请求。
///
/// @param out 汇总各次 release 解冻的 waiter；调用方负责重新调度。
TFL_FORCE_INLINE void Work::_release_semaphores(SmallVector<Work*>& out) {
    for (const auto& req : m_semaphores->releases) {
        req.sem->_release(req.count, out);
    }
}

/// @brief 在 callable 正式执行前依次通知当前 Work 注册的全部观察者。
TFL_FORCE_INLINE void Work::_notify_before(Worker& wr) const {
    if (!m_observers) [[likely]] return;
    for (auto& aspect : m_observers->observers) {
        aspect->on_before(WorkerView{wr});
    }
}

/// @brief 在 callable 执行结束后依次通知当前 Work 注册的全部观察者。
TFL_FORCE_INLINE void Work::_notify_after(Worker& wr) const {
    if (!m_observers) [[likely]] return;
    for (auto& aspect : m_observers->observers) {
        aspect->on_after(WorkerView{wr});
    }
}


/// @brief 内部协作执行作用域使用的栈绑定异常/完成锚点 Work。
///
/// AnchorWork 不承载用户 callable，Invoker 的 invoke/dump 均为空操作；
/// 它通过独立 Topology 和 `Control::EXPLICIT_ANCHOR` 聚合子链完成计数与异常。
///
/// @note 该类型用于 Runtime::corun、TaskGroup 等栈式协作作用域；
///       对象由调用栈直接管理，绝不能通过 `destroy_work()` 或 Work 对象池回收。
class AnchorWork final : public Work {
    class Invoker final : public TopologyStorage {
    public:
        static constexpr TaskType TYPE = TaskType::None;
        static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
        static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

        explicit Invoker(Topology* parent_topology, Executor* executor) noexcept
            : TopologyStorage{parent_topology, executor} {}

        void invoke(Work&, Worker&, Executor&, Work*&) noexcept {}

        void dump(const Work&, std::ostream&) const noexcept {}
    };

public:
    explicit AnchorWork(Work& parent, Executor& executor) noexcept
        : Work{std::in_place_type<Invoker>,
               std::addressof(parent),
               parent.m_topology,
               std::addressof(executor)} {
        TFL_ASSERT(parent.m_topology);
    }
};


// ============================================================================
// Work Pool：可选的全局分片 Work 对象池
// ============================================================================

#if TFL_ENABLE_TASK_POOL

namespace detail {

using WorkPoolHead = std::conditional_t<
    std::atomic<TaggedHead128>::is_always_lock_free,
    TaggedHead128,
    TaggedHead64<>
    >;

inline ObjectPool<Work, 32, 64, WorkPoolHead> work_pool;

}  // namespace detail

#endif


// ============================================================================
// Work Lifetime：统一创建/销毁入口
// ============================================================================

template <typename... Args>
    requires std::constructible_from<Work, Args...>
[[nodiscard]] TFL_FORCE_INLINE Work* create_work(Args&&... args) {
#if TFL_ENABLE_TASK_POOL
    return detail::work_pool.create(std::forward<Args>(args)...);
#else
    return new Work(std::forward<Args>(args)...);
#endif
}

TFL_FORCE_INLINE void destroy_work(Work* work) noexcept {
#if TFL_ENABLE_TASK_POOL
    detail::work_pool.destroy(work);
#else
    delete work;
#endif
}

}  // namespace tfl
