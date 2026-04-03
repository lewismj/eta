# Port System Usage Examples

## C++ Usage

### Creating and Using String Ports

```cpp
#include "eta/runtime/port.h"
#include "eta/runtime/factory.h"

using namespace eta::runtime;

// Create an output string port
auto output_port = std::make_shared<StringPort>(StringPort::Mode::Output);
auto port_val = make_port(heap, output_port);

// Write to the port
output_port->write_string("Hello, ");
output_port->write_string("World!");

// Get accumulated output
std::string result = output_port->get_string();
// result == "Hello, World!"

// Create an input string port
auto input_port = std::make_shared<StringPort>(StringPort::Mode::Input, "ABC");

// Read characters
auto ch1 = input_port->read_char();  // 'A'
auto ch2 = input_port->read_char();  // 'B'
auto ch3 = input_port->read_char();  // 'C'
auto eof = input_port->read_char();  // std::nullopt (EOF)
```

### Using Console Ports

```cpp
// Create console ports
auto stdin_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Input);
auto stdout_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Output);
auto stderr_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Error);

// Write to stdout
stdout_port->write_string("Hello from stdout\n");

// Write to stderr
stderr_port->write_string("Error message\n");

// Read from stdin
auto ch = stdin_port->read_char();
```

### Setting Current Ports in VM

```cpp
VM vm(heap, intern_table);

// Create a custom output port
auto string_out = std::make_shared<StringPort>(StringPort::Mode::Output);
auto port_val = make_port(heap, string_out);

// Set as current output port
vm.set_current_output_port(*port_val);

// Now all output operations will go to the string port
// (when display/newline are updated to use ports)

// Get the accumulated output
std::string captured = string_out->get_string();
```

## Scheme Usage (when primitives are registered)

### String Port Operations

```scheme
; Create an output string port
(define p (open-output-string))

; Write to it
(write-string "Hello, " p)
(write-string "World!" p)

; Get the accumulated string
(get-output-string p)  ; => "Hello, World!"

; Input from string
(define in (open-input-string "XYZ"))
(read-char in)  ; => #\X
(read-char in)  ; => #\Y
(read-char in)  ; => #\Z
(read-char in)  ; => #f (EOF)
```

### Current Port Operations

```scheme
; Get current ports
(current-input-port)   ; => #<port>
(current-output-port)  ; => #<port>
(current-error-port)   ; => #<port>

; Create a new output string port
(define my-port (open-output-string))

; Temporarily redirect output
(define old-out (current-output-port))
(set-current-output-port! my-port)

; Now output goes to our string port
; (when display is updated to use current-output-port)

; Restore original output
(set-current-output-port! old-out)

; Get what was captured
(get-output-string my-port)
```

### Port Predicates

```scheme
(define p (open-output-string))

(port? p)         ; => #t
(port? 42)        ; => #f

(output-port? p)  ; => #t
(input-port? p)   ; => #f

(define in (open-input-string "test"))
(input-port? in)  ; => #t
(output-port? in) ; => #f
```

### Writing and Reading

```scheme
; Write to specific port
(define p (open-output-string))
(write-string "custom output" p)

; Write to current output port (default)
(write-string "to stdout")

; Read from specific port
(define in (open-input-string "ABC"))
(read-char in)  ; => #\A

; Read from current input port (default)
(read-char)  ; reads from stdin
```

### Port Cleanup

```scheme
(define p (open-output-string))
(write-string "done" p)
(close-port p)
```

## Testing Example

```cpp
// From port_tests.cpp

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
```

## Integration with VM Test Fixture

```cpp
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

// Now you can test port functionality
BOOST_FIXTURE_TEST_SUITE(port_tests, PortTestFixture)
// ... tests here
BOOST_AUTO_TEST_SUITE_END()
```

## Common Patterns

### Capturing Output

```cpp
// Save current output port
auto old_output = vm.current_output_port();

// Create string port for capturing
auto capture = std::make_shared<StringPort>(StringPort::Mode::Output);
auto capture_val = make_port(heap, capture);
vm.set_current_output_port(*capture_val);

// ... code that produces output ...

// Restore original port
vm.set_current_output_port(old_output);

// Get captured output
std::string output = capture->get_string();
```

### Testing with String Input

```cpp
// Create input from string
auto input = std::make_shared<StringPort>(StringPort::Mode::Input, "test input");
auto input_val = make_port(heap, input);

// Set as current input
auto old_input = vm.current_input_port();
vm.set_current_input_port(*input_val);

// ... code that reads input ...

// Restore
vm.set_current_input_port(old_input);
```

### Error Handling

```cpp
auto port = std::make_shared<StringPort>(StringPort::Mode::Output);

auto result = port->write_string("data");
if (!result) {
    // Handle error
    auto error = result.error();
    std::cerr << "Write failed: " << error.message << "\n";
}

// Or with exceptions
if (!result) {
    throw std::runtime_error("Write failed");
}
```

