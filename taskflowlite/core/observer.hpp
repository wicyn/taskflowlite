/// @file observer.hpp
/// @brief 任务执行观察者 TaskObserver —— before/after 钩子的策略接口
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include "worker.hpp"

namespace tfl {

/// @brief 任务执行观察者接口（策略模式 / 观察者模式）。
///
/// @details
/// `TaskObserver` 把"任务执行前后" 这两个时间点暴露给用户，用于注入 tracing /
/// metrics / logging / debugging 等横切关注点。它是框架"用户可扩展性"的主要
/// 锚点之一，与 `WorkerHandler`（线程级钩子）形成层级互补：
///
/// | 钩子接口        | 触发粒度       | 注入点                          |
/// |-----------------|----------------|--------------------------------|
/// | `WorkerHandler` | **线程级**     | on_start / on_stop / on_exception |
/// | `TaskObserver`  | **任务级**     | on_before / on_after            |
///
/// ============================================================================
///  关键设计：传 WorkerView 而非 Worker
/// ============================================================================
/// 回调形参是 `WorkerView`（const 切片）而非 `Worker&`：
/// - 编译期屏蔽用户对调度器内部状态的写访问；
/// - 用户拿到的只是当前 worker 的只读状态快照（id / queue size / thread）；
/// - 与 `TaskView` 形成统一安全风格 —— "回调里能读但不能改"。
///
/// ============================================================================
///  为什么所有 ctor 是 protected？
/// ============================================================================
/// 这是 C++ Core Guidelines 推荐的接口类设计模式（C.67 / C.21）：
/// 1. **防直接实例化**：纯虚接口不应该被构造为对象；
/// 2. **防对象切片**：当用户把派生类实例传值（而非引用 / 智能指针）给函数时，
///    切片会丢掉派生类的成员 —— 把 ctor 设为 protected 让传值在编译期就失败；
/// 3. **保留派生类内部使用**：派生类自己的拷贝 / 移动构造仍可走默认实现，
///    不影响其内部需要的值语义。
///
/// 虚析构 + protected ctor 是表达"我是抽象基类，只能通过派生类指针 / 引用使用"
/// 的标准 C++ 习惯用法。
///
/// ============================================================================
///  线程安全约束
/// ============================================================================
/// `on_before` / `on_after` 在 worker 线程上同步调用，**必须线程安全且轻量**：
/// - 多个 worker 可能同时回调同一个 observer 实例（如全局 tracer）；
/// - 长时间执行会卡住调度循环，破坏吞吐；
/// - 用户实现里抛出异常会传到调度器路径，由 `WorkerHandler::on_exception` 接管。
///
/// @see Task::register_observer / Task::unregister_observer  注册入口
/// @see WorkerView  传给回调的 worker 安全切片
/// @see WorkerHandler  线程级钩子对偶
class TaskObserver {
public:
    /// @brief 在任务执行前触发
    /// @param wv 工作线程状态快照（只读视图）
    virtual void on_before(WorkerView wv) = 0;

    /// @brief 在任务执行后触发
    /// @param wv 工作线程状态快照
    virtual void on_after(WorkerView wv) = 0;

    /// @brief 虚析构函数，确保派生类析构时正确释放资源。
    virtual ~TaskObserver() = default;

protected:

    // Why: 构造/赋值操作均为 protected —— 遵循 C++ Core Guidelines 的接口隔离实践:
    //   1. 禁止外部实例化：本类为纯虚接口，不应直接构造；
    //   2. 防止对象切片：若按值传递派生类对象，编译器在传参时会发生隐式切片
    //      （派生类数据被截断），将赋值操作设为 protected 可编译期拦截此误用；
    //   3. 保留派生类内部使用：子类在自已的成员函数中仍可正常调用编译器生成的
    //      拷贝/移动默认实现。
    TaskObserver() = default;
    TaskObserver(const TaskObserver&) = default;
    TaskObserver(TaskObserver&&) = default;
    TaskObserver& operator=(const TaskObserver&) & = default;
    TaskObserver& operator=(TaskObserver&&) & = default;
};

} // namespace tfl
