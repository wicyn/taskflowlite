/// @file work.hpp
/// @brief 任务节点、执行状态、依赖关系及运行期辅助逻辑。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
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
#include <functional>

#include "enums.hpp"
#include "topology.hpp"
#include "utility.hpp"
#include "exception.hpp"
#include "observer.hpp"
#include "semaphore.hpp"
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
    friend class Context;
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
    friend class SharedWorkStack;
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
    /// `sem` 的生命周期必须覆盖相关 Work 执行期；`count` 表示一次完整
    /// acquire/release 操作需要取得或归还的配额数量。
    struct SemaphoreReq {
        Semaphore* sem;     ///< 非拥有的目标信号量。
        std::size_t count;  ///< 单次请求或归还的配额数量。
    };

    /// @brief 保存当前 Work 配置的执行前 acquire 与执行后 release 请求。
    ///
    /// `acquires` 按 Semaphore 指针的全局顺序严格递增且无重复，使运行期能够
    /// 按固定顺序锁定全部 acquire Semaphore，避免多锁场景形成循环等待。
    ///
    /// `releases` 无顺序要求，但同一 Semaphore 最多存在一条 release 请求。
    ///
    /// 容器拥有请求描述符本身，其中的 Semaphore 指针均为非拥有引用。
    struct SemaphoreData {
        std::vector<SemaphoreReq> acquires; ///< 执行前必须原子式全部取得的请求；严格有序且无重复。
        std::vector<SemaphoreReq> releases; ///< 执行完成后逐个归还的请求；无顺序要求且无重复。

        [[nodiscard]] bool empty() const noexcept {
            return acquires.empty() && releases.empty();
        }
    };

    /// @brief 按固定全局顺序锁定当前 Work 的全部 acquire Semaphore。
    ///
    /// 构造时按照 `acquires` 的既定顺序依次取得每个 Semaphore 的内部锁；
    /// 析构时释放全部锁。acquire 列表必须按照 Semaphore 指针的全局顺序
    /// 严格递增且无重复，从而保证不同 Work 的多锁获取顺序一致，避免循环等待。
    ///
    /// acquire 失败并将当前 Work 加入某个 Semaphore 的 waiter 链后，可通过
    /// `blocker()` 标记对应阻塞 Semaphore。析构时先释放其余全部锁，最后才
    /// 释放 blocker 的锁，使当前 Work 在仍可能访问自身 acquire 数据期间不会
    /// 被其他线程从 waiter 链摘出并重新发布。
    ///
    /// @pre acquires 中所有 Semaphore 指针均非空。
    /// @pre acquires 按 Semaphore 指针的全局顺序严格递增且无重复。
    /// @note 本类型只管理 Semaphore 内部锁，不直接修改配额或 waiter 链。
    /// @warning blocker 的锁一旦释放，当前 Work 可能立即被其他线程重新调度甚至销毁；
    ///          此后不得再访问 `m_acquires` 或当前 Work 的任何内部状态。
    class SemaphoreLock final : public Immovable<SemaphoreLock> {
    public:
        /// @brief 按 acquire 列表的固定全局顺序取得全部 Semaphore 锁。
        ///
        /// 调试模式下同时校验 Semaphore 非空，并通过相邻 Semaphore 指针严格递增
        /// 验证 acquire 列表保持有序且无重复。
        ///
        /// @param acquires 当前 Work 的 acquire 请求只读视图。
        TFL_FORCE_INLINE explicit SemaphoreLock(std::span<const SemaphoreReq> acquires) noexcept
            : m_acquires{acquires} {

            Semaphore* previous = nullptr;

            for (const auto& req : m_acquires) {
                TFL_ASSERT(req.sem);

                if (previous) {
                    TFL_ASSERT(std::less<>{}(previous, req.sem));
                }

                req.sem->m_lock.lock();
                previous = req.sem;
            }
        }

        /// @brief 释放构造期间取得的全部 Semaphore 锁。
        ///
        /// 正常路径按照 acquire 顺序的逆序释放全部锁。
        ///
        /// 若设置了 blocker，则当前 Work 已加入对应 Semaphore 的 waiter 链；
        /// 此时先释放其他全部 Semaphore，最后释放 blocker，使当前析构路径完成
        /// 对 `m_acquires` 的最后一次访问后，当前 Work 才能够被其他线程重新发布。
        TFL_FORCE_INLINE ~SemaphoreLock() noexcept {
            Semaphore* const blocker = m_blocker;

            if (blocker) {
                for (std::size_t i = m_acquires.size(); i > 0; --i) {
                    Semaphore* const sem = m_acquires[i - 1].sem;

                    if (sem != blocker) {
                        sem->m_lock.unlock();
                    }
                }

                blocker->m_lock.unlock();
                return;
            }

            for (std::size_t i = m_acquires.size(); i > 0; --i) {
                m_acquires[i - 1].sem->m_lock.unlock();
            }
        }

        /// @brief 标记保存当前 Work 的阻塞 Semaphore。
        ///
        /// 设置后，该 Semaphore 的内部锁将在析构时最后释放，以保证当前 Work
        /// 加入 waiter 链后，在当前执行路径仍访问 acquire 数据期间不会被其他线程
        /// 提前摘出并重新调度。
        ///
        /// @param sem 当前 Work 已加入其 waiter 链的 Semaphore。
        /// @pre sem 非空。
        /// @pre 当前尚未设置 blocker。
        TFL_FORCE_INLINE void blocker(Semaphore* sem) noexcept {
            TFL_ASSERT(sem);
            TFL_ASSERT(m_blocker == nullptr);

            m_blocker = sem;
        }

    private:
        std::span<const SemaphoreReq> m_acquires; ///< 当前 Work 的 acquire 请求只读视图。
        Semaphore* m_blocker{nullptr};            ///< 保存当前 Work 的阻塞 Semaphore；其内部锁必须最后释放。
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
        using Invoke = void (*)(Payload&, Work&, Worker&, Executor&, Work*&) noexcept;

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
        explicit Payload(std::in_place_type_t<T>, Args&&... args) noexcept(noexcept(_construct<T>(std::forward<Args>(args)...))) {
            _construct<T>(std::forward<Args>(args)...);
        }

        ~Payload() noexcept {
            reset();
        }

        template <typename T, typename... Args>
            requires (valid_type<T> && std::constructible_from<T, Args&&...>)
        T* emplace(Args&&... args) noexcept(noexcept(_construct<T>(std::forward<Args>(args)...))) {
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

        void invoke(Work& work, Worker& worker, Executor& executor, Work*& cache) noexcept {
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
        void _construct(Args&&... args) noexcept(!uses_heap<T> && std::is_nothrow_constructible_v<T, Args&&...>) {
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
        static void _invoke(Payload& payload, Work& work, Worker& worker, Executor& executor, Work*& cache) noexcept {
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
    T& emplace(Args&&... args) noexcept(noexcept(m_payload.template emplace<T>(std::forward<Args>(args)...))) {
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

    void invoke(Worker& worker, Executor& executor, Work*& cache) noexcept {
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

    Work*                               m_next{nullptr};                    ///< 运行期 intrusive 链链接槽；未链接时必须为空，同一时刻只能属于一条运行期链。
    Topology*                           m_topology{nullptr};                ///< 非拥有的执行拓扑上下文；提供停止域、完成状态和强引用计数。
    Work*                               m_parent{nullptr};                  ///< 非拥有的运行期父 Work；当前节点完成时向其归还或转移一个 join slot。
    Properties::type                    m_properties{Properties::NONE};     ///< 非原子属性、独占运行状态及静态 join weight 的位编码。
    std::atomic<Control::type>          m_control{Control::NONE};           ///< 并发控制位；保存异常锚点、异常传播/归档及可选执行检查状态。
    std::atomic<std::uint32_t>          m_join_counter{0};                  ///< 运行期 join 计数；用于 strong predecessor 到达、动态子任务等待及父节点恢复。
    std::uint32_t                       m_num_successors{0};                ///< `m_edges` 的后继数量，同时作为 `[后继 | 前驱]` 两个逻辑区间的分割点。
    std::vector<Work*>                  m_edges;                            ///< 非拥有的统一邻接表；布局固定为 `[后继 | 前驱]`：静态边和动态后继非拥有；异步前驱各持一份强引用。
    std::unique_ptr<SemaphoreData>      m_semaphores;                       ///< 按需分配的信号量约束；保存执行前 acquire 与执行后 release 请求。
    std::unique_ptr<ObserverData>       m_observers;                        ///< 按需分配的观察者集合；在节点执行前后触发生命周期通知。

    std::exception_ptr                  m_exception_ptr{nullptr};           ///< 当前 Work 获得异常归档权后保存的异常；由完成同步保证读取可见性。
    const Graph*                        m_graph{nullptr};                   ///< 非拥有的静态物理 Graph；仅静态图节点绑定，独立异步 Work 通常为空。
    std::unique_ptr<std::string>        m_name;                             ///< 按需分配的节点名称；仅用于调试、诊断和 D2 可视化。


    /// @brief 设置当前 Work 的调试名称.
    ///
    /// 将 @p value 转换为 `std::string` 后保存到按需分配的名称存储中。
    /// 空字符串表示清除名称并释放对应存储；已有名称时复用现有 `std::string`
    /// 对象并更新内容，避免重复分配外层名称对象.
    ///
    /// @tparam S 可用于构造 `std::string` 的字符串类型.
    /// @param value 新的节点名称；转换后为空时清除当前名称.
    ///
    /// @throws std::bad_alloc 名称字符串或名称存储分配失败时抛出.
    template <typename S>
        requires std::constructible_from<std::string, S>
    TFL_FORCE_INLINE void _set_name(S&& value) {
        std::string name(std::forward<S>(value));

        if (name.empty()) {
            m_name.reset();
        } else if (m_name) {
            *m_name = std::move(name);
        } else {
            m_name = std::make_unique<std::string>(std::move(name));
        }
    }

    /// @brief 获取当前 Work 的调试名称.
    ///
    /// 返回指向内部名称存储的只读视图；当前 Work 未设置名称时返回空
    /// `std::string_view`.
    ///
    /// @return 当前节点名称的只读视图；未设置名称时为空.
    ///
    /// @warning 返回的视图不拥有底层字符串；调用 `_set_name()`、销毁当前 Work
    ///          或其他导致名称存储失效的操作后，该视图不得继续使用.
    [[nodiscard]] TFL_FORCE_INLINE std::string_view _name() const noexcept {
        return m_name ? std::string_view{*m_name} : std::string_view{};
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
#if TFL_ENABLE_EXCEPTIONS
        return _has_exception() || m_topology->_stop_requested();
#else
        return _stop_requested();
#endif
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

    /// @brief 捕获当前 `TFL_CATCH_ALL` 正在处理的异常并进入统一传播/归档流程。
    ///
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
    ///
    /// @note `m_exception_ptr` 是非原子对象，只能在框架约定的完成同步之后由观察者读取。
    ///
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
    TFL_FORCE_INLINE void _process_exception(std::exception_ptr eptr) noexcept {
        TFL_ASSERT(eptr);

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
        //   - EXCEPTION        表示归档 Work 本身也处于异常路径；
        //   - EXCEPTION_CAUGHT 的旧值用于判断本次调用是否取得首次归档权。
        constexpr auto archive_mask = Control::EXCEPTION | Control::EXCEPTION_CAUGHT;

        // 优先级 1：显式锚点。
        if (explicit_anchor) {
            const auto prev = explicit_anchor->m_control.fetch_or(archive_mask, std::memory_order_relaxed);

            if ((prev & Control::EXCEPTION_CAUGHT) == 0) {
                explicit_anchor->m_exception_ptr = eptr;
                return;
            }

            // 显式锚点已有异常占用归档权；继续尝试当前 Work 的兜底位置。
        }
        // 优先级 2：仅在没有显式锚点时尝试首个隐式锚点。
        else if (implicit_anchor) {
            const auto prev = implicit_anchor->m_control.fetch_or(archive_mask, std::memory_order_relaxed);

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
        const auto prev = m_control.fetch_or(archive_mask, std::memory_order_relaxed);

        if ((prev & Control::EXCEPTION_CAUGHT) == 0) {
            m_exception_ptr = eptr;
        }
    }

    /// @brief 调用 Module 终止谓词，并将异常转换为终止条件.
    ///
    /// predicate 正常返回时直接返回其结果；若调用抛出异常，则将异常归档到当前
    /// Work，随后返回 true，使 Module 进入正常结束和资源清理路径.
    ///
    /// @tparam P 无参终止谓词类型。
    /// @param predicate 要调用的终止谓词。
    /// @return predicate 的返回值；发生异常时返回 true。
    template <predicate P>
    TFL_FORCE_INLINE bool _invoke_predicate(P& predicate) noexcept {
        if constexpr (noexcept(std::invoke(predicate))) {
            return std::invoke(predicate);
        } else {
            TFL_TRY {
                return std::invoke(predicate);
            } TFL_CATCH_ALL {
                _process_exception();
                return true;
            }
        }
    }

    /// @brief 调用完成回调，并将异常归档到当前 Work.
    ///
    /// callback 抛出的异常不会离开本函数，而是进入当前 Work 的统一传播和归档流程，
    /// 从而保证调用方能够继续完成 Observer、Semaphore、执行状态和 tear-down 等收尾流程.
    ///
    /// @tparam C 无参完成回调类型。
    /// @param callback 要调用的完成回调。
    template <callback C>
    TFL_FORCE_INLINE void _invoke_callback(C& callback) noexcept {
        if constexpr (noexcept(std::invoke(callback))) {
            std::invoke(callback);
        } else {
            TFL_TRY {
                std::invoke(callback);
            } TFL_CATCH_ALL {
                _process_exception();
            }
        }
    }

    /// @brief 获取 `m_edges` 中后继前缀的可修改 span 视图。
    ///
    /// `m_edges` 始终按照 `[successors | predecessors]` 保存，前 `m_num_successors`
    /// 个元素构成当前 Work 的物理后继区间。
    ///
    /// @return 指向当前后继区间的非拥有可修改视图。
    /// @note 返回视图仅在 `m_edges` 未发生重新分配、插入、删除或销毁期间有效。
    /// @warning 直接修改视图中的元素不会自动维护目标 Work 的反向 predecessor 关系；
    ///          通常应通过专用建边/删边接口修改静态图关系。
    [[nodiscard]] std::span<Work*> _successors() noexcept {
        return {m_edges.data(), m_num_successors};
    }

    /// @brief 获取 `m_edges` 中后继前缀的只读 span 视图。
    ///
    /// @return 指向当前后继区间的非拥有只读视图。
    /// @note 返回视图仅在 `m_edges` 未发生重新分配、插入、删除或销毁期间有效。
    [[nodiscard]] std::span<Work* const> _successors() const noexcept {
        return {m_edges.data(), m_num_successors};
    }

    /// @brief 获取 `m_edges` 中前驱后缀的可修改 span 视图。
    ///
    /// 前驱区间固定为 `[m_num_successors, m_edges.size())`，与后继前缀共同组成
    /// `m_edges == [successors | predecessors]` 的统一物理布局。
    ///
    /// @return 指向当前前驱区间的非拥有可修改视图。
    /// @note 返回视图仅在 `m_edges` 未发生重新分配、插入、删除或销毁期间有效。
    /// @warning 直接修改视图中的元素不会自动维护对应前驱 Work 的 successor 关系；
    ///          通常应通过专用静态边维护接口修改依赖关系。
    [[nodiscard]] std::span<Work*> _predecessors() noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }

    /// @brief 获取 `m_edges` 中前驱后缀的只读 span 视图。
    ///
    /// @return 指向当前前驱区间的非拥有只读 span；未创建 SemaphoreData 时返回空视图。
    /// @note 返回视图仅在 `m_edges` 未发生重新分配、插入、删除或销毁期间有效。
    [[nodiscard]] std::span<Work* const> _predecessors() const noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }

    /// @brief 返回当前 Work 的物理前驱数量。
    ///
    /// 前驱数量由统一边表总长度减去后继前缀长度得到，不额外保存独立计数字段。
    ///
    /// @return 当前 `m_edges` 前驱后缀中的元素数量。
    /// @pre `m_num_successors <= m_edges.size()`。
    [[nodiscard]] std::size_t _num_predecessors() const noexcept {
        return m_edges.size() - m_num_successors;
    }

    /// @brief 按需创建 SemaphoreData，并返回其可修改引用。
    ///
    /// 当前 Work 尚未配置任何 Semaphore 数据时分配一个新的 `SemaphoreData`；
    /// 已存在时直接复用原对象。
    ///
    /// @return 当前 Work 唯一拥有的 SemaphoreData 引用。
    /// @throws std::bad_alloc 首次创建 SemaphoreData 分配失败时抛出。
    /// @note 调用成功后 `m_semaphores` 必定非空。
    [[nodiscard]] SemaphoreData& _ensure_semaphore_data() {
        if (!m_semaphores) {
            m_semaphores = std::make_unique<SemaphoreData>();
        }
        return *m_semaphores;
    }

    /// @brief 在 acquire/release 配置均为空时回收 SemaphoreData。
    ///
    /// 仅当当前 Work 已存在 SemaphoreData，且其中 acquire 与 release 两个列表
    /// 均为空时释放该按需存储；任一列表仍有配置时保持对象不变。
    ///
    /// @post 若 acquire/release 均为空，则 `m_semaphores == nullptr`。
    /// @note 本函数只回收配置存储，不修改任何 Semaphore 对象或运行期配额。
    void _cleanup_semaphore_data() noexcept {
        if (m_semaphores && m_semaphores->empty()) {
            m_semaphores.reset();
        }
    }

    /// @brief 获取当前 Work 的 acquire 请求可修改视图。
    ///
    /// @return 已配置 acquire 请求的非拥有可修改 span；未创建 SemaphoreData 时返回空视图。
    /// @note acquire 列表按照 Semaphore 指针的全局顺序严格递增且无重复。
    /// @note 返回视图仅在 acquire vector 未发生插入、删除、清空、重新分配或
    ///       `m_semaphores` 未被释放期间有效。
    /// @warning 修改 `SemaphoreReq::sem` 会破坏 acquire 列表的全局排序不变量；
    ///          正常配置修改应通过 `_acquire()`、`_remove_acquire()` 或 `_clear_acquires()` 完成。
    [[nodiscard]] std::span<SemaphoreReq> _acquires() noexcept {
        return m_semaphores ? std::span<SemaphoreReq>{m_semaphores->acquires} : std::span<SemaphoreReq>{};
    }

    /// @brief 获取当前 Work 的 acquire 请求只读视图。
    ///
    /// @return 已配置 acquire 请求的非拥有只读 span；未创建 SemaphoreData 时返回空视图。
    /// @note acquire 列表按照 Semaphore 指针的全局顺序严格递增且无重复。
    /// @note 返回视图仅在 acquire vector 未发生结构修改或 `m_semaphores` 未被释放期间有效。
    [[nodiscard]] std::span<SemaphoreReq const> _acquires() const noexcept {
        return m_semaphores ? std::span<SemaphoreReq const>{m_semaphores->acquires} : std::span<SemaphoreReq const>{};
    }

    /// @brief 获取当前 Work 的 release 请求可修改视图。
    ///
    /// @return 已配置 release 请求的非拥有可修改 span；未创建 SemaphoreData 时返回空视图。
    /// @note release 列表无固定排序要求，但同一 Semaphore 最多存在一条请求。
    /// @note 返回视图仅在 release vector 未发生插入、删除、清空、重新分配或
    ///       `m_semaphores` 未被释放期间有效。
    [[nodiscard]] std::span<SemaphoreReq> _releases() noexcept {
        return m_semaphores ? std::span<SemaphoreReq>{m_semaphores->releases} : std::span<SemaphoreReq>{};
    }

    /// @brief 获取当前 Work 的 release 请求只读视图。
    ///
    /// @return 已配置 release 请求的非拥有只读 span；未创建 SemaphoreData 时返回空视图。
    /// @note release 列表无固定排序要求，但同一 Semaphore 最多存在一条请求。
    /// @note 返回视图仅在 release vector 未发生结构修改或 `m_semaphores` 未被释放期间有效。
    [[nodiscard]] std::span<SemaphoreReq const> _releases() const noexcept {
        return m_semaphores ? std::span<SemaphoreReq const>{m_semaphores->releases} : std::span<SemaphoreReq const>{};
    }

    /// @brief 返回当前 Work 配置的 acquire 请求数量。
    ///
    /// @return acquire 请求数量；尚未创建 SemaphoreData 时返回 0。
    [[nodiscard]] std::size_t _num_acquires() const noexcept {
        return m_semaphores ? m_semaphores->acquires.size() : 0;
    }

    /// @brief 返回当前 Work 配置的 release 请求数量。
    ///
    /// @return release 请求数量；尚未创建 SemaphoreData 时返回 0。
    [[nodiscard]] std::size_t _num_releases() const noexcept {
        return m_semaphores ? m_semaphores->releases.size() : 0;
    }

    /// @brief 返回当前 Work 注册的观察者数量。
    ///
    /// @return 已注册观察者数量；尚未创建 ObserverData 时返回 0。
    [[nodiscard]] std::size_t _num_observers() const noexcept {
        return m_observers ? m_observers->observers.size() : 0;
    }

    // ---- Semaphore 配置与运行期 acquire/release ----

    /// @brief 添加一条执行前 Semaphore acquire 请求。
    ///
    /// acquire 请求按 Semaphore 指针的全局顺序严格递增保存，同一 Semaphore
    /// 最多存在一条请求。
    ///
    /// @param sem 非拥有的目标 Semaphore。
    /// @param count 每次执行需要一次性取得的配额数量。
    /// @throws Exception sem 为空或同一 Semaphore 已存在于 acquire 列表时抛出。
    /// @note count == 0 时忽略该请求。
    void _acquire(Semaphore* sem, std::size_t count);

    /// @brief 添加一条执行后 Semaphore release 请求。
    ///
    /// @param sem 非拥有的目标 Semaphore。
    /// @param count 每次执行完成后需要归还的配额数量。
    /// @throws Exception sem 为空或同一 Semaphore 已存在于 release 列表时抛出。
    /// @note count == 0 时忽略该请求。
    void _release(Semaphore* sem, std::size_t count);

    /// @brief 移除指定 Semaphore 的 acquire 请求。
    ///
    /// @param sem 要移除的 Semaphore；为空或不存在时无操作。
    /// @note 删除后 acquire 列表继续保持严格有序且无重复。
    void _remove_acquire(Semaphore* sem) noexcept;

    /// @brief 移除指定 Semaphore 的 release 请求。
    ///
    /// @param sem 要移除的 Semaphore；为空或不存在时无操作。
    void _remove_release(Semaphore* sem) noexcept;

    /// @brief 清空当前 Work 的全部 acquire 请求。
    void _clear_acquires() noexcept;

    /// @brief 清空当前 Work 的全部 release 请求。
    void _clear_releases() noexcept;

    /// @brief 原子式尝试取得当前 Work 配置的全部 acquire 配额。
    ///
    /// 按 acquire 列表的全局固定顺序锁定全部 Semaphore，在所有锁保护下先检查
    /// 每条请求是否能够同时满足。全部满足时一次性扣减全部配额；任一请求不满足时
    /// 不修改任何配额，并将当前 Work 加入首个不满足请求的 Semaphore waiter 链。
    ///
    /// @return 全部 acquire 成功时返回 true；当前 Work 进入 waiter 链时返回 false。
    /// @pre acquire 列表非空、严格有序且无重复。
    /// @note 本操作具有 all-or-nothing 语义，不存在部分获取和失败回滚。
    /// @warning 返回 false 后当前 Work 已被发布到某个 Semaphore waiter 链，
    ///          调用方不得继续执行当前 Work，本次 invoke 应立即返回。
    [[nodiscard]] TFL_FORCE_INLINE bool _try_acquire_semaphores() noexcept;

    /// @brief 执行当前 Work 配置的全部 Semaphore release 请求。
    ///
    /// 每条 release 请求独立锁定对应 Semaphore 并归还配额。release 解冻的 waiter
    /// 通过 intrusive 链依次追加到 `[first, last]`，最终由调用方统一重新调度。
    ///
    /// @param first 接收全部 release 解冻的 waiter 链首节点；空链时为 nullptr。
    /// @param last 接收全部 release 解冻的 waiter 链尾节点；空链时为 nullptr。
    /// @pre first 和 last 必须同时为空或同时非空。
    /// @note release 路径不会同时持有多个 Semaphore 锁，因此 releases 无需固定排序。
    TFL_FORCE_INLINE void _release_semaphores(Work*& first, Work*& last) noexcept;

    // ---- 观察者执行前/后通知 ----
    /// @brief 在 callable 正式执行前依次通知当前 Work 注册的全部观察者。
    ///
    /// 单个观察者抛出的异常会被捕获并进入当前 Work 的统一异常归档流程，
    /// 不影响后续观察者继续接收 before 通知。
    TFL_FORCE_INLINE void _notify_before(Worker& wr) noexcept;

    /// @brief 在 callable 执行结束后依次通知当前 Work 注册的全部观察者。
    ///
    /// 单个观察者抛出的异常会被捕获并进入当前 Work 的统一异常归档流程，
    /// 不影响后续观察者继续接收 after 通知。
    TFL_FORCE_INLINE void _notify_after(Worker& wr) noexcept;

    // ---- 静态图双向边表维护 ----

    /// @brief 删除后继区间中指定位置的一个后继节点。
    ///
    /// `m_edges` 固定保存为 `[successors | predecessors]` 两个连续区间。
    /// 删除时通过常数次交换维持该分区布局，不保证后继区间内部顺序稳定。
    ///
    /// @param idx 要删除的后继在 `_successors()` 中的相对下标。
    /// @pre `idx < m_num_successors`。
    /// @post `m_edges` 总长度减少 1，`m_num_successors` 减少 1。
    /// @post 前驱数量保持不变，`m_edges` 继续满足 `[successors | predecessors]` 布局。
    void _erase_successor_at(std::size_t idx) noexcept;

    /// @brief 删除前驱区间中指定位置的一个前驱节点。
    ///
    /// 前驱区间位于 `m_edges[m_num_successors, m_edges.size())`。
    /// 删除时使用最后一个前驱覆盖目标位置，因此不保证前驱区间内部顺序稳定。
    ///
    /// @param idx 要删除的前驱在 `_predecessors()` 中的相对下标。
    /// @pre `idx < _num_predecessors()`。
    /// @post `m_edges` 总长度减少 1，`m_num_successors` 保持不变。
    /// @post 后继区间及 `[successors | predecessors]` 分区布局保持有效。
    void _erase_predecessor_at(std::size_t idx) noexcept;

    /// @brief 建立当前 Work 到目标 Work 的静态有向边。
    ///
    /// 在当前 Work 的后继区间加入 @p target，同时在 @p target 的前驱区间加入
    /// 当前 Work，从而维护双向对称的静态邻接关系。
    ///
    /// 当 @p Check 为 true 时，建立边之前会检查空目标、Graph 一致性、重复边、
    /// 非 Jump 自环以及不经过 Jump/MultiJump 的严格闭环等合法性约束。
    ///
    /// @tparam Check 是否执行静态建边合法性检查。
    /// @param target 要建立为当前 Work 后继的目标节点。
    ///
    /// @pre 当 Check 为 false 时，调用方必须保证 target 非空且建边关系合法。
    /// @throws Exception Check 为 true 且新增边不满足拓扑约束时抛出。
    /// @throws std::bad_alloc 任一侧边表扩容失败时抛出。
    /// @note 若 target 侧插入失败，会撤销当前 Work 侧已经完成的插入，保持逻辑边不存在。
    /// @post 成功后当前 Work 的后继区和 target 的前驱区各存在一条对应记录。
    template <bool Check = true>
    void _precede(Work* target);

    /// @brief 删除当前 Work 到指定目标 Work 的静态有向边。
    ///
    /// 若 @p target 当前是本 Work 的后继，则同时删除当前 Work 后继区中的 target
    /// 以及 target 前驱区中的当前 Work，保持双向邻接关系一致。
    ///
    /// @param target 要解除连接的目标后继；为空或当前不存在该边时无操作。
    /// @post 成功删除后，两端均不再保存该逻辑边对应的邻接记录。
    /// @note 本函数不保证剩余前驱或后继的相对顺序稳定。
    void _remove_successor(Work* target) noexcept;

    /// @brief 清除当前 Work 的全部静态前驱关系。
    ///
    /// 遍历当前前驱区间，并从每个前驱 Work 的后继区中删除当前 Work，
    /// 随后清空本 Work 的整个前驱区间。
    ///
    /// @pre 当前双向边表保持一致，每个本地前驱都必须在其后继区中包含当前 Work。
    /// @post `_num_predecessors() == 0`。
    /// @post 所有原前驱均不再以当前 Work 作为后继。
    /// @post 当前 Work 的后继区和 `m_num_successors` 保持不变。
    void _clear_predecessors() noexcept;

    /// @brief 清除当前 Work 的全部静态后继关系。
    ///
    /// 遍历当前后继区间，并从每个后继 Work 的前驱区中删除当前 Work，
    /// 随后删除本 Work 的整个后继前缀，使原前驱区整体前移到 `m_edges` 起始位置。
    ///
    /// @pre 当前双向边表保持一致，每个本地后继都必须在其前驱区中包含当前 Work。
    /// @post `m_num_successors == 0`。
    /// @post 所有原后继均不再以当前 Work 作为前驱。
    /// @post 当前 Work 原有前驱关系保持不变。
    void _clear_successors() noexcept;


    // ---- 静态图建边合法性与无 Jump 路径检测 ----

    /// @brief 检查从 @p from 到 @p to 是否存在一条不经过 Jump/MultiJump 的有向路径。
    ///
    /// 使用 DFS 沿静态 successor 边搜索。遇到 Jump 或 MultiJump 节点时停止从该节点
    /// 继续向后展开，使结果只反映普通控制流节点形成的严格路径关系。
    ///
    /// @param from 搜索起点。
    /// @param to 搜索目标。
    /// @return 存在满足条件的路径时返回 true，否则返回 false。
    /// @note 本函数可能为 DFS 工作集和 visited 集合进行动态内存分配。
    /// @note 本函数只读取静态边表，调用期间相关 Graph 结构不得被并发修改。
    [[nodiscard]] bool _has_path_without_jump(const Work* from, const Work* to) const;

    /// @brief 检查新增静态边 `this -> target` 是否满足当前图的建边规则。
    ///
    /// 校验内容包括：
    /// 1. target 非空；
    /// 2. 当前 Work 已绑定 Graph，且双方属于同一 Graph；
    /// 3. 不允许重复建立同一条边；
    /// 4. 普通节点不允许自环；
    /// 5. 普通节点之间不得形成完全不经过 Jump/MultiJump 的严格闭环。
    ///
    /// Jump 和 MultiJump 节点参与的循环由专用控制流语义处理，因此相关路径不会
    /// 按普通严格 DAG 规则直接判定为非法。
    ///
    /// @param target 待连接的目标后继。
    /// @return 合法时返回 `std::nullopt`；非法时返回对应错误描述。
    /// @note 本函数只负责检查，不修改任何边表。
    [[nodiscard]] std::optional<std::string_view> _can_precede(Work* target) const;


    /// @brief 销毁当前零引用异步 Work，并迭代回收其前驱强引用链。
    ///
    /// 当前 Work 的 Topology 强引用已经归零后，由唯一回收线程进入本函数。
    /// 实现先接管当前节点的边表，再销毁当前 Work，随后逐个释放其中异步前驱持有的
    /// 强引用；若某个前驱的引用同时归零，则借用该前驱的 `m_parent` 字段将其挂入
    /// 待回收链，并继续以迭代方式处理，避免递归销毁造成调用栈增长。
    ///
    /// 当前节点销毁前会先将需要继续使用的数据转移到局部对象中，因此 `destroy_work()`
    /// 返回后不再访问已经销毁节点的任何成员。
    ///
    /// @pre 当前 Work 的强引用已归零，并由当前线程独占回收。
    /// @pre 当前 Work 不再可能进入执行、调度、等待或其他并发访问路径。
    /// @pre `m_num_successors <= m_edges.size()`，且前驱区保存的异步前驱各持有一份强引用。
    /// @pre 待回收节点原有 `m_parent` 不再承担正常执行期父子关系语义，可安全借作回收链链接槽。
    /// @pre `destroy_work()` 只负责销毁当前节点自身，不再次递归释放其前驱引用。
    /// @pre 节点析构逻辑不得依赖已经从节点中接管出去的边表内容。
    ///
    /// @note 前驱强引用覆盖当前 Work 的完整析构过程，确保析构期间所有前驱仍然有效。
    /// @note 整个回收过程使用迭代链完成，不因依赖深度增加递归栈消耗。
    /// @note 借用零引用节点的 `m_parent` 串联待回收链，不额外分配辅助链表节点。
    /// @warning 本函数会销毁 `this`；进入实际销毁后不得再访问当前 Work。
    /// @warning 调用返回时 `this` 已经失效，调用方不得再读取、写入、比较解引用或重新调度该指针。
    void _destroy_async() noexcept;
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

template <bool Check>
inline void Work::_precede(Work* const target) {
    if constexpr (Check) {
        if (auto error = _can_precede(target)) {
            TFL_THROW(Exception("cannot precede: {}.", *error));
        }
    }

    // 先在 this 侧插入后继，并通过交换维持“后继在前、前驱在后”的统一布局。
    m_edges.push_back(target);
    if (m_num_successors < m_edges.size() - 1) {
        std::swap(m_edges[m_num_successors], m_edges.back());
    }
    ++m_num_successors;
    TFL_TRY {
        target->m_edges.push_back(this);
    } TFL_CATCH_ALL {
        // 第二端扩容失败时撤销第一端，保留已有前驱/后继和分区布局。
        _erase_successor_at(m_num_successors - 1);
        TFL_RETHROW();
    }
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

    // 擦除后继前缀，由 vector 将前驱区间整体前移到 m_edges 起始位置。
    m_edges.erase(m_edges.begin(), m_edges.begin() + m_num_successors);
    m_num_successors = 0;
}

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

inline void Work::_acquire(Semaphore* sem, std::size_t count) {
    if (!sem) TFL_THROW(Exception("cannot acquire null semaphore."));
    if (count == 0) return;

    auto& acquires = _ensure_semaphore_data().acquires;

    auto it = std::ranges::lower_bound(
        acquires,
        sem,
        std::less<>{},
        &SemaphoreReq::sem
        );

    if (it != acquires.end() && it->sem == sem) {
        TFL_THROW(Exception("semaphore already in acquire list."));
    }

    acquires.insert(it, SemaphoreReq{sem, count});
}

inline void Work::_release(Semaphore* sem, std::size_t count) {
    if (!sem) TFL_THROW(Exception("cannot release null semaphore."));
    if (count == 0) return;

    auto& sd = _ensure_semaphore_data();

    // release 列表通常较小，直接线性检查同一 Semaphore 是否已存在。
    for (std::size_t i = 0; i < sd.releases.size(); ++i) {
        if (sd.releases[i].sem == sem) {
            TFL_THROW(Exception("semaphore already in release list."));
        }
    }

    sd.releases.emplace_back(sem, count);
}

inline void Work::_remove_acquire(Semaphore* sem) noexcept {
    if (!m_semaphores || !sem) return;

    auto& acquires = m_semaphores->acquires;

    auto it = std::ranges::lower_bound(
        acquires,
        sem,
        std::less<>{},
        &SemaphoreReq::sem
        );

    if (it != acquires.end() && it->sem == sem) {
        acquires.erase(it);
        _cleanup_semaphore_data();
    }
}

inline void Work::_remove_release(Semaphore* sem) noexcept {
    if (m_semaphores) {
        auto& rels = m_semaphores->releases;

        for (std::size_t i = 0; i < rels.size(); ++i) {
            if (rels[i].sem == sem) {
                rels[i] = rels.back();
                rels.pop_back();
                _cleanup_semaphore_data();
                return;
            }
        }
    }
}

inline void Work::_clear_acquires() noexcept {
    if (m_semaphores) {
        m_semaphores->acquires.clear();
        _cleanup_semaphore_data();
    }
}

inline void Work::_clear_releases() noexcept {
    if (m_semaphores) {
        m_semaphores->releases.clear();
        _cleanup_semaphore_data();
    }
}

TFL_FORCE_INLINE bool Work::_try_acquire_semaphores() noexcept {
    auto acquires = _acquires();

    TFL_ASSERT(!acquires.empty());

    SemaphoreLock lock{acquires};

    Semaphore* blocker = nullptr;

    // 第一阶段：在全部 Semaphore 锁保护下检查所有请求。
    //
    // 本阶段只读取配额，不修改任何 m_value，因此任一请求失败时无需回滚。
    for (const auto& req : acquires) {
        TFL_ASSERT(req.sem);
        TFL_ASSERT(req.count != 0);
        TFL_ASSERT(req.sem->m_value <= req.sem->m_max_value);

        if (req.sem->m_value < req.count) {
            blocker = req.sem;
            break;
        }
    }

    // 第二阶段：只有所有请求全部满足时才统一提交配额扣减。
    if (!blocker) {
        for (const auto& req : acquires) {
            req.sem->m_value -= req.count;
        }

        return true;
    }

    // 当前 Work 即将加入 blocker 的 waiter intrusive 链，因此链接槽必须为空。
    //
    // blocker 的内部锁此时仍由 SemaphoreLock 持有，所以其他 release 线程不能
    // 同时修改 waiter 链，也不能在当前路径完成前提前摘出当前 Work。
    TFL_ASSERT(m_next == nullptr);

    if (blocker->m_waiter_tail) {
        blocker->m_waiter_tail->m_next = this;
    } else {
        blocker->m_waiter_head = this;
    }

    blocker->m_waiter_tail = this;

    // 当前 Work 已发布到 blocker waiter 链。
    //
    // 将 blocker 标记为最后解锁对象；SemaphoreLock 析构时先释放其余锁，
    // 最后释放 blocker。blocker 解锁后当前 Work 可能立即被重新调度，因此
    // 此后不得再访问当前 Work 的 acquire 数据。
    lock.blocker(blocker);

    return false;
}

TFL_FORCE_INLINE void Work::_release_semaphores(Work*& first, Work*& last) noexcept {
    TFL_ASSERT((first == nullptr) == (last == nullptr));

    for (const auto& req : m_semaphores->releases) {
        TFL_ASSERT(req.sem);
        TFL_ASSERT(req.count != 0);

        req.sem->_release(first, last, req.count);
    }
}

TFL_FORCE_INLINE void Work::_notify_before(Worker& wr) noexcept {
    if (m_observers) [[unlikely]] {
        for (auto& observer : m_observers->observers) {
            TFL_ASSERT(observer);

            TFL_TRY {
                observer->on_before(WorkerView{wr});
            } TFL_CATCH_ALL {
                _process_exception();
            }
        }
    }
}

TFL_FORCE_INLINE void Work::_notify_after(Worker& wr) noexcept {
    if (m_observers) [[unlikely]] {
        for (auto& observer : m_observers->observers) {
            TFL_ASSERT(observer);

            TFL_TRY {
                observer->on_after(WorkerView{wr});
            } TFL_CATCH_ALL {
                _process_exception();
            }
        }
    }
}

inline void Semaphore::_release(Work*& out_first, Work*& out_last, std::size_t count) noexcept {
    TFL_ASSERT((out_first == nullptr) == (out_last == nullptr));

    std::lock_guard lock{m_lock};

    // 当前可用配额必须始终位于合法范围内。
    TFL_ASSERT(m_value <= m_max_value);

    // release 不允许使可用配额超过配置上限。
    // 使用差值判断避免直接计算 m_value + count 时发生无符号溢出。
    TFL_ASSERT(count <= m_max_value - m_value);

    m_value += count;

    // 当前没有 waiter 时只完成配额归还，不修改调用方输出链。
    if (!m_waiter_head) {
        TFL_ASSERT(m_waiter_tail == nullptr);
        return;
    }

    TFL_ASSERT(m_waiter_tail);

    // 将 Semaphore 的整条 waiter 链 O(1) 追加到调用方输出链尾部。
    //
    // out 非空：
    //     out_first -> ... -> out_last
    //                               |
    //                               v
    //     waiter_head -> ... -> waiter_tail
    //
    // out 为空：
    //     out_first = waiter_head
    //
    // 最终 out_last 统一指向原 waiter_tail。
    if (out_last) {
        out_last->m_next = m_waiter_head;
    } else {
        out_first = m_waiter_head;
    }

    out_last = m_waiter_tail;

    // waiter 已整体转移给调用方，Semaphore 内部恢复为空链状态。
    m_waiter_head = nullptr;
    m_waiter_tail = nullptr;
}

/// @brief 内部执行作用域使用的栈绑定异常/完成锚点 Work。
///
/// AnchorWork 不承载用户 callable，通过独立 Topology 和
/// `Control::EXPLICIT_ANCHOR` 聚合子链完成计数与异常。
///
/// Preempted 为 false 时仅作为协作执行锚点；
/// Preempted 为 true 时参与 PREEMPTED 恢复，并在恢复执行后
/// 发布所属 Topology 完成状态并唤醒等待线程。
///
/// @tparam Preempted 是否在完成后恢复当前锚点。
/// @note 对象由调用栈直接管理，绝不能通过 `destroy_work()` 或 Work 对象池回收。
template <bool Preempted>
class AnchorWork final : public Work {
    class Invoker final : public TopologyStorage {
    public:
        static constexpr TaskType TYPE = TaskType::None;
        static constexpr Work::Properties::type PROPERTIES = Preempted ? Work::Properties::PREEMPTED : Work::Properties::NONE;
        static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

        explicit Invoker(Executor& executor, Topology* parent_topology) noexcept
            : TopologyStorage{executor, parent_topology} {}

        void invoke(Work& work, Worker&, Executor&, Work*&) noexcept {
            if constexpr (Preempted) {
                work.m_topology->_set_finished();
            }
        }

        void dump(const Work&, std::ostream&) const noexcept {}
    };

public:
    explicit AnchorWork(Work& parent, Executor& executor) noexcept
        : Work{std::in_place_type<Invoker>,
               std::addressof(parent),
               executor,
               parent.m_topology} {
        TFL_ASSERT(parent.m_topology);

        if constexpr (Preempted) {
            m_topology->_set_running();
        }
    }

    explicit AnchorWork(Executor& executor) noexcept
        : Work{std::in_place_type<Invoker>,
               static_cast<Work*>(nullptr),
               executor,
               nullptr} {
        if constexpr (Preempted) {
            m_topology->_set_running();
        }
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

inline void Work::_destroy_async() noexcept {
    Work* pending = this;
    pending->m_parent = nullptr;

    while (pending) {
        Work* const current = pending;
        pending = std::exchange(current->m_parent, nullptr);

        TFL_ASSERT(current->m_num_successors <= current->m_edges.size());

        const auto num_successors = current->m_num_successors;

        // 接管边表及前驱引用，并将当前节点的边表恢复为空。
        std::vector<Work*> edges;
        edges.swap(current->m_edges);
        current->m_num_successors = 0;

        // 前驱引用尚未释放，覆盖当前 Work 的完整析构。
        destroy_work(current);

        // current 已销毁，此后仅访问局部变量和其他待回收节点。
        while (edges.size() > num_successors) {
            Work* const predecessor = edges.back();
            TFL_ASSERT(predecessor);
            edges.pop_back();

            if (predecessor->m_topology->_decrement_ref()) {
                predecessor->m_parent = pending;
                pending = predecessor;
            }
        }
    }
}

}  // namespace tfl
