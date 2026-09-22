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

// ============================================================================
// TaskObject
// ============================================================================

/// @brief 同时提供 Task 操作和业务对象访问的非拥有类型化任务句柄。
///
/// TaskObject 在普通 `Task` 句柄基础上额外保存节点内部业务对象的非拥有指针。
/// 底层 Work、Payload 和业务对象均不由本句柄拥有，其生命周期仍由所属 Flow
/// 或其他节点存储负责管理。
///
/// 复制 TaskObject 仅复制底层 Work 关联和业务对象地址，不复制业务对象；
/// 移动 TaskObject 转移两者关联，并将源句柄同时置为空状态。
///
/// `object()` 采用类似指针的 const 语义，因此 const TaskObject 仍可返回可修改
/// 的业务对象引用；句柄自身的 const 不传播到其所引用的业务对象。
///
/// @tparam F 节点内部实际保存的业务对象类型。
///
/// @note TaskObject 不增加额外业务对象存储，也不要求 F 可复制或可移动。
/// @note 继承自 Task 的图结构、名称、Semaphore、Observer 等操作仍作用于同一 Work。
/// @warning 节点销毁、Payload 被替换或内部业务对象被重新构造后，`m_object` 立即失效。
/// @warning `object()` 不执行任何同步；不得与任务执行对同一业务对象发生未同步的数据访问。
/// @warning 不得通过 Task 基类引用重新绑定当前句柄，否则基类 Work 指针与
///          `m_object` 可能指向不同节点。
/// @warning Task 不提供虚析构，不得通过 Task 指针删除 TaskObject。
template <typename F>
class TaskObject final : public Task {
    friend class FlowBuilder;

    using Base = Task;

public:
    using object_type = F;

    /// @brief 构造不关联任何任务和业务对象的空句柄。
    TaskObject() noexcept;

    /// @brief 复制任务句柄和业务对象地址，不复制业务对象。
    ///
    /// @param other 要复制的句柄；允许为空。
    TaskObject(const TaskObject& other) noexcept;

    /// @brief 复制另一句柄的任务和业务对象关联。
    ///
    /// 当前句柄原有的非拥有关系直接被覆盖，不影响任何 Work 或业务对象生命周期。
    ///
    /// @param other 要复制的句柄；允许为空或与当前对象相同。
    /// @return `*this`。
    TaskObject& operator=(const TaskObject& other) noexcept;

    /// @brief 接管源句柄的任务和业务对象关联，并将源句柄置空。
    ///
    /// @param other 要移动的句柄。
    /// @post other 不再关联 Work 或业务对象。
    TaskObject(TaskObject&& other) noexcept;

    /// @brief 接管源句柄的任务和业务对象关联，并将源句柄置空。
    ///
    /// 自移动时保持当前句柄不变。
    ///
    /// @param other 要移动的句柄。
    /// @return `*this`。
    /// @post 非自移动时 other 不再关联 Work 或业务对象。
    TaskObject& operator=(TaskObject&& other) noexcept;

    /// @brief 将当前句柄置为空状态。
    ///
    /// 仅清除当前句柄保存的 Work 和业务对象地址，不销毁节点或业务对象。
    ///
    /// @return `*this`。
    TaskObject& operator=(std::nullptr_t) noexcept;

    /// @brief 清除当前任务和业务对象关联。
    ///
    /// 等价于同时清空基类 Task 的 Work 指针和本类保存的业务对象指针。
    /// 本操作不修改、销毁或重新构造底层节点及业务对象。
    ///
    /// @post `Task::valid() == false` 且 `m_object == nullptr`。
    void reset() noexcept;

    /// @brief 获取当前节点内部保存的业务对象。
    ///
    /// @return 当前任务关联的业务对象引用。
    /// @pre 当前 TaskObject 非空，且业务对象仍属于当前 Work 并保持存活。
    /// @note 返回引用不复制对象，也不延长 Work 或业务对象生命周期。
    /// @note const TaskObject 仍返回 `F&`，采用指针式 const 语义。
    /// @warning 不得与任务执行发生未同步的数据访问。
    [[nodiscard]] F& object() const noexcept;

private:
    /// @brief 使用已经创建的 Task 和其内部业务对象构造类型化句柄。
    ///
    /// @param task 已关联目标 Work 的普通 Task 句柄。
    /// @param object 该 Work 内部保存的业务对象。
    /// @pre task 有效，object 属于 task 对应的 Work，且对象地址在关联期间保持稳定。
    /// @note 构造过程只建立非拥有关联，不复制或移动业务对象本身。
    TaskObject(Task task, F& object) noexcept;

    F* m_object{nullptr};  ///< 节点内部业务对象的非拥有指针；空句柄时为 nullptr。
};


// ============================================================================
// TaskObject 实现
// ============================================================================

template <typename F>
TaskObject<F>::TaskObject() noexcept = default;

template <typename F>
TaskObject<F>::TaskObject(const TaskObject& other) noexcept = default;

template <typename F>
TaskObject<F>& TaskObject<F>::operator=(const TaskObject& other) noexcept = default;

template <typename F>
TaskObject<F>::TaskObject(TaskObject&& other) noexcept
    : Base{std::move(other)}
    , m_object{std::exchange(other.m_object, nullptr)} {
}

template <typename F>
TaskObject<F>& TaskObject<F>::operator=(TaskObject&& other) noexcept {
    if (this != std::addressof(other)) {
        Base::operator=(std::move(other));
        m_object = std::exchange(other.m_object, nullptr);
    }

    return *this;
}

template <typename F>
TaskObject<F>& TaskObject<F>::operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
}

template <typename F>
void TaskObject<F>::reset() noexcept {
    Base::reset();
    m_object = nullptr;
}

template <typename F>
F& TaskObject<F>::object() const noexcept {
    TFL_ASSERT(Base::valid() && m_object);
    return *m_object;
}

template <typename F>
TaskObject<F>::TaskObject(Task task, F& object) noexcept
    : Base{std::move(task)}
    , m_object{std::addressof(object)} {}

}  // namespace tfl
