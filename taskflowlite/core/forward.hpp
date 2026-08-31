/// @file forward.hpp
/// @brief 框架公开类型与内部协作类型的前向声明。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn


#pragma once

namespace tfl {

class SubFlow;
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
class Topology;
class Work;
class Graph;
class Worker;
class WorkerView;
class WorkerHandler;
class FlowBuilder;
class ScopedExceptionAnchor;
template <typename>             class AsyncFuture;
template <typename>             class AsyncTask;

class AnchorWork;
class PlaceholderInvoker;
template <typename, typename...>                            class BasicInvoker;
template <typename, typename...>                            class BranchInvoker;
template <typename, typename...>                            class MultiBranchInvoker;
template <typename, typename...>                            class JumpInvoker;
template <typename, typename...>                            class MultiJumpInvoker;
template <typename, typename...>                            class RuntimeInvoker;
template <typename, typename...>                            class SubFlowInvoker;
template <typename, typename>                               class ModuleInvoker;
template <typename, typename...>                            class DetachedBasicInvoker;
template <typename, typename...>                            class DetachedRuntimeInvoker;
template <typename, typename...>                            class DetachedSubFlowInvoker;
template <typename, typename, typename>                     class DetachedModuleInvoker;
template <typename, typename...>                            class JoinableBasicInvoker;
template <typename, typename...>                            class JoinableRuntimeInvoker;
template <typename, typename...>                            class JoinableSubFlowInvoker;
template <typename, typename, typename>                     class JoinableModuleInvoker;
template <typename, typename...>                            class AttachedBasicInvoker;
template <typename, typename...>                            class AttachedRuntimeInvoker;
template <typename, typename...>                            class AttachedSubFlowInvoker;
template <typename, typename, typename>                     class AttachedModuleInvoker;


#define TFL_WORK_SUBCLASS_FRIENDS                                                                                   \
friend class ::tfl::AnchorWork;                                                                                     \
    friend class ::tfl::PlaceholderInvoker;                                                                         \
    template <typename, typename...>                        friend class ::tfl::BasicInvoker;                       \
    template <typename, typename...>                        friend class ::tfl::BranchInvoker;                      \
    template <typename, typename...>                        friend class ::tfl::MultiBranchInvoker;                 \
    template <typename, typename...>                        friend class ::tfl::JumpInvoker;                        \
    template <typename, typename...>                        friend class ::tfl::MultiJumpInvoker;                   \
    template <typename, typename...>                        friend class ::tfl::RuntimeInvoker;                     \
    template <typename, typename...>                        friend class ::tfl::SubFlowInvoker;                     \
    template <typename, typename>                           friend class ::tfl::ModuleInvoker;                      \
    template <typename, typename...>                        friend class ::tfl::DetachedBasicInvoker;               \
    template <typename, typename...>                        friend class ::tfl::DetachedRuntimeInvoker;             \
    template <typename, typename...>                        friend class ::tfl::DetachedSubFlowInvoker;             \
    template <typename, typename, typename>                 friend class ::tfl::DetachedModuleInvoker;              \
    template <typename, typename...>                        friend class ::tfl::JoinableBasicInvoker;               \
    template <typename, typename...>                        friend class ::tfl::JoinableRuntimeInvoker;             \
    template <typename, typename...>                        friend class ::tfl::JoinableSubFlowInvoker;             \
    template <typename, typename, typename>                 friend class ::tfl::JoinableModuleInvoker;              \
    template <typename, typename...>                        friend class ::tfl::AttachedBasicInvoker;               \
    template <typename, typename...>                        friend class ::tfl::AttachedRuntimeInvoker;             \
    template <typename, typename...>                        friend class ::tfl::AttachedSubFlowInvoker;             \
    template <typename, typename, typename>                 friend class ::tfl::AttachedModuleInvoker;

} // namespace tfl
