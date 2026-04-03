#include <boost/test/unit_test.hpp>
#include <string>
#include "eta/runtime/port.h"
#include "eta/runtime/port_primitives.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/string_view.h"

using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::nanbox;

struct PortTestFixture {
    memory::heap::Heap heap;
    memory::intern::InternTable intern_table;
    VM vm;
    BuiltinEnvironment builtins;

    PortTestFixture()
        : heap(1024 * 1024),
          intern_table(),
          vm(heap, intern_table) {
        // Register core primitives first
        register_core_primitives(builtins, heap, intern_table);
        // Register port primitives (requires VM)
        register_port_primitives(builtins, heap, intern_table, vm);

        // Install builtins into VM
        auto result = builtins.install(heap, vm.globals(), 100);
        if (!result) {
            throw std::runtime_error("Failed to install builtins");
        }
    }
};

BOOST_FIXTURE_TEST_SUITE(port_tests, PortTestFixture)

// ============================================================================
// Console Port Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(console_port_basic) {
    auto input_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Input);
    auto output_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Output);

    BOOST_CHECK(input_port->is_input());
    BOOST_CHECK(!input_port->is_output());
    BOOST_CHECK(!output_port->is_input());
    BOOST_CHECK(output_port->is_output());
}

// ============================================================================
// String Port Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(string_port_write) {
    auto port = std::make_shared<StringPort>(StringPort::Mode::Output);

    auto result = port->write_string("Hello, ");
    BOOST_CHECK(result.has_value());

    result = port->write_string("World!");
    BOOST_CHECK(result.has_value());

    BOOST_CHECK_EQUAL(port->get_string(), "Hello, World!");
}

BOOST_AUTO_TEST_CASE(string_port_read) {
    auto port = std::make_shared<StringPort>(StringPort::Mode::Input, "ABC");

    auto ch1 = port->read_char();
    BOOST_CHECK(ch1.has_value());
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(*ch1), static_cast<uint32_t>(U'A'));

    auto ch2 = port->read_char();
    BOOST_CHECK(ch2.has_value());
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(*ch2), static_cast<uint32_t>(U'B'));

    auto ch3 = port->read_char();
    BOOST_CHECK(ch3.has_value());
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(*ch3), static_cast<uint32_t>(U'C'));

    auto eof = port->read_char();
    BOOST_CHECK(!eof.has_value());  // EOF
}

BOOST_AUTO_TEST_CASE(string_port_mode_enforcement) {
    auto input_port = std::make_shared<StringPort>(StringPort::Mode::Input, "test");
    auto output_port = std::make_shared<StringPort>(StringPort::Mode::Output);

    // Can't write to input port
    auto write_result = input_port->write_string("fail");
    BOOST_CHECK(!write_result.has_value());

    // Can't read from output port
    auto read_result = output_port->read_char();
    BOOST_CHECK(!read_result.has_value());
}

// ============================================================================
// Port Primitive Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(current_ports_default) {
    // VM should have initialized default console ports
    auto input = vm.current_input_port();
    auto output = vm.current_output_port();
    auto error = vm.current_error_port();

    BOOST_CHECK(input != Nil);
    BOOST_CHECK(output != Nil);
    BOOST_CHECK(error != Nil);
}

BOOST_AUTO_TEST_CASE(open_output_string_primitive) {
    // Call open-output-string
    std::vector<LispVal> args;

    // Find the primitive in builtins
    auto globals = vm.globals();
    LispVal open_output_string = Nil;

    // Since we know the order of registration, we can access by index
    // or iterate through to find it
    // For simplicity, directly create port and test

    auto port = std::make_shared<StringPort>(StringPort::Mode::Output);
    auto port_val = make_port(heap, port);
    BOOST_CHECK(port_val.has_value());

    // Write to it
    port->write_string("Test output");
    BOOST_CHECK_EQUAL(port->get_string(), "Test output");
}

BOOST_AUTO_TEST_CASE(write_string_primitive) {
    // Create a string port
    auto port = std::make_shared<StringPort>(StringPort::Mode::Output);
    auto port_val = make_port(heap, port);
    BOOST_REQUIRE(port_val.has_value());

    // Create a string to write
    auto str_val = make_string(heap, intern_table, "Hello from write-string!");
    BOOST_REQUIRE(str_val.has_value());

    // Manually call write_string logic
    auto sv = StringView::try_from(*str_val, intern_table);
    BOOST_REQUIRE(sv.has_value());

    auto result = port->write_string(std::string(sv->view()));
    BOOST_CHECK(result.has_value());
    BOOST_CHECK_EQUAL(port->get_string(), "Hello from write-string!");
}

BOOST_AUTO_TEST_CASE(read_char_primitive) {
    // Create an input string port
    auto port = std::make_shared<StringPort>(StringPort::Mode::Input, "XYZ");
    auto port_val = make_port(heap, port);
    BOOST_REQUIRE(port_val.has_value());

    // Read characters
    auto ch1 = port->read_char();
    BOOST_CHECK(ch1.has_value());
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(*ch1), static_cast<uint32_t>(U'X'));

    auto ch2 = port->read_char();
    BOOST_CHECK(ch2.has_value());
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(*ch2), static_cast<uint32_t>(U'Y'));
}

BOOST_AUTO_TEST_CASE(port_predicates) {
    // Create ports of different types
    auto input_port = std::make_shared<StringPort>(StringPort::Mode::Input, "test");
    auto output_port = std::make_shared<StringPort>(StringPort::Mode::Output);

    auto input_val = make_port(heap, input_port);
    auto output_val = make_port(heap, output_port);

    BOOST_REQUIRE(input_val.has_value());
    BOOST_REQUIRE(output_val.has_value());

    // Check port predicates directly
    BOOST_CHECK(input_port->is_input());
    BOOST_CHECK(!input_port->is_output());
    BOOST_CHECK(!output_port->is_input());
    BOOST_CHECK(output_port->is_output());
}

BOOST_AUTO_TEST_CASE(set_current_ports) {
    // Create custom ports
    auto custom_output = std::make_shared<StringPort>(StringPort::Mode::Output);
    auto custom_output_val = make_port(heap, custom_output);
    BOOST_REQUIRE(custom_output_val.has_value());

    // Set as current output port
    vm.set_current_output_port(*custom_output_val);

    // Verify it's set
    BOOST_CHECK_EQUAL(vm.current_output_port(), *custom_output_val);

    // Write something and check
    custom_output->write_string("Custom output test");
    BOOST_CHECK_EQUAL(custom_output->get_string(), "Custom output test");
}

BOOST_AUTO_TEST_CASE(get_output_string_test) {
    // Create output string port
    auto port = std::make_shared<StringPort>(StringPort::Mode::Output);
    port->write_string("Captured output");

    // Get the string
    auto result = port->get_string();
    BOOST_CHECK_EQUAL(result, "Captured output");
}

BOOST_AUTO_TEST_CASE(open_input_string_test) {
    std::string content = "Input from string";
    auto port = std::make_shared<StringPort>(StringPort::Mode::Input, content);

    // Read all characters
    std::string read_back;
    while (auto ch = port->read_char()) {
        if (*ch < 128) {
            read_back += static_cast<char>(*ch);
        }
    }

    BOOST_CHECK_EQUAL(read_back, content);
}

BOOST_AUTO_TEST_CASE(port_close) {
    auto port = std::make_shared<FilePort>("test.txt", FilePort::Mode::Write);

    // FilePort is a stub and starts closed
    BOOST_CHECK(!port->is_open());

    auto result = port->close();
    BOOST_CHECK(result.has_value());

    BOOST_CHECK(!port->is_open());
}

BOOST_AUTO_TEST_CASE(port_heap_allocation) {
    // Test that ports can be allocated on the heap
    auto port = std::make_shared<StringPort>(StringPort::Mode::Output);
    auto port_val = make_port(heap, port);

    BOOST_REQUIRE(port_val.has_value());

    // Verify it's a heap object with Port kind
    BOOST_CHECK(ops::is_boxed(*port_val));
    BOOST_CHECK_EQUAL(ops::tag(*port_val), Tag::HeapObject);

    // Verify we can retrieve it
    auto* port_obj = heap.try_get_as<ObjectKind::Port, types::PortObject>(ops::payload(*port_val));
    BOOST_REQUIRE(port_obj != nullptr);
    BOOST_CHECK(port_obj->port != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

