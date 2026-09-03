/// @file observer.hpp
/// @brief 任务执行观察者 TaskObserver —— before/after 钩子的策略接口。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <exception>

#include "worker.hpp"

namespace tfl {

/// @brief 定义任务 callable 执行前后的观察回调接口。
///
/// 观察者通过共享所有权注册到 `Work`，框架在实际执行该节点的 Worker 线程上调用
/// `on_before` 和 `on_after`；任务执行或观察回调出现异常时通过 `on_exception` 通知。
///
/// 观察者只观察执行事件，不控制任务调度、异常传播或节点生命周期。
///
/// @warning 同一实例可被不同 Worker 并发回调，派生类必须自行同步共享状态。
class TaskObserver {
public:
    /// @brief 在任务执行前触发。
    /// @param wv Worker 的只读实时视图；查询值可能随调度变化。
    virtual void on_before(WorkerView wv) = 0;

    /// @brief 在任务执行后触发。
    /// @param wv Worker 的只读实时视图；查询值可能随调度变化。
    virtual void on_after(WorkerView wv) = 0;

    /// @brief 在观察到异常时触发。
    ///
    /// 任务 callable 抛出的异常以及当前观察者 before/after 回调抛出的异常
    /// 均通过该接口通知。通知本身不改变 Work 的异常传播语义。
    ///
    /// @param wv Worker 的只读实时视图。
    /// @param eptr 捕获的异常。
    /// @warning 本函数不得抛出异常。
    virtual void on_exception(WorkerView wv, std::exception_ptr eptr) noexcept {}

    /// @brief 虚析构函数，确保派生类析构时正确释放资源。
    virtual ~TaskObserver() = default;

protected:
    /// @brief 允许派生观察者默认构造基类部分。
    TaskObserver() = default;

    /// @brief 允许派生观察者复制基类部分；具体状态复制由派生类决定。
    TaskObserver(const TaskObserver&) = default;

    /// @brief 允许派生观察者移动基类部分。
    TaskObserver(TaskObserver&&) = default;

    /// @brief 允许派生观察者复制赋值基类部分。
    /// @return `*this`。
    TaskObserver& operator=(const TaskObserver&) & = default;

    /// @brief 允许派生观察者移动赋值基类部分。
    /// @return `*this`。
    TaskObserver& operator=(TaskObserver&&) & = default;
};

}  // namespace tfl
