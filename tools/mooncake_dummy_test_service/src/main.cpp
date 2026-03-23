#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "http_test_service.h"

DEFINE_string(http_host, "0.0.0.0", "HTTP listen host");
DEFINE_int32(http_port, 18080, "HTTP listen port");
DEFINE_string(real_client_address, "127.0.0.1:50052",
              "Standalone realclient RPC address");
DEFINE_string(ipc_socket_path, "",
              "IPC socket path. When empty, derive @mooncake_client_<port>.sock "
              "from real_client_address");
DEFINE_uint64(mem_pool_size, 268435456, "DummyClient mem pool size in bytes");
DEFINE_uint64(local_buffer_size, 268435456,
              "DummyClient local buffer size in bytes");
DEFINE_uint64(default_replica_num, 1, "Default replica count for direct PUT");

namespace {

std::atomic<bool> g_shutdown_requested{false};

void HandleSignal(int) { g_shutdown_requested.store(true); }

}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::string error_message;
    try {
        mooncake::tools::HttpTestService service(
            FLAGS_http_host, static_cast<std::uint16_t>(FLAGS_http_port),
            FLAGS_real_client_address, FLAGS_ipc_socket_path,
            FLAGS_mem_pool_size, FLAGS_local_buffer_size,
            static_cast<std::size_t>(FLAGS_default_replica_num));

        if (!service.Start(error_message)) {
            LOG(ERROR) << "Failed to start dummy HTTP test service: "
                       << error_message;
            google::ShutdownGoogleLogging();
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
