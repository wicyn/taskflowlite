/// @file async_future.hpp
/// @brief AsyncFuture —— 异步任务结果、生命周期与协作式停止控制的共享句柄。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-09
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include "work.hpp"
#include "result_slot.hpp"
#include "exception.hpp"

namespace tfl {

/// @brief 持有异步任务强引用，并提供结果访问、状态查询与协作式停止控制。
///
/// 每个非空 AsyncFuture 同时关联一个底层 `Work` 和对应的 `ResultSlot<R>`。
/// AsyncFuture 通过增加 Work 的强引用计数保证句柄存活期间 Work 及其结果槽保持有效；
/// 当最后一个强引用离开且执行生命周期也已经结束时，由引用计数机制负责销毁 Work。
///
/// AsyncFuture 是可复制的共享句柄。复制不会复制任务或结果，而是增加同一 Work 的
/// 强引用计数，使多个独立 AsyncFuture 可以共享和观察同一个异步任务。
///
/// `get()` 是非消费式访问：调用后 AsyncFuture 仍然保持有效，并且可以重复调用。
/// 对值结果返回结果槽中对象的常量引用，对引用结果返回原始左值引用，
/// `R=void` 时仅等待任务完成并传播异常。
///
/// 停止操作采用协作式语义。`request_stop()` 仅设置当前任务的停止请求状态，
/// 实际任务是否以及何时停止由任务执行逻辑对停止状态的检查决定。
///
/// @note 不同 AsyncFuture 实例可以共享同一个底层 Work，并独立管理各自持有的强引用。
///       但同一个 AsyncFuture 对象不得与 `reset()`、赋值、移动或析构操作并发访问，
///       因为 `m_work` 和 `m_result` 属于句柄自身的非原子状态。
///
/// @warning `wait()` 和 `get()` 会阻塞当前调用线程。在 Executor Worker 内部等待
///          其他异步任务时，应优先使用框架提供的协作等待机制，避免占用 Worker
///          线程而降低可用于推进任务图的执行资源。
///
/// @warning 对值类型 R，`get()` 返回的引用指向底层 ResultSlot，其有效期依赖 Work
///          生命周期。调用方必须保证至少存在一个持有同一 Work 强引用的句柄，
///          不得在保存该引用后释放最后一个 AsyncFuture / AsyncTask 再继续使用它。
///
/// @tparam R callable 的结果类型，可为值类型、左值引用类型或 void。
template <typename R>
class AsyncFuture {
    friend class Executor;
    friend class Runtime;
    friend class SubFlow;
    friend class TaskGroup;
    template <typename> friend class AsyncTask;

public:
    /// @brief 构造不关联任何任务的空 AsyncFuture。
    AsyncFuture() noexcept = default;

    /// @brief 显式构造不关联任何任务的空 AsyncFuture。
    /// @param nullptr 空句柄标记。
    explicit AsyncFuture(std::nullptr_t) noexcept;

    /// @brief 释放当前持有的任务强引用。
    ///
    /// 如果当前句柄持有最后一个强引用，并且 Work 已经满足销毁条件，
    /// 则负责销毁对应的底层任务节点。
    ~AsyncFuture() noexcept;

    /// @brief 复制 AsyncFuture，并共享同一个异步任务和结果槽。
    /// @param other 要复制的 AsyncFuture。
    ///
    /// 如果 other 非空，则增加其底层 Work 的强引用计数。
    AsyncFuture(const AsyncFuture& other) noexcept;

    /// @brief 复制赋值，并共享另一 AsyncFuture 的异步任务和结果槽。
    /// @param other 要复制的 AsyncFuture。
    /// @return 当前 AsyncFuture。
    ///
    /// 赋值过程中会先为新 Work 建立强引用，再释放当前 Work 的强引用，
    /// 因而即使两个句柄引用同一 Work 也不会造成生命周期空窗。
    AsyncFuture& operator=(const AsyncFuture& other) noexcept;

    /// @brief 移动构造 AsyncFuture，并接管另一句柄持有的任务强引用。
    /// @param other 要移动的 AsyncFuture。
    ///
    /// 移动完成后 other 变为空句柄，不增加底层 Work 的强引用计数。
    AsyncFuture(AsyncFuture&& other) noexcept;

    /// @brief 移动赋值，并接管另一 AsyncFuture 持有的任务强引用。
    /// @param other 要移动的 AsyncFuture。
    /// @return 当前 AsyncFuture。
    ///
    /// 当前持有的强引用会先被释放；移动完成后 other 变为空句柄。
    AsyncFuture& operator=(AsyncFuture&& other) noexcept;

    /// @brief 释放当前任务强引用并将 AsyncFuture 置为空。
    /// @param nullptr 空句柄标记。
    /// @return 当前 AsyncFuture。
    AsyncFuture& operator=(std::nullptr_t) noexcept;

    /// @brief 释放当前 AsyncFuture 持有的任务强引用并将句柄置为空。
    ///
    /// 对空 AsyncFuture 调用没有副作用。
    ///
    /// @warning 对值类型 R，如果此前保存了 `get()` 返回的结果引用，并且当前句柄
    ///          是底层 Work 的最后一个强引用，则 reset 后该结果引用将失效。
    void reset() noexcept;

    /// @brief 判断 AsyncFuture 当前是否关联有效任务。
    /// @return 关联 Work 时返回 true，否则返回 false。
    [[nodiscard]] bool valid() const noexcept;

    /// @brief 以布尔形式判断 AsyncFuture 当前是否关联有效任务。
    /// @return 等价于 `valid()`。
    [[nodiscard]] explicit operator bool() const noexcept;

    /// @brief 查询关联任务是否已经完成。
    /// @return Work 已完成时返回 true；空 AsyncFuture 返回 false。
    [[nodiscard]] bool done() const noexcept;

    /// @brief 查询关联任务是否正在运行。
    /// @return Work 正在运行时返回 true；空 AsyncFuture 返回 false。
    [[nodiscard]] bool running() const noexcept;

    /// @brief 查询关联任务是否已经完成异常归档。
    ///
    /// 本函数查询 `Control::EXCEPTION_CAUGHT`，而不是仅表示异常传播路径的
    /// `Control::EXCEPTION`。返回 true 时，对应的异常已经写入并完成发布。
    ///
    /// @return 当前 Work 已经归档异常时返回 true；空 AsyncFuture 返回 false。
    [[nodiscard]] bool has_exception() const noexcept;

    /// @brief 获取当前任务的强引用计数。
    /// @return 包括 AsyncFuture 等外部句柄引用以及框架执行生命周期引用在内的总强引用数；
    ///         空 AsyncFuture 返回 0。
    ///
    /// @note 返回值仅表示调用瞬间观察到的引用计数快照。
    [[nodiscard]] std::size_t use_count() const noexcept;

    /// @brief 获取基于底层 Work 地址计算的哈希值。
    /// @return 当前 Work 指针的标准哈希值；空 AsyncFuture 按空指针计算。
    ///
    /// 引用同一个 Work 的 AsyncFuture 具有相同哈希值。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 判断两个 AsyncFuture 是否引用同一个底层任务。
    /// @param other 要比较的 AsyncFuture。
    /// @return 两者持有相同 Work 指针时返回 true。
    ///
    /// 两个空 AsyncFuture 也被视为相等。
    ///
    /// @note C++20 会根据该 `operator==` 自动重写对应的 `operator!=` 表达式，
    ///       因此无需单独定义不等比较运算符。
    [[nodiscard]] bool operator==(const AsyncFuture& other) const noexcept;

    /// @brief 阻塞等待关联任务完成。
    ///
    /// 空 AsyncFuture 不执行任何操作。该函数只负责等待，不传播任务异常；
    /// 如需访问结果并传播异常，应调用 `get()`。
    ///
    /// @warning 本函数会阻塞当前调用线程；在 Executor Worker 中应谨慎使用。
    void wait() const noexcept;

    /// @brief 等待任务完成、传播任务异常并访问保存的结果。
    ///
    /// 该操作不会消费结果，也不会使 AsyncFuture 失效，因此允许重复调用。
    /// 对普通值类型返回结果槽中对象的 `const R&`；对左值引用结果返回原始左值引用；
    /// `R=void` 时等待完成并检查异常后直接返回。
    ///
    /// @return 根据 R 返回对应的非消费式结果引用；`R=void` 时无返回值。
    /// @throws Exception AsyncFuture 为空。
    /// @note 任务归档的异常会以其原始动态类型重新抛出。
    ///
    /// @warning 对值类型 R，返回引用由底层 ResultSlot 持有。该引用只在对应 Work
    ///          仍然存活期间有效；不得在释放最后一个持有该 Work 的句柄后继续使用。
    ///
    /// @warning 本函数等待任务完成时会阻塞当前调用线程；在 Executor Worker
    ///          内应优先使用框架提供的协作等待机制。
    decltype(auto) get() const;

    /// @brief 查询当前任务或其继承的停止链路是否已经收到停止请求。
    /// @return 当前 Work 或其上游停止域存在停止请求时返回 true；
    ///         空 AsyncFuture 返回 false。
    [[nodiscard]] bool stop_requested() const noexcept;

    /// @brief 向当前任务发起协作式停止请求。
    /// @return 本次调用首次设置当前任务停止请求时返回 true；
    ///         AsyncFuture 为空或停止请求已经存在时返回 false。
    ///
    /// @note 停止请求仅表示请求任务尽快停止，不会强制中断正在执行的代码。
    bool request_stop() noexcept;

    /// @brief 获取关联任务的任务类型。
    /// @return 关联 Work 的 `TaskType`；空 AsyncFuture 返回 `TaskType::None`。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 将当前任务导出为完整的 D2 文本。
    /// @param direction D2 布局方向。
    /// @return 包含布局方向和任务描述的完整 D2 字符串。
    /// @throws Exception AsyncFuture 为空。
    [[nodiscard]] std::string dump(Direction direction = Direction::Default) const;

    /// @brief 将当前任务的 D2 描述写入指定输出流。
    /// @param stream 接收 D2 文本的输出流。
    /// @param direction D2 布局方向。
    /// @throws Exception AsyncFuture 为空。
    void dump(std::ostream& stream, Direction direction = Direction::Default) const;

protected:
    Work* m_work{nullptr};
    ResultSlot<R>* m_result{nullptr};

    /// @brief 由底层 Work 和结果槽构造有效 AsyncFuture，并建立一个新的任务强引用。
    /// @param work AsyncFuture 关联的底层任务节点。
    /// @param result 保存任务结果的结果槽。
    /// @pre work 与 result 均非空，并且 result 的生命周期受 work 管理。
    AsyncFuture(Work* work, ResultSlot<R>* result) noexcept;

private:
    /// @brief 为当前关联的 Work 增加一个强引用。
    ///
    /// 空 AsyncFuture 不执行任何操作。
    void _increment_ref() noexcept;

    /// @brief 释放当前持有的 Work 强引用并将句柄置为空。
    ///
    /// 如果释放的是最后一个强引用，则销毁对应的 Work。
    /// 无论当前句柄是否为空，调用结束后 m_work 与 m_result 均为空。
    void _decrement_ref() noexcept;
};


// ----------------------------------------------------------------------------
// AsyncFuture
// ----------------------------------------------------------------------------

template <typename R>
AsyncFuture<R>::AsyncFuture(std::nullptr_t) noexcept {
}

template <typename R>
AsyncFuture<R>::~AsyncFuture() noexcept {
    _decrement_ref();
}

template <typename R>
AsyncFuture<R>::AsyncFuture(const AsyncFuture& other) noexcept
    : m_work{other.m_work}
    , m_result{other.m_result} {
    _increment_ref();
}

template <typename R>
AsyncFuture<R>& AsyncFuture<R>::operator=(const AsyncFuture& other) noexcept {
    if (this != std::addressof(other)) {
        Work* work = other.m_work;
        ResultSlot<R>* result = other.m_result;

        if (work) {
            work->_increment_ref();
        }

        _decrement_ref();

        m_work = work;
        m_result = result;
    }

    return *this;
}

template <typename R>
AsyncFuture<R>::AsyncFuture(AsyncFuture&& other) noexcept
    : m_work{std::exchange(other.m_work, nullptr)}
    , m_result{std::exchange(other.m_result, nullptr)} {
}

template <typename R>
AsyncFuture<R>& AsyncFuture<R>::operator=(AsyncFuture&& other) noexcept {
    if (this != std::addressof(other)) {
        _decrement_ref();

        m_work = std::exchange(other.m_work, nullptr);
        m_result = std::exchange(other.m_result, nullptr);
    }

    return *this;
}

template <typename R>
AsyncFuture<R>& AsyncFuture<R>::operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
}

template <typename R>
void AsyncFuture<R>::reset() noexcept {
    _decrement_ref();
}

template <typename R>
bool AsyncFuture<R>::valid() const noexcept {
    return m_work != nullptr;
}

template <typename R>
AsyncFuture<R>::operator bool() const noexcept {
    return valid();
}

template <typename R>
bool AsyncFuture<R>::done() const noexcept {
    return m_work && m_work->_is_finished();
}

template <typename R>
bool AsyncFuture<R>::running() const noexcept {
    return m_work && m_work->_is_running();
}

template <typename R>
bool AsyncFuture<R>::has_exception() const noexcept {
    return m_work && m_work->_has_exception();
}

template <typename R>
std::size_t AsyncFuture<R>::use_count() const noexcept {
    return m_work ? m_work->_use_count() : 0;
}

template <typename R>
std::size_t AsyncFuture<R>::hash_value() const noexcept {
    return std::hash<Work*>{}(m_work);
}

template <typename R>
bool AsyncFuture<R>::operator==(const AsyncFuture& other) const noexcept {
    return m_work == other.m_work;
}

template <typename R>
void AsyncFuture<R>::wait() const noexcept {
    if (m_work) {
        m_work->_wait();
    }
}

template <typename R>
decltype(auto) AsyncFuture<R>::get() const {
    if (!m_work) [[unlikely]] {
        throw Exception{"AsyncFuture::get: no associated task."};
    }

    m_work->_wait();
    m_work->_rethrow_shared_exception();

    return std::as_const(*m_result).ref();
}

template <typename R>
bool AsyncFuture<R>::stop_requested() const noexcept {
    return m_work && m_work->_stop_requested();
}

template <typename R>
bool AsyncFuture<R>::request_stop() noexcept {
    return m_work && m_work->_request_stop();
}

template <typename R>
TaskType AsyncFuture<R>::type() const noexcept {
    return m_work ? m_work->type() : TaskType::None;
}

template <typename R>
std::string AsyncFuture<R>::dump(Direction direction) const {
    std::ostringstream stream;
    dump(stream, direction);
    return stream.str();
}

template <typename R>
void AsyncFuture<R>::dump(std::ostream& stream, Direction direction) const {
    if (!m_work) [[unlikely]] {
        throw Exception{"AsyncFuture::dump: no associated task."};
    }

    stream << "direction: " << to_string(direction) << "\n\n";
    m_work->dump(stream);
    stream << '\n';
}

template <typename R>
AsyncFuture<R>::AsyncFuture(Work* work, ResultSlot<R>* result) noexcept
    : m_work{work}
    , m_result{result} {
    TFL_ASSERT(m_work);
    TFL_ASSERT(m_result);
    _increment_ref();
}

template <typename R>
void AsyncFuture<R>::_increment_ref() noexcept {
    if (m_work) {
        m_work->_increment_ref();
    }
}

template <typename R>
void AsyncFuture<R>::_decrement_ref() noexcept {
    Work* work = std::exchange(m_work, nullptr);
    m_result = nullptr;

    if (work && work->_decrement_ref()) {
        destroy_work(work);
    }
}

}  // namespace tfl

namespace std {

/// @brief 为 `tfl::AsyncFuture<R>` 提供基于底层任务身份的标准哈希支持。
///
/// 哈希值由 AsyncFuture 持有的 Work 指针计算，因此共享同一个 Work 的
/// AsyncFuture 具有相同哈希值，并与 `operator==` 的任务身份比较语义保持一致。
///
/// @tparam R 异步任务的结果类型。
template <typename R>
struct hash<tfl::AsyncFuture<R>> {
    /// @brief 计算 AsyncFuture 所关联底层 Work 的哈希值。
    /// @param future 要计算哈希值的异步任务句柄。
    /// @return `future.hash_value()`；空句柄按空 Work 指针计算。
    std::size_t operator()(const tfl::AsyncFuture<R>& future) const noexcept {
        return future.hash_value();
    }
};

}  // namespace std
