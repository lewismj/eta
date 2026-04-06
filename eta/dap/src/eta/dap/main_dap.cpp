#include "dap_server.h"
#include <iostream>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

int main() {
    // Set stdin/stdout to binary mode on Windows to prevent \r\n mangling
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    // Disable stdio sync for throughput (same pattern as the LSP server)
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    eta::dap::DapServer server;
    server.run();

    return 0;
}

