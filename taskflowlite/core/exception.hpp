#pragma once
#include <exception>
#include <string>
#include <source_location>
#include <stacktrace>
#include <format>
#include "utility.hpp"

namespace tfl {
class Exception : public std::exception {
public:
    template <typename... Args>
    explicit Exception(Located<std::format_string<Args...>> fmt, Args&&... args)
        : m_message{std::vformat(fmt.format().get(), std::make_format_args(args...))}
        , m_location{fmt.location()}
    {}

    explicit Exception(std::string_view message,
                       std::source_location loc = std::source_location::current())
        : m_message{message}
        , m_location{loc}
    {}

    [[nodiscard]] const char* what() const noexcept override {
        return m_message.c_str();
    }

    [[nodiscard]] const std::source_location& where() const noexcept {
        return m_location;
    }

protected:
    std::string m_message;
    std::source_location m_location;
};

class TraceException : public Exception {
public:
    template <typename... Args>
    explicit TraceException(Traced<std::format_string<Args...>> fmt, Args&&... args)
        : Exception{static_cast<const Located<std::format_string<Args...>>&>(fmt), std::forward<Args>(args)...}
        , m_stacktrace{fmt.stacktrace()}
    {}

    explicit TraceException(std::string_view message,
                            std::source_location loc = std::source_location::current(),
                            std::stacktrace trace = std::stacktrace::current())
        : Exception{message, loc}
        , m_stacktrace{std::move(trace)}
    {}

    [[nodiscard]] const std::stacktrace& trace() const noexcept {
        return m_stacktrace;
    }

private:
    std::stacktrace m_stacktrace;
};
}  // namespace tfl
