/// @file coro.hpp
/// @brief Coro —— Coroutine 执行上下文与挂起恢复控制。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-10-03
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "async_future.hpp"
#include "forward.hpp"
#include "work_factory_fwd.hpp"
#include "traits.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief Coroutine 的执行上下文与外部恢复控制。
///
/// 一个 Coro 对应一个 Coroutine Work；同一 Work 内通过 co_await 进入的 CoroTask
/// 共享该 Coro，fork() / silent_fork() 则为派生任务创建独立 Work 与 Topology。
///
/// yield() 主动让出执行权，join() 等待当前 Work 已派生的子 Work；
/// silent_fork() 不返回结果句柄，fork() 在 await_resume() 中返回 AsyncFuture<R>。
/// 两种 fork 都支持 Basic、Runtime、SubFlow、Coroutine callable 及 Graph/Module。
/// Graph 可执行一次、指定次数，或由 predicate 控制重复执行；fork() 还支持
/// 完成回调和 AsyncFuture/AsyncTask 前置依赖。
///
/// m_resume 保存下一次由 Executor 恢复的 Coroutine handle；m_state 仲裁外部恢复请求。
/// 同一 Work 内父子 CoroTask 的对称转移不改变 m_resume。
///
/// @note fork() 返回 Future 表示子任务已经提交，不表示子任务已经完成。
/// @note 派生 Work 计入当前 Work 的 join_counter，父 Coroutine 可通过 co_await join() 等待。
/// @warning 当前双标志状态协议仍要求在 await_suspend 完成安全交接后才恢复父 Coroutine；
///          Yield/Fork 的提前发布竞态需要同步修改 Invoker 与调度协议，单靠本头文件不能解决。
/// @warning Coro 的绑定和跨 Worker 恢复依赖 Context 提供 _rebind() 及当前 Work 访问入口。
class Coro final : public Context {
    template <typename>
    friend class CoroAwaiter;

    template <typename>
    friend class CoroTask;

    TFL_WORK_SUBCLASS_FRIENDS;

    /// @brief Coroutine 的原子运行状态与恢复权标志。
    ///
    /// 使用两个高位标志表示三个逻辑状态：
    ///
    ///   NONE                  未启动或已经完成。
    ///   RUNNING               正在运行或已经取得恢复权。
    ///   RUNNING | SUSPENDED   已登记挂起点，等待外部恢复。
    ///
    /// RUNNING 在整个 Coroutine Work 生命周期内保持置位，
    /// SUSPENDED 由挂起操作设置，由取得恢复权的调用者清除。
    struct State {
        using type = std::uint32_t;

        static constexpr unsigned BITS = std::numeric_limits<type>::digits;
        static constexpr type NONE = 0;

        /// @brief Coroutine 已启动且尚未完成，或已经取得恢复权。
        static constexpr type RUNNING = type{1} << (BITS - 1);

        /// @brief Coroutine 已登记挂起点，等待外部恢复请求。
        static constexpr type SUSPENDED = type{1} << (BITS - 2);

        /// @brief 所有 Coroutine 运行状态标志。
        static constexpr type FLAG_MASK = RUNNING | SUSPENDED;
    };

    static_assert(std::atomic<State::type>::is_always_lock_free);

public:

    /// @brief 协作等待当前 Coroutine Work 派生的全部子 Work 完成。
    ///
    /// `co_await coro.join()` 在有未完成子 Work 时挂起当前 Coroutine，
    /// 暂时释放根 Coroutine 占用的基准 join slot。最后一个子 Work 归还 slot 后，
    /// Executor 负责重新调度父 Work；await_resume() 恢复基准 slot。
    ///
    /// @note 如果没有未完成子 Work，await_suspend 返回 false，不发生挂起。
    /// @note 不消费任何 AsyncFuture 结果；子任务结果仍由对应 Future 保存。
    /// @warning 依赖正确的 join_counter / PREEMPTED 协议和挂起交接。
    class [[nodiscard("JoinOperation must be co_awaited")]] JoinOperation final
        : public std::suspend_always, public Immovable<JoinOperation> {
        friend class Coro;

        template <typename>
        friend class CoroTask;
    public:
        /// @brief 释放父 Work 基准计数并决定是否等待子 Work。
        /// @param handle 当前 Coroutine handle。
        /// @return 没有在飞子 Work 时返回 false，否则返回 true 并等待调度恢复。
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;

        /// @brief 在父 Coroutine 恢复后重新占用基准 join slot。
        /// @note 当前实现不自动重抛任何子任务异常。
        void await_resume() const noexcept;

    private:
        explicit JoinOperation(Coro& coro) noexcept;

        Coro& m_coro;
    };

    /// @brief 派生不暴露结果 Future 的子 Work，并让当前 Worker 优先执行它。
    ///
    /// silent_fork(...) 创建子 Work，操作对象暂时拥有该 Work。
    /// co_await 时先为父 Work 增加 join slot，提交父 Work 后在当前 Worker 执行子 Work。
    /// 若操作对象未被 co_await，则析构负责销毁尚未提交的子 Work。
    ///
    /// @note 可使用 co_await coro.join() 等待这些子 Work 完成。
    /// @note SilentAsync 子任务的异常由框架的隐式/父异常锚点处理，不返回结果对象。
    /// @warning 当前父 Work 的提前重新调度仍依赖跨线程挂起交接安全。
    class [[nodiscard("SilentForkOperation must be co_awaited")]] SilentForkOperation final
        : public std::suspend_always, public Immovable<SilentForkOperation> {
        friend class Coro;

        template <typename>
        friend class CoroTask;

    public:
        /// @brief 提交子 Work，将父 Coroutine 挂起并直接执行子 Work。
        /// @param handle 当前父 Coroutine handle。
        /// @return true，父 Work 之后由 Executor 调度恢复。
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;

        /// @brief 若操作尚未提交，回收仍由本操作持有的子 Work。
        /// @note 已提交的 Work 由 Executor 的 SilentAsync 生命周期负责回收。
        ~SilentForkOperation() noexcept;

    private:
        /// @brief 构造 SilentFork 操作，并保存子 Work。
        explicit SilentForkOperation(Coro& coro, Work* child) noexcept;

        Coro& m_coro;
        Work* m_child{nullptr};
    };

    /// @brief 派生返回 AsyncFuture<R> 的子 Work，并支持动态前置依赖。
    ///
    /// fork(...) 构造子 Work 和结果槽，但推迟到 co_await 时才真正提交。
    /// await_suspend() 建立依赖边、执行强引用与父 Work 的 join slot；
    /// 没有未完成依赖时先发布父 Work，再由当前 Worker 直接执行子 Work；
    /// 若依赖尚未满足，则返回 false，父 Coroutine 继续运行，子 Work 等待依赖完成后调度。
    ///
    /// @tparam R 子 Work 的结果类型；Graph/Module 的结果类型为 void。
    /// @tparam Deps 已衰减保存的前置 AsyncFuture/AsyncTask 句柄类型包。
    /// @note await_resume() 返回 Future，但不会等待或消费子任务结果。
    /// @note 操作未被 co_await 时，Future 强引用使未提交 Work 仍遵守引用计数析构。
    /// @warning 子任务提交后，父 Work 及其 Topology 必须存活到子任务结束。
    /// @warning 动态依赖登记可能分配内存；现有调度协议尚无完整的提交失败回滚保证。
    template <typename R, async_future... Deps>
    class [[nodiscard("ForkOperation must be co_awaited")]] ForkOperation final
        : public std::suspend_always, public Immovable<ForkOperation<R, Deps...>> {
        friend class Coro;

        template <typename>
        friend class CoroTask;

    public:
        /// @brief 提交子 Work、建立前置依赖并决定父 Coroutine 是否挂起。
        /// @param handle 当前父 Coroutine handle。
        /// @return 无未完成依赖、需要直接执行子 Work 时返回 true；
        ///         存在未满足依赖时返回 false，由依赖完成路径负责调度子 Work。
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle);

        /// @brief 把子 Work 的 Future 所有权交给等待方。
        /// @return 共享子 Work 生命周期和结果槽的 AsyncFuture<R>；不等待结果。
        [[nodiscard]] AsyncFuture<R> await_resume() noexcept;

        /// @brief 释放未移动走的 Future 与依赖句柄。
        /// @note 已提交的子 Work 持有执行强引用，等待方销毁 Future 不会提前销毁执行中的任务。
        ~ForkOperation() noexcept = default;

    private:
        /// @brief 保存待提交子 Work 的 Future 和经衰减类型持有的前置依赖句柄。
        /// @tparam Args 依赖句柄的构造实参类型包。
        /// @param coro 所属 Coroutine 上下文。
        /// @param future 已持有子 Work 强引用的 Future。
        /// @param deps 前置依赖；操作对象在 co_await 之前保留引用计数。
        template <typename... Args>
            requires (sizeof...(Args) == sizeof...(Deps))
        explicit ForkOperation(Coro& coro, AsyncFuture<R>&& future, Args&&... deps);

        /// @brief 建立子 Work 的执行引用、动态依赖以及父 Work 的完成计数。
        /// @param deps 待验证并登记的前置异步任务句柄。
        /// @return 子 Work 已就绪且可由当前 Worker 立即执行时返回其指针；
        ///         尚有未满足前置依赖时返回 nullptr。
        /// @throws Exception 某个前置依赖尚未 start，或动态依赖登记分配失败。
        /// @warning Running 状态已发布后，依赖登记异常需要更完整的回滚机制。
        [[nodiscard]] Work* _launch_fork(const Deps&... deps);

        Coro& m_coro;
        AsyncFuture<R> m_future;
        std::tuple<Deps...> m_deps;
    };

public:
    // ============================================================================
    // SilentFork —— 不返回 Future 的派生任务
    // ============================================================================

    /// @brief 创建普通 callable 子任务，在 co_await 时提交并优先在当前 Worker 执行。
    /// @tparam T 满足 basic_invocable、capturable 的 callable 类型。
    /// @param task 将被子 Work 保存的 callable。
    /// @return 必须 co_await 的 SilentForkOperation，子 Work 仍计入当前父 Work。
    template <typename T>
        requires (basic_invocable<T> && capturable<T>)
    [[nodiscard]] SilentForkOperation silent_fork(T&& task);

    /// @brief 创建可接收 Runtime& 的子任务，执行时由框架注入 Runtime 上下文。
    /// @tparam T 满足 runtime_invocable、capturable 的 callable 类型。
    /// @param task Runtime callable，返回值不对外暴露。
    /// @return SilentForkOperation；Runtime 子任务及其动态子任务均由框架维护生命周期。
    template <typename T>
        requires (runtime_invocable<T> && capturable<T>)
    [[nodiscard]] SilentForkOperation silent_fork(T&& task);

    /// @brief 创建可接收 SubFlow& 的子任务，执行时注入局部 SubFlow。
    /// @tparam T 满足 subflow_invocable、capturable 的 callable 类型。
    /// @param task SubFlow callable。
    /// @return SilentForkOperation。
    /// @warning callable 不得保存框架注入的 SubFlow&。
    template <typename T>
        requires (subflow_invocable<T> && capturable<T>)
    [[nodiscard]] SilentForkOperation silent_fork(T&& task);

    /// @brief 创建 Coroutine callable 子任务，执行时注入其独立 Coro&。
    /// @tparam T 接收 Coro& 并返回 CoroTask<R> 的 callable 类型。
    /// @param task Coroutine callable，返回值不会生成 Future。
    /// @return SilentForkOperation；可通过父 Coroutine 的 join() 等待其完成。
    template <typename T>
        requires (coro_invocable<T> && capturable<T>)
    [[nodiscard]] SilentForkOperation silent_fork(T&& task);

    /// @brief 创建执行一次 Graph 的即发即弃 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam C 满足 callback 的完成回调类型，默认为 noop_callback。
    /// @param gh 要捕获或借用的子图持有者。
    /// @param cb 子图执行结束时调用的无参回调。
    /// @return SilentForkOperation；需要通过 co_await 提交。
    /// @warning 借用的 Graph 必须至少存活到子任务执行结束。
    template <graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    [[nodiscard]] SilentForkOperation silent_fork(Gh&& gh, C&& cb = C{});

    /// @brief 创建最多执行 num 次 Graph 的即发即弃 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam C 满足 callback 的完成回调类型。
    /// @param gh 要执行的子图持有者。
    /// @param num 执行次数；0 表示不运行子图。
    /// @param cb 整个 Module 完成后的回调。
    /// @return SilentForkOperation。
    template <graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    [[nodiscard]] SilentForkOperation silent_fork(Gh&& gh, std::uint64_t num, C&& cb = C{});

    /// @brief 创建由终止谓词控制重复执行的即发即弃 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam P 满足 predicate 的无参终止谓词；返回 true 时停止。
    /// @tparam C 满足 callback 的完成回调类型。
    /// @param gh 要执行的子图持有者。
    /// @param pred 在每轮之前调用的终止谓词。
    /// @param cb 子图整个执行链完成后的回调。
    /// @return SilentForkOperation。
    template <graph_holder Gh, predicate P, callback C = noop_callback>
        requires capturable<P, C>
    [[nodiscard]] SilentForkOperation silent_fork(Gh&& gh, P&& pred, C&& cb = C{});

    // ============================================================================
    // Fork —— 带结果通道和可选前置依赖的派生任务
    // ============================================================================

    /// @brief 创建普通 callable 异步子任务，返回值由 AsyncFuture 保存。
    /// @tparam T 满足 basic_invocable、capturable 的 callable 类型。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 类型包。
    /// @param task 普通 callable。
    /// @param deps 可选前置依赖，需已启动或已完成。
    /// @return 必须 co_await 的 ForkOperation；await_resume() 返回 AsyncFuture<basic_return_t<T>>。
    template <typename T, async_future... Deps>
        requires (basic_invocable<T> && capturable<T>)
    [[nodiscard]] auto fork(T&& task, Deps&&... deps) -> ForkOperation<basic_return_t<T>, std::decay_t<Deps>...>;

    /// @brief 创建 Runtime callable 异步子任务，并返回结果 Future。
    /// @tparam T 满足 runtime_invocable、capturable 的 callable 类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param task Runtime callable。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation；等待后得到 AsyncFuture<runtime_return_t<T>>。
    template <typename T, async_future... Deps>
        requires (runtime_invocable<T> && capturable<T>)
    [[nodiscard]] auto fork(T&& task, Deps&&... deps) -> ForkOperation<runtime_return_t<T>, std::decay_t<Deps>...>;

    /// @brief 创建 SubFlow callable 异步子任务，并返回结果 Future。
    /// @tparam T 满足 subflow_invocable、capturable 的 callable 类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param task SubFlow callable。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation；等待后得到 AsyncFuture<subflow_return_t<T>>。
    /// @warning callable 不得保存框架注入的 SubFlow&。
    template <typename T, async_future... Deps>
        requires (subflow_invocable<T> && capturable<T>)
    [[nodiscard]] auto fork(T&& task, Deps&&... deps) -> ForkOperation<subflow_return_t<T>, std::decay_t<Deps>...>;

    /// @brief 创建 Coroutine callable 异步子任务，并返回其 CoroTask 结果对应的 Future。
    /// @tparam T 满足 coro_invocable、capturable 的 callable 类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param task 接收 Coro&、返回 CoroTask<R> 的 callable。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation；等待后得到 AsyncFuture<coro_result_t<T>>。
    template <typename T, async_future... Deps>
        requires (coro_invocable<T> && capturable<T>)
    [[nodiscard]] auto fork(T&& task, Deps&&... deps) -> ForkOperation<coro_result_t<T>, std::decay_t<Deps>...>;

    /// @brief 创建执行一次 Graph 的异步 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param gh 要捕获或借用的子图。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation，co_await 后得到 AsyncFuture<void>。
    /// @warning 借用的 Graph 必须存活至 Module 执行结束。
    template <graph_holder Gh, async_future... Deps>
    [[nodiscard]] auto fork(Gh&& gh, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...>;

    /// @brief 创建执行一次 Graph、带完成回调的异步 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam C 满足 callback 的无参完成回调类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param gh 要执行的子图。
    /// @param cb Module 执行完毕时调用的回调。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation，co_await 后得到 AsyncFuture<void>。
    template <graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] auto fork(Gh&& gh, C&& cb, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...>;

    /// @brief 创建定次执行 Graph 的异步 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param gh 要执行的子图。
    /// @param num 循环次数；0 表示不执行子图。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation，co_await 后得到 AsyncFuture<void>。
    template <graph_holder Gh, async_future... Deps>
    [[nodiscard]] auto fork(Gh&& gh, std::uint64_t num, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...>;

    /// @brief 创建定次执行 Graph 且带完成回调的异步 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam C 满足 callback 的无参完成回调类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    /// @param cb 全部轮次结束后调用的回调。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation，co_await 后得到 AsyncFuture<void>。
    template <graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] auto fork(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...>;

    /// @brief 创建由终止谓词控制 Graph 重复执行的异步 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam P 满足 predicate 的无参终止谓词类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param gh 要执行的子图。
    /// @param pred 结束谓词；返回 true 停止，返回 false 继续下一轮。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation，co_await 后得到 AsyncFuture<void>。
    template <graph_holder Gh, predicate P, async_future... Deps>
        requires capturable<P>
    [[nodiscard]] auto fork(Gh&& gh, P&& pred, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...>;

    /// @brief 创建由终止谓词控制、带完成回调的异步 Module 子任务。
    /// @tparam Gh 满足 graph_holder 的图持有者类型。
    /// @tparam P 满足 predicate 的无参终止谓词类型。
    /// @tparam C 满足 callback 的无参完成回调类型。
    /// @tparam Deps 前置异步依赖类型包。
    /// @param gh 要执行的子图。
    /// @param pred 结束谓词；返回 true 停止。
    /// @param cb 全部轮次结束后调用的回调。
    /// @param deps 可选前置依赖。
    /// @return ForkOperation，co_await 后得到 AsyncFuture<void>。
    template <graph_holder Gh, predicate P, callback C, async_future... Deps>
        requires capturable<P, C>
    [[nodiscard]] auto fork(Gh&& gh, P&& pred, C&& cb, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...>;

    /// @brief 创建可通过 co_await 等待当前 Work 已派生子任务的操作。
    /// @return 关联当前 Coro 的 JoinOperation；不会消费任何 Future。
    [[nodiscard]] JoinOperation join() noexcept;

    /// @brief 尝试为当前已登记的外部挂起点取得唯一一次恢复调度权。
    ///
    /// 原子清除 SUSPENDED 位，执行 RUNNING|SUSPENDED -> RUNNING 的状态转换。
    /// 若原状态没有 SUSPENDED 位则不调度 Work，并返回 false。
    ///
    /// @return 本次调用是否唯一成功取得恢复调度权。
    /// @pre 外部事件仅能在 await_suspend 安全交接之后使 Coroutine 可恢复。
    /// @warning 当前实现不提供“恢复早于安全挂起交接”的抢先通知缓存。
    [[nodiscard]] bool resume();

private:
    /// @brief 获取内部原子状态，仅用于状态协议校验。
    [[nodiscard]] State::type state() const noexcept;

    /// @brief 首次启动根 Coroutine。
    ///
    /// NONE -> RUNNING
    ///
    /// 初始化恢复句柄和 Coroutine 运行状态。
    void _start(std::coroutine_handle<> handle) noexcept;

    /// @brief 登记当前 Coroutine 的外部挂起点。
    ///
    /// RUNNING -> RUNNING | SUSPENDED
    ///
    /// 必须由当前执行 Coroutine 的 Worker 调用。
    ///
    /// 先保存句柄，再发布挂起状态。
    void _suspend(std::coroutine_handle<> handle) noexcept;

    /// @brief 根 Coroutine 执行结束。
    ///
    /// RUNNING -> NONE
    ///
    /// @pre 根 Coroutine 已到达 final_suspend，且不存在未完成的恢复操作。
    void _finish() noexcept;

    /// @brief 重新调度当前 Coroutine Work（不生成新的 Work，也不更改 m_state）。
    /// @pre 当前 Work 仍持有执行期引用，且只有当前 Worker 执行该方法。
    /// @warning 在挂起交接完成前发布可能被其他 Worker 抢先执行。
    void _schedule() noexcept;

private:
    std::atomic<State::type> m_state{State::NONE};
    std::coroutine_handle<> m_resume{};
    Work* m_child{nullptr};
};

// ============================================================================
// Coro：类外成员函数定义
// ============================================================================
inline bool Coro::JoinOperation::await_suspend(std::coroutine_handle<> handle) noexcept {
    TFL_ASSERT(handle);
    TFL_ASSERT(!m_coro.m_resume);
    TFL_ASSERT(m_coro.state() == State::RUNNING);

    m_coro.m_resume = handle;
    auto& work = m_coro.work();
    if (work.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_coro.m_resume = nullptr;
        return false;
    }
    return true;
}

inline void Coro::JoinOperation::await_resume() const noexcept {
    m_coro.work().m_join_counter.fetch_add(1, std::memory_order_acq_rel);
}

inline Coro::JoinOperation::JoinOperation(Coro& coro) noexcept
    : m_coro{coro} {}

inline Coro::JoinOperation Coro::join() noexcept {
    return JoinOperation{*this};
}

inline bool Coro::resume() {
    const State::type previous = m_state.fetch_and(~State::SUSPENDED, std::memory_order_acq_rel);

    if (!(previous & State::SUSPENDED)) {
        return false;
    }

    executor()._dispatch(std::addressof(work()));
    return true;
}

inline Coro::State::type Coro::state() const noexcept {
    return m_state.load(std::memory_order_acquire);
}

inline void Coro::_start(std::coroutine_handle<> handle) noexcept {
    TFL_ASSERT(handle);
    TFL_ASSERT(!m_resume);
    TFL_ASSERT(state() == State::NONE);

    m_resume = handle;
    m_state.store(State::RUNNING, std::memory_order_release);
}

inline void Coro::_suspend(std::coroutine_handle<> handle) noexcept {
    TFL_ASSERT(handle);
    TFL_ASSERT(!m_resume);

    m_resume = handle;
    const State::type previous = m_state.fetch_or(State::SUSPENDED, std::memory_order_release);
    TFL_ASSERT(previous == State::RUNNING);
}

inline void Coro::_finish() noexcept {
    TFL_ASSERT(!m_resume);
    TFL_ASSERT(state() == State::RUNNING);

    m_state.store(State::NONE, std::memory_order_release);
}

inline void Coro::_schedule() noexcept {
    executor()._schedule(worker(), std::addressof(work()));
}

// ============================================================================
// Coro::SilentForkOperation
// ============================================================================

inline Coro::SilentForkOperation::SilentForkOperation(Coro& coro, Work* child) noexcept
    : m_coro{coro}
    , m_child{child} {}

inline Coro::SilentForkOperation::~SilentForkOperation() noexcept {
    if (m_child) {
        destroy_work(m_child);
    }
}

inline bool Coro::SilentForkOperation::await_suspend(std::coroutine_handle<> handle) noexcept {
    TFL_ASSERT(handle);
    TFL_ASSERT(m_child);
    TFL_ASSERT(!m_coro.m_resume);
    TFL_ASSERT(!m_coro.m_child);
    TFL_ASSERT(m_coro.state() == State::RUNNING);

    // 子 Work 占用父 Work 的一个 join slot。
    m_coro.work().m_join_counter.fetch_add(1, std::memory_order_relaxed);

    // 保存父 Coroutine 的恢复句柄。
    m_coro.m_resume = handle;

    // 保存当前 Worker 优先执行的子 Work，并转移所有权。
    m_coro.m_child = std::exchange(m_child, nullptr);
    return true;
}

// ============================================================================
// Coro::ForkOperation
// ============================================================================

template <typename R, async_future... Deps>
inline bool Coro::ForkOperation<R, Deps...>::await_suspend(std::coroutine_handle<> handle) {
    TFL_ASSERT(handle);
    TFL_ASSERT(!m_coro.m_resume);
    TFL_ASSERT(!m_coro.m_child);
    TFL_ASSERT(m_coro.state() == State::RUNNING);

    Work* ready = std::apply([this](const Deps&... deps) {
        return _launch_fork(deps...);
    }, m_deps);

    if (!ready) {
        return false;
    }

    // 保存父 Coroutine 的恢复句柄。
    m_coro.m_resume = handle;

    // 保存当前 Worker 优先执行的子 Work。
    m_coro.m_child = ready;
    return true;
}

template <typename R, async_future... Deps>
inline AsyncFuture<R> Coro::ForkOperation<R, Deps...>::await_resume() noexcept {
    return std::move(m_future);
}

template <typename R, async_future... Deps>
template <typename... Args>
    requires (sizeof...(Args) == sizeof...(Deps))
inline Coro::ForkOperation<R, Deps...>::ForkOperation(Coro& coro, AsyncFuture<R>&& future, Args&&... deps)
    : m_coro{coro}
    , m_future{std::move(future)}
    , m_deps{std::forward<Args>(deps)...} {}

template <typename R, async_future... Deps>
inline Work* Coro::ForkOperation<R, Deps...>::_launch_fork(const Deps&... deps) {
    static_assert(sizeof...(Deps) < std::numeric_limits<std::uint32_t>::max());

    Work* child = m_future.m_work;

    TFL_ASSERT(child);
    TFL_ASSERT(child->m_edges.empty() && child->m_num_successors == 0);

    constexpr std::size_t num_predecessors = sizeof...(Deps);

    if constexpr (num_predecessors != 0) {
        std::array<Work*, num_predecessors> predecessors{deps.m_work...};

        // 校验全部非空依赖均已启动。
        for (Work* predecessor : predecessors) {
            if (predecessor) {
                const auto current = predecessor->m_topology->m_control.load(std::memory_order_acquire);

                if (Topology::Control::status(current) == Topology::Control::Status::Idle) [[unlikely]] {
                    TFL_THROW(Exception{"Coro::fork: dependency has not been started."});
                }
            }
        }

        auto& edges = child->m_edges;
        edges.reserve(num_predecessors);

        // 保存前驱依赖，并建立强引用。
        for (Work* predecessor : predecessors) {
            if (predecessor) {
                edges.push_back(predecessor);
                predecessor->m_topology->_increment_ref();
            }
        }
    }

    auto& control = child->m_topology->m_control;
    const auto current = control.load(std::memory_order_relaxed);

    TFL_ASSERT(Topology::Control::status(current) == Topology::Control::Status::Idle);
    TFL_ASSERT(!Topology::Control::locked(current));

    control.store(Topology::Control::set_status(current, Topology::Control::Status::Running), std::memory_order_relaxed);

    // 建立子 Work 的执行生命周期引用。
    child->m_topology->_increment_ref();

    // 子 Work 占用父 Work 的一个 join slot。
    m_coro.work().m_join_counter.fetch_add(1, std::memory_order_relaxed);

    if constexpr (num_predecessors != 0) {
        auto& edges = child->m_edges;

        // 预置前驱数量及一个依赖提交保护计数。
        child->m_join_counter.store(edges.size() + 1, std::memory_order_relaxed);
        m_coro.executor()._link_predecessors(child, edges.begin(), edges.end());

        // 前驱尚未全部完成，由依赖完成路径调度子 Work。
        if (child->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return nullptr;
        }
    }

    return child;
}

// ============================================================================
// Coro::silent_fork
// ============================================================================

template <typename T>
    requires (basic_invocable<T> && capturable<T>)
inline Coro::SilentForkOperation Coro::silent_fork(T&& task) {
    auto& parent = work();

    Work* child = make_silent_async_basic(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return SilentForkOperation{*this, child};
}

template <typename T>
    requires (runtime_invocable<T> && capturable<T>)
inline Coro::SilentForkOperation Coro::silent_fork(T&& task) {
    auto& parent = work();

    Work* child = make_silent_async_runtime(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return SilentForkOperation{*this, child};
}

template <typename T>
    requires (subflow_invocable<T> && capturable<T>)
inline Coro::SilentForkOperation Coro::silent_fork(T&& task) {
    auto& parent = work();

    Work* child = make_silent_async_subflow(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return SilentForkOperation{*this, child};
}

template <typename T>
    requires (coro_invocable<T> && capturable<T>)
inline Coro::SilentForkOperation Coro::silent_fork(T&& task) {
    auto& parent = work();

    Work* child = make_silent_async_coro(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return SilentForkOperation{*this, child};
}

template <graph_holder Gh, callback C>
    requires capturable<C>
inline Coro::SilentForkOperation Coro::silent_fork(Gh&& gh, C&& cb) {
    return silent_fork(std::forward<Gh>(gh), std::uint64_t{1}, std::forward<C>(cb));
}

template <graph_holder Gh, callback C>
    requires capturable<C>
inline Coro::SilentForkOperation Coro::silent_fork(Gh&& gh, std::uint64_t num, C&& cb) {
    return silent_fork(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(cb));
}

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
inline Coro::SilentForkOperation Coro::silent_fork(Gh&& gh, P&& pred, C&& cb) {
    auto& parent = work();

    Work* child = make_silent_async_module(std::addressof(parent), executor(), parent.m_topology, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(cb));
    return SilentForkOperation{*this, child};
}

// ============================================================================
// Coro::fork
// ============================================================================

template <typename T, async_future... Deps>
    requires (basic_invocable<T> && capturable<T>)
inline auto Coro::fork(T&& task, Deps&&... deps) -> ForkOperation<basic_return_t<T>, std::decay_t<Deps>...> {
    using R = basic_return_t<T>;
    auto& parent = work();

    auto [child, result] = make_async_basic(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return ForkOperation<R, std::decay_t<Deps>...>{*this, AsyncFuture<R>{child, result}, std::forward<Deps>(deps)...};
}

template <typename T, async_future... Deps>
    requires (runtime_invocable<T> && capturable<T>)
inline auto Coro::fork(T&& task, Deps&&... deps) -> ForkOperation<runtime_return_t<T>, std::decay_t<Deps>...> {
    using R = runtime_return_t<T>;
    auto& parent = work();

    auto [child, result] = make_async_runtime(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return ForkOperation<R, std::decay_t<Deps>...>{*this, AsyncFuture<R>{child, result}, std::forward<Deps>(deps)...};
}

template <typename T, async_future... Deps>
    requires (subflow_invocable<T> && capturable<T>)
inline auto Coro::fork(T&& task, Deps&&... deps) -> ForkOperation<subflow_return_t<T>, std::decay_t<Deps>...> {
    using R = subflow_return_t<T>;
    auto& parent = work();

    auto [child, result] = make_async_subflow(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return ForkOperation<R, std::decay_t<Deps>...>{*this, AsyncFuture<R>{child, result}, std::forward<Deps>(deps)...};
}

template <typename T, async_future... Deps>
    requires (coro_invocable<T> && capturable<T>)
inline auto Coro::fork(T&& task, Deps&&... deps) -> ForkOperation<coro_result_t<T>, std::decay_t<Deps>...> {
    using R = coro_result_t<T>;
    auto& parent = work();

    auto [child, result] = make_async_coro(std::addressof(parent), executor(), parent.m_topology, std::forward<T>(task));
    return ForkOperation<R, std::decay_t<Deps>...>{*this, AsyncFuture<R>{child, result}, std::forward<Deps>(deps)...};
}

template <graph_holder Gh, async_future... Deps>
inline auto Coro::fork(Gh&& gh, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...> {
    return fork(std::forward<Gh>(gh), std::uint64_t{1}, noop_callback{}, std::forward<Deps>(deps)...);
}

template <graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline auto Coro::fork(Gh&& gh, C&& cb, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...> {
    return fork(std::forward<Gh>(gh), std::uint64_t{1}, std::forward<C>(cb), std::forward<Deps>(deps)...);
}

template <graph_holder Gh, async_future... Deps>
inline auto Coro::fork(Gh&& gh, std::uint64_t num, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...> {
    return fork(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, noop_callback{}, std::forward<Deps>(deps)...);
}

template <graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline auto Coro::fork(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...> {
    return fork(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(cb), std::forward<Deps>(deps)...);
}

template <graph_holder Gh, predicate P, async_future... Deps>
    requires capturable<P>
inline auto Coro::fork(Gh&& gh, P&& pred, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...> {
    return fork(std::forward<Gh>(gh), std::forward<P>(pred), noop_callback{}, std::forward<Deps>(deps)...);
}

template <graph_holder Gh, predicate P, callback C, async_future... Deps>
    requires capturable<P, C>
inline auto Coro::fork(Gh&& gh, P&& pred, C&& cb, Deps&&... deps) -> ForkOperation<void, std::decay_t<Deps>...> {
    auto& parent = work();

    auto [child, result] = make_async_module(std::addressof(parent), executor(), parent.m_topology, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(cb));
    return ForkOperation<void, std::decay_t<Deps>...>{*this, AsyncFuture<void>{child, result}, std::forward<Deps>(deps)...};
}

} // namespace tfl
