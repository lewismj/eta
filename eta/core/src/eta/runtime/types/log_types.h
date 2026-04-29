#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace eta::runtime::types {

    enum class LogFormatterMode : std::uint8_t {
        Human,
        Json,
    };

    /**
     * @brief Opaque logging sink runtime object.
     *
     * The underlying sink implementation is stored in `state` and resolved by
     * the logging subsystem. This runtime wrapper carries sink traits used by
     * primitive validation.
     */
    struct LogSink {
        std::shared_ptr<void> state;
        bool is_port_sink{false};
        bool is_current_error_sink{false};
    };

    /**
     * @brief Opaque logger runtime object.
     *
     * `state` points to the native logger state managed by the logging
     * subsystem. `name` and `formatter_mode` are mirrored for value formatting
     * and diagnostics.
     */
    struct LogLogger {
        std::shared_ptr<void> state;
        std::string name;
        LogFormatterMode formatter_mode{LogFormatterMode::Human};
    };

} ///< namespace eta::runtime::types
