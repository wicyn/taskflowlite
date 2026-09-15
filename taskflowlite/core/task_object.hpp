/// @file task_object.hpp
/// @brief 带业务对象类型信息的任务句柄。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-09-14
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "task.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief 同时提供 Task 操作和业务对象引用访问的非拥有句柄。
///
/// TaskObject 不拥有节点或业务对象；复制句柄不会复制内部业务对象。
/// object() 采用指针式 const 语义，常量句柄仍可访问可修改的业务对象。
///
/// @tparam F 节点实际保存的业务对象类型。
/// @warning 节点销毁或 callable 被替换后，业务对象引用失效。
/// @warning 不得通过 Task 基类赋值重新绑定节点后继续使用原有对象访问。
template <typename F>
class TaskObject final : public Task {
    friend class FlowBuilder;

public:
    using object_type = F;

    /// @brief 默认构造空句柄。
    TaskObject() noexcept = default;

    /// @brief 复制任务句柄和对象地址，不复制业务对象。
    TaskObject(const TaskObject&) noexcept = default;

    TaskObject& operator=(const TaskObject&) noexcept = default;

    /// @brief 移动任务句柄和对象地址，并将源句柄置空。
    TaskObject(TaskObject&& other) noexcept
        : Task{std::move(other)}
        , m_object{std::exchange(other.m_object, nullptr)} {}

    TaskObject& operator=(TaskObject&& other) noexcept {
        if (this != std::addressof(other)) {
            Task::operator=(std::move(other));
            m_object = std::exchange(other.m_object, nullptr);
        }
        return *this;
    }

    /// @brief 将当前句柄置空，不销毁节点或业务对象。
    TaskObject& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    /// @brief 将当前句柄置空，不销毁节点或业务对象。
    void reset() noexcept {
        Task::reset();
        m_object = nullptr;
    }

    /// @brief 返回节点内部保存的业务对象。
    /// @pre 当前句柄非空，且业务对象仍然存活并与句柄对应。
    /// @warning 引用不延长对象生命周期，不得与对象执行发生未同步的数据访问。
    [[nodiscard]] F& object() const noexcept {
        TFL_ASSERT(Task::valid() && m_object);
        return *m_object;
    }

private:
    /// @brief 使用已创建的节点句柄和内部对象引用构造类型句柄。
    TaskObject(Task task, F& object) noexcept
        : Task{std::move(task)}
        , m_object{std::addressof(object)} {}

    F* m_object{nullptr};
};

}  // namespace tfl
