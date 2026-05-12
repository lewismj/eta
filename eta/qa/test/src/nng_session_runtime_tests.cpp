/**
 * @file nng_session_runtime_tests.cpp
 * @brief Unit tests for eta::nng::SessionActorRuntime.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"
#include "eta/nng/session_actor_runtime.h"

namespace {

struct ScopedNngSocket {
    nng_socket socket{};
    bool open{false};

    ~ScopedNngSocket() {
        if (open) {
            nng_close(socket);
        }
    }
};

[[nodiscard]] std::string unique_inproc_endpoint() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return "inproc://eta_nng_session_runtime_test_" + std::to_string(stamp);
}

} // namespace

BOOST_AUTO_TEST_SUITE(nng_session_runtime_tests)

BOOST_AUTO_TEST_CASE(actor_lifecycle_listener_maps_events_and_can_be_cleared) {
    eta::nng::SessionActorRuntime runtime;
    std::vector<eta::native::ActorRuntime::ActorEvent> events;

    runtime.on_actor_lifecycle([&events](const eta::native::ActorRuntime::ActorEvent& event) {
        events.push_back(event);
        if (events.size() == 1u) {
            throw std::runtime_error("listener failure should be swallowed");
        }
    });

    auto* pm = runtime.process_manager();
    BOOST_REQUIRE(pm != nullptr);
    pm->notify_thread_started(nullptr, nullptr, "worker-alpha");
    pm->notify_thread_exited();

    BOOST_REQUIRE(events.size() == 2u);
    BOOST_TEST(
        static_cast<int>(events[0].kind)
        == static_cast<int>(eta::native::ActorRuntime::ActorEvent::Kind::Started));
    BOOST_TEST(events[0].name == "worker-alpha");
    BOOST_TEST(
        static_cast<int>(events[1].kind)
        == static_cast<int>(eta::native::ActorRuntime::ActorEvent::Kind::Exited));

    runtime.on_actor_lifecycle({});
    pm->notify_thread_started(nullptr, nullptr, "worker-beta");
    BOOST_TEST(events.size() == 2u);
}

BOOST_AUTO_TEST_CASE(install_mailbox_dials_endpoint_and_sets_mailbox_value) {
    eta::nng::SessionActorRuntime runtime;
    eta::runtime::memory::heap::Heap heap(4u * 1024u * 1024u);

    ScopedNngSocket listener;
    int rv = nng_pair0_open(&listener.socket);
    BOOST_REQUIRE_EQUAL(rv, 0);
    listener.open = true;

    const std::string endpoint = unique_inproc_endpoint();
    rv = nng_listen(listener.socket, endpoint.c_str(), nullptr, 0);
    BOOST_REQUIRE_EQUAL(rv, 0);

    const bool ok = runtime.install_mailbox(heap, endpoint);
    BOOST_TEST(ok);
    BOOST_TEST(runtime.mailbox() != eta::runtime::nanbox::Nil);
}

BOOST_AUTO_TEST_CASE(install_mailbox_rejects_invalid_endpoint) {
    eta::nng::SessionActorRuntime runtime;
    eta::runtime::memory::heap::Heap heap(4u * 1024u * 1024u);

    const bool ok = runtime.install_mailbox(heap, "invalid://eta-runtime-endpoint");
    BOOST_TEST(!ok);
    BOOST_TEST(runtime.mailbox() == eta::runtime::nanbox::Nil);
}

BOOST_AUTO_TEST_SUITE_END()
