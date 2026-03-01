#pragma once

#include <optional>
#include <string_view>
#include <cstddef>
#include <concepts>
#include <span>
#include "task.hpp"
#include "small_vector.hpp"
namespace tfl {


/**
 * @brief 单目标跳转控制器
 *
 * 只能跳转到一个后继，后设置覆盖前设置。
 * 未选中的后继不会收到 join_counter 递减。
 */
class Jump {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    // ---- 子类友元 ----
    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 跳转操作 ====================

    Jump& to(std::size_t index) noexcept;
    Jump& to(std::string_view name) noexcept;

    template <predicate<TaskView> Pred>
    Jump& to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    Jump& reset() noexcept;

    // ==================== 查询接口 ====================

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<std::string_view> name(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> index(std::string_view name) const noexcept;

private:
    Work&  m_work;
    Work*  m_target{nullptr};

    explicit Jump(Work& work) noexcept : m_work{work} {}

    [[nodiscard]] bool _empty() const noexcept { return m_target == nullptr; }
    [[nodiscard]] Work* _target() const noexcept { return m_target; }
};

// ============================================================
// 跳转操作实现
// ============================================================

inline Jump& Jump::to(std::size_t index) noexcept {
    auto succs = m_work._successors();
    m_target = (index < succs.size()) ? succs[index] : nullptr;
    return *this;
}

inline Jump& Jump::to(std::string_view name) noexcept {
    m_target = nullptr;
    for (auto* suc : m_work._successors()) {
        if (suc->m_name == name) { m_target = suc; return *this; }
    }
    return *this;
}

template <predicate<TaskView> Pred>
Jump& Jump::to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    m_target = nullptr;
    for (auto* suc : m_work._successors()) {
        if (std::invoke_r<bool>(pred, TaskView{*suc})) { m_target = suc; return *this; }
    }
    return *this;
}

inline Jump& Jump::reset() noexcept {
    m_target = nullptr;
    return *this;
}

// ============================================================
// 查询接口实现
// ============================================================

inline std::size_t Jump::size() const noexcept {
    return m_work.m_num_successors;
}

inline std::optional<std::string_view> Jump::name(std::size_t index) const noexcept {
    auto succs = m_work._successors();
    return index < succs.size() ? std::optional{succs[index]->m_name} : std::nullopt;
}

inline std::optional<std::size_t> Jump::index(std::string_view name) const noexcept {
    auto succs = m_work._successors();
    for (std::size_t i = 0; i < succs.size(); ++i) {
        if (succs[i]->m_name == name) return i;
    }
    return std::nullopt;
}


class MultiJump {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    friend class Runtime;

    // ---- 子类友元 ----
    TFL_WORK_SUBCLASS_FRIENDS;

public:

    // ==================== 跳转操作 ====================

    template <typename... Is>
        requires (sizeof...(Is) > 0) && (std::convertible_to<Is, std::size_t> && ...)
    MultiJump& to(Is... indices);

    template <typename... Ns>
        requires (sizeof...(Ns) > 0) && (std::convertible_to<Ns, std::string_view> && ...)
    MultiJump& to(Ns&&... names);

    MultiJump& to_all();
    MultiJump& reset() noexcept;

    template <predicate<TaskView> Pred>
    void to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>);

    // ==================== 查询接口 ====================

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<std::string_view> name(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> index(std::string_view name) const noexcept;

private:
    Work& m_work;
    SmallVector<Work*> m_targets;

    explicit MultiJump(Work& work) noexcept : m_work{work} {}

    void _insert(Work* w) {
        for (auto* t : m_targets) {
            if (t == w) return;
        }
        m_targets.push_back(w);
    }
};

// ============================================================
// 跳转操作实现
// ============================================================

template <typename... Is>
    requires (sizeof...(Is) > 0) && (std::convertible_to<Is, std::size_t> && ...)
MultiJump& MultiJump::to(Is... indices) {
    auto succs = m_work._successors();
    const std::size_t sz = succs.size();
    ([&](std::size_t idx) {
        if (idx < sz) _insert(succs[idx]);
    }(static_cast<std::size_t>(indices)), ...);
    return *this;
}

template <typename... Ns>
    requires (sizeof...(Ns) > 0) && (std::convertible_to<Ns, std::string_view> && ...)
MultiJump& MultiJump::to(Ns&&... names) {
    for (auto* suc : m_work._successors()) {
        if (((suc->m_name == static_cast<std::string_view>(names)) || ...)) {
            _insert(suc);
        }
    }
    return *this;
}

inline MultiJump& MultiJump::to_all() {
    auto succs = m_work._successors();
    for (auto* suc : succs) {
        _insert(suc);
    }
    return *this;
}

inline MultiJump& MultiJump::reset() noexcept {
    m_targets.clear();
    return *this;
}

template <predicate<TaskView> Pred>
void MultiJump::to_if(Pred&& pred) noexcept(noexcept_predicate<Pred>) {
    for (auto* suc : m_work._successors()) {
        if (std::invoke_r<bool>(pred, TaskView{*suc})) {
            _insert(suc);
        }
    }
}

// ============================================================
// 查询接口实现
// ============================================================

inline std::size_t MultiJump::size() const noexcept {
    return m_work.m_num_successors;
}

inline std::optional<std::string_view> MultiJump::name(std::size_t index) const noexcept {
    auto succs = m_work._successors();
    return index < succs.size() ? std::optional{succs[index]->m_name} : std::nullopt;
}

inline std::optional<std::size_t> MultiJump::index(std::string_view name) const noexcept {
    auto succs = m_work._successors();
    for (std::size_t i = 0; i < succs.size(); ++i) {
        if (succs[i]->m_name == name) return i;
    }
    return std::nullopt;
}


}  // namespace tfl
