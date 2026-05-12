/// @file async_task.hpp
/// @brief 提供异步任务句柄 AsyncTask 及其延迟启动派生 DeferredAsyncTask。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <sstream>

#include "work_factory_fwd.hpp"
#include "work.hpp"
#include "executor.hpp"
#include "semaphore.hpp"
#include "topology.hpp"

namespace tfl {

// ============================================================
// AsyncTask - 即发即用的轻量级句柄
// ============================================================

/// @brief 异步任务的引用计数式值类型句柄。
///
/// @details
/// `AsyncTask` 是用户与"运行期动态生成的 Work"打交道的唯一安全句柄。
/// 框架的所有动态调度 API
/// （`Executor::dependent_async` / `Executor::async` / `Runtime::dependent_async` / `Runtime::async` 等）
/// 都返回它。其本质是一个 **手动管理引用计数的 Work\* 包装**，引用计数
/// 寄存在所属 `Topology::m_use_count` 上 —— 计数归零即释放节点内存。
///
/// ============================================================================
///  为什么要单独一种句柄？—— 与 `Task` 的本质差异
/// ============================================================================
///
/// | 维度        | `Task`（弱引用）             | `AsyncTask`（强引用）              |
/// |-------------|------------------------------|------------------------------------|
/// | 节点拥有者  | `Flow::m_graph`              | `Topology` 引用计数                |
/// | 适用图风格  | **静态图** —— 用户编辑期持有  | **动态任务** —— 调度期才存在        |
/// | 拷贝代价    | 拷指针                        | 一次原子 fetch_add                 |
/// | 析构代价    | 0                             | 一次 fetch_sub，归零则销毁节点     |
/// | 失效时机    | Flow 析构即悬挂               | 引用计数归零才释放                 |
/// | 等待 / 取异常 | 不支持（节点无独立生命周期）   | `wait()` / `get()` / `stop()`      |
///
/// 这一切的根本原因：**动态任务没有 Flow 兜底生命周期** —— 任务被异步派发出去后，
/// 用户可能在它跑完前已经退出了创建作用域，必须有句柄持有引用计数维持其内存可达。
///
/// ============================================================================
///  Topology 引用计数 —— 内存释放的精确时机
/// ============================================================================
/// 每个 `AsyncTask` 拷贝 / 构造时 `_incref()`，析构 / 重置时 `_decref()`。
/// 计数归零的瞬间在 `_decref()` 内同步调用 `destroy(m_work)`，把节点归还
/// 内存池 —— 这意味着：
///
/// - 节点的析构时机由 **最后一份句柄释放** 决定，与任务是否完成无关；
/// - 任务执行完毕但用户仍持有 AsyncTask 时，节点保留在内存池里，直到句柄被释放；
/// - `done()` 返回 true 之后 `wait()` 仍是合法的（瞬时返回）。
///
/// 这种"句柄式生命周期"是用户态等待 / 取异常 API（`wait`/`get`）能成立的前提。
///
/// ============================================================================
///  状态查询 vs 控制
/// ============================================================================
/// 查询接口（const）：`running()` / `done()` / `has_exception()` / `name()`
///   均查 `Topology` 状态机或 `Work` 的常量字段，**线程安全**（原子读）。
///
/// 控制接口：
/// - `stop()`   —— 设置 topology 停止标志位（**软中断**：节点 invoke 前会自检）；
/// - `wait()`   —— 阻塞直至 topology 进入 Finished 状态，**不重抛异常**；
/// - `get()`    —— `wait() + _rethrow_exception()`，把节点截留的首个异常重抛。
///
/// `wait()` 与 `get()` 的差异
/// 想拿异常一定要走 get / coget，纯 wait 会静默吞掉。
///
/// ============================================================================
///  值语义与对比 / 哈希
/// ============================================================================
/// 拷贝 = 引用计数 +1（多个句柄指向同一节点）；
/// 移动 = 转移指针（源置 null，无原子操作，最快路径）；
/// 等值 = 指向同一 `Work*`；哈希 = `Work*` 地址哈希 →
///   可作 `unordered_map` 键，但要注意：键存活期间引用计数不会归零，节点不释放。
///
/// ============================================================================
///  与 `DeferredAsyncTask` 的派生关系
/// ============================================================================
/// `DeferredAsyncTask` 是 `AsyncTask` 的派生，**唯一区别** 是它处于"启动前"
/// 的 Idle 状态，因此可以再做配置（`name` setter、信号量、观察者、依赖收集），
/// 然后调用 `start(deps...)` 转入 Running。`start()` 之后再用 `AsyncTask` 切片
/// 视角操作，体现"配置阶段可写、启动后冻结"的设计意图。
///
/// 二进制布局通过 `static_assert` 强制保持一致 ——
/// 这是 `start(Deps&&...)` 内部把 `DeferredAsyncTask` 与 `AsyncTask` 混拌
/// 收集到 `std::array<AsyncTask>` 里时的 **slicing 安全前提**。一旦布局漂移
/// （例如派生类不慎加了字段），切片传递会破坏值语义，编译期就会被守卫拦下。
///
/// ============================================================================
///  使用示例
/// ============================================================================
/// @code
///   // 形态 A：dependent_async 直接拿到引用计数句柄
///   auto t1 = executor.dependent_async([]{ load(); });
///   auto t2 = executor.dependent_async([]{ process(); }, t1);
///   t2.get();                         // 等待 + 拿异常
///
///   // 形态 B：deferred_async 拿到可配置版，配置后启动
///   auto d = executor.deferred_async([]{ work(); });
///   d.name("worker").acquire(sem).register_observer<Tracer>();
///   d.start(prev_a, prev_b);          // 启动并声明依赖
///   d.wait();
///
///   // 形态 C：放进容器（注意句柄存活时节点不释放）
///   std::vector<AsyncTask> all;
///   for (auto& job : jobs) all.push_back(executor.dependent_async(job));
///   for (auto& t : all) t.get();
/// @endcode
///
/// @see Task              静态图侧的弱引用对偶
/// @see DeferredAsyncTask 启动前可写版本（本文件下方）
/// @see Topology          引用计数与状态机寄宿处
/// @see Executor::dependent_async  / Runtime::async  生产源
class AsyncTask {
    friend class Work;
    friend class Graph;
    friend class Flow;
    friend class Runtime;
    friend class Executor;
    friend class Branch;


public:
    /// @brief 构造空句柄，不绑定任何异步任务。
    AsyncTask() noexcept = default;

    /// @brief 析构句柄，并释放当前持有的任务引用。
    ~AsyncTask();

    /// @brief 构造空句柄，不绑定任何异步任务。
    explicit AsyncTask(std::nullptr_t) noexcept;

    /// @brief 拷贝构造，引用同一个异步任务并增加引用计数。
    AsyncTask(const AsyncTask& rhs) noexcept;

    /// @brief 拷贝赋值，释放当前引用后引用 rhs 指向的异步任务。
    AsyncTask& operator=(const AsyncTask& rhs) noexcept;

    /// @brief 移动构造，接管 rhs 的任务引用，并将 rhs 置空。
    AsyncTask(AsyncTask&& rhs) noexcept;

    /// @brief 移动赋值，释放当前引用后接管 rhs 的任务引用，并将 rhs 置空。
    AsyncTask& operator=(AsyncTask&& rhs) noexcept;

    /// @brief 释放当前任务引用，并将句柄置空。
    AsyncTask& operator=(std::nullptr_t) noexcept;

    /// @brief 判断两个句柄是否指向同一个底层任务节点。
    [[nodiscard]] bool operator==(const AsyncTask& rhs) const noexcept;

    /// @brief 判断两个句柄是否指向不同的底层任务节点。
    [[nodiscard]] bool operator!=(const AsyncTask& rhs) const noexcept;

    /// @brief 判断当前句柄是否绑定了有效任务。
    [[nodiscard]] explicit operator bool() const noexcept;

    /// @brief 立即释放当前持有的任务引用，并将句柄置空。
    void reset() noexcept;

    // ==================== 状态查询 ====================

    /// @brief 获取当前句柄的哈希值，基于底层 Work 指针地址。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取当前异步任务所属 Topology 的引用计数。
    [[nodiscard]] std::size_t use_count() const noexcept;

    /// @brief 判断当前句柄是否绑定了底层任务节点。
    [[nodiscard]] bool valid() const noexcept;

    /// @brief 检测任务是否正处于运行或锁定状态。
    [[nodiscard]] bool running() const noexcept;

    /// @brief 检测任务是否已经完全执行结束。
    [[nodiscard]] bool done() const noexcept;

    /// @brief 获取该异步任务对应的底层节点类型。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 获取任务名称。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 检测任务执行期间是否已经记录异常。
    [[nodiscard]] bool has_exception() const noexcept;

    // ==================== 可视化 ====================

    /// @brief 将当前异步任务导出为 D2 描述字符串。
    /// @param dir 图布局方向。
    /// @return D2 文本。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前异步任务的 D2 描述写入输出流。
    /// @param os 输出流。
    /// @param dir 图布局方向。
    void dump(std::ostream& os, Direction dir = Direction::Default) const;

    // ==================== 控制接口 ====================

    [[nodiscard]] std::stop_token stop_token() const noexcept;

    [[nodiscard]] bool stop_requested() const noexcept ;

    [[nodiscard]] bool stop_possible() const noexcept;

    bool request_stop() noexcept;

    /// @brief 阻塞当前线程，直到该异步任务完全执行完毕。
    void wait() const noexcept;

    /// @brief 同步等待并重新抛出任务执行期间捕获到的首个异常。
    void get();

protected:
    Work* m_work{nullptr};

    /// @brief 从底层 Work 指针构造句柄，并增加引用计数。
    explicit AsyncTask(Work* work) noexcept;

    /// @brief 增加当前任务所属 Topology 的引用计数。
    void _incref() noexcept;

    /// @brief 减少当前任务所属 Topology 的引用计数；归零时销毁底层任务节点。
    void _decref() noexcept;
};

// ============================================================
// AsyncTask 内联实现
// ============================================================

inline void AsyncTask::_incref() noexcept {
    if (m_work) m_work->m_topology->_incref();
}

inline void AsyncTask::_decref() noexcept {
    if (m_work && m_work->m_topology->_decref()) {
        destroy(m_work);
    }
}

inline AsyncTask::AsyncTask(Work* w) noexcept : m_work{w} {
    _incref();
}

inline AsyncTask::~AsyncTask() {
    _decref();
}

inline AsyncTask::AsyncTask(std::nullptr_t) noexcept : m_work{nullptr} {}

inline AsyncTask::AsyncTask(const AsyncTask& rhs) noexcept : m_work{rhs.m_work} {
    _incref();
}

inline AsyncTask& AsyncTask::operator=(const AsyncTask& rhs) noexcept {
    if (this != std::addressof(rhs)) {
        _decref();
        m_work = rhs.m_work;
        _incref();
    }
    return *this;
}

inline AsyncTask::AsyncTask(AsyncTask&& rhs) noexcept
    : m_work{std::exchange(rhs.m_work, nullptr)} {}

inline AsyncTask& AsyncTask::operator=(AsyncTask&& rhs) noexcept {
    if (this != std::addressof(rhs)) {
        _decref();
        m_work = std::exchange(rhs.m_work, nullptr);
    }
    return *this;
}

inline AsyncTask& AsyncTask::operator=(std::nullptr_t) noexcept {
    _decref();
    m_work = nullptr;
    return *this;
}

inline bool AsyncTask::operator==(const AsyncTask& rhs) const noexcept {
    return m_work == rhs.m_work;
}

inline bool AsyncTask::operator!=(const AsyncTask& rhs) const noexcept {
    return m_work != rhs.m_work;
}

inline AsyncTask::operator bool() const noexcept {
    return m_work != nullptr;
}

inline void AsyncTask::reset() noexcept {
    _decref();
    m_work = nullptr;
}

inline std::size_t AsyncTask::hash_value() const noexcept {
    return std::hash<Work*>{}(m_work);
}

inline std::size_t AsyncTask::use_count() const noexcept {
    return m_work->m_topology->_use_count();
}

inline bool AsyncTask::valid() const noexcept {
    return m_work != nullptr;
}

inline bool AsyncTask::running() const noexcept {
    return m_work->m_topology->_is_running();
}

inline bool AsyncTask::done() const noexcept {
    return m_work->m_topology->_is_finished();
}

inline TaskType AsyncTask::type() const noexcept {
    return m_work->m_type;
}

inline std::string_view AsyncTask::name() const noexcept {
    return m_work->m_name;
}

inline bool AsyncTask::has_exception() const noexcept {
    return m_work->_has_exception();
}

inline std::string AsyncTask::dump(Direction dir) const {
    std::ostringstream oss;
    dump(oss, dir);
    return std::move(oss).str();
}

inline void AsyncTask::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";
    m_work->dump(os);
    os << "\n";
}

inline std::stop_token AsyncTask::stop_token() const noexcept{
    return m_work->m_topology->m_stop_source.get_token();
}

inline bool AsyncTask::stop_requested() const noexcept{
    return m_work->m_topology->m_stop_source.stop_requested();
}

inline bool AsyncTask::stop_possible() const noexcept{
    return m_work->m_topology->m_stop_source.stop_possible();
}

inline bool AsyncTask::request_stop() noexcept{
    return m_work->m_topology->m_stop_source.request_stop();
}

inline void AsyncTask::wait() const noexcept {
    m_work->m_topology->_wait();
}

inline void AsyncTask::get() {
    m_work->m_topology->_wait();
    m_work->_rethrow_exception();
}

// ============================================================
// DeferredAsyncTask - 延迟启动 + 完整配置接口
// ============================================================

/// @brief 启动时机由用户掌控的异步任务句柄。
///
/// @details
/// `DeferredAsyncTask` 是 `AsyncTask` 的派生，对应"先建后启"的两阶段语义：
/// 调用 `Executor::deferred_async` 系列工厂返回 Idle 状态的 `DeferredAsyncTask`，
/// 用户在启动前可以充分配置（命名、信号量、观察者），然后用 `start(deps...)`
/// 一次性宣告依赖关系并切到 Running。
///
/// ============================================================================
///  为什么要派生而不是合一？
/// ============================================================================
/// 设计上把"可配置"切出独立类型而非合并到 `AsyncTask`，是为了 **类型安全地表达
/// 不可变性**：
///
/// - **`AsyncTask` = 已启动 / 已冻结**：任何 `name(...)` setter / 信号量配置
///   都不暴露 —— 编译期阻止用户对正在跑或已跑过的任务做结构性修改；
/// - **`DeferredAsyncTask` = 配置中**：`name` setter / `acquire` / `register_observer`
///   都可用；
/// - 一旦用户 `start()`，逻辑上对象就"切片回" `AsyncTask` —— 后续传递通常按
///   基类引用接收，可写接口自然消失。
///
/// 这是用 **类型系统编码生命周期阶段** 的常见手法（state-as-type 模式）。
///
/// ============================================================================
///  二进制布局兼容性 —— start() 的隐性依赖
/// ============================================================================
/// `start(Deps&&... deps)` 内部需要把 `Deps...` 中混杂的 `AsyncTask` 与
/// `DeferredAsyncTask` 收拢到 `std::array<AsyncTask, N>`。这一步是
/// **基类切片** —— 派生类对象按值拷贝到基类槽位。
///
/// 切片要值语义安全，派生类必须 **不引入任何额外字段**。文件底部的
/// `static_assert(sizeof(DeferredAsyncTask) == sizeof(AsyncTask))` 就是这个
/// 不变式的编译期守卫，违反它的修改不会偷偷过审 —— 这是项目里少数几处
/// "ABI 锁定" 的关键点。
///
/// ============================================================================
///  与 AsyncTask 的等价 API
/// ============================================================================
/// 所有状态查询 / 控制（`wait` / `get` / `stop` / `done` / `running`）都从
/// `AsyncTask` 继承，行为一致。`name` 的 getter 通过 `using AsyncTask::name`
/// 显式暴露，避免被派生类的 setter 同名屏蔽。
///
/// ============================================================================
///  使用示例
/// ============================================================================
/// @code
///   auto a = executor.deferred_async([]{ /* job 1 */ }).name("ingest");
///   auto b = executor.deferred_async([]{ /* job 2 */ });
///   auto c = executor.deferred_async([]{ /* job 3 */ })
///                .acquire(sem)                 // 信号量约束
///                .register_observer<Tracer>(); // 观察者
///
///   a.start();                                  // 无依赖直接启动
///   b.start(a);                                 // 依赖 a
///   c.start(a, b);                              // 依赖 a 和 b
///   c.get();                                    // 等待并取异常
/// @endcode
///
/// @warning 二进制布局必须与 `AsyncTask` 一致，由 static_assert 强制保护。
/// @see AsyncTask                  启动后的视角
/// @see Executor::deferred_async   生产源
class DeferredAsyncTask : public AsyncTask {
    friend class Work;
    friend class Graph;
    friend class Flow;
    friend class Runtime;
    friend class Executor;
    friend class Branch;

public:
    /// @brief 构造空的延迟启动异步任务句柄。
    DeferredAsyncTask() noexcept = default;

    /// @brief 拷贝构造，引用同一个延迟任务并增加引用计数。
    DeferredAsyncTask(const DeferredAsyncTask&) noexcept = default;

    /// @brief 移动构造，接管另一个延迟任务句柄。
    DeferredAsyncTask(DeferredAsyncTask&&) noexcept = default;

    /// @brief 拷贝赋值，引用同一个延迟任务并维护引用计数。
    DeferredAsyncTask& operator=(const DeferredAsyncTask&) noexcept = default;

    /// @brief 移动赋值，接管另一个延迟任务句柄。
    DeferredAsyncTask& operator=(DeferredAsyncTask&&) noexcept = default;

    /// @brief 释放当前任务引用，并将句柄置空。
    DeferredAsyncTask& operator=(std::nullptr_t) noexcept {
        AsyncTask::operator=(nullptr);
        return *this;
    }

    // ==================== 命名 ====================

    /// @brief 为任务设置易读的名称，用于调试和可视化。
    template <typename S>
        requires std::constructible_from<std::string, S>
    DeferredAsyncTask& name(S&& name) &;

    template <typename S>
        requires std::constructible_from<std::string, S>
    DeferredAsyncTask name(S&& name) &&;
    /// @brief 暴露基类的任务名称查询接口。
    using AsyncTask::name;

    // ==================== 信号量 ====================

    /// @brief 获取任务执行前需要获取的信号量数量。
    [[nodiscard]] std::size_t num_acquires() const noexcept;

    /// @brief 获取任务执行后需要释放的信号量数量。
    [[nodiscard]] std::size_t num_releases() const noexcept;

    /// @brief 获取任务已注册的观察者数量。
    [[nodiscard]] std::size_t num_observers() const noexcept;

    /// @brief 为任务添加执行前需要获取的信号量，每个信号量默认占用 1 个配额。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask& acquire(Ts&&... sems) &;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask acquire(Ts&&... sems) &&;

    /// @brief 为任务添加执行后需要释放的信号量，每个信号量默认释放 1 个配额。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask& release(Ts&&... sems) &;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask release(Ts&&... sems) &&;

    /// @brief 为任务添加执行前需要获取的信号量及对应配额。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    DeferredAsyncTask& acquire(Ts&&... args) &;

    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    DeferredAsyncTask acquire(Ts&&... args) &&;

    /// @brief 为任务添加执行后需要释放的信号量及对应配额。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    DeferredAsyncTask& release(Ts&&... args) &;

    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    DeferredAsyncTask release(Ts&&... args) &&;

    /// @brief 移除任务执行前需要获取的指定信号量约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask& remove_acquire(Ts&&... sems) & noexcept;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask remove_acquire(Ts&&... sems) && noexcept;

    /// @brief 移除任务执行后需要释放的指定信号量约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask& remove_release(Ts&&... sems) & noexcept;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    DeferredAsyncTask remove_release(Ts&&... sems) && noexcept;

    /// @brief 清空所有执行前信号量获取约束。
    DeferredAsyncTask& clear_acquires() & noexcept;

    DeferredAsyncTask clear_acquires() && noexcept;

    /// @brief 清空所有执行后信号量释放约束。
    DeferredAsyncTask& clear_releases() & noexcept;

    DeferredAsyncTask clear_releases() && noexcept;


    /// @brief 遍历任务执行前的信号量获取约束。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_acquire(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 遍历任务执行后的信号量释放约束。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_release(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    // ==================== 观察者 ====================

    /// @brief 注册一个任务观察者，在任务执行前后接收回调。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 注销指定任务观察者。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(std::shared_ptr<Observer> ptr) noexcept;

    // ==================== 启动 ====================

    /// @brief 手动启动该任务，并声明其依赖的前驱列表。
    /// @tparam Deps 必须为 AsyncTask 或其派生类。
    /// @pre 任务必须处于 Idle 状态且未被启动过。
    template <typename... Deps>
        requires (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    DeferredAsyncTask& start(Deps&&... deps) &;

    template <typename... Deps>
        requires (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    DeferredAsyncTask start(Deps&&... deps) &&;

    /// @brief 手动启动该任务，通过迭代器范围传递依赖。
    /// @tparam I 依赖范围起始迭代器类型。
    /// @tparam S 依赖范围哨兵类型。
    /// @pre 范围内元素必须为 AsyncTask 或其派生类。
    template <std::input_iterator I, std::sentinel_for<I> S>
        requires std::derived_from<std::iter_value_t<I>, AsyncTask>
    DeferredAsyncTask& start(I first, S last) &;

    template <std::input_iterator I, std::sentinel_for<I> S>
        requires std::derived_from<std::iter_value_t<I>, AsyncTask>
    DeferredAsyncTask start(I first, S last) &&;

protected:
    /// @brief 从底层 Work 指针构造延迟任务句柄，并增加引用计数。
    explicit DeferredAsyncTask(Work* work) noexcept : AsyncTask(work) {}
};

// Why: 派生类必须保持与基类相同的二进制布局,
//      否则切片传递(start() 内部依赖收集)将破坏值语义安全。
static_assert(sizeof(DeferredAsyncTask) == sizeof(AsyncTask),
              "DeferredAsyncTask must remain layout-compatible with AsyncTask");

// ============================================================
// DeferredAsyncTask 内联实现
// ============================================================

template <typename S>
    requires std::constructible_from<std::string, S>
inline DeferredAsyncTask& DeferredAsyncTask::name(S&& n) & {
    m_work->m_name = std::forward<S>(n);
    return *this;
}

template <typename S>
    requires std::constructible_from<std::string, S>
inline DeferredAsyncTask DeferredAsyncTask::name(S&& n) && {
    m_work->m_name = std::forward<S>(n);
    return std::move(*this);
}

inline std::size_t DeferredAsyncTask::num_acquires() const noexcept {
    return m_work->_num_acquires();
}

inline std::size_t DeferredAsyncTask::num_releases() const noexcept {
    return m_work->_num_releases();
}

inline std::size_t DeferredAsyncTask::num_observers() const noexcept {
    return m_work->_num_observers();
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask& DeferredAsyncTask::acquire(Ts&&... sems) & {
    (m_work->_acquire(&sems, 1ULL), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask DeferredAsyncTask::acquire(Ts&&... sems) && {
    (m_work->_acquire(&sems, 1ULL), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask& DeferredAsyncTask::release(Ts&&... sems) & {
    (m_work->_release(&sems, 1ULL), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask DeferredAsyncTask::release(Ts&&... sems) && {
    (m_work->_release(&sems, 1ULL), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline DeferredAsyncTask& DeferredAsyncTask::acquire(Ts&&... args) & {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_acquire(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline DeferredAsyncTask DeferredAsyncTask::acquire(Ts&&... args) && {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_acquire(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline DeferredAsyncTask& DeferredAsyncTask::release(Ts&&... args) & {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_release(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline DeferredAsyncTask DeferredAsyncTask::release(Ts&&... args) && {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_release(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask& DeferredAsyncTask::remove_acquire(Ts&&... sems) & noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask DeferredAsyncTask::remove_acquire(Ts&&... sems) && noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask& DeferredAsyncTask::remove_release(Ts&&... sems) & noexcept {
    (m_work->_remove_release(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline DeferredAsyncTask DeferredAsyncTask::remove_release(Ts&&... sems) && noexcept {
    (m_work->_remove_release(&sems), ...);
    return std::move(*this);
}

inline DeferredAsyncTask& DeferredAsyncTask::clear_acquires() & noexcept {
    m_work->_clear_acquires();
    return *this;
}

inline DeferredAsyncTask DeferredAsyncTask::clear_acquires() && noexcept {
    m_work->_clear_acquires();
    return std::move(*this);
}

inline DeferredAsyncTask& DeferredAsyncTask::clear_releases() & noexcept {
    m_work->_clear_releases();
    return *this;
}

inline DeferredAsyncTask DeferredAsyncTask::clear_releases() && noexcept {
    m_work->_clear_releases();
    return std::move(*this);
}


template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&>
             || std::invocable<F&, Semaphore&>
inline void DeferredAsyncTask::for_each_acquire(F&& visitor) noexcept(
    std::invocable<F&, Semaphore&, std::size_t&>
        ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
        : std::is_nothrow_invocable_v<F&, Semaphore&>) {
    for (auto& req : m_work->_acquires()) {
        if constexpr (std::invocable<F&, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void DeferredAsyncTask::for_each_acquire(F&& visitor) const noexcept(
    std::invocable<F&, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    for (const auto& req : m_work->_acquires()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&>
             || std::invocable<F&, Semaphore&>
inline void DeferredAsyncTask::for_each_release(F&& visitor) noexcept(
    std::invocable<F&, Semaphore&, std::size_t&>
        ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
        : std::is_nothrow_invocable_v<F&, Semaphore&>) {
    for (auto& req : m_work->_releases()) {
        if constexpr (std::invocable<F&, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void DeferredAsyncTask::for_each_release(F&& visitor) const noexcept(
    std::invocable<F&, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    for (const auto& req : m_work->_releases()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
inline std::shared_ptr<Observer> DeferredAsyncTask::register_observer(Args&&... args) {
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    m_work->m_observers->observers.emplace_back(
        std::static_pointer_cast<TaskObserver>(ptr));
    return ptr;
}

template <std::derived_from<TaskObserver> Observer>
inline void DeferredAsyncTask::unregister_observer(std::shared_ptr<Observer> ptr) noexcept {
    if (!m_work->m_observers) return;

    auto base = std::static_pointer_cast<TaskObserver>(ptr);
    auto& observers = m_work->m_observers->observers;
    for (auto it = observers.begin(); it != observers.end(); ++it) {
        if (*it == base) {
            observers.erase(it);
            break;
        }
    }
    if (observers.empty()) {
        m_work->m_observers.reset();
    }
}

template <typename... Deps>
    requires (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
inline DeferredAsyncTask& DeferredAsyncTask::start(Deps&&... deps) & {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        static_cast<AsyncTask>(std::forward<Deps>(deps))...
    };
    return start(arr.begin(), arr.end());
}

template <typename... Deps>
    requires (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
inline DeferredAsyncTask DeferredAsyncTask::start(Deps&&... deps) && {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        static_cast<AsyncTask>(std::forward<Deps>(deps))...
    };
    return start(arr.begin(), arr.end());
}

// ============================================================================
//  实现部分：AsyncTask 与各类 Work 子类的 Invoke 具体派发机制
// ============================================================================

template <std::input_iterator I, std::sentinel_for<I> S>
    requires std::derived_from<std::iter_value_t<I>, AsyncTask>
inline DeferredAsyncTask& DeferredAsyncTask::start(I first, S last) & {
    auto* topo = m_work->m_topology;
    auto& exec = topo->m_executor;

    if (topo->_is_finished()) {
        throw Exception("DeferredAsyncTask Error: Cannot start a finished task.");
    }

    auto expected = Topology::State::Idle;
    if (!topo->m_state.compare_exchange_strong(expected, Topology::State::Running,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        throw Exception("DeferredAsyncTask Error: Task is already running.");
    }

    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    m_work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    exec._process_dependent_async(m_work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        if (Worker* wr = exec._this_worker(); wr) {
            exec._schedule(*wr, m_work);
        } else {
            exec._schedule(m_work);
        }
    }

    return *this;
}


template <std::input_iterator I, std::sentinel_for<I> S>
    requires std::derived_from<std::iter_value_t<I>, AsyncTask>
inline DeferredAsyncTask DeferredAsyncTask::start(I first, S last) && {
    auto* topo = m_work->m_topology;
    auto& exec = topo->m_executor;

    if (topo->_is_finished()) {
        throw Exception("DeferredAsyncTask Error: Cannot start a finished task.");
    }

    auto expected = Topology::State::Idle;
    if (!topo->m_state.compare_exchange_strong(expected, Topology::State::Running,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        throw Exception("DeferredAsyncTask Error: Task is already running.");
    }

    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    m_work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    exec._process_dependent_async(m_work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        if (Worker* wr = exec._this_worker(); wr) {
            exec._schedule(*wr, m_work);
        } else {
            exec._schedule(m_work);
        }
    }

    return std::move(*this);
}


// ============================================================================
//  任务提交路由
// ============================================================================

template <typename Gh>
    requires graph_holder<Gh>
inline DeferredAsyncTask Executor::deferred_async(Gh&& gh) {
    return deferred_async(std::forward<Gh>(gh), 1ULL);
}

template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline DeferredAsyncTask Executor::deferred_async(Gh&& gh, C&& cb) {
    return deferred_async(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <typename Gh>
    requires graph_holder<Gh>
inline DeferredAsyncTask Executor::deferred_async(Gh&& gh, std::uint64_t num) {
    return deferred_async(std::forward<Gh>(gh), num, []() noexcept {});
}

template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline DeferredAsyncTask Executor::deferred_async(Gh&& gh, std::uint64_t num, C&& cb) {
    // 将次数转换为谓词
    return deferred_async(std::forward<Gh>(gh)
                  ,[num]() mutable noexcept { return num-- == 0; }
                  ,std::forward<C>(cb));
}

template <typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
inline DeferredAsyncTask Executor::deferred_async(Gh&& gh, P&& pred) {
    return deferred_async(std::forward<Gh>(gh)
                  ,std::forward<P>(pred)
                  ,[]() noexcept {});
}

template <typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
inline DeferredAsyncTask Executor::deferred_async(Gh&& gh, P&& pred, C&& cb) {
    Work* work = make_dep_deferred_async_flow<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));

    return DeferredAsyncTask{work};
}


template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline DeferredAsyncTask Executor::deferred_async(T&& task, Args&&... args) {
    Work* work = make_dep_deferred_async_basic<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<T>(task), std::forward<Args>(args)...);
    return DeferredAsyncTask{work};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline DeferredAsyncTask Executor::deferred_async(T&& task, Args&&... args) {
    Work* work = make_dep_deferred_async_runtime<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<T>(task), std::forward<Args>(args)...);
    return DeferredAsyncTask{work};
}


// ============================================================================
//  依赖启动
// ============================================================================


template <typename Gh, typename... Deps>
    requires graph_holder<Gh> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
inline AsyncTask Executor::dependent_async(Gh&& gh, Deps&&... deps) {
    return dependent_async(std::forward<Gh>(gh), 1ULL, std::forward<Deps>(deps)...);
}

template <typename Gh, typename C, typename... Deps>
    requires (capturable<C> && graph_holder<Gh> && callback<C> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Executor::dependent_async(Gh&& gh, C&& cb, Deps&&... deps) {
    return dependent_async(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb),
                           std::forward<Deps>(deps)...);
}

template <typename Gh, typename... Deps>
    requires graph_holder<Gh> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
inline AsyncTask Executor::dependent_async(Gh&& gh, std::uint64_t num, Deps&&... deps) {
    return dependent_async(std::forward<Gh>(gh), num, []() noexcept {},
                           std::forward<Deps>(deps)...);
}

template <typename Gh, typename C, typename... Deps>
    requires (capturable<C> && graph_holder<Gh> && callback<C> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Executor::dependent_async(Gh&& gh, std::uint64_t num, C&& cb,
                                           Deps&&... deps) {
    return dependent_async(
        std::forward<Gh>(gh),
        [num]() mutable noexcept { return num-- == 0; },
        std::forward<C>(cb),
        std::forward<Deps>(deps)...);
}

template <typename Gh, typename P, typename... Deps>
    requires (capturable<P> && graph_holder<Gh> && predicate<P> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Executor::dependent_async(Gh&& gh, P&& pred, Deps&&... deps) {
    return dependent_async(std::forward<Gh>(gh),
                           std::forward<P>(pred),
                           []() noexcept {},
                           std::forward<Deps>(deps)...);
}

template <typename Gh, typename P, typename C, typename... Deps>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Executor::dependent_async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps) {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        AsyncTask(static_cast<const AsyncTask&>(deps))...
    };
    return dependent_async(
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb),
        arr.begin(), arr.end()
        );
}

template <typename Gh, typename P, typename C, std::input_iterator I, std::sentinel_for<I> S>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
             std::derived_from<std::iter_value_t<I>, AsyncTask>)
inline AsyncTask Executor::dependent_async(Gh&& gh, P&& pred, C&& cb, I first, S last) {
    Work* work = make_dep_async_flow<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));

    auto* topo = work->m_topology;
    topo->m_state.store(Topology::State::Running, std::memory_order_relaxed);
    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    _process_dependent_async(work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        if (Worker* wr = _this_worker(); wr) {
            _schedule(*wr, work);
        } else {
            _schedule(work);
        }
    }

    return AsyncTask{work};
}

template <typename T, typename... Deps>
    requires (basic_invocable<T> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Executor::dependent_async(T&& task, Deps&&... deps) {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        AsyncTask(static_cast<const AsyncTask&>(deps))...
    };
    return dependent_async(std::forward<T>(task), arr.begin(), arr.end());
}

template <typename T, std::input_iterator I, std::sentinel_for<I> S>
    requires (basic_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
inline AsyncTask Executor::dependent_async(T&& task, I first, S last) {
    Work* work = make_dep_async_basic<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<T>(task));

    auto* topo = work->m_topology;
    topo->m_state.store(Topology::State::Running, std::memory_order_relaxed);
    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    _process_dependent_async(work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        if (Worker* wr = _this_worker(); wr) {
            _schedule(*wr, work);
        } else {
            _schedule(work);
        }
    }

    return AsyncTask{work};
}

template <typename T, typename... Deps>
    requires (runtime_invocable<T> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Executor::dependent_async(T&& task, Deps&&... deps) {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        AsyncTask(static_cast<const AsyncTask&>(deps))...
    };
    return dependent_async(std::forward<T>(task), arr.begin(), arr.end());
}

template <typename T, std::input_iterator I, std::sentinel_for<I> S>
    requires (runtime_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
inline AsyncTask Executor::dependent_async(T&& task, I first, S last) {
    Work* work = make_dep_async_runtime<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<T>(task));

    auto* topo = work->m_topology;
    topo->m_state.store(Topology::State::Running, std::memory_order_relaxed);
    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    _process_dependent_async(work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        if (Worker* wr = _this_worker(); wr) {
            _schedule(*wr, work);
        } else {
            _schedule(work);
        }
    }

    return AsyncTask{work};
}



template <typename Gh>
    requires graph_holder<Gh>
inline void Executor::silent_async(Gh&& gh) {
    return silent_async(std::forward<Gh>(gh), 1ULL);
}

template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Executor::silent_async(Gh&& gh, C&& cb) {
    return silent_async(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <typename Gh>
    requires graph_holder<Gh>
inline void Executor::silent_async(Gh&& gh, std::uint64_t num) {
    return silent_async(std::forward<Gh>(gh), num, []() noexcept {});
}

template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Executor::silent_async(Gh&& gh, std::uint64_t num, C&& cb) {
    // 将次数转换为谓词
    return silent_async(std::forward<Gh>(gh)
                        ,[num]() mutable noexcept { return num-- == 0; }
                        ,std::forward<C>(cb));
}

template <typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
inline void Executor::silent_async(Gh&& gh, P&& pred) {
    return silent_async(std::forward<Gh>(gh)
                        ,std::forward<P>(pred)
                        ,[]() noexcept {});
}

template <typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
inline void Executor::silent_async(Gh&& gh, P&& pred, C&& cb) {
    Work* work = make_silent_async_flow<anchor::explicit_t>(
        /*parent=*/nullptr,
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));

    _increment_topology();

    // 线程本地提交：直接放入本地队列（零队列操作）
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}


template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
inline void Executor::silent_async(T&& task, Args&&... args) {
    Work* work = make_silent_async_basic<anchor::explicit_t>(
        /*parent=*/nullptr,
        std::forward<T>(task), std::forward<Args>(args)...);

    _increment_topology();

    // 线程本地提交：直接放入本地队列（零队列操作）
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
inline void Executor::silent_async(T&& task, Args&&... args) {
    Work* work = make_silent_async_runtime<anchor::explicit_t>(
        /*parent=*/nullptr,
        std::forward<T>(task), std::forward<Args>(args)...);

    _increment_topology();
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}



// ============================================================================
//  Executor::async ── 任务图提交，返回 Future<void>
// ============================================================================

template <graph_holder Gh>
inline Future<void> Executor::async(Gh&& gh) {
    return async(std::forward<Gh>(gh), 1ULL);
}

template <graph_holder Gh, typename C>
    requires (capturable<C> && callback<C>)
inline Future<void> Executor::async(Gh&& gh, C&& cb) {
    return async(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <graph_holder Gh>
inline Future<void> Executor::async(Gh&& gh, std::uint64_t num) {
    return async(std::forward<Gh>(gh), num, []() noexcept {});
}

template <graph_holder Gh, typename C>
    requires (capturable<C> && callback<C>)
inline Future<void> Executor::async(Gh&& gh, std::uint64_t num, C&& cb) {
    return async(std::forward<Gh>(gh),
                 [num]() mutable noexcept { return num-- == 0; },
                 std::forward<C>(cb));
}

template <graph_holder Gh, typename P>
    requires (capturable<P> && predicate<P>)
inline Future<void> Executor::async(Gh&& gh, P&& pred) {
    return async(std::forward<Gh>(gh),
                 std::forward<P>(pred),
                 []() noexcept {});
}

template <graph_holder Gh, typename P, typename C>
    requires (capturable<P, C> && predicate<P> && callback<C>)
inline Future<void> Executor::async(Gh&& gh, P&& pred, C&& cb) {
    std::promise<void> promise;
    std::future<void>  std_future = promise.get_future();

    Work* work = make_async_flow<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb),
        std::move(promise));

    // Why: 必须在 _schedule 之前抓拷贝。派发后 work 可能被其它 worker
    //      立即执行并 tear_down，work->m_topology 变 use-after-free。
    //      stop_source 是 shared_ptr 语义，拷贝即共享 control block。
    std::stop_source ss = work->m_topology->m_stop_source;

    _increment_topology();

    // 线程本地提交：worker 内调用走本地队列（零队列操作）
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }

    return Future<void>{std::move(std_future), std::move(ss)};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline auto Executor::async(T&& task, Args&&... args) -> Future<basic_return_t<T, Args...>> {
    using R = basic_return_t<T, Args...>;

    std::promise<R> promise;
    std::future<R> std_future = promise.get_future();

    Work* work = make_async_basic<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<T>(task), std::move(promise),
        std::forward<Args>(args)...);

    // Why: 必须在 _schedule 之前抓拷贝。派发后 work 可能被其它 worker
    //      立即执行并 tear_down，work->m_topology 变 use-after-free。
    //      stop_source 是 shared_ptr 语义，拷贝即共享 control block；
    //      Future 端持有令其在 topology 析构后仍可安全调用 request_stop。
    std::stop_source ss = work->m_topology->m_stop_source;

    _increment_topology();
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }

    return Future<R>{std::move(std_future), std::move(ss)};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline auto Executor::async(T&& task, Args&&... args) -> Future<runtime_return_t<T, Args...>> {
    using R = runtime_return_t<T, Args...>;

    std::promise<R> promise;
    std::future<R> std_future = promise.get_future();

    Work* work = make_async_runtime<anchor::explicit_t>(
        *this, /*parent=*/nullptr,
        std::forward<T>(task), std::move(promise),
        std::forward<Args>(args)...);

    std::stop_source ss = work->m_topology->m_stop_source;

    _increment_topology();
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }

    return Future<R>{std::move(std_future), std::move(ss)};
}

inline std::ostream& operator << (std::ostream& os, const AsyncTask& task) {
    task.dump(os);
    return os;
}

inline std::ostream& operator << (std::ostream& os, const DeferredAsyncTask& task) {
    task.dump(os);
    return os;
}

}  // namespace tfl

// ==================== 标准库扩展 ====================

namespace std {
template <>
struct hash<tfl::AsyncTask> {
    inline std::size_t operator()(const tfl::AsyncTask& task) const noexcept {
        return task.hash_value();
    }
};

template <>
struct hash<tfl::DeferredAsyncTask> {
    inline std::size_t operator()(const tfl::DeferredAsyncTask& task) const noexcept {
        return task.hash_value();
    }
};
}  // namespace std
