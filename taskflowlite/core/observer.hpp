/// @file observer.hpp
/// @brief 提供任务执行观察者接口，用于监控任务的开始与结束事件。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include "worker.hpp"

namespace tfl {

/// @brief 任务执行观察者接口
///
/// @details 可在任务执行前后插入自定义回调，用于监控、性能分析或日志记录。
/// 回调在 Worker 线程同步执行，需确保轻量快速避免影响调度性能。
class TaskObserver {
public:
    /// @brief 在任务执行前触发
    /// @param wv 工作线程状态快照（只读视图）
    virtual void on_before(WorkerView wv) = 0;

    /// @brief 在任务执行后触发
    /// @param wv 工作线程状态快照
    virtual void on_after(WorkerView wv) = 0;

    /// @brief 虚析构函数，确保你以后继承这个类写的子类，在销毁时能把自己的内存收拾干净。
    virtual ~TaskObserver() = default;

protected:

    // Why: 为什么要把构造和赋值操作都藏在 protected 里？这可是 C++ Core Guidelines 推荐的老手艺：
    // 1. 防误伤：这就是个纯虚的接口，根本不该在外面被直接实例化。
    // 2. 防切片：新手把子类对象当参数直接传值（而不是传指针/引用）时，很容易发生“对象切片（Object Slicing）”，
    //    把子类特有的数据给无情切丢了。把赋值藏起来，编译器直接就会在编译阶段报错拦住这种危险操作。
    // 3. 留后门：虽然外面的人不能调，但你的子类自己在家里依然可以正常用编译器生成的默认拷贝和移动。
    TaskObserver() = default;
    TaskObserver(const TaskObserver&) = default;
    TaskObserver(TaskObserver&&) = default;
    TaskObserver& operator=(const TaskObserver&) & = default;
    TaskObserver& operator=(TaskObserver&&) & = default;
};

} // namespace tfl
