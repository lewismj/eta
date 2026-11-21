/**
 * @file ui_window.cpp
 * @brief Standalone native window for std.ui apps (`etai app.eta --ui window`).
 *
 * Uses webview/webview (header-only): WebView2 on Windows, WKWebView on
 * macOS, WebKitGTK on Linux. Compiled only into the eta-ui sidecar when the
 * ETA_UI_WINDOW CMake option is on; it registers itself through
 * eta::ui::window_hooks() at load time.
 *
 * The window runs its own message loop on a dedicated thread. Closing it
 * posts {"type":"quit"} to the Eta event loop, so `ui:run` returns and the
 * program ends; stopping the UI from Eta closes the window.
 */

#include <future>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <webview/webview.h>

#include "eta/ui/ui_server.h"

namespace eta::ui {
namespace {

struct WindowState {
    std::mutex mu;
    webview_t view{nullptr};
    bool open{false};
};

WindowState& state() {
    static auto* s = new WindowState();  // intentionally leaked (see UiServer)
    return *s;
}

bool open_window(const std::string& url, const std::string& title, int width, int height) {
    auto& s = state();
    {
        std::lock_guard lock(s.mu);
        if (s.open) return true;
        s.open = true;
    }
    std::promise<bool> started;
    auto ready = started.get_future();
    std::thread([url, title, width, height, promise = std::move(started)]() mutable {
        auto& st = state();
        webview_t view = webview_create(0, nullptr);
        if (view == nullptr) {
            {
                std::lock_guard lock(st.mu);
                st.open = false;
            }
            promise.set_value(false);
            return;
        }
        {
            std::lock_guard lock(st.mu);
            st.view = view;
        }
        webview_set_title(view, title.c_str());
        webview_set_size(view, width > 0 ? width : 1440, height > 0 ? height : 900, WEBVIEW_HINT_NONE);
        webview_navigate(view, url.c_str());
        promise.set_value(true);
        webview_run(view);
        {
            std::lock_guard lock(st.mu);
            st.view = nullptr;
            st.open = false;
        }
        webview_destroy(view);
        UiServer::instance().post("{\"type\":\"quit\",\"reason\":\"window-closed\"}");
    }).detach();
    return ready.get();
}

void close_window() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    if (s.view == nullptr) return;
    // webview calls must run on the window's own thread.
    webview_dispatch(s.view, [](webview_t view, void*) { webview_terminate(view); }, nullptr);
}

struct Registrar {
    Registrar() {
        window_hooks().open = &open_window;
        window_hooks().close = &close_window;
    }
};

const Registrar registrar;

} // namespace
} // namespace eta::ui
