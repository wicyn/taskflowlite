// forward.hpp
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
template <typename F> class BasicWork;
template <typename F> class BranchWork;
template <typename F> class MultiBranchWork;
template <typename F> class JumpWork;
template <typename F> class MultiJumpWork;
template <typename F> class RuntimeWork;
template <typename FlowStore, typename P> class SubflowWork;
template <typename F> class AsyncBasicWork;
template <typename F> class AsyncRuntimeWork;
template <typename F, typename R> class AsyncBasicPromiseWork;
template <typename F, typename R> class AsyncRuntimePromiseWork;
template <typename F> class DepAsyncBasicWork;
template <typename F> class DepAsyncRuntimeWork;
template <typename FlowStore, typename P, typename C> class DepFlowWork;
class NullWork;

#define TFL_WORK_SUBCLASS_FRIENDS                                              \
template <typename> friend class ::tfl::BasicWork;                         \
    template <typename> friend class ::tfl::BranchWork;                        \
    template <typename> friend class ::tfl::MultiBranchWork;                   \
    template <typename> friend class ::tfl::JumpWork;                          \
    template <typename> friend class ::tfl::MultiJumpWork;                     \
    template <typename> friend class ::tfl::RuntimeWork;                       \
    template <typename, typename> friend class ::tfl::SubflowWork;             \
    template <typename> friend class ::tfl::AsyncBasicWork;                    \
    template <typename> friend class ::tfl::AsyncRuntimeWork;                  \
    template <typename, typename> friend class ::tfl::AsyncBasicPromiseWork;   \
    template <typename, typename> friend class ::tfl::AsyncRuntimePromiseWork; \
    template <typename> friend class ::tfl::DepAsyncBasicWork;                 \
    template <typename> friend class ::tfl::DepAsyncRuntimeWork;               \
    template <typename, typename, typename> friend class ::tfl::DepFlowWork;   \
    friend class ::tfl::NullWork;


} // namespace tfl
