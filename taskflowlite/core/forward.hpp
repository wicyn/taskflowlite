/// @file forward.hpp
/// @brief 前向声明与类型别名定义
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

namespace tfl {

class Work;
class Task;
class TaskView;
class AsyncTask;
class Graph;
class Flow;
class Worker;
class Executor;
class Runtime;
class Branch;
class MultiBranch;
class Jump;
class MultiJump;


// Work 子类前向声明
template <typename F, typename... Args> class BasicWork;
template <typename F, typename... Args> class BranchWork;
template <typename F, typename... Args> class MultiBranchWork;
template <typename F, typename... Args> class JumpWork;
template <typename F, typename... Args> class MultiJumpWork;
template <typename F, typename... Args> class RuntimeWork;
template <typename FlowStore, typename P> class SubflowWork;
template <typename F, typename... Args> class AsyncBasicWork;
template <typename F, typename... Args> class AsyncRuntimeWork;
template <typename F, typename R, typename... Args> class AsyncBasicPromiseWork;
template <typename F, typename R, typename... Args> class AsyncRuntimePromiseWork;
template <typename F, typename... Args> class DepAsyncBasicWork;
template <typename F, typename... Args> class DepAsyncRuntimeWork;
template <typename FlowStore, typename P, typename C> class DepFlowWork;
class NullWork;

#define TFL_WORK_SUBCLASS_FRIENDS                                              \
template <typename, typename...> friend class ::tfl::BasicWork;            \
    template <typename, typename...> friend class ::tfl::BranchWork;           \
    template <typename, typename...> friend class ::tfl::MultiBranchWork;      \
    template <typename, typename...> friend class ::tfl::JumpWork;             \
    template <typename, typename...> friend class ::tfl::MultiJumpWork;        \
    template <typename, typename...> friend class ::tfl::RuntimeWork;          \
    template <typename, typename> friend class ::tfl::SubflowWork;             \
    template <typename, typename...> friend class ::tfl::AsyncBasicWork;       \
    template <typename, typename...> friend class ::tfl::AsyncRuntimeWork;     \
    template <typename, typename, typename...> friend class ::tfl::AsyncBasicPromiseWork; \
    template <typename, typename, typename...> friend class ::tfl::AsyncRuntimePromiseWork; \
    template <typename, typename...> friend class ::tfl::DepAsyncBasicWork;    \
    template <typename, typename...> friend class ::tfl::DepAsyncRuntimeWork;  \
    template <typename, typename, typename> friend class ::tfl::DepFlowWork;   \
    friend class ::tfl::NullWork;


} // namespace tfl
