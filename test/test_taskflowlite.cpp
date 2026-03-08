#define CATCH_CONFIG_MAIN

#include "taskflowlite/taskflowlite.hpp"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <numeric>
#include <vector>

TEST_CASE("Flow: Basic Construction", "[flow]") {
  tfl::Flow flow;

  REQUIRE(flow.empty() == true);
  REQUIRE(flow.size() == 0);

  auto task = flow.emplace([] {});
  REQUIRE(flow.empty() == false);
  REQUIRE(flow.size() == 1);
}

TEST_CASE("Flow: Clear and Empty", "[flow]") {
  tfl::Flow flow;

  flow.emplace([] {});
  flow.emplace([] {});
  REQUIRE(flow.size() == 2);

  flow.clear();
  REQUIRE(flow.empty() == true);
  REQUIRE(flow.size() == 0);
}

TEST_CASE("Flow: Erase Single Task", "[flow]") {
  tfl::Flow flow;

  auto t1 = flow.emplace([] {});
  auto t2 = flow.emplace([] {});

  REQUIRE(flow.size() == 2);

  flow.erase(t1);
  REQUIRE(flow.size() == 1);

  flow.erase(t2);
  REQUIRE(flow.empty() == true);
}

TEST_CASE("Flow: Erase Multiple Tasks", "[flow]") {
  tfl::Flow flow;

  auto t1 = flow.emplace([] {});
  auto t2 = flow.emplace([] {});
  auto t3 = flow.emplace([] {});

  REQUIRE(flow.size() == 3);

  flow.erase(t1, t2);
  REQUIRE(flow.size() == 1);
}

TEST_CASE("Flow: For Each", "[flow]") {
  tfl::Flow flow;
  std::atomic<int> counter{0};

  for (int i = 0; i < 10; ++i) {
    flow.emplace([&counter] { counter.fetch_add(1); });
  }

  int count = 0;
  flow.for_each([&count](tfl::Task) { ++count; });
  REQUIRE(count == 10);
}

TEST_CASE("Flow: Name", "[flow]") {
  tfl::Flow flow;

  REQUIRE(flow.name().empty());

  flow.name("TestFlow");
  REQUIRE(flow.name() == "TestFlow");

  auto task = flow.emplace([] {});
  task.name("Task1");
}

TEST_CASE("Task: Precede and Succeed", "[task]") {
  tfl::Flow flow;

  auto A = flow.emplace([] {});
  auto B = flow.emplace([] {});
  auto C = flow.emplace([] {});

  A.precede(B);
  A.precede(C);

  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);

  executor.submit(flow).start().wait();
}

TEST_CASE("Task: Acquire and Release", "[task]") {
  tfl::Flow flow;
  tfl::Semaphore sem(2);

  auto t1 = flow.emplace([] {});
  auto t2 = flow.emplace([] {});

  t1.acquire(sem);
  t1.release(sem);
  t2.acquire(sem);
  t2.release(sem);

  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);

  executor.submit(flow).start().wait();
}

TEST_CASE("Task: Name", "[task]") {
  tfl::Flow flow;

  auto task = flow.emplace([] {});
  REQUIRE(task.name().empty());

  task.name("MyTask");
  REQUIRE(task.name() == "MyTask");
}

TEST_CASE("Executor: Basic Submit and Wait", "[executor]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  auto t1 = flow.emplace([&] { counter.fetch_add(1); });
  auto t2 = flow.emplace([&] { counter.fetch_add(1); });

  t1.precede(t2);

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 2);
}

TEST_CASE("Executor: Loop Execution", "[executor]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  flow.emplace([&] { counter.fetch_add(1); });

  constexpr int iterations = 5;
  executor.submit(flow, iterations).start().wait();

  REQUIRE(counter.load() == iterations);
}

TEST_CASE("Executor: Empty Flow", "[executor]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  executor.submit(flow).start().wait();

  REQUIRE(true);
}

TEST_CASE("Executor: Single Task", "[executor]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  bool executed = false;
  flow.emplace([&executed] { executed = true; });

  executor.submit(flow).start().wait();

  REQUIRE(executed == true);
}

TEST_CASE("Executor: Multiple Thread Counts", "[executor]") {
  tfl::ResumeNever handler;
  std::atomic<int> counter{0};

  for (int threads = 1; threads <= 4; ++threads) {
    tfl::Executor executor(handler, threads);
    tfl::Flow flow;

    counter.store(0);

    auto t1 = flow.emplace([&] { counter.fetch_add(1); });
    auto t2 = flow.emplace([&] { counter.fetch_add(1); });

    t1.precede(t2);

    executor.submit(flow).start().wait();

    REQUIRE(counter.load() == 2);
  }
}

TEST_CASE("Executor: Parallel Tasks", "[executor]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  auto t1 = flow.emplace([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); counter.fetch_add(1); });
  auto t2 = flow.emplace([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); counter.fetch_add(1); });
  auto t3 = flow.emplace([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); counter.fetch_add(1); });

  t1.precede(t3);
  t2.precede(t3);

  auto start = std::chrono::high_resolution_clock::now();
  executor.submit(flow).start().wait();
  auto end = std::chrono::high_resolution_clock::now();

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  REQUIRE(counter.load() == 3);
}

TEST_CASE("Executor: Complex DAG", "[executor]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  auto start = flow.emplace([&] { counter.fetch_add(1); });

  auto p1 = flow.emplace([&] { counter.fetch_add(1); });
  auto p2 = flow.emplace([&] { counter.fetch_add(1); });
  auto p3 = flow.emplace([&] { counter.fetch_add(1); });

  auto end = flow.emplace([&] { counter.fetch_add(1); });

  start.precede(p1, p2, p3);
  p1.precede(end);
  p2.precede(end);
  p3.precede(end);

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 5);
}

TEST_CASE("Runtime: Async Task", "[runtime]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  int result = 0;

  flow.emplace([&](tfl::Runtime& rt) {
    auto fut = rt.async([] { return 42; });
    rt.wait_until([&] { return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    result = fut.get();
  });

  executor.submit(flow).start().wait();

  REQUIRE(result == 42);
}

TEST_CASE("Runtime: Multiple Async", "[runtime]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  flow.emplace([&](tfl::Runtime& rt) {
    auto f1 = rt.async([&] { counter.fetch_add(1); return 1; });
    auto f2 = rt.async([&] { counter.fetch_add(1); return 2; });
    rt.wait_until([&] { return f1.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                               f2.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
  });

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 2);
}

TEST_CASE("Runtime: Silent Async", "[runtime]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  flow.emplace([&](tfl::Runtime& rt) {
    for (int i = 0; i < 10; ++i) {
      rt.silent_async([&] { counter.fetch_add(1); });
    }
    rt.wait_until([&] { return counter == 10; });
  });

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 10);
}

TEST_CASE("Runtime: Nested Flow", "[runtime]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  tfl::Flow subflow;
  subflow.emplace([&] { counter.fetch_add(1); });
  subflow.emplace([&] { counter.fetch_add(1); });

  flow.emplace(subflow);

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 2);
}

TEST_CASE("Runtime: Subflow with Loop", "[runtime]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  tfl::Flow subflow;
  subflow.emplace([&] { counter.fetch_add(1); });

  flow.emplace(subflow, 5);

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 5);
}

TEST_CASE("Branch: Single Branch Allow", "[branch]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> result{-1};

  auto branch = flow.emplace([](tfl::Branch& br) {
    br.allow(0);
  });

  auto path0 = flow.emplace([&] { result.store(0); });
  auto path1 = flow.emplace([&] { result.store(1); });

  branch.precede(path0, path1);

  executor.submit(flow).start().wait();

  REQUIRE(result.load() == 0);
}

TEST_CASE("Branch: Alternate Path", "[branch]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);

  for (int i = 0; i < 2; ++i) {
    tfl::Flow flow;

    std::atomic<int> result{-1};

    auto branch = flow.emplace([i](tfl::Branch& br) {
      br.allow(i);
    });

    auto path0 = flow.emplace([&] { result.store(0); });
    auto path1 = flow.emplace([&] { result.store(1); });

    branch.precede(path0, path1);

    executor.submit(flow).start().wait();

    REQUIRE(result.load() == i);
  }
}

TEST_CASE("Branch: MultiBranch", "[branch]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  auto multibranch = flow.emplace([](tfl::MultiBranch& br) {
    br.allow(0).allow(2);
  });

  auto task0 = flow.emplace([&] { counter.fetch_add(1); });
  auto task1 = flow.emplace([&] { counter.fetch_add(1); });
  auto task2 = flow.emplace([&] { counter.fetch_add(1); });

  multibranch.precede(task0, task1, task2);

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 2);
}

TEST_CASE("Jump: Basic Jump", "[jump]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  auto start = flow.emplace([&] { counter.fetch_add(1); });
  auto check = flow.emplace([](tfl::Jump& jmp) {
    jmp.to(0);
  });
  auto end = flow.emplace([&] { counter.fetch_add(100); });

  start.precede(check);
  check.precede(end);

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() >= 1);
}

TEST_CASE("Jump: Retry Loop", "[jump]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> attempts{0};
  std::atomic<int> success{0};

  auto start = flow.emplace([&] { attempts.fetch_add(1); });
  auto check = flow.emplace([&](tfl::Jump& jmp) {
    if (attempts.load() < 3) {
      jmp.to(0);
    } else {
      success.store(1);
    }
  });
  auto end = flow.emplace([] {});
  end.precede(start);
  start.precede(check);
  check.precede(start);

  executor.submit(flow).start().wait();

  REQUIRE(success.load() == 1);
  REQUIRE(attempts.load() >= 3);
}

TEST_CASE("Jump: MultiJump", "[jump]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  auto start = flow.emplace([&] { counter.fetch_add(1); });
  auto mj = flow.emplace([](tfl::MultiJump& mj) {
    mj.to(0).to(1);
  });

  auto target0 = flow.emplace([&] { counter.fetch_add(10); });
  auto target1 = flow.emplace([&] { counter.fetch_add(100); });
  auto join = flow.emplace([&] { counter.fetch_add(1000); });

  start.precede(mj);
  mj.precede(target0, target1);
  target0.precede(join);
  target1.precede(join);

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 1111);
}

TEST_CASE("Semaphore: Basic", "[semaphore]") {
  tfl::Flow flow;
  tfl::Semaphore sem(2);

  auto t1 = flow.emplace([] {});
  auto t2 = flow.emplace([] {});

  t1.acquire(sem);
  t1.release(sem);

  REQUIRE(t1.num_acquires() == 1);
  REQUIRE(t1.num_releases() == 1);
}

TEST_CASE("Semaphore: Task Methods", "[semaphore]") {
  tfl::Flow flow;
  tfl::Semaphore sem(1);

  auto t1 = flow.emplace([] {});
  auto t2 = flow.emplace([] {});

  t1.acquire(sem);
  t1.release(sem);
  t2.acquire(sem);
  t2.release(sem);

  REQUIRE(true);
}

TEST_CASE("Exception: ResumeNever Handler", "[exception]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  flow.emplace([] { throw std::runtime_error("Test exception"); });

  bool thrown = false;
  try {
    executor.submit(flow).start().get();
  } catch (const std::runtime_error&) {
    thrown = true;
  }
  REQUIRE(thrown == true);
}

TEST_CASE("Exception: ResumeAlways Handler", "[exception]") {
  tfl::ResumeAlways handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  flow.emplace([&] {
    counter.fetch_add(1);
    throw std::runtime_error("Test");
  });
  flow.emplace([&] { counter.fetch_add(1); });

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 2);
}

TEST_CASE("Dump: Basic Flow", "[dump]") {
  tfl::Flow flow;

  auto t1 = flow.emplace([] {});
  auto t2 = flow.emplace([] {});

  t1.precede(t2);

  auto dump = flow.dump();
  REQUIRE(dump.empty() == false);
}

TEST_CASE("Dump: Flow with Name", "[dump]") {
  tfl::Flow flow;
  flow.name("TestPipeline");

  auto t1 = flow.emplace([] {});
  auto t2 = flow.emplace([] {});

  t1.precede(t2);

  auto dump = flow.dump();
  REQUIRE(dump.find("TestPipeline") != std::string::npos);
}

TEST_CASE("Hash: Flow Hash Value", "[flow]") {
  tfl::Flow flow1;
  flow1.emplace([] {});

  tfl::Flow flow2;
  flow2.emplace([] {});

  auto h1 = flow1.hash_value();
  auto h2 = flow2.hash_value();

  REQUIRE(h1 != h2);
}

TEST_CASE("Stress: Large DAG", "[stress]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);
  tfl::Flow flow;

  constexpr std::size_t N = 50;
  std::atomic<std::size_t> counter{0};

  std::vector<tfl::Task> tasks;
  tasks.reserve(N);

  for (std::size_t i = 0; i < N; ++i) {
    tasks.push_back(flow.emplace([&] { counter.fetch_add(1); }));
  }

  for (std::size_t i = 1; i < N; ++i) {
    tasks[i].precede(tasks[0]);
  }

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == N);
}

TEST_CASE("Stress: Many Small Flows", "[stress]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);

  std::atomic<int> total{0};

  for (int i = 0; i < 20; ++i) {
    tfl::Flow flow;

    int local = i;
    flow.emplace([&total, local] { total.fetch_add(local); });

    executor.submit(flow).start().wait();
  }

  REQUIRE(total.load() == 190);
}

TEST_CASE("Stress: Concurrent Submissions", "[stress]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);

  std::atomic<int> counter{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&executor, &counter, t] {
      for (int i = 0; i < 10; ++i) {
        tfl::Flow flow;
        flow.emplace([&counter] { counter.fetch_add(1); });
        executor.submit(flow).start().wait();
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  REQUIRE(counter.load() == 40);
}

TEST_CASE("Edge: Disconnected Tasks", "[edge]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  flow.emplace([&] { counter.fetch_add(1); });
  flow.emplace([&] { counter.fetch_add(1); });
  flow.emplace([&] { counter.fetch_add(1); });

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 3);
}

TEST_CASE("Edge: Chain Dependency", "[edge]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 2);
  tfl::Flow flow;

  std::vector<int> order;

  auto t1 = flow.emplace([&] { order.push_back(1); });
  auto t2 = flow.emplace([&] { order.push_back(2); });
  auto t3 = flow.emplace([&] { order.push_back(3); });
  auto t4 = flow.emplace([&] { order.push_back(4); });
  auto t5 = flow.emplace([&] { order.push_back(5); });

  t1.precede(t2);
  t2.precede(t3);
  t3.precede(t4);
  t4.precede(t5);

  executor.submit(flow).start().wait();

  REQUIRE(order.size() == 5);
  REQUIRE(order[0] == 1);
  REQUIRE(order[4] == 5);
}

TEST_CASE("Edge: Fan In Fan Out", "[edge]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);
  tfl::Flow flow;

  std::atomic<int> counter{0};

  auto start = flow.emplace([&] { counter.fetch_add(1); });

  std::vector<tfl::Task> middle;
  for (int i = 0; i < 5; ++i) {
    middle.push_back(flow.emplace([&] { counter.fetch_add(1); }));
    start.precede(middle.back());
  }

  auto end = flow.emplace([&] { counter.fetch_add(1); });
  for (auto& m : middle) {
    m.precede(end);
  }

  executor.submit(flow).start().wait();

  REQUIRE(counter.load() == 7);
}

TEST_CASE("Performance: High Throughput", "[performance]") {
  tfl::ResumeNever handler;
  tfl::Executor executor(handler, 4);
  tfl::Flow flow;

  std::atomic<std::size_t> counter{0};

  constexpr std::size_t batch = 100;
  for (std::size_t i = 0; i < batch; ++i) {
    flow.emplace([&] { counter.fetch_add(1); });
  }

  auto start = std::chrono::high_resolution_clock::now();
  executor.submit(flow, 10).start().wait();
  auto end = std::chrono::high_resolution_clock::now();

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  REQUIRE(counter.load() == batch * 10);
  REQUIRE(ms < 5000);
}
