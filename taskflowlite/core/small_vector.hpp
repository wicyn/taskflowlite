/// @file small_vector.hpp
/// @brief LLVM SmallVector 设计取向的独立单头文件实现；N == 0 时为纯堆模式。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-09-06
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @note 该文件是面向 TaskflowLite 的独立实现，不逐字包含 LLVM 源码。
///       设计参考 LLVM llvm/ADT/SmallVector.h 当前实现中的紧凑 size/capacity、
///       小型 trivial 类型按值传递、扩容冷路径、内部引用保护和 N == 0 优化等思路。
///       所有实现均集中于本头文件，不依赖 LLVM Support/ADT 组件。

#pragma once

#include <algorithm>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <version>

namespace tfl {
namespace detail {

#if defined(_MSC_VER)
#define TFL_SMALL_VECTOR_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define TFL_SMALL_VECTOR_NOINLINE __attribute__((noinline))
#else
#define TFL_SMALL_VECTOR_NOINLINE
#endif

/// @brief SmallVector 内部 size/capacity 的紧凑存储类型。
template <typename T>
using SmallVectorStorageSizeType = std::conditional_t<
    (sizeof(T) < 4 && sizeof(void*) >= 8),
    std::uint64_t,
    std::uint32_t
    >;

/// @brief SmallVector 的固定元数据。
///
/// 单独作为第一个私有基类，使 data/size/capacity 保持在对象头部；
/// InlineStorage 作为第二个私有基类，N == 0 时通过 EBO 不增加对象尺寸。
template <typename T, typename SizeType>
struct SmallVectorMetadata {
    T* m_data{nullptr};
    SizeType m_size{0};
    SizeType m_capacity{0};
};

/// @brief SmallVector 的内联原始存储。
template <typename T, std::size_t N>
struct SmallVectorInlineStorage {
    alignas(T) std::byte storage[sizeof(T) * N];

    [[nodiscard]] T* data() noexcept {
        return reinterpret_cast<T*>(storage);
    }

    [[nodiscard]] const T* data() const noexcept {
        return reinterpret_cast<const T*>(storage);
    }
};

/// @brief N == 0 时不在对象内部保留任何 T 存储。
template <typename T>
struct SmallVectorInlineStorage<T, 0> {
    [[nodiscard]] T* data() noexcept {
        return nullptr;
    }

    [[nodiscard]] const T* data() const noexcept {
        return nullptr;
    }
};

}  // namespace detail

/// @brief 带固定内联容量的连续动态数组。
///
/// SmallVector<T, N> 在元素数量不超过 N 时直接使用对象内部存储；容量超过 N 后
/// 转为堆存储。SmallVector<T, 0> 不包含元素内联缓冲区，行为类似紧凑版动态 vector。
///
/// 公开接口尽量保持与 std::vector 一致。内部默认使用 32 位 size/capacity 元数据；
/// 在 64 位平台上，对于 sizeof(T) < 4 的小元素自动改用 64 位元数据，避免将字节型
/// 容器限制在约 4 GiB。对于指针及常见 TaskflowLite 元素，N == 0 时基础元数据通常
/// 为 16 字节。
///
/// @tparam T 元素类型。
/// @tparam N 内联元素容量；N == 0 表示纯堆存储。
template <typename T, std::size_t N = 4>
class SmallVector final
    : private detail::SmallVectorMetadata<T, detail::SmallVectorStorageSizeType<T>>,
      private detail::SmallVectorInlineStorage<T, N> {
    template <typename, std::size_t>
    friend class SmallVector;

    using storage_size_type = detail::SmallVectorStorageSizeType<T>;
    using metadata_type = detail::SmallVectorMetadata<T, storage_size_type>;
    using inline_storage_type = detail::SmallVectorInlineStorage<T, N>;

    using metadata_type::m_data;
    using metadata_type::m_size;
    using metadata_type::m_capacity;
    using relocate_reference = decltype(std::move_if_noexcept(std::declval<T&>()));

    static constexpr bool can_relocate_v = std::is_constructible_v<T, relocate_reference>;
    static constexpr bool nothrow_relocate_v = std::is_nothrow_constructible_v<T, relocate_reference>;
    static constexpr bool trivial_relocate_v =
        std::is_trivially_copyable_v<T> &&
        std::is_trivially_move_constructible_v<T> &&
        std::is_trivially_destructible_v<T>;

    static_assert(std::is_object_v<T>, "SmallVector<T, N> requires an object type");
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>, "SmallVector<T, N> does not support cv-qualified T");
    static_assert(N <= std::numeric_limits<storage_size_type>::max(), "SmallVector inline capacity exceeds internal size type");
    static_assert(N <= std::numeric_limits<std::size_t>::max() / sizeof(T), "SmallVector inline storage size overflows size_t");

public:
    using value_type             = T;
    using size_type              = std::size_t;
    using difference_type        = std::ptrdiff_t;
    using reference              = T&;
    using const_reference        = const T&;
    using pointer                = T*;
    using const_pointer          = const T*;
    using iterator               = T*;
    using const_iterator         = const T*;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    static constexpr size_type inline_capacity = N;

    // ========================================================================
    // 构造 / 析构 / 赋值
    // ========================================================================

    SmallVector() noexcept {
        _reset_empty();
    }

    explicit SmallVector(size_type count)
        requires std::default_initializable<T>
        : SmallVector() {
        try {
            resize(count);
        }
        catch (...) {
            _cleanup_failed_construction();
            throw;
        }
    }

    SmallVector(size_type count, const T& value)
        requires std::is_copy_constructible_v<T>
        : SmallVector() {
        try {
            _construct_fill(count, value);
        }
        catch (...) {
            _cleanup_failed_construction();
            throw;
        }
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
        requires std::constructible_from<T, std::iter_reference_t<It>>
    SmallVector(It first, S last)
        : SmallVector() {
        try {
            _construct_range(first, last);
        }
        catch (...) {
            _cleanup_failed_construction();
            throw;
        }
    }

#if defined(__cpp_lib_containers_ranges) && __cpp_lib_containers_ranges >= 202202L
    template <std::ranges::input_range R>
        requires std::constructible_from<T, std::ranges::range_reference_t<R>>
    SmallVector(std::from_range_t, R&& range)
        : SmallVector(std::ranges::begin(range), std::ranges::end(range)) {
    }
#endif

    SmallVector(std::initializer_list<T> init)
        requires std::is_copy_constructible_v<T>
        : SmallVector(init.begin(), init.end()) {
    }

    SmallVector(const SmallVector& rhs)
        requires std::is_copy_constructible_v<T>
        : SmallVector() {
        try {
            _construct_range(rhs.begin(), rhs.end());
        }
        catch (...) {
            _cleanup_failed_construction();
            throw;
        }
    }

    SmallVector(const SmallVector&)
        requires (!std::is_copy_constructible_v<T>) = delete;

    template <std::size_t M>
        requires (M != N && std::is_copy_constructible_v<T>)
    SmallVector(const SmallVector<T, M>& rhs)
        : SmallVector() {
        try {
            _construct_range(rhs.begin(), rhs.end());
        }
        catch (...) {
            _cleanup_failed_construction();
            throw;
        }
    }

    SmallVector(SmallVector&& rhs) noexcept(N == 0 || nothrow_relocate_v)
        requires (N == 0 || can_relocate_v)
        : SmallVector() {
        if constexpr (N == 0 || nothrow_relocate_v) {
            _move_construct_from(rhs);
        }
        else {
            try {
                _move_construct_from(rhs);
            }
            catch (...) {
                _cleanup_failed_construction();
                throw;
            }
        }
    }

    template <std::size_t M>
        requires (M != N && (M == 0 || can_relocate_v))
    SmallVector(SmallVector<T, M>&& rhs) noexcept(M == 0 || (M <= N && nothrow_relocate_v))
        : SmallVector() {
        if constexpr (M == 0 || (M <= N && nothrow_relocate_v)) {
            _move_construct_from(rhs);
        }
        else {
            try {
                _move_construct_from(rhs);
            }
            catch (...) {
                _cleanup_failed_construction();
                throw;
            }
        }
    }

    ~SmallVector() noexcept {
        _destroy_n(m_data, _size_storage());
        _release_heap();
    }

    SmallVector& operator=(const SmallVector& rhs)
        requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>) {
        if (this != std::addressof(rhs)) {
            assign(rhs.begin(), rhs.end());
        }
        return *this;
    }

    SmallVector& operator=(const SmallVector&)
        requires (!(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>)) = delete;

    template <std::size_t M>
        requires (M != N && std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>)
    SmallVector& operator=(const SmallVector<T, M>& rhs) {
        assign(rhs.begin(), rhs.end());
        return *this;
    }

    SmallVector& operator=(SmallVector&& rhs) noexcept(N == 0 || nothrow_relocate_v)
        requires (N == 0 || can_relocate_v) {
        if (this != std::addressof(rhs)) {
            _move_assign_from(rhs);
        }
        return *this;
    }

    template <std::size_t M>
        requires (M != N && (M == 0 || can_relocate_v))
    SmallVector& operator=(SmallVector<T, M>&& rhs) noexcept(M == 0 || (M <= N && nothrow_relocate_v)) {
        _move_assign_from(rhs);
        return *this;
    }

    SmallVector& operator=(std::initializer_list<T> init)
        requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>) {
        assign(init.begin(), init.end());
        return *this;
    }

    // ========================================================================
    // Iterator
    // ========================================================================

    [[nodiscard]] iterator begin() noexcept {
        return m_data;
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return m_data;
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return m_data;
    }

    [[nodiscard]] iterator end() noexcept {
        if constexpr (N == 0) {
            return m_size == 0 ? m_data : m_data + m_size;
        }
        else {
            return m_data + m_size;
        }
    }

    [[nodiscard]] const_iterator end() const noexcept {
        if constexpr (N == 0) {
            return m_size == 0 ? m_data : m_data + m_size;
        }
        else {
            return m_data + m_size;
        }
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        if constexpr (N == 0) {
            return m_size == 0 ? m_data : m_data + m_size;
        }
        else {
            return m_data + m_size;
        }
    }

    [[nodiscard]] reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }

    [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }

    [[nodiscard]] const_reverse_iterator crbegin() const noexcept {
        return const_reverse_iterator(cend());
    }

    [[nodiscard]] reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }

    [[nodiscard]] const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    [[nodiscard]] const_reverse_iterator crend() const noexcept {
        return const_reverse_iterator(cbegin());
    }

    // ========================================================================
    // Capacity
    // ========================================================================

    [[nodiscard]] bool empty() const noexcept {
        return m_size == 0;
    }

    [[nodiscard]] size_type size() const noexcept {
        return static_cast<size_type>(m_size);
    }

    [[nodiscard]] size_type capacity() const noexcept {
        return static_cast<size_type>(m_capacity);
    }

    [[nodiscard]] size_type max_size() const noexcept {
        return _max_size();
    }

    void reserve(size_type new_capacity) {
        if (new_capacity <= capacity()) {
            return;
        }
        _reallocate(_checked_storage_size(new_capacity));
    }

    void shrink_to_fit() {
        if (!_is_heap()) {
            return;
        }

        if (m_size == 0) {
            _release_heap();
            _reset_empty();
            return;
        }

        if constexpr (N != 0) {
            if (m_size <= N) {
                T* target = _inline_data();
                _relocate_construct_n(target, m_data, m_size);
                _destroy_n(m_data, m_size);
                _deallocate_raw(m_data);
                m_data = target;
                m_capacity = static_cast<storage_size_type>(N);
                return;
            }
        }

        if (m_capacity != m_size) {
            _reallocate(m_size);
        }
    }

    // ========================================================================
    // Element access
    // ========================================================================

    [[nodiscard]] reference operator[](size_type pos) noexcept {
        assert(pos < size());
        return m_data[pos];
    }

    [[nodiscard]] const_reference operator[](size_type pos) const noexcept {
        assert(pos < size());
        return m_data[pos];
    }

    [[nodiscard]] reference at(size_type pos) {
        if (pos >= size()) [[unlikely]] {
            throw std::out_of_range("tfl::SmallVector::at");
        }
        return m_data[pos];
    }

    [[nodiscard]] const_reference at(size_type pos) const {
        if (pos >= size()) [[unlikely]] {
            throw std::out_of_range("tfl::SmallVector::at");
        }
        return m_data[pos];
    }

    [[nodiscard]] reference front() noexcept {
        assert(!empty());
        return m_data[0];
    }

    [[nodiscard]] const_reference front() const noexcept {
        assert(!empty());
        return m_data[0];
    }

    [[nodiscard]] reference back() noexcept {
        assert(!empty());
        return m_data[m_size - 1];
    }

    [[nodiscard]] const_reference back() const noexcept {
        assert(!empty());
        return m_data[m_size - 1];
    }

    [[nodiscard]] pointer data() noexcept {
        return m_data;
    }

    [[nodiscard]] const_pointer data() const noexcept {
        return m_data;
    }

    // ========================================================================
    // Modifiers
    // ========================================================================

    void clear() noexcept {
        _destroy_n(m_data, m_size);
        m_size = 0;
    }

    template <typename... Args>
        requires std::constructible_from<T, Args...>
    reference emplace_back(Args&&... args) {
        if (m_size < m_capacity) [[likely]] {
            T* ptr = std::construct_at(m_data + m_size, std::forward<Args>(args)...);
            ++m_size;
            return *ptr;
        }
        return _grow_and_emplace_back(std::forward<Args>(args)...);
    }

    void push_back(const T& value)
        requires std::is_copy_constructible_v<T> {
        emplace_back(value);
    }

    void push_back(T&& value)
        requires std::is_move_constructible_v<T> {
        emplace_back(std::move(value));
    }

    template <typename... Args>
        requires (std::constructible_from<T, Args...> && can_relocate_v && std::is_move_assignable_v<T>)
    iterator emplace(const_iterator pos, Args&&... args) {
        const storage_size_type index = _index_of(pos);

        if (index == m_size) {
            emplace_back(std::forward<Args>(args)...);
            return m_data + index;
        }

        if (m_size == m_capacity) {
            return _grow_and_emplace_at(index, std::forward<Args>(args)...);
        }

        T temporary(std::forward<Args>(args)...);

        if constexpr (trivial_relocate_v) {
            std::memmove(
                m_data + index + 1,
                m_data + index,
                static_cast<size_type>(m_size - index) * sizeof(T)
                );
            std::memcpy(m_data + index, std::addressof(temporary), sizeof(T));
            ++m_size;
        }
        else {
            std::construct_at(m_data + m_size, std::move_if_noexcept(m_data[m_size - 1]));
            ++m_size;
            std::move_backward(m_data + index, m_data + m_size - 2, m_data + m_size - 1);
            m_data[index] = std::move(temporary);
        }

        return m_data + index;
    }

    iterator insert(const_iterator pos, const T& value)
        requires (std::is_copy_constructible_v<T> && can_relocate_v && std::is_move_assignable_v<T>) {
        return emplace(pos, value);
    }

    iterator insert(const_iterator pos, T&& value)
        requires (std::is_move_constructible_v<T> && can_relocate_v && std::is_move_assignable_v<T>) {
        return emplace(pos, std::move(value));
    }

    iterator insert(const_iterator pos, size_type count, const T& value)
        requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T> && can_relocate_v && std::is_move_assignable_v<T>) {
        const storage_size_type index = _index_of(pos);
        if (count == 0) {
            return _ptr_at(index);
        }

        const storage_size_type insert_count = _checked_storage_size(count);
        if (_contains_address(std::addressof(value))) {
            T temporary(value);
            return _insert_fill(index, insert_count, temporary);
        }
        return _insert_fill(index, insert_count, value);
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
        requires (
            std::constructible_from<T, std::iter_reference_t<It>> &&
            std::is_assignable_v<T&, std::iter_reference_t<It>> &&
            can_relocate_v &&
            std::is_move_assignable_v<T>
            )
    iterator insert(const_iterator pos, It first, S last) {
        const storage_size_type index = _index_of(pos);

        if (first == last) {
            return _ptr_at(index);
        }

        if constexpr (std::forward_iterator<It>) {
            if (_self_iterator_range(first, last)) {
                SmallVector<T, 0> temporary(first, last);
                return _insert_temporary(index, temporary);
            }

            const storage_size_type count = _checked_distance(std::ranges::distance(first, last));
            return _insert_forward(index, first, count);
        }
        else {
            SmallVector<T, 0> temporary(first, last);
            return _insert_temporary(index, temporary);
        }
    }

    iterator insert(const_iterator pos, std::initializer_list<T> init)
        requires (
            std::is_copy_constructible_v<T> &&
            std::is_copy_assignable_v<T> &&
            can_relocate_v &&
            std::is_move_assignable_v<T>
            ) {
        return insert(pos, init.begin(), init.end());
    }

    template <std::ranges::input_range R>
        requires (
            std::constructible_from<T, std::ranges::range_reference_t<R>> &&
            std::is_assignable_v<T&, std::ranges::range_reference_t<R>> &&
            can_relocate_v &&
            std::is_move_assignable_v<T>
            )
    iterator insert_range(const_iterator pos, R&& range) {
        return insert(pos, std::ranges::begin(range), std::ranges::end(range));
    }

    iterator erase(const_iterator pos)
        requires std::is_move_assignable_v<T> {
        const storage_size_type index = _index_of(pos);
        assert(index < m_size);
        return erase(pos, _ptr_at(index + 1));
    }

    iterator erase(const_iterator first, const_iterator last)
        requires std::is_move_assignable_v<T> {
        const storage_size_type first_index = _index_of(first);
        const storage_size_type last_index = _index_of(last);
        assert(first_index <= last_index);

        const storage_size_type count = last_index - first_index;
        if (count == 0) {
            return _ptr_at(first_index);
        }

        const storage_size_type tail = m_size - last_index;

        if constexpr (trivial_relocate_v) {
            if (tail != 0) {
                std::memmove(
                    m_data + first_index,
                    m_data + last_index,
                    static_cast<size_type>(tail) * sizeof(T)
                    );
            }
        }
        else {
            std::move(m_data + last_index, m_data + m_size, m_data + first_index);
            _destroy_n(m_data + (m_size - count), count);
        }

        m_size -= count;
        return _ptr_at(first_index);
    }

    void pop_back() noexcept {
        assert(!empty());
        --m_size;
        if constexpr (!std::is_trivially_destructible_v<T>) {
            std::destroy_at(m_data + m_size);
        }
    }

    void resize(size_type count)
        requires std::default_initializable<T> {
        const storage_size_type new_size = _checked_storage_size(count);

        if (new_size < m_size) {
            _destroy_n(m_data + new_size, m_size - new_size);
            m_size = new_size;
            return;
        }

        if (new_size == m_size) {
            return;
        }

        reserve(count);
        const storage_size_type old_size = m_size;
        try {
            while (m_size < new_size) {
                std::construct_at(m_data + m_size);
                ++m_size;
            }
        }
        catch (...) {
            _destroy_n(m_data + old_size, m_size - old_size);
            m_size = old_size;
            throw;
        }
    }

    void resize(size_type count, const T& value)
        requires std::is_copy_constructible_v<T> {
        const storage_size_type new_size = _checked_storage_size(count);

        if (new_size < m_size) {
            _destroy_n(m_data + new_size, m_size - new_size);
            m_size = new_size;
            return;
        }

        if (new_size == m_size) {
            return;
        }

        if (new_size > m_capacity && _contains_address(std::addressof(value))) {
            T temporary(value);
            reserve(count);
            _append_fill_unchecked(new_size - m_size, temporary);
            return;
        }

        reserve(count);
        _append_fill_unchecked(new_size - m_size, value);
    }

    /// @brief 调整大小；新增元素使用 default-initialization。
    ///
    /// 对标 LLVM SmallVector::resize_for_overwrite：对于标量类型，新元素不会被
    /// value-initialize；对于类类型仍会调用默认构造函数。
    void resize_for_overwrite(size_type count)
        requires std::default_initializable<T> {
        const storage_size_type new_size = _checked_storage_size(count);
        if (new_size < m_size) {
            truncate(count);
            return;
        }
        if (new_size == m_size) {
            return;
        }
        if (new_size > m_capacity) {
            reserve(count);
        }

        storage_size_type constructed = 0;
        try {
            for (; m_size + constructed < new_size; ++constructed) {
                ::new (static_cast<void*>(m_data + m_size + constructed)) T;
            }
        }
        catch (...) {
            _destroy_n(m_data + m_size, constructed);
            throw;
        }
        m_size = new_size;
    }

    /// @brief 将 size 缩小到 count，不允许增加 size。
    void truncate(size_type count) noexcept {
        const storage_size_type new_size = static_cast<storage_size_type>(count);
        assert(count <= size());
        _destroy_n(m_data + new_size, m_size - new_size);
        m_size = new_size;
    }

    /// @brief 一次移除末尾 count 个元素。
    void pop_back_n(size_type count) noexcept {
        assert(count <= size());
        truncate(size() - count);
    }


    void swap(SmallVector& rhs) noexcept(N == 0 || nothrow_relocate_v)
        requires (N == 0 || can_relocate_v) {
        if (this == std::addressof(rhs)) {
            return;
        }

        if (_is_heap() && rhs._is_heap()) {
            std::swap(m_data, rhs.m_data);
            std::swap(m_size, rhs.m_size);
            std::swap(m_capacity, rhs.m_capacity);
            return;
        }

        SmallVector temporary(std::move(*this));
        *this = std::move(rhs);
        rhs = std::move(temporary);
    }

    // ========================================================================
    // Assign
    // ========================================================================

    void assign(size_type count, const T& value)
        requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>) {
        const storage_size_type new_size = _checked_storage_size(count);

        if (_contains_address(std::addressof(value))) {
            T temporary(value);
            _assign_fill(new_size, temporary);
            return;
        }
        _assign_fill(new_size, value);
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
        requires (
            std::constructible_from<T, std::iter_reference_t<It>> &&
            std::is_assignable_v<T&, std::iter_reference_t<It>>
            )
    void assign(It first, S last) {
        if (_self_iterator_range(first, last)) {
            SmallVector<T, 0> temporary(first, last);
            assign(temporary.begin(), temporary.end());
            return;
        }

        if constexpr (std::forward_iterator<It>) {
            const storage_size_type count = _checked_distance(std::ranges::distance(first, last));
            _assign_forward(first, count);
        }
        else {
            _assign_input(first, last);
        }
    }

    void assign(std::initializer_list<T> init)
        requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>) {
        assign(init.begin(), init.end());
    }

    template <std::ranges::input_range R>
        requires (
            std::constructible_from<T, std::ranges::range_reference_t<R>> &&
            std::is_assignable_v<T&, std::ranges::range_reference_t<R>>
            )
    void assign_range(R&& range) {
        assign(std::ranges::begin(range), std::ranges::end(range));
    }

    // ========================================================================
    // C++23 range append + LLVM-compatible append extension
    // ========================================================================

    template <std::ranges::input_range R>
        requires std::constructible_from<T, std::ranges::range_reference_t<R>>
    void append_range(R&& range) {
        append(std::ranges::begin(range), std::ranges::end(range));
    }

    void append(size_type count, const T& value)
        requires std::is_copy_constructible_v<T> {
        if (count == 0) {
            return;
        }

        const storage_size_type append_count = _checked_storage_size(count);
        const storage_size_type new_size = _checked_add(m_size, append_count);

        if (new_size > m_capacity && _contains_address(std::addressof(value))) {
            T temporary(value);
            reserve(new_size);
            _append_fill_unchecked(append_count, temporary);
            return;
        }

        reserve(new_size);
        _append_fill_unchecked(append_count, value);
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
        requires std::constructible_from<T, std::iter_reference_t<It>>
    void append(It first, S last) {
        if (first == last) {
            return;
        }

        if (_self_iterator_range(first, last)) {
            SmallVector<T, 0> temporary(first, last);
            append(temporary.begin(), temporary.end());
            return;
        }

        if constexpr (std::forward_iterator<It>) {
            const storage_size_type count = _checked_distance(std::ranges::distance(first, last));
            reserve(_checked_add(m_size, count));
        }

        for (; first != last; ++first) {
            emplace_back(*first);
        }
    }

    void append(std::initializer_list<T> init)
        requires std::is_copy_constructible_v<T> {
        append(init.begin(), init.end());
    }

    // ========================================================================
    // TaskflowLite / LLVM-style extensions
    // ========================================================================

    [[nodiscard]] bool is_inline() const noexcept {
        return _is_inline();
    }

    [[nodiscard]] bool is_heap() const noexcept {
        return _is_heap();
    }

    [[nodiscard]] size_type size_in_bytes() const noexcept {
        return size() * sizeof(T);
    }

    [[nodiscard]] size_type capacity_in_bytes() const noexcept {
        return capacity() * sizeof(T);
    }

    [[nodiscard]] T pop_back_val()
        requires std::is_move_constructible_v<T> {
        T value(std::move(back()));
        pop_back();
        return value;
    }

    /// @brief 仅修改逻辑大小，不构造或析构元素。
    /// @warning 当 count > size() 时，调用者必须已经在对应槽位建立 T 的对象生命周期。
    void unsafe_set_size(size_type count) noexcept {
        assert(count <= capacity());
        assert(count <= std::numeric_limits<storage_size_type>::max());
        m_size = static_cast<storage_size_type>(count);
    }

private:
    // ========================================================================
    // 基础状态 / 容量
    // ========================================================================

    [[nodiscard]] T* _inline_data() noexcept {
        return inline_storage_type::data();
    }

    [[nodiscard]] const T* _inline_data() const noexcept {
        return inline_storage_type::data();
    }

    [[nodiscard]] bool _is_inline() const noexcept {
        if constexpr (N == 0) {
            return false;
        }
        else {
            return m_data == _inline_data();
        }
    }

    [[nodiscard]] bool _is_heap() const noexcept {
        return m_data != nullptr && !_is_inline();
    }

    [[nodiscard]] storage_size_type _size_storage() const noexcept {
        return m_size;
    }

    void _reset_empty() noexcept {
        m_data = _inline_data();
        m_size = 0;
        m_capacity = static_cast<storage_size_type>(N);
    }

    void _cleanup_failed_construction() noexcept {
        _destroy_n(m_data, m_size);
        m_size = 0;
        _release_heap();
        _reset_empty();
    }

    [[nodiscard]] static constexpr size_type _max_size() noexcept {
        constexpr size_type size_limit = std::numeric_limits<size_type>::max() / sizeof(T);
        constexpr size_type storage_limit = std::numeric_limits<storage_size_type>::max();
        return size_limit < storage_limit ? size_limit : storage_limit;
    }

    [[noreturn]] static void _throw_length_error() {
        throw std::length_error("tfl::SmallVector capacity exceeds max_size()");
    }

    [[nodiscard]] static storage_size_type _checked_storage_size(size_type value) {
        if (value > _max_size()) [[unlikely]] {
            _throw_length_error();
        }
        return static_cast<storage_size_type>(value);
    }

    [[nodiscard]] static storage_size_type _checked_add(storage_size_type lhs, storage_size_type rhs) {
        const size_type maximum = _max_size();
        if (static_cast<size_type>(rhs) > maximum - static_cast<size_type>(lhs)) [[unlikely]] {
            _throw_length_error();
        }
        return static_cast<storage_size_type>(lhs + rhs);
    }

    template <typename D>
    [[nodiscard]] static storage_size_type _checked_distance(D distance) {
        if constexpr (std::is_signed_v<D>) {
            if (distance < 0) [[unlikely]] {
                _throw_length_error();
            }
        }
        using U = std::make_unsigned_t<D>;
        const auto value = static_cast<U>(distance);
        if (value > _max_size()) [[unlikely]] {
            _throw_length_error();
        }
        return static_cast<storage_size_type>(value);
    }

    [[nodiscard]] static storage_size_type _next_capacity(storage_size_type current, storage_size_type required) {
        if (required > _max_size()) [[unlikely]] {
            _throw_length_error();
        }

        const size_type maximum = _max_size();
        const size_type current_size = static_cast<size_type>(current);
        const size_type grown = current_size > (maximum - 1) / 2
                                    ? maximum
                                    : current_size * 2 + 1;
        const size_type next = std::max(grown, static_cast<size_type>(required));
        return static_cast<storage_size_type>(next);
    }

    [[nodiscard]] static size_type _bytes(storage_size_type capacity) noexcept {
        return static_cast<size_type>(capacity) * sizeof(T);
    }

    // ========================================================================
    // 指针 / iterator 验证
    // ========================================================================

    [[nodiscard]] iterator _ptr_at(storage_size_type index) noexcept {
        assert(index <= m_size);
        return index == 0 && m_data == nullptr ? nullptr : m_data + index;
    }

    [[nodiscard]] const_iterator _ptr_at(storage_size_type index) const noexcept {
        assert(index <= m_size);
        return index == 0 && m_data == nullptr ? nullptr : m_data + index;
    }

    [[nodiscard]] storage_size_type _index_of(const_iterator pos) const noexcept {
        if (m_size == 0) {
            assert(pos == m_data);
            return 0;
        }

        const std::less<const T*> less{};
        assert(!less(pos, m_data));
        assert(!less(m_data + m_size, pos));
        return static_cast<storage_size_type>(pos - m_data);
    }

    [[nodiscard]] bool _contains_address(const T* ptr) const noexcept {
        if (ptr == nullptr || m_size == 0) {
            return false;
        }

        const std::less<const T*> less{};
        return !less(ptr, m_data) && less(ptr, m_data + m_size);
    }

    template <typename It>
    [[nodiscard]] static auto _iterator_pointer(const It& it) noexcept -> std::pair<bool, const T*> {
        using I = std::remove_cvref_t<It>;

        if constexpr (
            std::is_pointer_v<I> &&
            std::same_as<std::remove_cv_t<std::remove_pointer_t<I>>, T>
            ) {
            return {true, it};
        }
        else if constexpr (requires { it.base(); }) {
            return _iterator_pointer(it.base());
        }
        else {
            return {false, nullptr};
        }
    }

    template <typename It, typename S>
    [[nodiscard]] bool _self_iterator_range(const It& first, const S& last) const noexcept {
        const auto [has_first, first_ptr] = _iterator_pointer(first);
        const auto [has_last, last_ptr] = _iterator_pointer(last);
        if (!has_first || !has_last) {
            return false;
        }

        if (m_size == 0) {
            return first_ptr == m_data && last_ptr == m_data;
        }

        const std::less<const T*> less{};
        const T* finish = m_data + m_size;
        const bool first_inside = !less(first_ptr, m_data) && !less(finish, first_ptr);
        const bool last_inside = !less(last_ptr, m_data) && !less(finish, last_ptr);
        return first_inside && last_inside;
    }

    // ========================================================================
    // 原始内存管理
    // ========================================================================

    [[nodiscard]] static T* _allocate_raw(storage_size_type capacity) {
        if (capacity == 0) {
            return nullptr;
        }

        const size_type bytes = _bytes(capacity);
        void* ptr = nullptr;

        if constexpr (alignof(T) <= alignof(std::max_align_t)) {
            ptr = std::malloc(bytes);
            if (ptr == nullptr) [[unlikely]] {
                throw std::bad_alloc{};
            }
        }
        else {
            ptr = ::operator new(bytes, std::align_val_t{alignof(T)});
        }

        return static_cast<T*>(ptr);
    }

    static void _deallocate_raw(T* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }

        if constexpr (alignof(T) <= alignof(std::max_align_t)) {
            std::free(ptr);
        }
        else {
            ::operator delete(ptr, std::align_val_t{alignof(T)});
        }
    }

    void _release_heap() noexcept {
        if (_is_heap()) {
            _deallocate_raw(m_data);
        }
    }

    static void _destroy_n(T* ptr, storage_size_type count) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            while (count != 0) {
                --count;
                std::destroy_at(ptr + count);
            }
        }
    }

    static void _relocate_construct_n(T* destination, T* source, storage_size_type count) {
        if (count == 0) {
            return;
        }

        if constexpr (trivial_relocate_v) {
            std::memcpy(destination, source, static_cast<size_type>(count) * sizeof(T));
        }
        else {
            storage_size_type constructed = 0;
            try {
                for (; constructed < count; ++constructed) {
                    std::construct_at(destination + constructed, std::move_if_noexcept(source[constructed]));
                }
            }
            catch (...) {
                _destroy_n(destination, constructed);
                throw;
            }
        }
    }

    template <typename It>
    static void _copy_construct_n(T* destination, It first, storage_size_type count) {
        storage_size_type constructed = 0;
        try {
            for (; constructed < count; ++constructed, ++first) {
                std::construct_at(destination + constructed, *first);
            }
        }
        catch (...) {
            _destroy_n(destination, constructed);
            throw;
        }
    }

    static void _fill_construct_n(T* destination, storage_size_type count, const T& value) {
        storage_size_type constructed = 0;
        try {
            for (; constructed < count; ++constructed) {
                std::construct_at(destination + constructed, value);
            }
        }
        catch (...) {
            _destroy_n(destination, constructed);
            throw;
        }
    }

    TFL_SMALL_VECTOR_NOINLINE void _reallocate(storage_size_type new_capacity) {
        assert(new_capacity >= m_size);
        assert(new_capacity != m_capacity);

        if constexpr (trivial_relocate_v && alignof(T) <= alignof(std::max_align_t)) {
            if (_is_heap()) {
                void* ptr = std::realloc(m_data, _bytes(new_capacity));
                if (ptr == nullptr) [[unlikely]] {
                    throw std::bad_alloc{};
                }
                m_data = static_cast<T*>(ptr);
                m_capacity = new_capacity;
                return;
            }
        }

        T* new_data = _allocate_raw(new_capacity);
        try {
            _relocate_construct_n(new_data, m_data, m_size);
        }
        catch (...) {
            _deallocate_raw(new_data);
            throw;
        }

        _destroy_n(m_data, m_size);
        _release_heap();
        m_data = new_data;
        m_capacity = new_capacity;
    }

    // ========================================================================
    // 构造辅助
    // ========================================================================

    void _construct_fill(size_type count, const T& value) {
        const storage_size_type new_size = _checked_storage_size(count);
        if (new_size > m_capacity) {
            m_data = _allocate_raw(new_size);
            m_capacity = new_size;
        }
        _fill_construct_n(m_data, new_size, value);
        m_size = new_size;
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
    void _construct_range(It first, S last) {
        if constexpr (std::forward_iterator<It>) {
            const storage_size_type count = _checked_distance(std::ranges::distance(first, last));
            if (count > m_capacity) {
                m_data = _allocate_raw(count);
                m_capacity = count;
            }
            _copy_construct_n(m_data, first, count);
            m_size = count;
        }
        else {
            for (; first != last; ++first) {
                emplace_back(*first);
            }
        }
    }

    // ========================================================================
    // grow + emplace/insert 冷路径
    // ========================================================================

    template <typename... Args>
    TFL_SMALL_VECTOR_NOINLINE reference _grow_and_emplace_back(Args&&... args) {
        const storage_size_type old_size = m_size;
        const storage_size_type new_size = _checked_add(old_size, 1);
        const storage_size_type new_capacity = _next_capacity(m_capacity, new_size);
        T* new_data = _allocate_raw(new_capacity);

        bool inserted = false;
        try {
            // 先构造新增元素，确保 args 引用当前 storage 时不会被 grow 失效。
            std::construct_at(new_data + old_size, std::forward<Args>(args)...);
            inserted = true;
            _relocate_construct_n(new_data, m_data, old_size);
        }
        catch (...) {
            if (inserted) {
                _destroy_n(new_data + old_size, 1);
            }
            _deallocate_raw(new_data);
            throw;
        }

        _destroy_n(m_data, old_size);
        _release_heap();
        m_data = new_data;
        m_size = new_size;
        m_capacity = new_capacity;
        return m_data[old_size];
    }

    template <typename... Args>
    TFL_SMALL_VECTOR_NOINLINE iterator _grow_and_emplace_at(storage_size_type index, Args&&... args) {
        const storage_size_type old_size = m_size;
        const storage_size_type new_size = _checked_add(old_size, 1);
        const storage_size_type new_capacity = _next_capacity(m_capacity, new_size);
        T* new_data = _allocate_raw(new_capacity);

        bool inserted = false;
        bool prefix_done = false;
        try {
            // 同样先构造插入元素，避免 args 指向旧 storage 时失效。
            std::construct_at(new_data + index, std::forward<Args>(args)...);
            inserted = true;
            _relocate_construct_n(new_data, m_data, index);
            prefix_done = true;
            _relocate_construct_n(new_data + index + 1, m_data + index, old_size - index);
        }
        catch (...) {
            if (prefix_done) {
                _destroy_n(new_data, index);
            }
            if (inserted) {
                _destroy_n(new_data + index, 1);
            }
            _deallocate_raw(new_data);
            throw;
        }

        _destroy_n(m_data, old_size);
        _release_heap();
        m_data = new_data;
        m_size = new_size;
        m_capacity = new_capacity;
        return m_data + index;
    }

    TFL_SMALL_VECTOR_NOINLINE iterator _grow_and_insert_fill(
        storage_size_type index,
        storage_size_type count,
        const T& value
        ) {
        const storage_size_type old_size = m_size;
        const storage_size_type new_size = _checked_add(old_size, count);
        const storage_size_type new_capacity = _next_capacity(m_capacity, new_size);
        T* new_data = _allocate_raw(new_capacity);

        bool fill_done = false;
        bool prefix_done = false;
        try {
            _fill_construct_n(new_data + index, count, value);
            fill_done = true;
            _relocate_construct_n(new_data, m_data, index);
            prefix_done = true;
            _relocate_construct_n(new_data + index + count, m_data + index, old_size - index);
        }
        catch (...) {
            if (prefix_done) {
                _destroy_n(new_data, index);
            }
            if (fill_done) {
                _destroy_n(new_data + index, count);
            }
            _deallocate_raw(new_data);
            throw;
        }

        _destroy_n(m_data, old_size);
        _release_heap();
        m_data = new_data;
        m_size = new_size;
        m_capacity = new_capacity;
        return m_data + index;
    }

    template <std::forward_iterator It>
    TFL_SMALL_VECTOR_NOINLINE iterator _grow_and_insert_forward(
        storage_size_type index,
        It first,
        storage_size_type count
        ) {
        const storage_size_type old_size = m_size;
        const storage_size_type new_size = _checked_add(old_size, count);
        const storage_size_type new_capacity = _next_capacity(m_capacity, new_size);
        T* new_data = _allocate_raw(new_capacity);

        bool range_done = false;
        bool prefix_done = false;
        try {
            _copy_construct_n(new_data + index, first, count);
            range_done = true;
            _relocate_construct_n(new_data, m_data, index);
            prefix_done = true;
            _relocate_construct_n(new_data + index + count, m_data + index, old_size - index);
        }
        catch (...) {
            if (prefix_done) {
                _destroy_n(new_data, index);
            }
            if (range_done) {
                _destroy_n(new_data + index, count);
            }
            _deallocate_raw(new_data);
            throw;
        }

        _destroy_n(m_data, old_size);
        _release_heap();
        m_data = new_data;
        m_size = new_size;
        m_capacity = new_capacity;
        return m_data + index;
    }

    // ========================================================================
    // insert 辅助
    // ========================================================================

    iterator _insert_fill(storage_size_type index, storage_size_type count, const T& value) {
        const storage_size_type old_size = m_size;
        const storage_size_type new_size = _checked_add(old_size, count);

        if (new_size > m_capacity) {
            return _grow_and_insert_fill(index, count, value);
        }

        const storage_size_type tail = old_size - index;

        if constexpr (trivial_relocate_v) {
            if (tail != 0) {
                std::memmove(
                    m_data + index + count,
                    m_data + index,
                    static_cast<size_type>(tail) * sizeof(T)
                    );
            }
            std::fill_n(m_data + index, count, value);
            m_size = new_size;
            return m_data + index;
        }

        if (count <= tail) {
            _relocate_construct_n(m_data + old_size, m_data + old_size - count, count);
            m_size = new_size;
            std::move_backward(m_data + index, m_data + old_size - count, m_data + old_size);
            std::fill_n(m_data + index, count, value);
            return m_data + index;
        }

        const storage_size_type extra = count - tail;
        storage_size_type extra_constructed = 0;
        try {
            for (; extra_constructed < extra; ++extra_constructed) {
                std::construct_at(m_data + old_size + extra_constructed, value);
            }
            _relocate_construct_n(m_data + old_size + extra, m_data + index, tail);
        }
        catch (...) {
            _destroy_n(m_data + old_size, extra_constructed);
            throw;
        }

        m_size = new_size;
        std::fill_n(m_data + index, tail, value);
        return m_data + index;
    }

    template <std::forward_iterator It>
    iterator _insert_forward(storage_size_type index, It first, storage_size_type count) {
        if (count == 0) {
            return _ptr_at(index);
        }

        const storage_size_type old_size = m_size;
        const storage_size_type new_size = _checked_add(old_size, count);

        if (new_size > m_capacity) {
            return _grow_and_insert_forward(index, first, count);
        }

        const storage_size_type tail = old_size - index;

        if constexpr (trivial_relocate_v) {
            if (tail != 0) {
                std::memmove(
                    m_data + index + count,
                    m_data + index,
                    static_cast<size_type>(tail) * sizeof(T)
                    );
            }
            for (storage_size_type i = 0; i < count; ++i, ++first) {
                m_data[index + i] = *first;
            }
            m_size = new_size;
            return m_data + index;
        }

        if (count <= tail) {
            _relocate_construct_n(m_data + old_size, m_data + old_size - count, count);
            m_size = new_size;
            std::move_backward(m_data + index, m_data + old_size - count, m_data + old_size);
            for (storage_size_type i = 0; i < count; ++i, ++first) {
                m_data[index + i] = *first;
            }
            return m_data + index;
        }

        const storage_size_type extra = count - tail;
        It middle = first;
        std::ranges::advance(middle, static_cast<difference_type>(tail));

        storage_size_type extra_constructed = 0;
        try {
            It it = middle;
            for (; extra_constructed < extra; ++extra_constructed, ++it) {
                std::construct_at(m_data + old_size + extra_constructed, *it);
            }
            _relocate_construct_n(m_data + old_size + extra, m_data + index, tail);
        }
        catch (...) {
            _destroy_n(m_data + old_size, extra_constructed);
            throw;
        }

        m_size = new_size;
        for (storage_size_type i = 0; i < tail; ++i, ++first) {
            m_data[index + i] = *first;
        }
        return m_data + index;
    }

    iterator _insert_temporary(storage_size_type index, SmallVector<T, 0>& temporary) {
        if constexpr (std::is_constructible_v<T, T&&> && std::is_assignable_v<T&, T&&>) {
            return _insert_forward(index, std::make_move_iterator(temporary.begin()), temporary.m_size);
        }
        else {
            return _insert_forward(index, temporary.begin(), temporary.m_size);
        }
    }

    // ========================================================================
    // assign / append 辅助
    // ========================================================================

    void _append_fill_unchecked(storage_size_type count, const T& value) {
        const storage_size_type old_size = m_size;
        try {
            while (count != 0) {
                std::construct_at(m_data + m_size, value);
                ++m_size;
                --count;
            }
        }
        catch (...) {
            _destroy_n(m_data + old_size, m_size - old_size);
            m_size = old_size;
            throw;
        }
    }

    void _assign_fill(storage_size_type count, const T& value) {
        if (count > m_capacity) {
            T* new_data = _allocate_raw(count);
            try {
                _fill_construct_n(new_data, count, value);
            }
            catch (...) {
                _deallocate_raw(new_data);
                throw;
            }

            _destroy_n(m_data, m_size);
            _release_heap();
            m_data = new_data;
            m_size = count;
            m_capacity = count;
            return;
        }

        const storage_size_type common = std::min(m_size, count);
        std::fill_n(m_data, common, value);

        if (count < m_size) {
            _destroy_n(m_data + count, m_size - count);
            m_size = count;
            return;
        }

        if (count > m_size) {
            const storage_size_type old_size = m_size;
            try {
                for (; m_size < count; ++m_size) {
                    std::construct_at(m_data + m_size, value);
                }
            }
            catch (...) {
                _destroy_n(m_data + old_size, m_size - old_size);
                m_size = old_size;
                throw;
            }
        }
    }

    template <std::forward_iterator It>
    void _assign_forward(It first, storage_size_type count) {
        if (count > m_capacity) {
            T* new_data = _allocate_raw(count);
            try {
                _copy_construct_n(new_data, first, count);
            }
            catch (...) {
                _deallocate_raw(new_data);
                throw;
            }

            _destroy_n(m_data, m_size);
            _release_heap();
            m_data = new_data;
            m_size = count;
            m_capacity = count;
            return;
        }

        const storage_size_type common = std::min(m_size, count);
        storage_size_type i = 0;
        for (; i < common; ++i, ++first) {
            m_data[i] = *first;
        }

        if (count < m_size) {
            _destroy_n(m_data + count, m_size - count);
            m_size = count;
            return;
        }

        const storage_size_type old_size = m_size;
        try {
            for (; i < count; ++i, ++first) {
                std::construct_at(m_data + i, *first);
                ++m_size;
            }
        }
        catch (...) {
            _destroy_n(m_data + old_size, m_size - old_size);
            m_size = old_size;
            throw;
        }
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
    void _assign_input(It first, S last) {
        storage_size_type i = 0;
        while (i < m_size && first != last) {
            m_data[i] = *first;
            ++i;
            ++first;
        }

        if (first == last) {
            _destroy_n(m_data + i, m_size - i);
            m_size = i;
            return;
        }

        m_size = i;
        for (; first != last; ++first) {
            emplace_back(*first);
        }
    }

    // ========================================================================
    // move 辅助
    // ========================================================================

    template <std::size_t M>
    void _move_construct_from(SmallVector<T, M>& rhs) {
        if constexpr (M == 0) {
            m_data = rhs.m_data;
            m_size = rhs.m_size;
            m_capacity = rhs.m_capacity;
            rhs._reset_empty();
        }
        else {
            if (rhs._is_heap()) {
                m_data = rhs.m_data;
                m_size = rhs.m_size;
                m_capacity = rhs.m_capacity;
                rhs._reset_empty();
                return;
            }

            const storage_size_type count = rhs.m_size;
            if (count > m_capacity) {
                m_data = _allocate_raw(count);
                m_capacity = count;
            }

            _relocate_construct_n(m_data, rhs.m_data, count);
            m_size = count;
            rhs.clear();
        }
    }

    template <std::size_t M>
    void _move_assign_from(SmallVector<T, M>& rhs) {
        if constexpr (M == 0) {
            _destroy_n(m_data, m_size);
            _release_heap();

            m_data = rhs.m_data;
            m_size = rhs.m_size;
            m_capacity = rhs.m_capacity;
            rhs._reset_empty();
        }
        else {
            if (rhs._is_heap()) {
                _destroy_n(m_data, m_size);
                _release_heap();

                m_data = rhs.m_data;
                m_size = rhs.m_size;
                m_capacity = rhs.m_capacity;
                rhs._reset_empty();
                return;
            }

            _destroy_n(m_data, m_size);
            m_size = 0;

            const storage_size_type count = rhs.m_size;
            if (count > m_capacity) {
                _release_heap();
                _reset_empty();
                if (count > m_capacity) {
                    m_data = _allocate_raw(count);
                    m_capacity = count;
                }
            }

            _relocate_construct_n(m_data, rhs.m_data, count);
            m_size = count;
            rhs.clear();
        }
    }
};

// ============================================================================
// 非成员比较 / swap / erase
// ============================================================================

template <typename T, std::size_t N, std::size_t M>
[[nodiscard]] bool operator==(const SmallVector<T, N>& lhs, const SmallVector<T, M>& rhs)
    requires std::equality_comparable<T> {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

template <typename T, std::size_t N, std::size_t M>
[[nodiscard]] auto operator<=>(const SmallVector<T, N>& lhs, const SmallVector<T, M>& rhs)
    requires std::three_way_comparable<T> {
    return std::lexicographical_compare_three_way(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::compare_three_way{}
        );
}

template <typename T, std::size_t N>
void swap(SmallVector<T, N>& lhs, SmallVector<T, N>& rhs) noexcept(noexcept(lhs.swap(rhs)))
    requires requires { lhs.swap(rhs); } {
    lhs.swap(rhs);
}

template <typename T, std::size_t N, typename U = T>
typename SmallVector<T, N>::size_type erase(SmallVector<T, N>& vector, const U& value)
    requires std::is_move_assignable_v<T> {
    const auto old_size = vector.size();
    const auto new_end = std::remove(vector.begin(), vector.end(), value);
    vector.erase(new_end, vector.end());
    return old_size - vector.size();
}

template <typename T, std::size_t N, typename Pred>
typename SmallVector<T, N>::size_type erase_if(SmallVector<T, N>& vector, Pred pred)
    requires std::is_move_assignable_v<T> {
    const auto old_size = vector.size();
    const auto new_end = std::remove_if(vector.begin(), vector.end(), std::move(pred));
    vector.erase(new_end, vector.end());
    return old_size - vector.size();
}

template <typename T, std::size_t N>
[[nodiscard]] std::size_t capacity_in_bytes(const SmallVector<T, N>& vector) noexcept {
    return vector.capacity_in_bytes();
}

#undef TFL_SMALL_VECTOR_NOINLINE

}  // namespace tfl
