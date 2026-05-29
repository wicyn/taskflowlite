/// @file  forward.hpp
/// @brief 框架全体前向声明 + 架构总览 —— 组件分层、协作关系与快速索引。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn


#pragma once

namespace tfl {

class Flow;
class Task;
class TaskView;
class Runtime;
class Executor;
class Branch;
class MultiBranch;
class Jump;
class MultiJump;
class Semaphore;
class TaskObserver;
class Work;
class Graph;
class Worker;
class WorkerView;
class WorkerHandler;

template <typename>     class AsyncTask;


// Work 子类前向声明
                                                                    class NoopInvoker;
template <typename F, typename... Args>                             class BasicInvoker;
template <typename F, typename... Args>                             class BranchInvoker;
template <typename F, typename... Args>                             class MultiBranchInvoker;
template <typename F, typename... Args>                             class JumpInvoker;
template <typename F, typename... Args>                             class MultiJumpInvoker;
template <typename F, typename... Args>                             class RuntimeInvoker;
template <typename GhStore, typename P>                             class SubflowInvoker;
template <typename A, typename F, typename... Args>                 class DetachedBasicInvoker;
template <typename A, typename F, typename... Args>                 class DetachedRuntimeInvoker;
template <typename A, typename GhStore, typename P, typename C>     class DetachedFlowInvoker;
template <typename A, typename F, typename R, typename... Args>     class PromisedBasicInvoker;
template <typename A, typename F, typename R, typename... Args>     class PromisedRuntimeInvoker;
template <typename A, typename GhStore, typename P, typename C>     class PromisedFlowInvoker;
template <typename A, typename F, typename... Args>                 class AttachedBasicInvoker;
template <typename A, typename F, typename... Args>                 class AttachedRuntimeInvoker;
template <typename A, typename GhStore, typename P, typename C>     class AttachedFlowInvoker;
template <typename A>                                               class AnchorWork;


#define TFL_WORK_SUBCLASS_FRIENDS                                                                                 \
                                                            friend class ::tfl::NoopInvoker;               \
    template <typename, typename...>                        friend class ::tfl::BasicInvoker;                     \
    template <typename, typename...>                        friend class ::tfl::BranchInvoker;                    \
    template <typename, typename...>                        friend class ::tfl::MultiBranchInvoker;               \
    template <typename, typename...>                        friend class ::tfl::JumpInvoker;                      \
    template <typename, typename...>                        friend class ::tfl::MultiJumpInvoker;                 \
    template <typename, typename...>                        friend class ::tfl::RuntimeInvoker;                   \
    template <typename, typename>                           friend class ::tfl::SubflowInvoker;                   \
    template <typename, typename, typename...>              friend class ::tfl::DetachedBasicInvoker;          \
    template <typename, typename, typename...>              friend class ::tfl::DetachedRuntimeInvoker;        \
    template <typename, typename, typename, typename>       friend class ::tfl::DetachedFlowInvoker;           \
    template <typename, typename, typename, typename...>    friend class ::tfl::PromisedBasicInvoker;                \
    template <typename, typename, typename, typename...>    friend class ::tfl::PromisedRuntimeInvoker;              \
    template <typename, typename, typename, typename>       friend class ::tfl::PromisedFlowInvoker;                 \
    template <typename, typename, typename...>              friend class ::tfl::AttachedBasicInvoker;             \
    template <typename, typename, typename...>              friend class ::tfl::AttachedRuntimeInvoker;           \
    template <typename, typename, typename, typename>       friend class ::tfl::AttachedFlowInvoker;              \
    template <typename>                                     friend class ::tfl::AnchorWork;


} // namespace tfl
