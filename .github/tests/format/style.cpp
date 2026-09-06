// Formatting fixture: intentionally preserve this include order.
#include <utility>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

struct Payload {
    template <typename T>
    static constexpr bool valid_type = sizeof(T) != 0;
};

class FormatProbe {
public:
    using Unsigned = unsigned;
    using Integer = int;

    template <typename T>
        requires (Payload::template valid_type<T>)
    [[nodiscard]] T& target(T& value) noexcept {
        return value;
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_count.load(std::memory_order_relaxed) == 0;
    }

private:
    std::atomic<std::size_t>     m_count{0};         ///< 运行期计数；保持初始化与声明在同一行。
    const Payload*               m_payload{nullptr}; ///< 非拥有的载荷指针；由调用者保证生命周期。
    std::unique_ptr<std::string> m_name;             ///< 按需分配的节点名称；中文注释保持完整，长说明不会挤断成员声明、初始化表达式或者原有的手动换行，也不把普通多行函数压缩成单行；此处用于验证超过一百列的 Doxygen 注释仍然保留原样。
};
