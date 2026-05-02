#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>

#include "eta/runtime/error.h"

namespace eta::runtime {

/**
 * @brief UTF-8 encoding/decoding utilities
 */
namespace utf8 {
    /**
     * @brief Encode a Unicode code point to UTF-8 bytes
     */
    inline std::string encode(char32_t codepoint) {
        std::string result;
        if (codepoint <= 0x7F) {
            result += static_cast<char>(codepoint);
        } else if (codepoint <= 0x7FF) {
            result += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint <= 0xFFFF) {
            result += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
            result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint <= 0x10FFFF) {
            result += static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07));
            result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        return result;
    }

    /**
     * @brief Decode UTF-8 bytes to a Unicode code point
     * @param input The input string (will be advanced past the decoded character)
     * @param pos Position in the string (will be updated)
     * @return The decoded code point, or std::nullopt on error
     */
    inline std::optional<char32_t> decode(const std::string& input, size_t& pos) {
        if (pos >= input.size()) return std::nullopt;

        unsigned char b1 = input[pos++];

        /// 1-byte sequence (ASCII)
        if ((b1 & 0x80) == 0) {
            return static_cast<char32_t>(b1);
        }

        /// 2-byte sequence
        if ((b1 & 0xE0) == 0xC0) {
            if (pos >= input.size()) return std::nullopt;
            unsigned char b2 = input[pos++];
            if ((b2 & 0xC0) != 0x80) return std::nullopt;
            return static_cast<char32_t>(((b1 & 0x1F) << 6) | (b2 & 0x3F));
        }

        /// 3-byte sequence
        if ((b1 & 0xF0) == 0xE0) {
            if (pos + 1 >= input.size()) return std::nullopt;
            unsigned char b2 = input[pos++];
            unsigned char b3 = input[pos++];
            if ((b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return std::nullopt;
            return static_cast<char32_t>(((b1 & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F));
        }

        /// 4-byte sequence
        if ((b1 & 0xF8) == 0xF0) {
            if (pos + 2 >= input.size()) return std::nullopt;
            unsigned char b2 = input[pos++];
            unsigned char b3 = input[pos++];
            unsigned char b4 = input[pos++];
            if ((b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80 || (b4 & 0xC0) != 0x80) return std::nullopt;
            return static_cast<char32_t>(((b1 & 0x07) << 18) | ((b2 & 0x3F) << 12) | ((b3 & 0x3F) << 6) | (b4 & 0x3F));
        }

        return std::nullopt; ///< Invalid UTF-8
    }
}

/**
 * @brief Port buffering mode
 */
enum class BufferMode {
    None,      ///< Unbuffered
    Line,      ///< Line buffered (flush on newline)
    Block      ///< Block buffered (flush on buffer full or explicit flush)
};

/**
 * @brief Port encoding type
 */
enum class Encoding {
    UTF8,      ///< UTF-8 encoding (default)
    ASCII,     ///< ASCII only (7-bit)
    Binary     ///< Raw bytes (no encoding)
};

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
        return std::nullopt;  ///< Default: not readable
    }

    /**
     * @brief Write a string to the output port.
     * @param str The string to write.
     * @return An error if the write failed.
     */
    virtual std::expected<void, error::RuntimeError> write_string(const std::string& /*str*/) {
        return std::unexpected(error::RuntimeError{
            error::VMError{error::RuntimeErrorCode::TypeError, "Port is not writable"}
        });
    }

    /**
     * @brief Flush any buffered output.
     * @return An error if the flush failed.
     */
    virtual std::expected<void, error::RuntimeError> flush() {
        return {};  ///< Default: no-op for ports without buffering
    }

    /**
     * @brief Close the port and release any resources.
     * @return An error if closing failed.
     */
    virtual std::expected<void, error::RuntimeError> close() {
        return {};  ///< Default: no-op
    }

    /**
     * @brief Check if the port is open.
     * @return true if the port is open, false otherwise.
     */
    virtual bool is_open() const {
        return true;  ///< Default: always open
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

    /**
     * @brief Check if the port is a binary port.
     */
    virtual bool is_binary() const {
        return false;
    }

    /**
     * @brief Get the port's encoding.
     */
    virtual Encoding encoding() const {
        return Encoding::UTF8;
    }

    /**
     * @brief Get the port's buffer mode.
     */
    virtual BufferMode buffer_mode() const {
        return BufferMode::Block;
    }
};

/**
 * @brief Console port that reads from stdin and writes to stdout/stderr.
 */
class ConsolePort : public Port {
public:
    enum class StreamType {
        Input,   ///< stdin
        Output,  ///< stdout
        Error    ///< stderr
    };

    explicit ConsolePort(StreamType type) : stream_type_(type) {}

    std::optional<char32_t> read_char() override {
        if (stream_type_ != StreamType::Input) {
            return std::nullopt;
        }

        /// Read UTF-8 encoded character from stdin
        std::string utf8_bytes;
        int ch = std::cin.get();
        if (ch == EOF) {
            return std::nullopt;
        }

        utf8_bytes += static_cast<char>(ch);

        /// Check if this is a multi-byte UTF-8 sequence
        unsigned char first_byte = static_cast<unsigned char>(ch);
        int expected_bytes = 0;

        if ((first_byte & 0x80) == 0) {
            /// ASCII (1 byte)
            expected_bytes = 0;
        } else if ((first_byte & 0xE0) == 0xC0) {
            expected_bytes = 1;
        } else if ((first_byte & 0xF0) == 0xE0) {
            expected_bytes = 2;
        } else if ((first_byte & 0xF8) == 0xF0) {
            expected_bytes = 3;
        }

        /// Read continuation bytes
        for (int i = 0; i < expected_bytes; ++i) {
            ch = std::cin.get();
            if (ch == EOF) return std::nullopt;
            utf8_bytes += static_cast<char>(ch);
        }

        /// Decode UTF-8
        size_t pos = 0;
        return utf8::decode(utf8_bytes, pos);
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

    BufferMode buffer_mode() const override {
        /// stdout is typically line-buffered, stderr is unbuffered
        return stream_type_ == StreamType::Error ? BufferMode::None : BufferMode::Line;
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

    explicit StringPort(Mode mode, std::string initial_content = "", Encoding enc = Encoding::UTF8)
        : mode_(mode), content_(std::move(initial_content)), read_pos_(0), encoding_(enc) {}

    std::optional<char32_t> read_char() override {
        if (mode_ != Mode::Input) {
            return std::nullopt;
        }
        if (read_pos_ >= content_.size()) {
            return std::nullopt;  ///< EOF
        }

        if (encoding_ == Encoding::UTF8) {
            return utf8::decode(content_, read_pos_);
        } else {
            /// ASCII: just read single byte
            char ch = content_[read_pos_++];
            return static_cast<char32_t>(ch);
        }
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

    Encoding encoding() const override {
        return encoding_;
    }

private:
    Mode mode_;
    std::string content_;
    size_t read_pos_;
    Encoding encoding_;
};

/**
 * @brief File port for file I/O operations.
 */
class FilePort : public Port {
public:
    enum class Mode {
        Read,
        Write,
        Append
    };

    explicit FilePort(const std::string& filename, Mode mode, Encoding enc = Encoding::UTF8)
        : filename_(filename), mode_(mode), encoding_(enc), is_open_(false) {

        std::ios_base::openmode flags = std::ios::binary;  ///< Always binary for UTF-8 control

        if (mode == Mode::Read) {
            flags |= std::ios::in;
            file_.open(filename, flags);
        } else if (mode == Mode::Write) {
            flags |= std::ios::out | std::ios::trunc;
            file_.open(filename, flags);
        } else {  ///< Append
            flags |= std::ios::out | std::ios::app;
            file_.open(filename, flags);
        }

        is_open_ = file_.is_open();
    }

    ~FilePort() override {
        if (file_.is_open()) {
            file_.close();
        }
    }

    std::optional<char32_t> read_char() override {
        if (!is_open_ || mode_ != Mode::Read) {
            return std::nullopt;
        }

        if (encoding_ == Encoding::UTF8) {
            /// Read UTF-8 encoded character
            std::string utf8_bytes;
            int ch = file_.get();
            if (ch == EOF) {
                return std::nullopt;
            }

            utf8_bytes += static_cast<char>(ch);
            unsigned char first_byte = static_cast<unsigned char>(ch);
            int expected_bytes = 0;

            if ((first_byte & 0x80) == 0) {
                expected_bytes = 0;
            } else if ((first_byte & 0xE0) == 0xC0) {
                expected_bytes = 1;
            } else if ((first_byte & 0xF0) == 0xE0) {
                expected_bytes = 2;
            } else if ((first_byte & 0xF8) == 0xF0) {
                expected_bytes = 3;
            }

            for (int i = 0; i < expected_bytes; ++i) {
                ch = file_.get();
                if (ch == EOF) return std::nullopt;
                utf8_bytes += static_cast<char>(ch);
            }

            size_t pos = 0;
            return utf8::decode(utf8_bytes, pos);
        } else {
            /// ASCII
            int ch = file_.get();
            if (ch == EOF) {
                return std::nullopt;
            }
            return static_cast<char32_t>(ch);
        }
    }

    std::expected<void, error::RuntimeError> write_string(const std::string& str) override {
        if (!is_open_ || mode_ == Mode::Read) {
            return std::unexpected(error::RuntimeError{
                error::VMError{error::RuntimeErrorCode::TypeError, "FilePort is not writable"}
            });
        }

        file_ << str;

        if (file_.fail()) {
            return std::unexpected(error::RuntimeError{
                error::VMError{error::RuntimeErrorCode::TypeError, "File write failed"}
            });
        }

        return {};
    }

    std::expected<void, error::RuntimeError> flush() override {
        if (is_open_ && mode_ != Mode::Read) {
            file_.flush();
        }
        return {};
    }

    std::expected<void, error::RuntimeError> close() override {
        if (file_.is_open()) {
            file_.close();
            is_open_ = false;
        }
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

    Encoding encoding() const override {
        return encoding_;
    }

private:
    std::string filename_;
    Mode mode_;
    Encoding encoding_;
    bool is_open_;
    std::fstream file_;
};

/**
 * @brief Base class for ports that support byte-oriented I/O.
 */
class BytePort : public Port {
public:
    /**
     * @brief Read a single byte.
     * @return The byte read, or std::nullopt on EOF.
     */
    virtual std::optional<uint8_t> read_byte() {
        return std::nullopt;
    }

    /**
     * @brief Write a single byte.
     * @return An error if the write failed.
     */
    virtual std::expected<void, error::RuntimeError> write_byte(uint8_t /*byte*/) {
        return std::unexpected(error::RuntimeError{
            error::VMError{error::RuntimeErrorCode::TypeError, "Port is not writable"}
        });
    }

    /**
     * @brief Write multiple bytes.
     * @return An error if the write failed.
     */
    virtual std::expected<void, error::RuntimeError> write_bytes(const std::vector<uint8_t>& bytes) {
        for (uint8_t b : bytes) {
            auto r = write_byte(b);
            if (!r) return r;
        }
        return {};
    }

    bool is_binary() const override {
        return true;
    }

    Encoding encoding() const override {
        return Encoding::Binary;
    }
};

/**
 * @brief Binary port for bytevector I/O operations.
 *
 * Reads and writes raw bytes without any encoding.
 */
class BinaryPort : public BytePort {
public:
    enum class Mode {
        Input,
        Output
    };

    explicit BinaryPort(Mode mode, std::vector<uint8_t> initial_data = {})
        : mode_(mode), data_(std::move(initial_data)), read_pos_(0) {}

    /**
     * @brief Read a single byte.
     * @return The byte read, or std::nullopt if EOF.
     */
    std::optional<uint8_t> read_byte() override {
        if (mode_ != Mode::Input || read_pos_ >= data_.size()) {
            return std::nullopt;
        }
        return data_[read_pos_++];
    }

    /**
     * @brief Write a single byte.
     */
    std::expected<void, error::RuntimeError> write_byte(uint8_t byte) override {
        if (mode_ != Mode::Output) {
            return std::unexpected(error::RuntimeError{
                error::VMError{error::RuntimeErrorCode::TypeError, "Cannot write to input binary port"}
            });
        }
        data_.push_back(byte);
        return {};
    }

    /**
     * @brief Write multiple bytes.
     */
    std::expected<void, error::RuntimeError> write_bytes(const std::vector<uint8_t>& bytes) override {
        if (mode_ != Mode::Output) {
            return std::unexpected(error::RuntimeError{
                error::VMError{error::RuntimeErrorCode::TypeError, "Cannot write to input binary port"}
            });
        }
        data_.insert(data_.end(), bytes.begin(), bytes.end());
        return {};
    }

    /**
     * @brief Get the accumulated byte data.
     */
    const std::vector<uint8_t>& get_bytes() const {
        return data_;
    }

    bool is_input() const override {
        return mode_ == Mode::Input;
    }

    bool is_output() const override {
        return mode_ == Mode::Output;
    }

private:
    Mode mode_;
    std::vector<uint8_t> data_;
    size_t read_pos_;
};

/**
 * @brief Output port that invokes a callback instead of writing to a stream.
 *
 * Used by the DAP server to redirect script output into DAP "output" events
 * instead of letting it corrupt the stdin/stdout protocol pipe.
 * Also useful in tests to capture VM output into a std::string.
 */
class CallbackPort : public Port {
public:
    using Callback = std::function<void(const std::string&)>;

    explicit CallbackPort(Callback cb) : cb_(std::move(cb)) {}

    std::expected<void, error::RuntimeError> write_string(const std::string& str) override {
        if (cb_) cb_(str);
        return {};
    }

    bool is_output() const override { return true; }

    BufferMode buffer_mode() const override { return BufferMode::Line; }

private:
    Callback cb_;
};

}  ///< namespace eta::runtime

