#pragma once

#include <variant>
#include <utility>

namespace pitrove {

template <typename T, typename E>
class Expected {
public:
    Expected(T value)
        : data_(std::move(value)) {}

    Expected(E error)
        : data_(Error{std::move(error)}) {}

    bool has_value() const {
        return std::holds_alternative<T>(data_);
    }

    explicit operator bool() const {
        return has_value();
    }

    const T& value() const {
        return std::get<T>(data_);
    }

    T& value() {
        return std::get<T>(data_);
    }

    const E& error() const {
        return std::get<Error>(data_).error;
    }

private:
    struct Error {
        E error;
    };

    std::variant<T, Error> data_;
};

} // namespace pitrove
