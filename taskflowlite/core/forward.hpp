/// @file forward.hpp
/// @brief 框架公开类型、内部协作类型及 Work Invoker 的前向声明。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn


#pragma once

namespace tfl {

// ============================================================================
// 核心类型
// ============================================================================

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

template <typename>
class AsyncFuture;

template <typename>
class AsyncTask;


// ============================================================================
// Work Invoker
// ============================================================================

class AnchorWork;
class PlaceholderInvoker;

template <typename>
class BasicInvoker;

template <typename>
class BranchInvoker;

template <typename>
class MultiBranchInvoker;

template <typename>
class JumpInvoker;

template <typename>
class MultiJumpInvoker;

template <typename>
class RuntimeInvoker;

template <typename>
class SubFlowInvoker;

template <typename, typename>
class ModuleInvoker;

template <typename>
class SilentAsyncBasicInvoker;

template <typename>
class SilentAsyncRuntimeInvoker;

template <typename>
class SilentAsyncSubFlowInvoker;

template <typename, typename, typename>
class SilentAsyncModuleInvoker;

template <typename>
class AsyncBasicInvoker;

template <typename>
class AsyncRuntimeInvoker;

template <typename>
class AsyncSubFlowInvoker;

template <typename, typename, typename>
class AsyncModuleInvoker;

template <typename>
class AsyncTaskBasicInvoker;

template <typename>
class AsyncTaskRuntimeInvoker;

template <typename>
class AsyncTaskSubFlowInvoker;

template <typename, typename, typename>
class AsyncTaskModuleInvoker;


// ============================================================================
// Work Invoker friend 声明
// ============================================================================

/// @brief 向所有直接参与 Work 执行协议的内部类型授予私有成员访问权限。
///
/// 统一维护 Work 与各 Invoker 之间的 friend 关系，避免在多个核心类型中重复声明。
/// 模板参数数量必须与对应 Invoker 的前向声明及实际定义保持一致。
#define TFL_WORK_SUBCLASS_FRIENDS                                                                                   \
friend class ::tfl::AnchorWork;                                                                                 \
    friend class ::tfl::PlaceholderInvoker;                                                                         \
    template <typename>                                  friend class ::tfl::BasicInvoker;                          \
    template <typename>                                  friend class ::tfl::BranchInvoker;                         \
    template <typename>                                  friend class ::tfl::MultiBranchInvoker;                    \
    template <typename>                                  friend class ::tfl::JumpInvoker;                           \
    template <typename>                                  friend class ::tfl::MultiJumpInvoker;                      \
    template <typename>                                  friend class ::tfl::RuntimeInvoker;                        \
    template <typename>                                  friend class ::tfl::SubFlowInvoker;                        \
    template <typename, typename>                        friend class ::tfl::ModuleInvoker;                         \
    template <typename>                                  friend class ::tfl::SilentAsyncBasicInvoker;                  \
    template <typename>                                  friend class ::tfl::SilentAsyncRuntimeInvoker;                \
    template <typename>                                  friend class ::tfl::SilentAsyncSubFlowInvoker;                \
    template <typename, typename, typename>              friend class ::tfl::SilentAsyncModuleInvoker;                 \
    template <typename>                                  friend class ::tfl::AsyncBasicInvoker;                  \
    template <typename>                                  friend class ::tfl::AsyncRuntimeInvoker;                \
    template <typename>                                  friend class ::tfl::AsyncSubFlowInvoker;                \
    template <typename, typename, typename>              friend class ::tfl::AsyncModuleInvoker;                 \
    template <typename>                                  friend class ::tfl::AsyncTaskBasicInvoker;                  \
    template <typename>                                  friend class ::tfl::AsyncTaskRuntimeInvoker;                \
    template <typename>                                  friend class ::tfl::AsyncTaskSubFlowInvoker;                \
    template <typename, typename, typename>              friend class ::tfl::AsyncTaskModuleInvoker;

} // namespace tfl
