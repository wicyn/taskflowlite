#pragma once

#include "worker.hpp"
namespace tfl {

class TaskObserver {
public:
    virtual void on_before(WorkerView wv) = 0;
    virtual void on_after(WorkerView wv) = 0;

    virtual ~TaskObserver() = default;

protected:
    TaskObserver() = default;
    TaskObserver(const TaskObserver&) = default;
    TaskObserver(TaskObserver&&) = default;
    TaskObserver& operator=(const TaskObserver&) & = default;
    TaskObserver& operator=(TaskObserver&&) & = default;
};


} // namespace tfl
