#pragma once

/**
 * @file ui_test_support.h
 * @brief Shared helpers for std.ui tests: frame decoding, an NNG WebSocket
 * client, a temporary UI config home and small HTTP/inbox utilities.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nng/nng.h>
#include <nng/supplemental/http/http.h>

#include <eta/ui/ui_server.h>
#include <eta/util/json.h>

namespace eta_ui_test {

namespace fs = std::filesystem;
namespace ui = eta::ui;

struct DecodedFrame {
    std::uint32_t total{0};
    std::uint32_t json_len{0};
    eta::json::Value header;
    std::string raw;

    [[nodiscard]] const eta::json::Value& body() const { return header["body"]; }

    [[nodiscard]] std::vector<double> buffer(std::size_t i) const {
        const auto& info = header["buffers"].as_array().at(i);
        const auto offset = static_cast<std::size_t>(info["offset"].as_int());
        const auto length = static_cast<std::size_t>(info["length"].as_int());
        std::vector<double> out(length);
        std::memcpy(out.data(), raw.data() + 8 + json_len + offset, length * sizeof(double));
        return out;
    }
};

inline DecodedFrame decode(const std::string& raw) {
    DecodedFrame f;
    BOOST_REQUIRE_GE(raw.size(), 8u);
    f.raw = raw;
    f.total = ui::get_u32_le(raw.data());
    f.json_len = ui::get_u32_le(raw.data() + 4);
    BOOST_REQUIRE_EQUAL(static_cast<std::size_t>(f.total) + 4, raw.size());
    f.header = eta::json::parse(std::string_view(raw.data() + 8, f.json_len));
    return f;
}

inline void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

inline void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/// Sets an environment variable for the lifetime of the guard.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = std::string(old);
        set_env(name, value);
    }
    ~ScopedEnv() {
        if (old_) set_env(name_, *old_);
        else unset_env(name_);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

struct TempUiHome {
    fs::path root;
    fs::path web;
    std::optional<ScopedEnv> appdata;
    std::optional<ScopedEnv> xdg;

    TempUiHome() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("eta_ui_tests_" + std::to_string(stamp));
        web = root / "web";
        fs::create_directories(web / "assets");
        std::ofstream(web / "index.html") << "<!doctype html><title>eta-ui-test</title>";
        std::ofstream(web / "assets" / "app.js") << "console.log('ok');";
        // Keep session/discovery files out of the real user profile.
        appdata.emplace("APPDATA", (root / "config").string());
        xdg.emplace("XDG_CONFIG_HOME", (root / "config").string());
    }
    ~TempUiHome() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

struct ParsedUrl {
    int port{0};
    std::string token;
};

inline ParsedUrl parse_url(const std::string& url) {
    // http://127.0.0.1:<port>/?t=<token>
    ParsedUrl out;
    const auto colon = url.rfind(':');
    out.port = std::atoi(url.c_str() + colon + 1);
    const auto t = url.find("?t=");
    BOOST_REQUIRE(t != std::string::npos);
    out.token = url.substr(t + 3);
    return out;
}

/// Minimal blocking WebSocket client built on NNG's stream API.
class WsClient {
public:
    WsClient(int port, const std::string& query) {
        const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/ws" + query;
        BOOST_REQUIRE_EQUAL(nng_stream_dialer_alloc(&dialer_, url.c_str()), 0);
        BOOST_REQUIRE_EQUAL(nng_aio_alloc(&aio_, nullptr, nullptr), 0);
        nng_aio_set_timeout(aio_, 3000);
        nng_stream_dialer_dial(dialer_, aio_);
        nng_aio_wait(aio_);
        if (nng_aio_result(aio_) == 0) {
            stream_ = static_cast<nng_stream*>(nng_aio_get_output(aio_, 0));
        }
    }
    ~WsClient() {
        if (stream_) {
            nng_stream_close(stream_);
            nng_stream_free(stream_);
        }
        nng_aio_free(aio_);
        nng_stream_dialer_free(dialer_);
    }

    [[nodiscard]] bool connected() const { return stream_ != nullptr; }

    bool send_json(const std::string& json) {
        std::string msg;
        ui::put_u32_le(msg, static_cast<std::uint32_t>(json.size()));
        msg += json;
        nng_iov iov{msg.data(), msg.size()};
        nng_aio_set_iov(aio_, 1, &iov);
        nng_stream_send(stream_, aio_);
        nng_aio_wait(aio_);
        return nng_aio_result(aio_) == 0;
    }

    /// Read one complete server frame, or nullopt on error/timeout.
    std::optional<std::string> read_frame() {
        while (true) {
            if (acc_.size() >= 4) {
                const auto total = ui::get_u32_le(acc_.data());
                if (acc_.size() >= 4u + total) {
                    std::string frame = acc_.substr(0, 4u + total);
                    acc_.erase(0, 4u + total);
                    return frame;
                }
            }
            char buf[65536];
            nng_iov iov{buf, sizeof buf};
            nng_aio_set_iov(aio_, 1, &iov);
            nng_stream_recv(stream_, aio_);
            nng_aio_wait(aio_);
            if (nng_aio_result(aio_) != 0) return std::nullopt;
            acc_.append(buf, nng_aio_count(aio_));
        }
    }

private:
    nng_stream_dialer* dialer_{nullptr};
    nng_aio* aio_{nullptr};
    nng_stream* stream_{nullptr};
    std::string acc_;
};

inline std::optional<std::string> http_get(int port, const std::string& path, int* status) {
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
    nng_url* u = nullptr;
    if (nng_url_parse(&u, url.c_str()) != 0) return std::nullopt;
    nng_http_client* client = nullptr;
    nng_http_req* req = nullptr;
    nng_http_res* res = nullptr;
    nng_aio* aio = nullptr;
    std::optional<std::string> body;
    if (nng_http_client_alloc(&client, u) == 0 && nng_http_req_alloc(&req, u) == 0
        && nng_http_res_alloc(&res) == 0 && nng_aio_alloc(&aio, nullptr, nullptr) == 0) {
        nng_aio_set_timeout(aio, 3000);
        nng_http_client_transact(client, req, res, aio);
        nng_aio_wait(aio);
        if (nng_aio_result(aio) == 0) {
            *status = nng_http_res_get_status(res);
            void* data = nullptr;
            std::size_t size = 0;
            nng_http_res_get_data(res, &data, &size);
            body = std::string(static_cast<const char*>(data), size);
        }
    }
    if (aio) nng_aio_free(aio);
    if (res) nng_http_res_free(res);
    if (req) nng_http_req_free(req);
    if (client) nng_http_client_free(client);
    nng_url_free(u);
    return body;
}

/// Drain server inbox messages until one contains `needle`.
inline std::optional<std::string> recv_until(const std::string& needle, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto msg = ui::UiServer::instance().recv(100);
        if (msg && msg->find(needle) != std::string::npos) return msg;
    }
    return std::nullopt;
}


} // namespace eta_ui_test
