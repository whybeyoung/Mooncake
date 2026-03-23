#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dummy_client.h"

namespace mooncake::tools {

enum class BenchmarkState {
    Idle,
    Running,
    Stopping,
    Completed,
    Failed,
};

struct BenchmarkConfig {
    std::string key_prefix{"bench"};
    std::size_t object_size{0};
    std::size_t batch_size{0};
    std::size_t iterations{0};
    std::size_t concurrency{1};
    bool cleanup{true};
    std::size_t replica_num{1};
    std::uint64_t seed{0x4d6f6f6e63616b65ULL};
};

struct BenchmarkSnapshot {
    BenchmarkState state{BenchmarkState::Idle};
    std::string run_id;
    BenchmarkConfig config;
    std::uint64_t started_at_ms{0};
    std::uint64_t elapsed_ms{0};
    std::uint64_t total_put_ops{0};
    std::uint64_t total_get_ops{0};
    std::uint64_t bytes_written{0};
    std::uint64_t bytes_read{0};
    std::uint64_t put_failures{0};
    std::uint64_t get_failures{0};
    std::uint64_t verify_failures{0};
    std::uint64_t cleanup_failures{0};
    double put_ops_per_sec{0.0};
    double get_ops_per_sec{0.0};
    double write_mib_per_sec{0.0};
    double read_mib_per_sec{0.0};
    double avg_batch_put_ms{0.0};
    double avg_batch_get_ms{0.0};
    double p50_batch_put_ms{0.0};
    double p95_batch_put_ms{0.0};
    double p50_batch_get_ms{0.0};
    double p95_batch_get_ms{0.0};
    std::string last_error;
};

class BenchmarkRunner {
   public:
    using ClientFactory =
        std::function<std::shared_ptr<mooncake::DummyClient>()>;

    explicit BenchmarkRunner(ClientFactory client_factory);
    ~BenchmarkRunner();

    bool Start(const BenchmarkConfig& config, std::string& error_message);
    bool Stop(std::string& error_message);

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] BenchmarkSnapshot GetSnapshot() const;

   private:
    void Run(BenchmarkConfig config, std::string run_id);
    void WorkerMain(const BenchmarkConfig& config, const std::string& run_id,
                    std::size_t worker_id);
    void MarkFatal(const std::string& error_message);

    void RecordPutBatch(const std::vector<int>& results,
                        const std::vector<std::size_t>& sizes,
                        double latency_ms);
    void RecordGetBatch(const std::vector<int64_t>& results,
                        const std::vector<std::size_t>& sizes,
                        double latency_ms);
    void RecordVerifyFailure(std::uint64_t count);
    void RecordCleanupFailure(std::uint64_t count);

    [[nodiscard]] std::string MakeRunId() const;
    void JoinControllerThreadIfNeeded();

    ClientFactory client_factory_;

    mutable std::mutex mutex_;
    BenchmarkState state_{BenchmarkState::Idle};
    BenchmarkConfig config_{};
    std::string run_id_;
    std::uint64_t started_at_ms_{0};
    std::uint64_t elapsed_ms_{0};
    std::uint64_t total_put_ops_{0};
    std::uint64_t total_get_ops_{0};
    std::uint64_t bytes_written_{0};
    std::uint64_t bytes_read_{0};
    std::uint64_t put_failures_{0};
    std::uint64_t get_failures_{0};
    std::uint64_t verify_failures_{0};
    std::uint64_t cleanup_failures_{0};
    std::string last_error_;
    std::vector<double> batch_put_latencies_ms_;
    std::vector<double> batch_get_latencies_ms_;

    std::thread controller_thread_;
    std::atomic<bool> stop_requested_{false};
    mutable std::atomic<std::uint64_t> run_counter_{0};
};

}  // namespace mooncake::tools
