#include "test_server.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>

static std::atomic<bool> g_quit{false};

static void SignalHandler(int) { g_quit = true; }

int main(int argc, char* argv[]) {
    uint16_t port = skr::testserver::kDefaultPort;
    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 0 && p < 65536) port = static_cast<uint16_t>(p);
    }

    printf("=== SKR Test Server ===\n");
    printf("Port: %u (pass a port number as argument to change)\n\n", port);

    signal(SIGINT, SignalHandler);

    skr::testserver::TestServer server(port);
    if (!server.Start()) {
        printf("Failed to start server\n");
        return 1;
    }

    printf("\nPress Ctrl+C to stop.\n\n");

    while (!g_quit) {
        Sleep(100);
    }

    printf("\nShutting down...\n");
    server.Stop();
    return 0;
}
