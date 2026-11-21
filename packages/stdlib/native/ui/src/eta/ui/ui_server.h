#pragma once

/**
 * @file ui_server.h
 * @brief Localhost HTTP + WebSocket server for the Eta visual environment.
 *
 * The Eta process serves its own front end:
 *   - HTTP  GET /...        static files of the front-end bundle (web root)
 *   - WS    /ws?t=<token>   the Eta UI protocol
 *
 * Built on the NNG already linked into the runtime: an `nng_stream_listener`
 * on a `ws://` URL gives plain WebSocket streams, and the HTTP server that
 * NNG creates for that listener is reused (same host:port) for the static
 * file handler.
 *
 * NNG's stream mode sends every message as a binary frame and does not keep
 * frame boundaries on receive, so the protocol carries its own framing:
 *
 *   server -> client:  [u32 total][u32 json_len][json (space padded)][buffers]
 *   client -> server:  [u32 json_len][json]
 *
 * All integers are little-endian. `total` counts every byte after itself.
 * The JSON header is padded so that buffers start on an 8-byte boundary
 * (relative to the start of the frame), letting the browser view them as
 * Float64Array without copying.
 *
 * The server is a process-wide singleton that is deliberately never
 * destroyed: NNG callbacks may still be running during static destruction.
 */

#include <nng/nng.h>
#include <nng/supplemental/http/http.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace eta::ui {

namespace fs = std::filesystem;

using Frame = std::shared_ptr<const std::string>;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline void put_u32_le(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

inline std::uint32_t get_u32_le(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(u[0])
         | (static_cast<std::uint32_t>(u[1]) << 8)
         | (static_cast<std::uint32_t>(u[2]) << 16)
         | (static_cast<std::uint32_t>(u[3]) << 24);
}

/// Build a server->client frame from a JSON header and concatenated buffers.
/// `buffers` must already be laid out with 8-byte aligned offsets.
inline Frame make_frame(std::string json, const std::string& buffers) {
    // Header bytes before buffers: 4 (total) + 4 (json_len) + json.
    while ((8 + json.size()) % 8 != 0) json.push_back(' ');
    std::string out;
    out.reserve(8 + json.size() + buffers.size());
    put_u32_le(out, static_cast<std::uint32_t>(4 + json.size() + buffers.size()));
    put_u32_le(out, static_cast<std::uint32_t>(json.size()));
    out.append(json);
    out.append(buffers);
    return std::make_shared<const std::string>(std::move(out));
}

inline std::string json_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

inline std::string random_hex(std::size_t bytes) {
    std::random_device rd;
    std::string out;
    out.reserve(bytes * 2);
    static constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto b = static_cast<unsigned>(rd() & 0xFFu);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

/// Per-user configuration directory for the UI: %APPDATA%\eta\ui or
/// $XDG_CONFIG_HOME/eta/ui (falling back to ~/.config/eta/ui).
inline fs::path ui_config_dir() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        return fs::path(appdata) / "eta" / "ui";
    }
#endif
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "eta" / "ui";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".config" / "eta" / "ui";
    }
    return fs::temp_directory_path() / "eta" / "ui";
}

/// Restrict an app id to characters that are safe in a file name.
inline std::string sanitize_app_id(std::string_view id) {
    std::string out;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) out = "app";
    return out;
}

inline std::optional<std::string> read_text_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool write_text_file(const fs::path& p, std::string_view text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

inline std::string_view mime_type_for(const fs::path& p) {
    const auto ext = p.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json" || ext == ".map") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/// Open `url` with the platform's default browser. Returns false if the
/// launcher command could not be run.
inline bool open_in_browser(const std::string& url) {
    // The URL is produced by this server (127.0.0.1, port, hex token), so it
    // contains no shell metacharacters; quote it anyway.
#ifdef _WIN32
    const std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    const std::string cmd = "open '" + url + "' >/dev/null 2>&1 &";
#else
    const std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
#endif
    return std::system(cmd.c_str()) == 0;
}

/// Ask a previous run of the same app (listening on `port`, same token) to
/// quit, so this process can take over the port and the open page
/// reconnects. Returns true if the request was delivered.
inline bool request_previous_run_quit(int port, const std::string& token) {
    const std::string url =
        "ws://127.0.0.1:" + std::to_string(port) + "/ws?t=" + token;
    nng_stream_dialer* dialer = nullptr;
    if (nng_stream_dialer_alloc(&dialer, url.c_str()) != 0) return false;
    nng_aio* aio = nullptr;
    if (nng_aio_alloc(&aio, nullptr, nullptr) != 0) {
        nng_stream_dialer_free(dialer);
        return false;
    }
    nng_aio_set_timeout(aio, 1000);
    nng_stream_dialer_dial(dialer, aio);
    nng_aio_wait(aio);
    bool delivered = false;
    if (nng_aio_result(aio) == 0) {
        auto* stream = static_cast<nng_stream*>(nng_aio_get_output(aio, 0));
        const std::string body = "{\"type\":\"quit\"}";
        std::string msg;
        put_u32_le(msg, static_cast<std::uint32_t>(body.size()));
        msg += body;
        nng_iov iov{msg.data(), msg.size()};
        nng_aio_set_iov(aio, 1, &iov);
        nng_stream_send(stream, aio);
        nng_aio_wait(aio);
        delivered = nng_aio_result(aio) == 0;
        nng_stream_close(stream);
        nng_stream_free(stream);
    }
    nng_aio_free(aio);
    nng_stream_dialer_free(dialer);
    return delivered;
}

// ---------------------------------------------------------------------------
// Standalone window hooks
// ---------------------------------------------------------------------------

/**
 * Native window support is optional (webview/webview, built only into the
 * eta-ui sidecar when ETA_UI_WINDOW is on). ui_window.cpp fills these hooks
 * at load time; when they are empty, `--ui window` falls back to a browser.
 */
struct WindowHooks {
    /// Open a window on `url` (non-blocking). Returns false if unavailable.
    bool (*open)(const std::string& url, const std::string& title, int width, int height) = nullptr;
    /// Close the window if one is open (safe from any thread).
    void (*close)() = nullptr;
};

inline WindowHooks& window_hooks() {
    static WindowHooks hooks;
    return hooks;
}

// ---------------------------------------------------------------------------
// UiServer
// ---------------------------------------------------------------------------

class UiServer {
public:
    static UiServer& instance() {
        static UiServer* server = new UiServer();  // intentionally leaked
        return *server;
    }

    UiServer(const UiServer&) = delete;
    UiServer& operator=(const UiServer&) = delete;

    /**
     * @brief Start the server for `app_id` (idempotent while running).
     *
     * The port and token are remembered per app id in the UI config
     * directory, so a page left open from a previous run of the same app
     * reconnects to the new process. If the remembered port is busy, a
     * random one is used instead.
     *
     * @return the page URL (including the token).
     */
    std::expected<std::string, std::string> start(std::string_view app_id,
                                                  const fs::path& web_root) {
        std::lock_guard lock(mu_);
        if (listener_ != nullptr) return url_;

        web_root_ = web_root;
        app_id_ = sanitize_app_id(app_id);

        const auto session_path = ui_config_dir() / (app_id_ + ".session");
        int preferred_port = 0;
        if (auto text = read_text_file(session_path)) {
            // Format: "<port> <token>"
            std::istringstream ss(*text);
            std::string token;
            ss >> preferred_port >> token;
            if (token.size() == 32) token_ = token;
        }
        if (token_.empty()) token_ = random_hex(16);

        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(20000, 59999);

        int rv = NNG_EADDRINUSE;
        const auto try_listen = [this](int port) -> int {
            const std::string ws_url = "ws://127.0.0.1:" + std::to_string(port) + "/ws";
            nng_stream_listener* l = nullptr;
            int r = nng_stream_listener_alloc(&l, ws_url.c_str());
            if (r != 0) return r;
            // Client messages are small; allow up to 16 MiB just in case.
            nng_stream_listener_set_size(l, "ws:rxframe-max", static_cast<std::size_t>(16) << 20);
            r = nng_stream_listener_listen(l);
            if (r == 0) {
                listener_ = l;
            } else {
                nng_stream_listener_free(l);
            }
            return r;
        };

        int port = 0;
        if (preferred_port > 0) {
            port = preferred_port;
            rv = try_listen(port);
            if (rv == NNG_EADDRINUSE && request_previous_run_quit(port, token_)) {
                // Give the previous run time to shut down and release the port.
                for (int i = 0; i < 30 && rv == NNG_EADDRINUSE; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    rv = try_listen(port);
                }
            }
        }
        for (int attempt = 0; listener_ == nullptr && attempt < 40; ++attempt) {
            port = dist(rng);
            rv = try_listen(port);
            if (rv != 0 && rv != NNG_EADDRINUSE && rv != NNG_EPERM) break;
        }
        if (listener_ == nullptr) {
            return std::unexpected(std::string("ui: cannot listen: ") + nng_strerror(rv));
        }
        port_ = port;

        // The ws listener created and started an HTTP server for this
        // host:port; holding the same URL returns that server.
        const std::string http_url = "http://127.0.0.1:" + std::to_string(port_);
        nng_url* parsed = nullptr;
        if ((rv = nng_url_parse(&parsed, http_url.c_str())) != 0) {
            return fail_start(std::string("ui: bad url: ") + nng_strerror(rv));
        }
        rv = nng_http_server_hold(&http_, parsed);
        nng_url_free(parsed);
        if (rv != 0) return fail_start(std::string("ui: http server: ") + nng_strerror(rv));

        nng_http_handler* files = nullptr;
        if ((rv = nng_http_handler_alloc(&files, "/", &UiServer::on_http_file)) != 0) {
            return fail_start(std::string("ui: http handler: ") + nng_strerror(rv));
        }
        nng_http_handler_set_tree(files);
        nng_http_handler_set_data(files, this, nullptr);
        if ((rv = nng_http_server_add_handler(http_, files)) != 0) {
            nng_http_handler_free(files);
            return fail_start(std::string("ui: add handler: ") + nng_strerror(rv));
        }
        if ((rv = nng_http_server_start(http_)) != 0) {
            return fail_start(std::string("ui: http start: ") + nng_strerror(rv));
        }

        if ((rv = nng_aio_alloc(&accept_aio_, &UiServer::on_accept, this)) != 0) {
            return fail_start(std::string("ui: aio: ") + nng_strerror(rv));
        }
        nng_stream_listener_accept(listener_, accept_aio_);

        url_ = http_url + "/?t=" + token_;
        write_text_file(session_path, std::to_string(port_) + " " + token_ + "\n");
        // Discovery file for hosts such as the VS Code extension.
        write_text_file(ui_config_dir() / (app_id_ + ".url"), url_ + "\n");
        return url_;
    }

    [[nodiscard]] bool running() const {
        std::lock_guard lock(mu_);
        return listener_ != nullptr;
    }

    [[nodiscard]] std::string url() const {
        std::lock_guard lock(mu_);
        return url_;
    }

    [[nodiscard]] std::size_t client_count() const {
        std::lock_guard lock(mu_);
        return clients_.size();
    }

    /// Queue `frame` for every connected client. When `retain` is set the
    /// frame also becomes the snapshot replayed to clients that connect later.
    std::size_t broadcast(const Frame& frame, bool retain) {
        std::vector<std::shared_ptr<Client>> targets;
        {
            std::lock_guard lock(mu_);
            if (retain) retained_ = frame;
            targets.reserve(clients_.size());
            for (auto& [id, c] : clients_) targets.push_back(c);
        }
        for (auto& c : targets) c->enqueue(frame);
        return targets.size();
    }

    /// Wait up to `timeout_ms` (negative = forever) for the next client
    /// message. Returns the JSON text, or nullopt on timeout.
    std::optional<std::string> recv(std::int64_t timeout_ms) {
        std::unique_lock lock(inbox_mu_);
        const auto ready = [this] { return !inbox_.empty(); };
        if (timeout_ms < 0) {
            inbox_cv_.wait(lock, ready);
        } else if (!inbox_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            return std::nullopt;
        }
        std::string msg = std::move(inbox_.front());
        inbox_.pop_front();
        return msg;
    }

    /// Queue a message for the Eta event loop as if a page had sent it
    /// (used by the native window to report that it was closed).
    void post(std::string json) { push_inbox(std::move(json)); }

    /// Close every client and stop listening. The HTTP server is released.
    void stop() {
        std::vector<std::shared_ptr<Client>> victims;
        nng_stream_listener* listener = nullptr;
        nng_aio* accept_aio = nullptr;
        nng_http_server* http = nullptr;
        {
            std::lock_guard lock(mu_);
            listener = listener_;
            accept_aio = accept_aio_;
            http = http_;
            listener_ = nullptr;
            accept_aio_ = nullptr;
            http_ = nullptr;
            retained_.reset();
            url_.clear();
            for (auto& [id, c] : clients_) victims.push_back(c);
            clients_.clear();
        }
        for (auto& c : victims) c->close();
        if (listener) nng_stream_listener_close(listener);
        if (accept_aio) nng_aio_stop(accept_aio);
        if (accept_aio) nng_aio_free(accept_aio);
        if (listener) nng_stream_listener_free(listener);
        if (http) nng_http_server_release(http);
    }

private:
    // -----------------------------------------------------------------------
    // Client connection
    // -----------------------------------------------------------------------
    struct Client : std::enable_shared_from_this<Client> {
        UiServer* server{nullptr};
        std::uint64_t id{0};
        nng_stream* stream{nullptr};
        nng_aio* rx_aio{nullptr};
        nng_aio* tx_aio{nullptr};
        std::vector<char> rx_buf = std::vector<char>(64 * 1024);
        std::string rx_acc;

        std::mutex mu;
        std::deque<Frame> txq;
        std::size_t tx_off{0};
        bool tx_busy{false};
        bool closed{false};

        // Self references held while an asynchronous operation is pending.
        std::shared_ptr<Client> rx_hold;
        std::shared_ptr<Client> tx_hold;

        ~Client() {
            // Destruction can happen inside an NNG callback, where freeing
            // the stream or aio directly is not allowed. Hand them to a
            // short-lived thread instead.
            nng_stream* s = stream;
            nng_aio* r = rx_aio;
            nng_aio* t = tx_aio;
            std::thread([s, r, t] {
                if (r) nng_aio_free(r);
                if (t) nng_aio_free(t);
                if (s) nng_stream_free(s);
            }).detach();
        }

        void start_recv() {
            rx_hold = shared_from_this();
            nng_iov iov{rx_buf.data(), rx_buf.size()};
            nng_aio_set_iov(rx_aio, 1, &iov);
            nng_stream_recv(stream, rx_aio);
        }

        void enqueue(const Frame& frame) {
            std::lock_guard lock(mu);
            if (closed) return;
            txq.push_back(frame);
            if (!tx_busy) send_next_locked();
        }

        void send_next_locked() {
            if (closed || txq.empty()) {
                tx_busy = false;
                return;
            }
            tx_busy = true;
            tx_hold = shared_from_this();
            const Frame& f = txq.front();
            nng_iov iov{const_cast<char*>(f->data()) + tx_off, f->size() - tx_off};
            nng_aio_set_iov(tx_aio, 1, &iov);
            nng_stream_send(stream, tx_aio);
        }

        void close() {
            {
                std::lock_guard lock(mu);
                if (closed) return;
                closed = true;
                txq.clear();
            }
            nng_stream_close(stream);
        }

        static void on_recv(void* arg) {
            auto* self = static_cast<Client*>(arg);
            auto hold = std::move(self->rx_hold);
            const int rv = nng_aio_result(self->rx_aio);
            if (rv != 0) {
                self->server->drop_client(self->id);
                return;
            }
            const std::size_t n = nng_aio_count(self->rx_aio);
            self->rx_acc.append(self->rx_buf.data(), n);
            // Parse [u32 len][json] messages.
            std::size_t pos = 0;
            while (self->rx_acc.size() - pos >= 4) {
                const std::uint32_t len = get_u32_le(self->rx_acc.data() + pos);
                if (len > (16u << 20)) {  // protocol violation
                    self->server->drop_client(self->id);
                    return;
                }
                if (self->rx_acc.size() - pos - 4 < len) break;
                self->server->push_inbox(self->rx_acc.substr(pos + 4, len));
                pos += 4 + len;
            }
            self->rx_acc.erase(0, pos);
            {
                std::lock_guard lock(self->mu);
                if (self->closed) return;
            }
            self->start_recv();
        }

        static void on_send(void* arg) {
            auto* self = static_cast<Client*>(arg);
            auto hold = std::move(self->tx_hold);
            const int rv = nng_aio_result(self->tx_aio);
            if (rv != 0) {
                self->server->drop_client(self->id);
                return;
            }
            std::lock_guard lock(self->mu);
            if (self->closed || self->txq.empty()) {
                self->tx_busy = false;
                return;
            }
            self->tx_off += nng_aio_count(self->tx_aio);
            if (self->tx_off >= self->txq.front()->size()) {
                self->txq.pop_front();
                self->tx_off = 0;
            }
            self->send_next_locked();
        }
    };

    UiServer() = default;

    std::unexpected<std::string> fail_start(std::string message) {
        // Called with mu_ held.
        if (http_) {
            nng_http_server_release(http_);
            http_ = nullptr;
        }
        if (listener_) {
            nng_stream_listener_close(listener_);
            nng_stream_listener_free(listener_);
            listener_ = nullptr;
        }
        return std::unexpected(std::move(message));
    }

    void push_inbox(std::string msg) {
        {
            std::lock_guard lock(inbox_mu_);
            inbox_.push_back(std::move(msg));
        }
        inbox_cv_.notify_one();
    }

    void drop_client(std::uint64_t id) {
        std::shared_ptr<Client> victim;
        std::size_t remaining = 0;
        {
            std::lock_guard lock(mu_);
            auto it = clients_.find(id);
            if (it == clients_.end()) return;
            victim = it->second;
            clients_.erase(it);
            remaining = clients_.size();
        }
        victim->close();
        push_inbox("{\"type\":\"disconnected\",\"client\":" + std::to_string(id)
                   + ",\"clients\":" + std::to_string(remaining) + "}");
    }

    static void on_accept(void* arg) {
        auto* self = static_cast<UiServer*>(arg);
        nng_aio* aio = nullptr;
        nng_stream_listener* listener = nullptr;
        {
            std::lock_guard lock(self->mu_);
            aio = self->accept_aio_;
            listener = self->listener_;
        }
        if (aio == nullptr || listener == nullptr) return;  // stopping

        const int rv = nng_aio_result(aio);
        if (rv == NNG_ECLOSED || rv == NNG_ECANCELED) return;
        if (rv == 0) {
            auto* stream = static_cast<nng_stream*>(nng_aio_get_output(aio, 0));
            self->admit(stream);
        }
        nng_stream_listener_accept(listener, aio);
    }

    void admit(nng_stream* stream) {
        // Check the session token in the request URI.
        bool authorised = false;
        char* uri = nullptr;
        if (nng_stream_get_string(stream, "ws:request-uri", &uri) == 0 && uri != nullptr) {
            const std::string_view u(uri);
            authorised = u.find("t=" + token_) != std::string_view::npos;
            nng_strfree(uri);
        }
        if (!authorised) {
            nng_stream_close(stream);
            std::thread([stream] { nng_stream_free(stream); }).detach();
            return;
        }

        auto client = std::make_shared<Client>();
        client->server = this;
        client->stream = stream;
        if (nng_aio_alloc(&client->rx_aio, &Client::on_recv, client.get()) != 0
            || nng_aio_alloc(&client->tx_aio, &Client::on_send, client.get()) != 0) {
            nng_stream_close(stream);
            return;  // ~Client frees what was allocated
        }

        Frame retained;
        std::size_t count = 0;
        {
            std::lock_guard lock(mu_);
            client->id = ++next_client_id_;
            clients_.emplace(client->id, client);
            retained = retained_;
            count = clients_.size();
        }
        if (retained) client->enqueue(retained);
        client->start_recv();
        push_inbox("{\"type\":\"connected\",\"client\":" + std::to_string(client->id)
                   + ",\"clients\":" + std::to_string(count) + "}");
    }

    static void on_http_file(nng_aio* aio) {
        auto* req = static_cast<nng_http_req*>(nng_aio_get_input(aio, 0));
        auto* handler = static_cast<nng_http_handler*>(nng_aio_get_input(aio, 1));
        auto* self = static_cast<UiServer*>(nng_http_handler_get_data(handler));

        std::string path(nng_http_req_get_uri(req));
        if (const auto q = path.find_first_of("?#"); q != std::string::npos) path.erase(q);

        nng_http_res* res = nullptr;
        const auto body = self->load_file(path);
        if (!body) {
            if (nng_http_res_alloc_error(&res, NNG_HTTP_STATUS_NOT_FOUND) != 0) {
                nng_aio_finish(aio, NNG_ENOMEM);
                return;
            }
            nng_aio_set_output(aio, 0, res);
            nng_aio_finish(aio, 0);
            return;
        }
        if (nng_http_res_alloc(&res) != 0) {
            nng_aio_finish(aio, NNG_ENOMEM);
            return;
        }
        nng_http_res_set_header(res, "Content-Type", std::string(body->second).c_str());
        nng_http_res_set_header(res, "Cache-Control", "no-cache");
        nng_http_res_copy_data(res, body->first.data(), body->first.size());
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
    }

    /// Resolve a request path against the web root. Unknown paths without an
    /// extension fall back to index.html.
    std::optional<std::pair<std::string, std::string_view>> load_file(std::string path) {
        fs::path root;
        {
            std::lock_guard lock(mu_);
            root = web_root_;
        }
        if (root.empty()) return std::nullopt;
        if (path.empty() || path == "/") path = "/index.html";
        if (path.find("..") != std::string::npos) return std::nullopt;

        fs::path rel = fs::path(path.substr(1)).lexically_normal();
        fs::path full = root / rel;
        std::error_code ec;
        if (!fs::is_regular_file(full, ec)) {
            if (rel.has_extension()) return std::nullopt;
            full = root / "index.html";
        }
        auto text = read_text_file(full);
        if (!text) return std::nullopt;
        return std::make_pair(std::move(*text), mime_type_for(full));
    }

    mutable std::mutex mu_;
    nng_stream_listener* listener_{nullptr};
    nng_aio* accept_aio_{nullptr};
    nng_http_server* http_{nullptr};
    fs::path web_root_;
    std::string app_id_;
    std::string token_;
    std::string url_;
    int port_{0};
    std::uint64_t next_client_id_{0};
    std::unordered_map<std::uint64_t, std::shared_ptr<Client>> clients_;
    Frame retained_;

    std::mutex inbox_mu_;
    std::condition_variable inbox_cv_;
    std::deque<std::string> inbox_;
};

} // namespace eta::ui
