#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <iostream>

#include "eta/runtime/error.h"

namespace eta::runtime {

/**
 * @brief Abstract base class for I/O ports in the Eta Scheme runtime.
 *
 * Ports provide a unified interface for reading from and writing to
 * various sources/destinations including console, strings, and files.
 */
class Port {
public:
    virtual ~Port() = default;

    /**
     * @brief Read a single character from the input port.
     * @return The character read, or std::nullopt if EOF is reached.
     */
    virtual std::optional<char32_t> read_char() {
        return std::nullopt;  // Default: not readable
    }

    /**
     * @brief Write a string to the output port.
     * @param str The string to write.
     * @return An error if the write failed.
     */
    virtual std::expected<void, error::RuntimeError> write_string(const std::string& str) {
        return std::unexpected(error::RuntimeError{
            error::VMError{error::RuntimeErrorCode::TypeError, "Port is not writable"}
        });
    }

    /**
     * @brief Flush any buffered output.
     * @return An error if the flush failed.
     */
    virtual std::expected<void, error::RuntimeError> flush() {
        return {};  // Default: no-op for ports without buffering
    }

    /**
     * @brief Close the port and release any resources.
     * @return An error if closing failed.
     */
    virtual std::expected<void, error::RuntimeError> close() {
        return {};  // Default: no-op
    }

    /**
     * @brief Check if the port is open.
     * @return true if the port is open, false otherwise.
     */
    virtual bool is_open() const {
        return true;  // Default: always open
    }

    /**
     * @brief Check if the port is an input port.
     */
    virtual bool is_input() const {
        return false;
    }

    /**
     * @brief Check if the port is an output port.
     */
    virtual bool is_output() const {
        return false;
    }
};

/**
 * @brief Console port that reads from stdin and writes to stdout/stderr.
 */
class ConsolePort : public Port {
public:
    enum class StreamType {
        Input,   // stdin
        Output,  // stdout
        Error    // stderr
    };

    explicit ConsolePort(StreamType type) : stream_type_(type) {}

    std::optional<char32_t> read_char() override {
        if (stream_type_ != StreamType::Input) {
            return std::nullopt;
        }
        int ch = std::cin.get();
        if (ch == EOF) {
            return std::nullopt;
        }
        // Simplified: only handle ASCII for now
        return static_cast<char32_t>(ch);
    }

    std::expected<void, error::RuntimeError> write_string(const std::string& str) override {
        if (stream_type_ == StreamType::Input) {
            return std::unexpected(error::RuntimeError{
                error::VMError{error::RuntimeErrorCode::TypeError, "Cannot write to input port"}
            });
        }

        if (stream_type_ == StreamType::Output) {
            std::cout << str;
        } else {
            std::cerr << str;
        }
        return {};
    }

    std::expected<void, error::RuntimeError> flush() override {
        if (stream_type_ == StreamType::Output) {
            std::cout << std::flush;
        } else if (stream_type_ == StreamType::Error) {
            std::cerr << std::flush;
        }
        return {};
    }

    bool is_input() const override {
        return stream_type_ == StreamType::Input;
    }

    bool is_output() const override {
        return stream_type_ == StreamType::Output || stream_type_ == StreamType::Error;
    }

private:
    StreamType stream_type_;
};

/**
 * @brief String port that reads from or writes to an in-memory string buffer.
 */
class StringPort : public Port {
public:
    enum class Mode {
        Input,
        Output
    };

    explicit StringPort(Mode mode, std::string initial_content = "")
        : mode_(mode), content_(std::move(initial_content)), read_pos_(0) {}

    std::optional<char32_t> read_char() override {
        if (mode_ != Mode::Input) {
            return std::nullopt;
        }
        if (read_pos_ >= content_.size()) {
            return std::nullopt;  // EOF
        }
        // Simplified: only handle ASCII for now
        char ch = content_[read_pos_++];
        return static_cast<char32_t>(ch);
    }

    std::expected<void, error::RuntimeError> write_string(const std::string& str) override {
        if (mode_ != Mode::Output) {
            return std::unexpected(error::RuntimeError{
                error::VMError{error::RuntimeErrorCode::TypeError, "Cannot write to input string port"}
            });
        }
        content_ += str;
        return {};
    }

    /**
     * @brief Get the accumulated string content (for output ports).
     */
    const std::string& get_string() const {
        return content_;
    }

    bool is_input() const override {
        return mode_ == Mode::Input;
    }

    bool is_output() const override {
        return mode_ == Mode::Output;
    }

private:
    Mode mode_;
    std::string content_;
    size_t read_pos_;
};

/**
 * @brief File port stub for future file I/O implementation.
 *
 * Currently provides a placeholder implementation.
 */
class FilePort : public Port {
public:
    enum class Mode {
        Read,
        Write,
        Append
    };

    explicit FilePort(const std::string& filename, Mode mode)
        : filename_(filename), mode_(mode), is_open_(false) {
        // TODO: Implement actual file I/O
        // For now, this is just a stub
    }

    std::optional<char32_t> read_char() override {
        // TODO: Implement file reading
        return std::nullopt;
    }

    std::expected<void, error::RuntimeError> write_string(const std::string& str) override {
        // TODO: Implement file writing
        return std::unexpected(error::RuntimeError{
            error::VMError{error::RuntimeErrorCode::TypeError, "FilePort not yet implemented"}
        });
    }

    std::expected<void, error::RuntimeError> close() override {
        is_open_ = false;
        // TODO: Actually close file
        return {};
    }

    bool is_open() const override {
        return is_open_;
    }

    bool is_input() const override {
        return mode_ == Mode::Read;
    }

    bool is_output() const override {
        return mode_ == Mode::Write || mode_ == Mode::Append;
    }

private:
    std::string filename_;
    Mode mode_;
    bool is_open_;
};

}  // namespace eta::runtime

