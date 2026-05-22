#define BOOST_TEST_MODULE eta.unit.test
#include <boost/test/unit_test.hpp>

#include <cstdlib>

namespace {

struct TestEnvironmentFixture {
    TestEnvironmentFixture() {
#if defined(_WIN32)
        _putenv_s("ETA_MODULE_PATH", "");
#else
        unsetenv("ETA_MODULE_PATH");
#endif
    }
};

BOOST_GLOBAL_FIXTURE(TestEnvironmentFixture);

} // namespace
