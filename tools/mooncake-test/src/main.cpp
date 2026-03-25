#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <thread>

#include <CLI11/CLI11.hpp>
#include <glog/logging.h>

#include "http_kv_service.hpp"

namespace {

std::atomic<bool> g_shutdown_requested{false};

void HandleSignal(int) { g_shutdown_requested.store(true); }

// ASCII art banner for mooncake_test
const char* kBanner = R"(
 _ __ ___   ___   ___  _ __   ___ __ _| | _____      | |_ ___  ___| |_
| '_ ` _ \ / _ \ / _ \| '_ \ / __/ _` | |/ / _ \_____| __/ _ \/ __| __|
| | | | | | (_) | (_) | | | | (_| (_| |   <  __/_____| ||  __/\__ \ |_
|_| |_| |_|\___/ \___/|_| |_|\___\__,_|_|\_\___|      \__\___||___/\__|

)";

std::string GetGitAuthor() {
    // Git author information is embedded at build time via CMake
    return MOONCAKE_TEST_AUTHOR;
}

}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    // Print banner with author information
    std::cout << kBanner << std::endl;
    std::cout << "Author: " << GetGitAuthor() << std::endl << std::endl;

    std::string http_host = "0.0.0.0";
    std::uint16_t http_port = 18080;
    std::string real_client_address = "127.0.0.1:50052";
    std::string ipc_socket_path;
    std::uint64_t mem_pool_size = 268435456;
    std::uint64_t local_buffer_size = 268435456;
    double min_put_size_mb = 1.0;
    double max_put_size_mb = 100.0;

    CLI::App app{"Mooncake test — HTTP KV API"};
    app.add_option("--http-host", http_host, "HTTP listen host");
    app.add_option("--http-port", http_port,
                   "HTTP listen port for this tool's KV API")
        ->check(CLI::Range(1, 65535));
    app.add_option("--real-client-address", real_client_address,
                   "Real client RPC address (host:port)");
    app.add_option(
        "--ipc-socket-path", ipc_socket_path,
        "IPC socket path. When empty, use @mooncake_client_<port>.sock "
        "where <port> is parsed from --real-client-address");
    app.add_option("--mem-pool-size", mem_pool_size,
                   "Memory pool size in bytes")
        ->check(CLI::PositiveNumber);
    app.add_option("--local-buffer-size", local_buffer_size,
                   "Local buffer size in bytes")
        ->check(CLI::PositiveNumber);
    app.add_option("--min-put-size-mb", min_put_size_mb,
                   "Minimum value size (MB) allowed for put / batch_put")
        ->check(CLI::PositiveNumber);
    app.add_option("--max-put-size-mb", max_put_size_mb,
                   "Maximum value size (MB) allowed for put / batch_put")
        ->check(CLI::PositiveNumber);

    CLI11_PARSE(app, argc, argv);

    if (min_put_size_mb > max_put_size_mb) {
        LOG(ERROR) << "--min-put-size-mb must be less than or equal to "
                      "--max-put-size-mb";
        google::ShutdownGoogleLogging();
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::string error_message;
    try {
        mooncake::tools::HttpKVService service(
            http_host, http_port, real_client_address,
            ipc_socket_path, static_cast<std::size_t>(mem_pool_size),
            static_cast<std::size_t>(local_buffer_size), min_put_size_mb,
            max_put_size_mb);

        if (!service.Start(error_message)) {
            LOG(ERROR) << "Failed to start HTTP KV service: " << error_message;
            return 1;
        }

        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        service.Stop();
    } catch (const std::exception& ex) {
        LOG(ERROR) << "Fatal startup error: " << ex.what();
        google::ShutdownGoogleLogging();
        return 1;
    }

    google::ShutdownGoogleLogging();
    return 0;
}
