#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <string>
#include <tuple>
#include <functional>
#include <memory>
#include <atomic>
#include <future>
#include <vector>


// ============================================================================
// 准备测试用的各种 Callable (函数、结构体、类)
// ============================================================================

// 1. 普通全局函数 (值传递与常量引用)
void free_func_val(int x, const std::string& s) {
    std::cout << "[FreeFunc] val: " << x << ", str: " << s << "\n";
}

// 2. 普通全局函数 (要求左值引用)
void free_func_ref(int& counter) {
    counter += 10;
    std::cout << "[FreeFunc] modified counter to: " << counter << "\n";
}

// 3. 普通全局函数 (无参)
void free_func_void() {
    std::cout << "[FreeFunc] void\n";
}

// 4. 普通全局函数 (noexcept)
void free_func_noexcept() noexcept {
    std::cout << "[FreeFunc] noexcept\n";
}

// 5. 普通全局函数 (多参数)
void free_func_multi(int a, double b, const std::string& c) {
    std::cout << "[FreeFunc] a=" << a << " b=" << b << " c=" << c << "\n";
}

// 6. 有状态的仿函数 (Functor)
struct MyFunctor {
    int multiplier;
    void operator()(int& val) const { val *= multiplier; }
};

// 7. 各种仿函数变体
struct ConstFunctor {
    void operator()() const { std::cout << "[ConstFunctor]\n"; }
};

struct MutableFunctor {
    int state = 0;
    void operator()() { state++; std::cout << "[MutableFunctor] state=" << state << "\n"; }
};

struct NoexceptFunctor {
    void operator()() noexcept { std::cout << "[NoexceptFunctor]\n"; }
};

struct GenericFunctor {
    template <typename T>
    void operator()(T&& x) { std::cout << "[GenericFunctor] x=" << x << "\n"; }
};

struct MultiArgFunctor {
    void operator()(int a, double b, const std::string& s) {
        std::cout << "[MultiArgFunctor] a=" << a << " b=" << b << " s=" << s << "\n";
    }
};

struct MoveOnlyFunctor {
    std::unique_ptr<int> payload;
    MoveOnlyFunctor(int v) : payload(std::make_unique<int>(v)) {}
    MoveOnlyFunctor(MoveOnlyFunctor&&) = default;
    MoveOnlyFunctor& operator=(MoveOnlyFunctor&&) = default;
    void operator()() { std::cout << "[MoveOnlyFunctor] payload=" << *payload << "\n"; }
};

// 8. 带协议对象的仿函数
struct BranchFunctor {
    void operator()(tfl::Branch& br) { std::cout << "[BranchFunctor]\n"; }
};

struct BranchArgsFunctor {
    void operator()(int threshold, int& counter, tfl::Branch& br) {
        std::cout << "[BranchArgsFunctor] threshold=" << threshold << " counter=" << counter << "\n";
    }
};

struct MultiBranchFunctor {
    void operator()(tfl::MultiBranch& br) { std::cout << "[MultiBranchFunctor]\n"; }
};

struct JumpFunctor {
    void operator()(tfl::Jump& jmp) { std::cout << "[JumpFunctor]\n"; }
};

struct MultiJumpFunctor {
    void operator()(tfl::MultiJump& jmp) { std::cout << "[MultiJumpFunctor]\n"; }
};

struct RuntimeFunctor {
    void operator()(tfl::Runtime& rt) { std::cout << "[RuntimeFunctor]\n"; }
};

struct RuntimeArgsFunctor {
    void operator()(int x, tfl::Runtime& rt) {
        std::cout << "[RuntimeArgsFunctor] x=" << x << "\n";
    }
};

// 9. 普通类 (包含各种成员函数)
struct MyService {
    std::string name;

    void process(int data) {
        std::cout << "[Service] " << name << " processing: " << data << "\n";
    }

    void const_process() const {
        std::cout << "[Service] " << name << " doing const work.\n";
    }

    void ref_process(int& counter) {
        counter += 100;
        std::cout << "[Service] " << name << " ref_process, counter=" << counter << "\n";
    }

    void multi_args(int a, double b) {
        std::cout << "[Service] " << name << " multi_args a=" << a << " b=" << b << "\n";
    }

    static void static_method() {
        std::cout << "[Service] static_method\n";
    }

    static void static_method_args(int x) {
        std::cout << "[Service] static_method x=" << x << "\n";
    }

    // 协议成员函数
    void branch_method(tfl::Branch& br) {
        std::cout << "[Service] " << name << " branch_method\n";
    }

    void runtime_method(tfl::Runtime& rt) {
        std::cout << "[Service] " << name << " runtime_method\n";
    }
};

// 10. 子图谓词仿函数
struct LoopCondition {
    int max_loops;
    int current = 0;
    bool operator()() noexcept { return current++ >= max_loops; }
};

// ============================================================================
// 穷举测试开始
// ============================================================================

int main() {
    tfl::Flow flow;
    flow.name("Invoke_Exhaustive_Demo");

    int global_counter = 0;
    MyService service{"MainNode"};

    // ========================================================================
    // 类别 A: Lambda 表达式 — 捕获方式 × 参数传递 × 修饰符
    // ========================================================================

    // A1. 无捕获 Lambda (退化为函数指针)
    flow.emplace([] { std::cout << "A1. Empty lambda\n"; });

    // A2. 按值捕获 (捕获时拷贝，任务执行时使用副本)
    flow.emplace([val = global_counter]() {
        std::cout << "A2. Value capture: " << val << "\n";
    });

    // A3. 按引用捕获 (生命周期由调用者保证，框架不负责)
    flow.emplace([&global_counter]() {
        global_counter++;
        std::cout << "A3. Ref capture: " << global_counter << "\n";
    });

    // A4. Mutable Lambda (允许修改按值捕获的副本)
    flow.emplace([state = 0]() mutable {
        state++;
        std::cout << "A4. Mutable state: " << state << "\n";
    });

    // A5. noexcept Lambda (编译期消除 try-catch 分支)
    flow.emplace([]() noexcept {
        std::cout << "A5. noexcept lambda\n";
    });

    // A6. Lambda + 右值参数 (参数在 emplace 时被 move 进 tuple)
    flow.emplace([](int x, double y) {
        std::cout << "A6. rvalue args: x=" << x << " y=" << y << "\n";
    }, 42, 3.14);

    // A7. Lambda + 左值参数 (参数在 emplace 时被 decay copy 进 tuple)
    // 修改 a 不会影响 global_counter，因为是拷贝
    int a = 10; double b = 20.5;
    flow.emplace([](int x, double y) {
        std::cout << "A7. lvalue args (decay copy): x=" << x << " y=" << y << "\n";
    }, a, b);

    // A8. Lambda + std::ref 参数 (存储 reference_wrapper，invoke 时 unwrap 还原为引用)
    // 修改 r 会写回 global_counter
    flow.emplace([](int& r) {
        r = 999;
        std::cout << "A8. std::ref write-back: " << r << "\n";
    }, std::ref(global_counter));

    // A9. Lambda + 混合参数 (右值copy + 左值copy + std::ref 同时出现)
    int ref_val = 0;
    flow.emplace([](int rv, int lv, int& r) {
        r = rv + lv;
        std::cout << "A9. mixed args: rv=" << rv << " lv=" << lv << " r=" << r << "\n";
    }, 42, a, std::ref(ref_val));

    // A10. Lambda + 大量参数
    flow.emplace([](int a, int b, int c, int d, int e) {
        std::cout << "A10. many args sum=" << (a+b+c+d+e) << "\n";
    }, 1, 2, 3, 4, 5);

    // A11. Lambda + string 右值 (move 语义)
    flow.emplace([](std::string s) {
        std::cout << "A11. string rvalue: " << s << "\n";
    }, std::string("hello_move"));

    // A12. Lambda + string 左值 (decay copy)
    std::string str_lval = "hello_copy";
    flow.emplace([](std::string s) {
        std::cout << "A12. string lvalue copy: " << s << "\n";
    }, str_lval);

    // A13. Lambda + vector 右值 (move 整个容器)
    flow.emplace([](std::vector<int> v) {
        std::cout << "A13. vector rvalue, size=" << v.size() << "\n";
    }, std::vector<int>{1, 2, 3, 4, 5});

    // A14. Lambda + move-only 捕获 (unique_ptr)
    auto uptr = std::make_unique<int>(12345);
    flow.emplace([p = std::move(uptr)]() {
        std::cout << "A14. move-only capture: " << *p << "\n";
    });

    // A15. Lambda + move-only 参数通过 std::ref 传递
    // unique_ptr 不可拷贝，必须用 std::ref 让 invoke 拿到引用
    auto uptr2 = std::make_unique<int>(777);
    flow.emplace([](std::unique_ptr<int>& p) {
        std::cout << "A15. move-only ref arg: " << *p << "\n";
        *p = 888;
    }, std::ref(uptr2));

    // A16. Lambda + std::ref(atomic) (atomic 不可拷贝，必须 std::ref)
    std::atomic<int> atomic_counter{0};
    flow.emplace([](std::atomic<int>& c) {
        c.fetch_add(1, std::memory_order_relaxed);
        std::cout << "A16. atomic ref: " << c.load() << "\n";
    }, std::ref(atomic_counter));

    // A17. 泛型 Lambda (auto 参数，C++14)
    flow.emplace([](auto x) {
        std::cout << "A17. generic lambda: " << x << "\n";
    }, 42);

    // A17. 泛型 Lambda (auto 参数，C++14)
    flow.emplace([](auto x, tfl::Branch& br) {
        std::cout << "A17. generic lambda: " << x << "\n";
    }, 42);

    // A18. 泛型 Lambda + 多参数
    flow.emplace([](auto x, auto y) {
        std::cout << "A18. generic multi: " << x << ", " << y << "\n";
    }, 42, std::string("hello"));

    // A19. 工厂 Lambda (返回另一个 Lambda 给框架执行)
    auto make_task = [](int x) {
        return [x]() { std::cout << "A19. factory lambda: " << x << "\n"; };
    };
    flow.emplace(make_task(42));
    flow.emplace(make_task(100));

    // ========================================================================
    // 类别 B: 全局函数 — 所有传参形式
    // ========================================================================

    // B1. 值拷贝传递：参数在 emplace 时被拷贝进 Work 的 tuple 中
    flow.emplace(free_func_val, 42, "Hello");

    // B2. 左值引用传递：必须使用 std::ref！
    // 框架底层存储 std::reference_wrapper，invoke 时通过 unwrap 还原为 int&
    flow.emplace(free_func_ref, std::ref(global_counter));

    // B3. 函数指针 (显式取地址)
    flow.emplace(&free_func_void);

    // B4. 函数指针变量
    void (*fn_ptr)(int, const std::string&) = free_func_val;
    flow.emplace(fn_ptr, 100, std::string("via_ptr"));

    // B5. noexcept 全局函数 (编译期优化 try-catch)
    flow.emplace(free_func_noexcept);

    // B6. 无参全局函数
    flow.emplace(free_func_void);

    // B7. 多参数全局函数
    flow.emplace(free_func_multi, 1, 2.5, std::string("multi"));

    // B8. 多参数全局函数 + 混合传递 (左值copy + 右值)
    int b8_lval = 99;
    flow.emplace(free_func_multi, b8_lval, 3.14, std::string("mixed"));

    // ========================================================================
    // 类别 C: Functor (函数对象) — 传入方式 × 参数传递
    // ========================================================================

    // C1. 传入匿名 Functor 实例 (右值，Move 进 Work)
    flow.emplace(MyFunctor{2}, std::ref(global_counter));

    // C2. 传入已有 Functor 的引用 (std::ref 避免拷贝 Functor)
    MyFunctor ftor{3};
    flow.emplace(std::ref(ftor), std::ref(global_counter));

    // C3. 传入已有 Functor 的左值 (decay copy Functor 本身)
    MyFunctor ftor2{5};
    flow.emplace(ftor2, std::ref(global_counter));

    // C4. 传入 std::move(Functor) (显式 move 语义)
    MyFunctor ftor3{7};
    flow.emplace(std::move(ftor3), std::ref(global_counter));

    // C5. const 仿函数
    flow.emplace(ConstFunctor{});

    // C6. mutable 仿函数
    flow.emplace(MutableFunctor{});

    // C7. noexcept 仿函数
    flow.emplace(NoexceptFunctor{});

    // C8. 泛型仿函数 + int 参数
    flow.emplace(GenericFunctor{}, 42);

    // C9. 泛型仿函数 + string 参数
    flow.emplace(GenericFunctor{}, std::string("generic"));

    // C10. 多参数仿函数
    flow.emplace(MultiArgFunctor{}, 1, 2.5, std::string("functor_multi"));

    // C11. move-only 仿函数 (右值)
    flow.emplace(MoveOnlyFunctor{42});

    // C12. move-only 仿函数 (显式 move)
    MoveOnlyFunctor mof{100};
    flow.emplace(std::move(mof));

    // ========================================================================
    // 类别 D: 成员函数指针 — std::invoke 的终极杀器
    // ========================================================================
    // std::invoke 支持 (obj.*func)() 或 (ptr->*func)()。
    // 第一个参数是成员函数指针，第二个参数是对象实体/指针/引用包装器。

    // D1. 传递对象指针 (生命周期由外部保证)
    flow.emplace(&MyService::process, &service, 100);

    // D2. 传递对象引用 (使用 std::ref 包装以避免拷贝对象)
    flow.emplace(&MyService::process, std::ref(service), 200);

    // D3. 传递对象副本 (对象会被拷贝/移动到 Work 的 tuple 中，适合小对象)
    flow.emplace(&MyService::const_process, MyService{"CopyNode"});

    // D4. const 成员函数 + 指针
    flow.emplace(&MyService::const_process, &service);

    // D5. 成员函数 + std::ref 参数 (验证引用写回)
    flow.emplace(&MyService::ref_process, &service, std::ref(global_counter));

    // D6. 成员函数 + 多参数
    flow.emplace(&MyService::multi_args, &service, 42, 3.14);

    // D7. 成员函数 + std::ref(对象) + 多参数
    flow.emplace(&MyService::multi_args, std::ref(service), 99, 2.718);

    // D8. 静态成员函数 (和普通全局函数一样，不需要对象)
    flow.emplace(&MyService::static_method);

    // D9. 静态成员函数 + 参数
    flow.emplace(&MyService::static_method_args, 42);

    // D10. 智能指针持有对象 (通过 Lambda 捕获 shared_ptr，比直接传更可控)
    auto shared_svc = std::make_shared<MyService>(MyService{"SharedNode"});
    flow.emplace([shared_svc]() { shared_svc->process(300); });

    // D11. 智能指针 + 成员函数指针 (shared_ptr 直接作为 std::invoke 的对象参数)
    // std::invoke 原生支持 shared_ptr：shared_ptr->*member_func
    auto shared_svc2 = std::make_shared<MyService>(MyService{"SharedDirect"});
    flow.emplace(&MyService::process, shared_svc2, 400);

    // D12. std::mem_fn 包装成员函数
    auto mem_fn_wrapper = std::mem_fn(&MyService::process);
    flow.emplace(mem_fn_wrapper, &service, 500);

    // D13. std::mem_fn + std::ref(对象)
    flow.emplace(std::mem_fn(&MyService::multi_args), std::ref(service), 66, 7.77);

    // ========================================================================
    // 类别 E: std::function — 类型擦除包装
    // ========================================================================

    // E1. std::function 包装 Lambda (左值 copy)
    std::function<void()> stdfn1 = []() { std::cout << "E1. std::function<lambda>\n"; };
    flow.emplace(stdfn1);

    // E2. std::function (move 语义)
    std::function<void()> stdfn2 = []() { std::cout << "E2. std::function move\n"; };
    flow.emplace(std::move(stdfn2));

    // E3. std::function + std::ref (不拷贝 std::function 本身)
    std::function<void()> stdfn3 = []() { std::cout << "E3. std::function ref\n"; };
    flow.emplace(std::ref(stdfn3));

    // E4. std::function + 参数
    std::function<void(int, double)> stdfn4 = [](int a, double b) {
        std::cout << "E4. std::function args: " << a << " " << b << "\n";
    };
    flow.emplace(stdfn4, 42, 3.14);

    // E5. std::function + std::ref 参数
    std::function<void(int&)> stdfn5 = [](int& x) { x += 1000; };
    flow.emplace(stdfn5, std::ref(global_counter));

    // E6. std::function 包装成员函数 (通过 bind_front)
    std::function<void()> stdfn6 = std::bind_front(&MyService::const_process, &service);
    flow.emplace(stdfn6);

    // ========================================================================
    // 类别 F: std::bind / std::bind_front — 部分应用
    // ========================================================================

    // F1. bind_front 绑定普通函数的部分参数 (剩余参数在 emplace 时传入)
    auto bound1 = std::bind_front(free_func_val, 42);
    flow.emplace(bound1, std::string("bind_front"));

    // F2. bind_front 绑定成员函数 + 对象
    auto bound2 = std::bind_front(&MyService::process, &service);
    flow.emplace(bound2, 600);

    // F3. bind_front 绑定成员函数 + 对象 + 部分参数
    auto bound3 = std::bind_front(&MyService::multi_args, &service, 88);
    flow.emplace(bound3, 9.99);

    // // F4. std::bind + 占位符
    // using namespace std::placeholders;
    // auto bound4 = std::bind(free_func_val, 42, _1);
    // flow.emplace(bound4, std::string("bind_placeholder"));

    // // F5. std::bind + 多占位符
    // auto bound5 = std::bind(free_func_multi, _1, _2, _3);
    // flow.emplace(bound5, 10, 20.5, std::string("bind_multi"));

    // F6. bind_front (左值 copy)
    auto bound6 = std::bind_front(free_func_void);
    flow.emplace(bound6);

    // F7. bind_front (move 进 Work)
    flow.emplace(std::bind_front(&MyService::process, MyService{"BindMoveNode"}), 700);

    // ========================================================================
    // 类别 G: 注入框架协议对象 — 所有控制流类型
    // ========================================================================
    // 框架规则：协议对象 (Branch&, MultiBranch&, Jump&, MultiJump&, Runtime&)
    // 始终作为最后一个参数由框架注入，前面可以带任意数量的自定义参数。

    // --- G.1: Branch (条件分支) ---

    // G1a. Lambda 无参
    flow.emplace([](tfl::Branch& br) {
        std::cout << "G1a. Branch lambda\n";
    });

    // G1b. Lambda + 右值参数
    flow.emplace([](int threshold, tfl::Branch& br) {
        std::cout << "G1b. Branch + rvalue arg: " << threshold << "\n";
    }, 50);

    // G1c. Lambda + std::ref 参数 (验证写回)
    flow.emplace([](int& counter, tfl::Branch& br) {
        counter += 1;
        std::cout << "G1c. Branch + ref arg: " << counter << "\n";
    }, std::ref(global_counter));

    // G1d. Lambda + 混合参数
    flow.emplace([](int threshold, int& counter, tfl::Branch& br) {
        if (counter > threshold) {
            std::cout << "G1d. Branch + mixed: over threshold\n";
        }
    }, 50, std::ref(global_counter));

    // G1e. 仿函数
    flow.emplace(BranchFunctor{});

    // G1f. 仿函数 + 参数
    flow.emplace(BranchArgsFunctor{}, 10, std::ref(global_counter));

    // G1g. 成员函数
    flow.emplace(&MyService::branch_method, &service);

    // G1h. 成员函数 + std::ref(对象)
    flow.emplace(&MyService::branch_method, std::ref(service));

    // G1i. noexcept Lambda
    flow.emplace([](tfl::Branch& br) noexcept {
        // noexcept 分支，编译期消除 try-catch
    });

    // --- G.2: MultiBranch (多路分支) ---

    // G2a. Lambda 无参
    flow.emplace([](tfl::MultiBranch& br) {
        std::cout << "G2a. MultiBranch lambda\n";
    });

    // G2b. Lambda + 参数
    flow.emplace([](int val, tfl::MultiBranch& br) {
        std::cout << "G2b. MultiBranch + arg: " << val << "\n";
    }, 42);

    // G2c. Lambda + ref 参数
    flow.emplace([](int& counter, tfl::MultiBranch& br) {
        counter += 1;
        std::cout << "G2c. MultiBranch + ref: " << counter << "\n";
    }, std::ref(global_counter));

    // G2d. 仿函数
    flow.emplace(MultiBranchFunctor{});

    // --- G.3: Jump (无条件跳转) ---

    // G3a. Lambda 无参
    flow.emplace([](tfl::Jump& jmp) {
        std::cout << "G3a. Jump lambda\n";
    });

    // G3b. Lambda + 参数
    flow.emplace([](int val, tfl::Jump& jmp) {
        std::cout << "G3b. Jump + arg: " << val << "\n";
    }, 42);

    // G3c. Lambda + ref 参数
    flow.emplace([](int& counter, tfl::Jump& jmp) {
        counter += 1;
        std::cout << "G3c. Jump + ref: " << counter << "\n";
    }, std::ref(global_counter));

    // G3d. 仿函数
    flow.emplace(JumpFunctor{});

    // --- G.4: MultiJump (多路跳转) ---

    // G4a. Lambda 无参
    flow.emplace([](tfl::MultiJump& jmp) {
        std::cout << "G4a. MultiJump lambda\n";
    });

    // G4b. Lambda + 参数
    flow.emplace([](int val, tfl::MultiJump& jmp) {
        std::cout << "G4b. MultiJump + arg: " << val << "\n";
    }, 42);

    // G4c. Lambda + ref 参数
    flow.emplace([](int& counter, tfl::MultiJump& jmp) {
        counter += 1;
        std::cout << "G4c. MultiJump + ref: " << counter << "\n";
    }, std::ref(global_counter));

    // G4d. 仿函数
    flow.emplace(MultiJumpFunctor{});

    // --- G.5: Runtime (运行时动态任务) ---

    // G5a. Lambda 无参
    flow.emplace([](tfl::Runtime& rt) {
        std::cout << "G5a. Runtime lambda\n";
    });

    // G5b. Lambda + 右值参数
    flow.emplace([](int val, tfl::Runtime& rt) {
        std::cout << "G5b. Runtime + rvalue arg: " << val << "\n";
    }, 42);

    // G5c. Lambda + std::ref 参数
    flow.emplace([](int& counter, tfl::Runtime& rt) {
        counter += 1;
        std::cout << "G5c. Runtime + ref: " << counter << "\n";
    }, std::ref(global_counter));

    // G5d. Lambda + atomic ref
    flow.emplace([](std::atomic<int>& c, tfl::Runtime& rt) {
        c.fetch_add(1);
        std::cout << "G5d. Runtime + atomic ref: " << c.load() << "\n";
    }, std::ref(atomic_counter));

    // G5e. 仿函数
    flow.emplace(RuntimeFunctor{});

    // G5f. 仿函数 + 参数
    flow.emplace(RuntimeArgsFunctor{}, 42);

    // G5g. 成员函数
    flow.emplace(&MyService::runtime_method, &service);

    // G5h. 成员函数 + std::ref(对象)
    flow.emplace(&MyService::runtime_method, std::ref(service));

    // G5i. noexcept Lambda
    flow.emplace([](tfl::Runtime& rt) noexcept {
        // 编译期消除 try-catch
    });

    // ========================================================================
    // 类别 H: Subflow (子图展开) — 所有花样
    // ========================================================================

    // H1. 子图右值 (move 进 SubflowWork，SubflowWork 拥有子图)
    {
        tfl::Flow sub;
        sub.emplace([]{ std::cout << "H1. subflow rvalue inner\n"; });
        flow.emplace(std::move(sub));
    }

    // H2. 子图左值 (detail::wrap → reference_wrapper，子图生命周期由外部保证)
    tfl::Flow persistent_sub;
    persistent_sub.emplace([]{ std::cout << "H2. subflow lvalue inner\n"; });
    flow.emplace(persistent_sub);

    // H3. 固定次数循环
    flow.emplace(persistent_sub, 5ULL);

    // H4. 谓词循环 — Lambda
    {
        int runs = 0;
        flow.emplace(persistent_sub, [&runs]() mutable noexcept -> bool {
            return ++runs >= 3;
        });
    }

    // H5. 谓词循环 — 有状态仿函数 (move 进 SubflowWork)
    flow.emplace(persistent_sub, LoopCondition{3});

    // H6. 子图右值 + 固定次数
    {
        tfl::Flow sub2;
        sub2.emplace([]{ std::cout << "H6. subflow rvalue + count\n"; });
        flow.emplace(std::move(sub2), 2ULL);
    }

    // H7. 子图右值 + 谓词
    {
        tfl::Flow sub3;
        sub3.emplace([]{ std::cout << "H7. subflow rvalue + pred\n"; });
        flow.emplace(std::move(sub3), LoopCondition{2});
    }

    // ========================================================================
    // 类别 I: 批量插入 — emplace(Ts&&...) 与 emplace(Tuples&&...)
    // ========================================================================

    // I1. 批量插入多个无参 Lambda + 结构化绑定
    auto [i1_t1, i1_t2, i1_t3] = flow.emplace(
        []{ std::cout << "I1. batch_1\n"; },
        []{ std::cout << "I1. batch_2\n"; },
        []{ std::cout << "I1. batch_3\n"; }
        );
    i1_t1.precede(i1_t2);
    i1_t2.precede(i1_t3);

    // I2. 批量插入混合类型 (仿函数 + Lambda + noexcept Lambda)
    auto [i2_t1, i2_t2] = flow.emplace(
        ConstFunctor{},
        []() noexcept { std::cout << "I2. mixed batch noexcept\n"; }
        );
    i2_t1.precede(i2_t2);

    // I3. 批量 Tuple 插入 — 每个 tuple 是一个带参数的任务
    auto [i3_t1, i3_t2, i3_t3] = flow.emplace(
        // 成员函数 + 对象指针 + 参数
        std::tuple{&MyService::process, &service, 999},
        // Functor + std::ref 参数
        std::tuple{MyFunctor{10}, std::ref(global_counter)},
        // Lambda + 参数
        std::tuple{[](int x) { std::cout << "I3. tuple lambda x=" << x << "\n"; }, 42}
        );

    // I4. 批量 Tuple + ref 参数
    auto [i4_t1, i4_t2] = flow.emplace(
        std::tuple{free_func_ref, std::ref(global_counter)},
        std::tuple{[](int& r) { r += 1; }, std::ref(global_counter)}
        );

    // ========================================================================
    // 类别 J: std::thread 风格语义验证
    // ========================================================================

    // J1. 左值 decay copy — 修改不影响外部
    {
        int local = 42;
        flow.emplace([](int v) {
            // v 是 local 的副本，修改 v 不影响 local
            std::cout << "J1. decay copy: v=" << v << "\n";
        }, local);
        // local 保持 42
    }

    // J2. std::ref 写回 — 修改影响外部
    {
        int local = 0;
        flow.emplace([](int& r) {
            r = 42;
            std::cout << "J2. ref write-back: r=" << r << "\n";
        }, std::ref(local));
        // 执行后 local == 42
    }

    // J3. 右值 move — 原变量被掏空
    {
        std::string s = "original";
        flow.emplace([](std::string str) {
            std::cout << "J3. rvalue move: " << str << "\n";
        }, std::move(s));
        // s 可能为空
    }

    // J4. shared_ptr 通过 ref 不增加引用计数
    {
        auto sp = std::make_shared<int>(42);
        long use_before = sp.use_count();
        flow.emplace([](std::shared_ptr<int>& p) {
            std::cout << "J4. shared_ptr ref: " << *p << "\n";
        }, std::ref(sp));
        // use_count 不变
    }

    // J5. 多个 ref 同时写回
    {
        int x = 0, y = 0, z = 0;
        flow.emplace([](int& a, int& b, int& c) {
            a = 1; b = 2; c = 3;
            std::cout << "J5. multi ref: " << a << " " << b << " " << c << "\n";
        }, std::ref(x), std::ref(y), std::ref(z));
    }

    // J6. atomic 必须用 std::ref (不可拷贝)
    {
        std::atomic<int> ac{0};
        int plain = 0;
        flow.emplace([](std::atomic<int>& c, int v) {
            c.fetch_add(v, std::memory_order_relaxed);
            std::cout << "J6. atomic=" << c.load() << " plain_copy=" << v << "\n";
        }, std::ref(ac), plain);  // ac 引用，plain decay copy
    }

    // ========================================================================
    // 输出 D2 可视化图
    // ========================================================================

    std::cout << "\n========== D2 Visualization ==========\n";
    std::cout << flow.dump() << "\n";

    // ========================================================================
    // 类别 K: Executor 提交 — 所有 submit/async/silent_async 变体
    // ========================================================================

    // 注意：Flow 提交需要 Executor，以下单独建图测试

    tfl::ResumeNever handler;
    tfl::Executor exe(handler, 4);

    // K1. submit(Flow) — 执行1次
    {
        tfl::Flow f; f.emplace([]{ std::cout << "K1. submit once\n"; });
        exe.submit(f).start().wait();
    }

    // K2. submit(Flow, callback) — 执行1次 + 完成回调
    {
        tfl::Flow f; f.emplace([]{ std::cout << "K2. submit + callback\n"; });
        exe.submit(f, []() noexcept { std::cout << "K2. callback fired\n"; }).start().wait();
    }

    // K3. submit(Flow, N) — 执行N次
    {
        tfl::Flow f;
        std::atomic<int> c{0};
        f.emplace([](std::atomic<int>& c) { c.fetch_add(1); }, std::ref(c));
        exe.submit(f, 5ULL).start().wait();
        exe.wait_for_all();
        std::cout << "K3. submit N=5, count=" << c.load() << "\n";
    }

    // K4. submit(Flow, N, callback) — 执行N次 + 回调
    {
        tfl::Flow f;
        std::atomic<int> c{0};
        f.emplace([](std::atomic<int>& c) { c.fetch_add(1); }, std::ref(c));
        exe.submit(f, 3ULL, []() noexcept {
               std::cout << "K4. callback after 3 runs\n";
           }).start().wait();
        exe.wait_for_all();
        std::cout << "K4. count=" << c.load() << "\n";
    }

    // K5. submit(Flow, predicate) — 谓词循环
    {
        tfl::Flow f;
        std::atomic<int> c{0};
        f.emplace([](std::atomic<int>& c) { c.fetch_add(1); }, std::ref(c));
        int runs = 0;
        exe.submit(f, [&runs]() mutable noexcept -> bool {
               return ++runs >= 4;
           }).start().wait();
        exe.wait_for_all();
        std::cout << "K5. predicate loop, count=" << c.load() << "\n";
    }

    // K6. submit(Flow, predicate, callback)
    {
        tfl::Flow f;
        std::atomic<int> c{0};
        f.emplace([](std::atomic<int>& c) { c.fetch_add(1); }, std::ref(c));
        int runs = 0;
        exe.submit(f,
                   [&runs]() mutable noexcept -> bool { return ++runs >= 2; },
                   []() noexcept { std::cout << "K6. pred+callback done\n"; }
                   ).start().wait();
        exe.wait_for_all();
        std::cout << "K6. count=" << c.load() << "\n";
    }

    // K7. submit(basic_task) — 独立基础任务
    {
        exe.submit([]{ std::cout << "K7. submit basic task\n"; }).start().wait();
    }

    // K8. submit(basic_task, args) — 独立基础任务 + 参数
    {
        int x = 0;
        exe.submit([](int& v) { v = 42; std::cout << "K8. v=" << v << "\n"; },
                   std::ref(x)).start().wait();
        exe.wait_for_all();
    }

    // K9. submit(runtime_task)
    {
        exe.submit([](tfl::Runtime& rt) {
               std::cout << "K9. submit runtime task\n";
           }).start().wait();
    }

    // K10. submit(runtime_task, args)
    {
        int x = 0;
        exe.submit([](int& v, tfl::Runtime& rt) {
               v = 77;
               std::cout << "K10. runtime v=" << v << "\n";
           }, std::ref(x)).start().wait();
        exe.wait_for_all();
    }

    // K11. silent_async(basic_task) — fire-and-forget
    {
        exe.silent_async([]{ std::cout << "K11. silent_async basic\n"; });
        exe.wait_for_all();
    }

    // K12. silent_async(basic_task, args)
    {
        std::atomic<int> c{0};
        exe.silent_async([](std::atomic<int>& v) { v.fetch_add(1); }, std::ref(c));
        exe.wait_for_all();
        std::cout << "K12. silent_async count=" << c.load() << "\n";
    }

    // K13. silent_async(runtime_task)
    {
        exe.silent_async([](tfl::Runtime& rt) {
            std::cout << "K13. silent_async runtime\n";
        });
        exe.wait_for_all();
    }

    // K14. silent_async(runtime_task, args)
    {
        std::atomic<int> c{0};
        exe.silent_async([](std::atomic<int>& v, tfl::Runtime& rt) { v.fetch_add(1); },
                         std::ref(c));
        exe.wait_for_all();
        std::cout << "K14. silent_async runtime count=" << c.load() << "\n";
    }

    // K15. async(task) → future<void>
    {
        auto fut = exe.async([]{ std::cout << "K15. async void\n"; });
        fut.get();
        exe.wait_for_all();
    }

    // K16. async(task) → future<int>
    {
        auto fut = exe.async([]() -> int {
            std::cout << "K16. async int\n";
            return 42;
        });
        int result = fut.get();
        exe.wait_for_all();
        std::cout << "K16. result=" << result << "\n";
    }

    // K17. async(task, args) → future<int>
    {
        auto fut = exe.async([](int a, int b) -> int { return a + b; }, 10, 32);
        int result = fut.get();
        exe.wait_for_all();
        std::cout << "K17. async args result=" << result << "\n";
    }

    // K18. async(runtime_task) → future<void>
    {
        auto fut = exe.async([](tfl::Runtime& rt) {
            std::cout << "K18. async runtime\n";
        });
        fut.get();
        exe.wait_for_all();
    }

    // K19. async(runtime_task, args) → future<int>
    {
        auto fut = exe.async([](int x, tfl::Runtime& rt) -> int { return x * 2; }, 21);
        int result = fut.get();
        exe.wait_for_all();
        std::cout << "K19. async runtime result=" << result << "\n";
    }

    // ========================================================================
    // 类别 L: AsyncTask 依赖链 — start(deps...)
    // ========================================================================

    // L1. 两个独立任务，t2 依赖 t1
    {
        std::atomic<int> order{0};
        auto t1 = exe.submit([&order]{ order.store(1); std::cout << "L1. t1\n"; });
        auto t2 = exe.submit([&order]{
            std::cout << "L1. t2, order=" << order.load() << "\n";
        });
        t1.start().wait();
        t2.start(t1).wait();
        exe.wait_for_all();
    }

    // L2. 三级依赖链
    {
        std::atomic<int> step{0};
        auto s1 = exe.submit([&step]{ step.store(1); });
        auto s2 = exe.submit([&step]{ step.store(2); });
        auto s3 = exe.submit([&step]{
            std::cout << "L2. final step=" << step.load() << "\n";
        });
        s1.start().wait();
        s2.start(s1).wait();
        s3.start(s2).wait();
        exe.wait_for_all();
    }

    // ========================================================================
    // 类别 M: 多线程全连接 DAG 压力测试
    // ========================================================================
    {
        constexpr int LAYERS = 10;
        constexpr int PER_LAYER = 10;
        constexpr int ITERATIONS = 5;

        tfl::Flow dag;
        std::atomic<int> dag_counter{0};
        std::vector<std::vector<tfl::Task>> layers(LAYERS);

        for (int layer = 0; layer < LAYERS; ++layer) {
            layers[layer].reserve(PER_LAYER);
            for (int i = 0; i < PER_LAYER; ++i) {
                auto t = dag.emplace([](std::atomic<int>& c) {
                    c.fetch_add(1, std::memory_order_relaxed);
                }, std::ref(dag_counter));
                layers[layer].push_back(t);
            }
            if (layer > 0) {
                for (auto& prev : layers[layer - 1]) {
                    for (auto& curr : layers[layer]) {
                        prev.precede(curr);
                    }
                }
            }
        }

        exe.submit(dag, static_cast<uint64_t>(ITERATIONS)).start().wait();
        exe.wait_for_all();

        int expected = LAYERS * PER_LAYER * ITERATIONS;
        std::cout << "M. DAG counter=" << dag_counter.load()
                  << " expected=" << expected << "\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "All exhaustive tests completed!\n";
    std::cout << "global_counter=" << global_counter << "\n";
    std::cout << "atomic_counter=" << atomic_counter.load() << "\n";
    std::cout << "========================================\n";

    return 0;
}
