// core/domain/Result.h
//
// Error handling for the core. No exceptions cross a port boundary: adapters
// catch whatever their platform throws and translate it into a Result.
//
// ErrorCode itself lives in Enums.h with every other enumeration.

#pragma once

#include <string>
#include <utility>

#include "core/domain/Enums.h"

namespace anytime {

/// A void-returning outcome. Construct successes with Result::ok().
class Result {
public:
    Result() = default;  ///< default-constructs to success

    static Result ok() { return Result{}; }
    static Result error(ErrorCode code, std::string message = {}) {
        return Result{code, std::move(message)};
    }

    bool      isOk()    const noexcept { return code_ == ErrorCode::Ok; }
    bool      isError() const noexcept { return !isOk(); }
    ErrorCode code()    const noexcept { return code_; }

    /// Human-readable detail for logs and API responses. May be empty.
    const std::string& message() const noexcept { return message_; }

    explicit operator bool() const noexcept { return isOk(); }

private:
    Result(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    ErrorCode   code_ = ErrorCode::Ok;
    std::string message_;
};

/// A value-returning outcome. Access value() only when isOk() is true.
template <typename T>
class Outcome {
public:
    Outcome(T value) : value_(std::move(value)) {}       // NOLINT(google-explicit-constructor)
    Outcome(Result error) : result_(std::move(error)) {} // NOLINT(google-explicit-constructor)

    static Outcome error(ErrorCode code, std::string message = {}) {
        return Outcome{Result::error(code, std::move(message))};
    }

    bool          isOk()    const noexcept { return result_.isOk(); }
    bool          isError() const noexcept { return result_.isError(); }
    const Result& result()  const noexcept { return result_; }

    const T& value() const& noexcept { return value_; }
    T&       value() & noexcept      { return value_; }
    T&&      value() && noexcept     { return std::move(value_); }

    T valueOr(T fallback) const { return isOk() ? value_ : std::move(fallback); }

    explicit operator bool() const noexcept { return isOk(); }

private:
    Result result_;
    T      value_{};
};

}  // namespace anytime
