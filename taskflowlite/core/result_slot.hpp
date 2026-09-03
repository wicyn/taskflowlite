/// @file result_slot.hpp
/// @brief 临时任务结果存储，支持普通值、左值引用与 void。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include "macros.hpp"

namespace tfl {

/// @brief 选择普通值结果是否优先使用“预先默认构造、执行后赋值”的存储策略。
///
/// 默认仅对可默认构造且可从右值赋值的类型启用；用户可特化为 false 以改用延迟
/// 构造，但特化为 true 仍不能绕过实际构造和赋值能力检查。
///
/// @tparam R 结果对象类型。
template <typename R>
struct prefer_direct_result_storage : std::bool_constant<std::default_initializable<R> && std::is_assignable_v<R&, R&&>> {};

namespace detail {

template <typename R>
inline constexpr bool use_direct_result_storage = prefer_direct_result_storage<R>::value && std::default_initializable<R> && std::is_assignable_v<R&, R&&>;

template <typename R, bool Direct = use_direct_result_storage<R>>
class ResultStorage;

/// @brief 为可复用结果预先构造一个 `R`，并在每次写入时对该对象赋值。
///
/// 存储始终包含一个有效 `R`，没有“尚未产生结果”状态；类型仅供 `ResultSlot`
/// 继承使用，不提供同步。
///
/// @tparam R 可默认构造并可由结果值赋值的对象类型。
template <typename R>
class ResultStorage<R, true> {
protected:
    template <typename U>
    static constexpr bool directly_storable = std::is_assignable_v<R&, U&&>;

    template <typename U>
    static constexpr bool converted_storable = std::constructible_from<R, U&&> && std::is_assignable_v<R&, R&&>;

    template <typename U>
    static constexpr bool storable = directly_storable<U> || converted_storable<U>;

    template <typename U>
    static constexpr bool nothrow_storable = [] {
        if constexpr (directly_storable<U>) return std::is_nothrow_assignable_v<R&, U&&>;
        else if constexpr (converted_storable<U>) return noexcept(std::declval<R&>() = R(std::declval<U&&>()));
        else return false;
    }();

    /// @brief 将值直接赋给已有对象，必要时先转换为 R。
    /// @tparam U 输入值类型。
    /// @param value 要保存的结果。
    template <typename U> requires storable<U>
    void store(U&& value) noexcept(nothrow_storable<U>) {
        if constexpr (directly_storable<U>) m_value = std::forward<U>(value);
        else m_value = R(std::forward<U>(value));
    }

    /// @brief 获取当前结果对象的可修改引用。
    /// @return 内部直接存储对象。
    [[nodiscard]] R& value() noexcept { return m_value; }

    /// @brief 获取当前结果对象的只读引用。
    /// @return 内部直接存储对象。
    [[nodiscard]] const R& value() const noexcept { return m_value; }

private:
    TFL_NO_UNIQUE_ADDRESS R m_value{};
};

/// @brief 通过 `std::optional<R>` 延迟构造或替换不适合直接赋值复用的结果。
///
/// 首次 `store()` 前不含对象，读取要求结果已经写入；类型仅供 `ResultSlot`
/// 继承使用，不提供同步。
///
/// @tparam R 可由任务返回值构造的对象类型。
template <typename R>
class ResultStorage<R, false> {
protected:
    template <typename U>
    static constexpr bool storable = std::constructible_from<R, U&&>;

    template <typename U>
    static constexpr bool nothrow_storable = std::is_nothrow_constructible_v<R, U&&>;

    /// @brief 在 optional 中构造或替换结果对象。
    /// @tparam U 输入值类型。
    /// @param value 用于构造 R 的值。
    template <typename U> requires storable<U>
    void store(U&& value) noexcept(nothrow_storable<U>) { m_value.emplace(std::forward<U>(value)); }

    /// @brief 获取已构造结果的可修改引用。
    /// @return optional 中保存的 R。
    /// @pre 结果必须已经由 `store()` 构造；仅在断言启用时检查。
    [[nodiscard]] R& value() noexcept { TFL_ASSERT(m_value); return *m_value; }

    /// @brief 获取已构造结果的只读引用。
    /// @return optional 中保存的 R。
    /// @pre 结果必须已经由 `store()` 构造；仅在断言启用时检查。
    [[nodiscard]] const R& value() const noexcept { TFL_ASSERT(m_value); return *m_value; }

private:
    std::optional<R> m_value;
};

} // namespace detail

/// @brief 保存一个普通值任务结果，并统一提供写入、引用访问和移动取出接口。
///
/// 具体采用预构造复用还是 optional 延迟构造由 `prefer_direct_result_storage<R>`
/// 决定；`take()` 只返回内部对象的右值引用，不会清空或重建结果槽。
///
/// @tparam R 无 cv 限定、非数组且可析构的对象类型。
/// @note 结果槽不提供同步；读写必须位于任务完成关系所保证的时序内。
template <typename R>
class ResultSlot : private detail::ResultStorage<R> {
    static_assert(std::is_object_v<R>, "ResultSlot<R>: R must be an object type.");
    static_assert(!std::is_array_v<R>, "ResultSlot<R>: R must not be an array type.");
    static_assert(std::same_as<R, std::remove_cv_t<R>>, "ResultSlot<R>: R must not be cv-qualified.");
    static_assert(std::is_destructible_v<R>, "ResultSlot<R>: R must be destructible.");

    using Storage = detail::ResultStorage<R>;

public:
    /// @brief 调用 callable 并保存其非 void 返回值。
    /// @tparam F 可调用对象类型；返回值必须可保存为 R。
    /// @tparam Args 调用时传递给 callable 的参数类型。
    /// @param f 要调用的对象。
    /// @param args 转发给 callable 的参数。
    template <typename F, typename... Args>
        requires std::invocable<F&&, Args&&...> && (!std::is_void_v<std::invoke_result_t<F&&, Args&&...>>) && Storage::template storable<std::invoke_result_t<F&&, Args&&...>>
        void invoke(F&& f, Args&&... args) noexcept(std::is_nothrow_invocable_v<F&&, Args&&...> && Storage::template nothrow_storable<std::invoke_result_t<F&&, Args&&...>>) {
        Storage::store(std::invoke(std::forward<F>(f), std::forward<Args>(args)...));
    }

    /// @brief 直接保存一个可转换或可赋值为 R 的结果。
    /// @tparam U 输入值类型。
    /// @param value 要保存的值。
    template <typename U> requires Storage::template storable<U>
        void return_value(U&& value) noexcept(Storage::template nothrow_storable<U>) { Storage::store(std::forward<U>(value)); }

    /// @brief 保存一个 R 右值。
    /// @param value 要移动进结果槽的对象。
    void return_value(R&& value) noexcept(Storage::template nothrow_storable<R>)
        requires Storage::template storable<R> { Storage::store(std::move(value)); }

    /// @brief 获取保存结果的可修改引用。
    /// @return 内部结果对象；延迟存储模式要求结果已构造。
    [[nodiscard]] R& ref() noexcept { return Storage::value(); }

    /// @brief 获取保存结果的只读引用。
    /// @return 内部结果对象；延迟存储模式要求结果已构造。
    [[nodiscard]] const R& ref() const noexcept { return Storage::value(); }

    /// @brief 取得保存结果的右值引用。
    /// @return 指向内部对象的 `R&&`；本函数本身不重置结果槽。
    /// @warning 移动构造外部结果后，槽内对象仍存在但通常处于 moved-from 状态。
    [[nodiscard]] R&& take() && noexcept { return std::move(Storage::value()); }
};

/// @brief 以裸指针保存一个左值引用任务结果，而不取得对象所有权。
///
/// 初始状态为空，任务写入后 `ref()` 与 `take()` 均返回同一对象且不会消费结果。
///
/// @tparam T 被引用对象类型。
/// @warning 读取前必须已经写入结果，且被引用对象必须在全部访问期间保持存活。
template <typename T>
class ResultSlot<T&> {
public:
    /// @brief 调用 callable 并保存其返回左值的地址。
    /// @tparam F 返回可转换为 `T&` 的左值引用的 callable 类型。
    /// @tparam Args 调用时传递给 callable 的参数类型。
    /// @param f 要调用的对象。
    /// @param args 转发给 callable 的参数。
    template <typename F, typename... Args>
        requires std::invocable<F&&, Args&&...> && std::is_lvalue_reference_v<std::invoke_result_t<F&&, Args&&...>> && std::convertible_to<std::invoke_result_t<F&&, Args&&...>, T&>
    void invoke(F&& f, Args&&... args) noexcept(std::is_nothrow_invocable_v<F&&, Args&&...>) {
        T& value = std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
        m_value = std::addressof(value);
    }

    /// @brief 保存一个兼容左值引用的地址。
    /// @tparam U 被引用对象的推导类型。
    /// @param value 必须是可转换为 `T&` 的左值；对象所有权不转移。
    template <typename U>
        requires std::is_lvalue_reference_v<U&&> && std::convertible_to<U&&, T&>
    void return_value(U&& value) noexcept {
        T& reference = static_cast<T&>(std::forward<U>(value));
        m_value = std::addressof(reference);
    }

    /// @brief 获取保存的左值引用。
    /// @return 被引用对象。
    /// @pre 已调用 `invoke()` 或 `return_value()`，且被引用对象仍然存活。
    [[nodiscard]] T& ref() const noexcept { TFL_ASSERT(m_value); return *m_value; }

    /// @brief 获取保存的左值引用；引用结果不存在消费操作。
    /// @return 与 `ref()` 相同的被引用对象。
    [[nodiscard]] T& take() const noexcept { return ref(); }

private:
    T* m_value{nullptr};
};

/// @brief 为 void 任务提供与普通结果槽一致的无状态完成接口。
///
/// 该特化只调用 callable，不分配或保存结果；`ref()` 和 `take()` 均为空操作。
template <>
class ResultSlot<void> {
public:
    /// @brief 调用返回 void 的 callable。
    /// @tparam F 返回类型严格为 void 的 callable 类型。
    /// @tparam Args 调用时传递给 callable 的参数类型。
    /// @param f 要调用的对象。
    /// @param args 转发给 callable 的参数。
    template <typename F, typename... Args>
        requires std::invocable<F&&, Args&&...> && std::same_as<std::invoke_result_t<F&&, Args&&...>, void>
    void invoke(F&& f, Args&&... args) noexcept(std::is_nothrow_invocable_v<F&&, Args&&...>) { std::invoke(std::forward<F>(f), std::forward<Args>(args)...); }

    /// @brief 标记 void 结果完成；无需保存任何值。
    static constexpr void return_void() noexcept {}

    /// @brief 访问 void 结果的占位操作。
    static constexpr void ref() noexcept {}

    /// @brief 取得 void 结果的占位操作。
    static constexpr void take() noexcept {}
};

} // namespace tfl
