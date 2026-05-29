/// @file  observer.hpp
/// @brief 任务执行观察者 TaskObserver —— before/after 钩子的策略接口。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include "worker.hpp"

namespace tfl {

/// @brief 任务执行观察者接口（策略模式 / 观察者模式）。
///
/// 在任务执行前后注入 tracing / metrics / logging 等横切关注点。
/// 传 WorkerView 而非 Worker& 以编译期屏蔽调度器内部状态写访问。
/// protected ctor 遵循 C++ Core Guidelines C.67（防直接实例化与对象切片）。
///
/// @note on_before / on_after 在 Worker 线程上同步调用，实现必须线程安全且轻量。
class TaskObserver {
public:
    /// @brief 在任务执行前触发。
    /// @param wv 工作线程状态快照 (只读视图)。
    virtual void on_before(WorkerView wv) = 0;

    /// @brief 在任务执行后触发。
    /// @param wv 工作线程状态快照。
    virtual void on_after(WorkerView wv) = 0;

    /// @brief 虚析构函数, 确保派生类析构时正确释放资源。
    virtual ~TaskObserver() = default;

protected:

    /// @brief 构造/赋值操作均为 protected —— 遵循 C++ Core Guidelines 的接口隔离实践:
    ///   1. 禁止外部实例化: 本类为纯虚接口, 不应直接构造
    ///   2. 防止对象切片: 若按值传递派生类对象, 编译器在传参时会发生隐式切片
    ///      (派生类数据被截断), 将构造/赋值设为 protected 可编译期拦截此误用
    ///   3. 保留派生类内部使用: 子类在自已的成员函数中仍可正常调用编译器生成的
    ///      拷贝/移动默认实现
    TaskObserver() = default;
    TaskObserver(const TaskObserver&) = default;
    TaskObserver(TaskObserver&&) = default;
    TaskObserver& operator=(const TaskObserver&) & = default;
    TaskObserver& operator=(TaskObserver&&) & = default;
};

} // namespace tfl
