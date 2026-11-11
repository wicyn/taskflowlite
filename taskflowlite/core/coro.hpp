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
#include <exception>
#include <functional>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "stack.hpp"
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
/// join() 等待当前 Work 已派生的子 Work；
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
/// 每次 resume() 前必须绑定本次 Invoker 的 cache 和局部返回标志。
/// fork()、join() 和外部等待都在 await_suspend() 内交接；交接后旧 Invoker
/// 只读自己的局部标志并退出，不再访问 Coro 或 Coroutine frame。
/// @warning 每轮执行首次绑定 Work 与 Executor；跨 Worker 恢复只更新 Worker。
/// @warning Frame 必须按 LIFO 销毁；Coro 必须比所有属于它的 Frame 存活更久。
/// @warning Frame 的创建和销毁必须在当前绑定的 Worker 上进行。
/// @warning 只有在当前 Worker 独占执行该 Coro 时，才能访问 Worker 的栈段缓存。
class Coro final : public Context {
    template <typename>
    friend class CoroAwaiter;

    template <typename>
    friend class CoroTask;

    TFL_WORK_SUBCLASS_FRIENDS;

    /// @brief Coroutine 的原子运行状态与恢复权标志。
    ///
    /// 外部等待使用现有两个高位标志完成通知与 Awaiter 的交接：
    ///
    ///   RUNNING               正常执行，或已经取得恢复权。
    ///   SUSPENDED             已登记等待，用户操作尚未完成挂起注册。
    ///   NONE                  通知提前到达，由当前 Awaiter 直接继续。
    ///   RUNNING | SUSPENDED   Awaiter 已交接，等待通知调度。
    ///
    /// NONE 也用于未启动或已经完成；这些阶段不存在有效的外部恢复请求。
    /// join() 不修改这些状态位，只通过 join_counter 完成交接。
    struct State {
        using type = std::uint32_t;

        static constexpr unsigned BITS = std::numeric_limits<type>::digits;
        static constexpr type NONE = 0;

        /// @brief 正常执行；与 SUSPENDED 组合时表示 Awaiter 已完成交接。
        static constexpr type RUNNING = type{1} << (BITS - 1);

        /// @brief Coroutine 已登记挂起点，等待外部恢复请求。
        static constexpr type SUSPENDED = type{1} << (BITS - 2);

        /// @brief 所有 Coroutine 运行状态标志。
        static constexpr type FLAG_MASK = RUNNING | SUSPENDED;
    };

    static_assert(std::atomic<State::type>::is_always_lock_free);

public:
    /// @brief 销毁 Coroutine 上下文并释放剩余空根栈段；所有 Frame 必须已先完成 LIFO 回收。
    /// @note 正常执行收尾已归还根栈段；析构不访问 Worker。
    ~Coro() noexcept {
        Stacklet::destroy(m_stacklet);
    }

    /// @brief 协作等待当前 Coroutine Work 派生的全部子 Work 完成。
    ///
    /// 每次 join() 都归还一次基准 join slot，并由 await_resume() 无条件加回。
    /// await_suspend() 先置位本次 Invoker 的返回标志，再释放 slot。
    /// 当前 Worker 最后归还时直接继续，否则等待子 Work 调度。
    ///
    /// @pre Invoker 已绑定本次 resume() 的局部返回标志，并在 resume() 返回后首先检查它。
    /// @note 没有未完成子 Work 时，await_suspend 返回 false，直接继续。
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
        /// @return 当前 Worker 在此归还最后一个 slot 时返回 false，否则返回 true。
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;

        /// @brief 在父 Coroutine 恢复后重新占用基准 join slot。
        /// @note 当前实现不自动重抛任何子任务异常。
        void await_resume() const noexcept;

    private:
        explicit JoinOperation(Coro& coro) noexcept;

        Coro& m_coro;
    };

    /// @brief 延迟创建子 Coroutine，通过对称转移执行并回收其 Frame。
    /// @note func 与 args 必须与协程函数形参匹配，不自动注入 Coro 或 self。
    /// @note 支持顺序多次 co_await，但不允许同一个 Operation 被并发等待。
    template <typename F, typename... Args>
    class [[nodiscard("CallOperation must be co_awaited")]] CallOperation final
        : public std::suspend_always, public Immovable<CallOperation<F, Args...>> {
        friend class Coro;

        template <typename>
        friend class CoroTask;

        static_assert(std::invocable<F, Args...>,
                      "Coro::call arguments do not match the coroutine function");

        using task_type = std::invoke_result_t<F, Args...>;
        static_assert(coro_task<task_type> && std::same_as<task_type, std::remove_cvref_t<task_type>>,
                      "Coro::call requires a non-const CoroTask returned by value");

        using R = typename task_type::result_type;
        using handle_type = typename task_type::handle_type;

        /// @brief 确保结果提取完成或发生异常后销毁子 Coroutine Frame。
        struct FrameGuard {
            handle_type handle;

            FrameGuard(const FrameGuard&) = delete;
            FrameGuard& operator=(const FrameGuard&) = delete;

            explicit FrameGuard(handle_type handle) noexcept;
            ~FrameGuard() noexcept;
        };

    public:
        /// @brief 回收仍由当前操作持有的子 Coroutine Frame。
        ~CallOperation() noexcept;

        /// @brief 创建子 Coroutine，设置 continuation 并执行对称转移。
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent);

        /// @brief 获取子 Coroutine 结果并传播异常，随后立即回收 Frame。
        R await_resume();

    private:
        /// @brief 保存 callable 和完整参数，延迟到 await_suspend 时创建子 Coroutine。
        explicit CallOperation(Coro& coro, F&& func, Args&&... args);

        Coro& m_coro;
        F m_func;
        std::tuple<Args...> m_args;
        handle_type m_handle{};
    };

    /// @brief 派生不暴露结果 Future 的子 Work，并让当前 Worker 优先执行它。
    ///
    /// silent_fork(...) 创建子 Work，操作对象暂时拥有该 Work。
    /// co_await 时先为父 Work 增加 join slot，提交父 Work 后在当前 Worker 执行子 Work。
    /// 若操作对象未被 co_await，则析构负责销毁尚未提交的子 Work。
    ///
    /// @note 可使用 co_await coro.join() 等待这些子 Work 完成。
    /// @note SilentAsync 子任务的异常由框架的隐式/父异常锚点处理，不返回结果对象。
    /// @note 子 Work 直接写入当前 Invoker 的 cache；先置位返回标志，再发布父 Work。
    class [[nodiscard("SilentForkOperation must be co_awaited")]] SilentForkOperation final
        : public std::suspend_always, public Immovable<SilentForkOperation> {
        friend class Coro;

        template <typename>
        friend class CoroTask;

    public:
        /// @brief 提交子 Work，将父 Coroutine 挂起并直接执行子 Work。
        /// @param handle 当前父 Coroutine handle。
        /// @return true，父 Work 之后由 Executor 调度恢复。
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle);

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
    /// @note 动态依赖登记分配失败时撤销部分建边和预占计数，操作对象保留未提交的 Future 以便重试。
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
        /// @throws Exception 某个前置依赖尚未 start。
        /// @throws std::bad_alloc 动态依赖登记分配失败。
        /// @note 登记失败时撤销部分依赖、归还父 slot 和执行引用，并恢复 Idle 以便重试。
        [[nodiscard]] Work* _launch_fork(const Deps&... deps);

        Coro& m_coro;
        AsyncFuture<R> m_future;
        std::tuple<Deps...> m_deps;
    };

public:
    /// @brief 延迟调用协程函数，要求显式传入至少一个非 const Coro&。
    template <typename F, typename... Args>
        requires ((std::same_as<Args, Coro&> || ...) && std::invocable<F, Args...>)
    [[nodiscard]] auto call(F&& func, Args&&... args) -> CallOperation<F, Args...>;

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

    /// @brief 尝试为当前已登记的外部挂起点提交一次恢复通知。
    ///
    /// 原子清除 SUSPENDED 位；Awaiter 已交接时调度 Work，否则只记录提前通知，
    /// 由当前 Awaiter 在交接处直接继续。没有 SUSPENDED 位时返回 false。
    ///
    /// @return 本次通知是否被接受。
    /// @pre Token 属于当前等待，且调用期间 Coro 仍然存活。
    [[nodiscard]] bool resume() noexcept;

private:
    /// @brief 获取内部原子状态，仅用于状态协议校验。
    [[nodiscard]] State::type state() const noexcept;

    /// @brief 首次启动根 Coroutine。
    ///
    /// NONE -> RUNNING
    ///
    /// 初始化恢复句柄和 Coroutine 运行状态。
    void _start(std::coroutine_handle<> handle) noexcept;

    /// @brief 准备外部挂起，登记恢复句柄并开始接收完成通知。
    ///
    /// RUNNING -> SUSPENDED
    ///
    /// 必须由当前执行 Coroutine 的 Worker 调用。
    ///
    /// 必须早于用户异步操作的启动；此时只记录通知，由 _commit_suspend() 完成交接。
    void _prepare_suspend(std::coroutine_handle<> handle) noexcept;

    /// @brief 撤销返回 false 或抛异常的外部等待登记。
    /// @pre 用户操作没有提交恢复通知，也没有留下有效回调。
    void _cancel_suspend() noexcept;

    /// @brief 在用户操作返回 true 后交接外部等待，并设置本次 Invoker 的返回标志。
    /// @pre m_resume 有效，cache 为空，用户操作已完成挂起注册。
    /// @return true：立即返回且不再访问当前节点；false：提前通知已到达，直接继续 co_await。
    [[nodiscard]] bool _commit_suspend() noexcept;

    /// @brief 结束本轮执行，在当前 Worker 归还根栈段，并重置 Coroutine 状态。
    ///
    /// RUNNING -> NONE；根 Coroutine 创建失败时保持 NONE。
    ///
    /// @pre 根 Coroutine Frame 已销毁或创建失败，不存在活动 Frame 或未完成的恢复操作。
    /// @note 由 Stack::release() 根据栈段大小决定缓存或释放。
    void _reset() noexcept;

    /// @brief 重新调度当前 Coroutine Work（不生成新的 Work，也不更改 m_state）。
    /// @pre 当前 Work 仍持有执行期引用，且只有当前 Worker 执行该方法。
    /// @pre 已保存恢复句柄、移交子 Work，并置位本次 Invoker 的返回标志。
    /// @warning 发布后不得再访问当前 Coro 或 awaiter。
    void _schedule() noexcept;

    /// @brief 优先从当前 Stacklet 分配 Frame；空间不足时向当前 Worker 取得独立栈段。
    /// @pre 已绑定有效 Worker，且由该 Worker 独占执行当前 Coro。
    [[nodiscard]] void* _allocate_frame(std::size_t size);

    /// @brief 按 LIFO 回退 Frame；扩展段退空时归还 Worker，根段由 _reset() 归还。
    /// @pre 已绑定有效 Worker，且由该 Worker 独占执行当前 Coro。
    void _deallocate_frame(void* pointer) noexcept;

private:
    Stacklet* m_stacklet{nullptr}; ///< 当前 Coro 的活动栈顶；通过 m_prev 连接更早的栈段。
    std::atomic<State::type> m_state{State::NONE};
    std::coroutine_handle<> m_resume{};
    Work** m_cache{nullptr}; ///< 指向本次 Invoker 的 cache；每次 resume() 前重新绑定，退出时无需清空。
    bool* m_return_flag{nullptr}; ///< 每次 resume() 前重新绑定；仅在该次执行期间解引用，退出时无需清空。
};

// ============================================================================
// Coro：类外成员函数定义
// ============================================================================
// ============================================================================
// Coro::CallOperation
// ============================================================================

template <typename F, typename... Args>
inline Coro::CallOperation<F, Args...>::FrameGuard::FrameGuard(handle_type handle) noexcept
    : handle{handle} {}

template <typename F, typename... Args>
inline Coro::CallOperation<F, Args...>::FrameGuard::~FrameGuard() noexcept {
    if (handle) {
        handle.destroy();
    }
}

template <typename F, typename... Args>
inline Coro::CallOperation<F, Args...>::~CallOperation() noexcept {
    if (m_handle) {
        m_handle.destroy();
    }
}

template <typename F, typename... Args>
inline std::coroutine_handle<> Coro::CallOperation<F, Args...>::await_suspend(std::coroutine_handle<> parent) {
    TFL_ASSERT(parent);
    TFL_ASSERT(!m_handle);

    auto task = std::apply([this](auto&... args) {
        return std::invoke(std::forward<F>(m_func), std::forward<Args>(args)...);
    }, m_args);

    TFL_ASSERT(task.m_handle);

    if (std::addressof(task.m_handle.promise().m_coro) != std::addressof(m_coro)) [[unlikely]] {
        TFL_THROW(Exception{"Coro::call: child Coroutine belongs to another Coro."});
    }

    m_handle = std::exchange(task.m_handle, handle_type{});
    m_handle.promise()._set_continuation(parent);

    return m_handle;
}

template <typename F, typename... Args>
inline auto Coro::CallOperation<F, Args...>::await_resume() -> R {
    TFL_ASSERT(m_handle && m_handle.done());

    FrameGuard guard{std::exchange(m_handle, handle_type{})};
    auto& promise = guard.handle.promise();

    if (auto eptr = promise._take_exception()) [[unlikely]] {
        std::rethrow_exception(eptr);
    }

    return std::move(promise).take();
}

template <typename F, typename... Args>
inline Coro::CallOperation<F, Args...>::CallOperation(Coro& coro, F&& func, Args&&... args)
    : m_coro{coro}
    , m_func{std::forward<F>(func)}
    , m_args{std::forward<Args>(args)...} {}

template <typename F, typename... Args>
    requires ((std::same_as<Args, Coro&> || ...) && std::invocable<F, Args...>)
inline auto Coro::call(F&& func, Args&&... args) -> CallOperation<F, Args...> {
    return CallOperation<F, Args...>{*this, std::forward<F>(func), std::forward<Args>(args)...};
}

inline bool Coro::JoinOperation::await_suspend(std::coroutine_handle<> handle) noexcept {
    TFL_ASSERT(handle);
    TFL_ASSERT(!m_coro.m_resume);
    TFL_ASSERT(m_coro.state() == State::RUNNING);

    auto& counter = m_coro.work().m_join_counter;
    m_coro.m_resume = handle;
    bool* const return_flag = m_coro.m_return_flag;
    TFL_ASSERT(return_flag);

    *return_flag = true;
    if (counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        // 子 Work 随时可能恢复并销毁当前节点，此后不访问 Coro 或 awaiter。
        return true;
    }

    // 当前 Worker 最后归还，仍独占执行权，直接继续本次 co_await。
    m_coro.m_resume = {};
    *return_flag = false;
    return false;
}

inline void Coro::JoinOperation::await_resume() const noexcept {
    m_coro.work().m_join_counter.fetch_add(1, std::memory_order_acq_rel);
}

inline Coro::JoinOperation::JoinOperation(Coro& coro) noexcept
    : m_coro{coro} {}

inline Coro::JoinOperation Coro::join() noexcept {
    return JoinOperation{*this};
}

inline bool Coro::resume() noexcept {
    const State::type previous = m_state.fetch_and(~State::SUSPENDED, std::memory_order_acq_rel);

    if (!(previous & State::SUSPENDED)) {
        return false;
    }

    if (previous & State::RUNNING) {
        // 只有 Awaiter 已交接才能访问执行上下文并调度。
        executor()._dispatch(std::addressof(work()));
    }
    // 提前通知路径在原子操作后不再访问成员，Awaiter 可能已经继续并销毁当前 Coro。
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

inline void Coro::_prepare_suspend(std::coroutine_handle<> handle) noexcept {
    TFL_ASSERT(handle);
    TFL_ASSERT(!m_resume);
    TFL_ASSERT(m_cache && !*m_cache);
    TFL_ASSERT(state() == State::RUNNING);

    m_resume = handle;
    m_state.store(State::SUSPENDED, std::memory_order_release);
}

inline void Coro::_cancel_suspend() noexcept {
    TFL_ASSERT(m_resume);
    TFL_ASSERT(state() == State::SUSPENDED);

    m_resume = {};
    m_state.store(State::RUNNING, std::memory_order_release);
}

inline bool Coro::_commit_suspend() noexcept {
    TFL_ASSERT(m_resume && m_cache && !*m_cache);
    bool* const return_flag = m_return_flag;
    TFL_ASSERT(return_flag);

    *return_flag = true;
    if ((m_state.fetch_or(State::RUNNING, std::memory_order_acq_rel) & State::SUSPENDED) != 0) {
        // 已允许通知调度；另一个 Worker 随时可能恢复并销毁当前节点。
        return true;
    }

    // 提前通知已到达：当前 Worker 仍独占执行权，无需再次调度。
    m_resume = {};
    *return_flag = false;
    return false;
}

inline void Coro::_reset() noexcept {
    TFL_ASSERT(!m_resume);
    TFL_ASSERT(state() == State::RUNNING || state() == State::NONE);
    TFL_ASSERT(!m_stacklet || (m_stacklet->empty() && !m_stacklet->previous()));

    // 在当前执行线程归还根栈段，由 Stack 统一决定缓存或释放。
    if (m_stacklet) {
        worker().m_stack.release(std::exchange(m_stacklet, nullptr));
    }

    m_state.store(State::NONE, std::memory_order_release);
}

inline void Coro::_schedule() noexcept {
    executor()._schedule(worker(), std::addressof(work()));
}

inline void* Coro::_allocate_frame(std::size_t size) {
    TFL_ASSERT(m_worker && size > 0);

    if (m_stacklet) {
        if (void* memory = m_stacklet->try_allocate(size)) [[likely]] {
            return memory;
        }
    }

    // 先取得新段；申请失败时不修改原有活动链。
    Stacklet* next = worker().m_stack.acquire(size);
    m_stacklet = m_stacklet ? m_stacklet->push(next) : next;

    return m_stacklet->try_allocate(size);
}

inline void Coro::_deallocate_frame(void* pointer) noexcept {
    TFL_ASSERT(m_worker && pointer && m_stacklet);

    Stacklet* current = m_stacklet;
    current->deallocate(pointer);

    if (!current->empty() || !current->previous()) [[likely]] {
        return;
    }

    m_stacklet = current->pop();
    worker().m_stack.release(current);
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
inline bool Coro::SilentForkOperation::await_suspend(std::coroutine_handle<> handle) {
    TFL_ASSERT(handle);

    if (!m_child) [[unlikely]] {
        TFL_THROW(Exception{"Coro::silent_fork: operation already awaited."});
    }

    TFL_ASSERT(!m_coro.m_resume);
    TFL_ASSERT(m_coro.m_cache && !*m_coro.m_cache);
    TFL_ASSERT(m_coro.m_return_flag);
    TFL_ASSERT(m_coro.state() == State::RUNNING);

    // 子 Work 占用父 Work 的一个 join slot。
    m_coro.work().m_join_counter.fetch_add(1, std::memory_order_relaxed);

    // 保存父 Coroutine 的恢复句柄。
    m_coro.m_resume = handle;

    // 当前 Worker 接手子 Work；先置位返回标志，发布父 Work 后不再访问 Coro 或 awaiter。
    *m_coro.m_cache = std::exchange(m_child, nullptr);
    *m_coro.m_return_flag = true;
    m_coro._schedule();
    return true;
}

// ============================================================================
// Coro::ForkOperation
// ============================================================================
template <typename R, async_future... Deps>
inline bool Coro::ForkOperation<R, Deps...>::await_suspend(std::coroutine_handle<> handle) {
    TFL_ASSERT(handle);

    if (!m_future.valid()) [[unlikely]] {
        TFL_THROW(Exception{"Coro::fork: operation already awaited."});
    }

    TFL_ASSERT(!m_coro.m_resume);
    TFL_ASSERT(m_coro.m_cache && !*m_coro.m_cache);
    TFL_ASSERT(m_coro.m_return_flag);
    TFL_ASSERT(m_coro.state() == State::RUNNING);

    Work* ready = std::apply([this](const Deps&... deps) {
        return _launch_fork(deps...);
    }, m_deps);

    if (!ready) {
        return false;
    }

    // 保存父 Coroutine 的恢复句柄。
    m_coro.m_resume = handle;

    // 当前 Worker 接手已就绪的子 Work；发布父 Work 前完成全部交接。
    *m_coro.m_cache = ready;
    *m_coro.m_return_flag = true;
    m_coro._schedule();
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

    constexpr std::size_t num_dependencies = sizeof...(Deps);
    std::array<Work*, num_dependencies> predecessors{deps.m_work...};
    std::size_t num_predecessors = 0;

    if constexpr (num_dependencies != 0) {
        // 校验全部非空依赖均已启动。
        for (Work* predecessor : predecessors) {
            if (predecessor) {
                const auto current = predecessor->m_topology->m_control.load(std::memory_order_acquire);

                if (Topology::Control::status(current) == Topology::Control::Status::Idle) [[unlikely]] {
                    TFL_THROW(Exception{"Coro::fork: dependency has not been started."});
                }

                predecessors[num_predecessors++] = predecessor;
            }
        }
    }

    auto& control = child->m_topology->m_control;
    const auto current = control.load(std::memory_order_relaxed);

    TFL_ASSERT(Topology::Control::status(current) == Topology::Control::Status::Idle);
    TFL_ASSERT(!Topology::Control::locked(current));

    // 建立子 Work 的执行生命周期引用。
    control.store(Topology::Control::set_status(current, Topology::Control::Status::Running), std::memory_order_relaxed);
    child->m_topology->_increment_ref();

    // 子 Work 占用父 Work 的一个 join slot。
    m_coro.work().m_join_counter.fetch_add(1, std::memory_order_relaxed);

    if constexpr (num_dependencies != 0) {
        child->m_join_counter.store(static_cast<std::uint32_t>(num_predecessors + 1), std::memory_order_relaxed);
        TFL_TRY {
            child->_register_async_dependencies(predecessors.data(), predecessors.data() + num_predecessors);
        } TFL_CATCH_ALL {
            // 部分依赖已撤销，归还父 Coroutine 的子任务计数和执行引用。
            control.fetch_and(~Topology::Control::STATE_MASK, std::memory_order_relaxed);
            m_coro.work().m_join_counter.fetch_sub(1, std::memory_order_acq_rel);
            [[maybe_unused]] const bool last = child->m_topology->_decrement_ref();
            TFL_ASSERT(!last); // ForkOperation 的 Future 仍持有一份引用。
            TFL_RETHROW();
        }

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
