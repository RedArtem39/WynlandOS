#pragma once
#include <optional>
#include <cstdint>
#include <string>

namespace Hyprutils::String {
    enum eNumericParseResult : uint8_t {
        NUMERIC_PARSE_OK = 0,
        NUMERIC_PARSE_BAD,
        NUMERIC_PARSE_GARBAGE,
        NUMERIC_PARSE_OUT_OF_RANGE
    };

    template <typename T>
    struct CNumericResult {
        std::optional<T> val;
        eNumericParseResult err = NUMERIC_PARSE_OK;
        CNumericResult() = default;
        CNumericResult(T v) : val(v), err(NUMERIC_PARSE_OK) {}
        CNumericResult(std::nullopt_t) : val(std::nullopt), err(NUMERIC_PARSE_BAD) {}
        
        T value_or(T def) const {
            return val.value_or(def);
        }
        operator bool() const { return val.has_value(); }
        T value() const { return val.value(); }
        T operator*() const { return *val; }
        const T* operator->() const { return &(*val); }
        eNumericParseResult error() const { return err; }
        operator std::optional<T>() const { return val; }
    };
}

#include "String.hpp"
