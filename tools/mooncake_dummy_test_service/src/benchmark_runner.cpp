#include "benchmark_runner.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>

#include <glog/logging.h>

#include "replica.h"

namespace mooncake::tools {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowEpochMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::uint64_t NowSteadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch())
            .count());
}

std::uint64_t SplitMix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
}

void FillDeterministicBuffer(char* buffer, std::size_t size, std::uint64_t seed,
                             std::size_t worker_id, std::size_t iteration,
                             std::size_t item_index) {
    std::uint64_t state = seed ^ (static_cast<std::uint64_t>(worker_id) << 48U) ^
                          (static_cast<std::uint64_t>(iteration) << 24U) ^
                          static_cast<std::uint64_t>(item_index);
    for (std::size_t offset = 0; offset < size; ++offset) {
        if ((offset % sizeof(std::uint64_t)) == 0) {
            state = SplitMix64(state);
        }
        buffer[offset] = static_cast<char>(
            (state >> ((offset % sizeof(std::uint64_t)) * 8U)) & 0xffU);
    }
}

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double index =
        (percentile / 100.0) * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(index);
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = index - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

double Average(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    const double sum =
        std::accumulate(values.begin(), values.end(), 0.0, std::plus<>());
    return sum / static_cast<double>(values.size());
}

std::string BuildKey(const BenchmarkConfig& config, const std::string& run_id,
                     std::size_t worker_id, std::size_t iteration,
                     std::size_t item_index) {
    std::ostringstream oss;
    oss << config.key_prefix << "/" << run_id << "/worker-" << worker_id
        << "/iter-" << iteration << "/item-" << item_index;
    return oss.str();
}

}  // namespace

BenchmarkRunner::BenchmarkRunner(ClientFactory client_factory)
    : client_factory_(std::move(client_factory)) {}

BenchmarkRunner::~BenchmarkRunner() {
    stop_requested_.store(true);
    if (controller_thread_.joinable()) {
        controller_thread_.join();
    }
}

bool BenchmarkRunner::Start(const BenchmarkConfig& config,
                            std::string& error_message) {
    if (config.object_size == 0 || config.batch_size == 0 ||
        config.iterations == 0 || config.concurrency == 0 ||
        config.replica_num == 0) {
        error_message =
            "object_size, batch_size, iterations, concurrency, replica_num "
            "must all be greater than 0";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == BenchmarkState::Running ||
            state_ == BenchmarkState::Stopping) {
            error_message = "benchmark is already running";
            return false;
        }
    }

    JoinControllerThreadIfNeeded();

    BenchmarkConfig config_copy;
    std::string run_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_.store(false);
        state_ = BenchmarkState::Running;
        config_ = config;
        run_id_ = MakeRunId();
        started_at_ms_ = NowEpochMs();
        elapsed_ms_ = 0;
        total_put_ops_ = 0;
        total_get_ops_ = 0;
        bytes_written_ = 0;
        bytes_read_ = 0;
        put_failures_ = 0;
        get_failures_ = 0;
        verify_failures_ = 0;
        cleanup_failures_ = 0;
        last_error_.clear();
        batch_put_latencies_ms_.clear();
        batch_get_latencies_ms_.clear();
        config_copy = config_;
        run_id = run_id_;
    }

    controller_thread_ =
        std::thread([this, config = std::move(config_copy), run_id = std::move(run_id)]() {
            Run(config, run_id);
        });
    return true;
}

bool BenchmarkRunner::Stop(std::string& error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != BenchmarkState::Running && state_ != BenchmarkState::Stopping) {
        error_message = "benchmark is not running";
        return false;
    }

    stop_requested_.store(true);
    if (state_ == BenchmarkState::Running) {
        state_ = BenchmarkState::Stopping;
    }
    return true;
}

bool BenchmarkRunner::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == BenchmarkState::Running || state_ == BenchmarkState::Stopping;
}

BenchmarkSnapshot BenchmarkRunner::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    BenchmarkSnapshot snapshot;
    snapshot.state = state_;
    snapshot.run_id = run_id_;
    snapshot.config = config_;
    snapshot.started_at_ms = started_at_ms_;
    snapshot.elapsed_ms = elapsed_ms_;
    if (snapshot.state == BenchmarkState::Running ||
        snapshot.state == BenchmarkState::Stopping) {
        const auto now_ms = NowEpochMs();
        if (now_ms >= snapshot.started_at_ms) {
            snapshot.elapsed_ms = now_ms - snapshot.started_at_ms;
        }
    }
    snapshot.total_put_ops = total_put_ops_;
    snapshot.total_get_ops = total_get_ops_;
    snapshot.bytes_written = bytes_written_;
    snapshot.bytes_read = bytes_read_;
    snapshot.put_failures = put_failures_;
    snapshot.get_failures = get_failures_;
    snapshot.verify_failures = verify_failures_;
    snapshot.cleanup_failures = cleanup_failures_;
    snapshot.last_error = last_error_;
    snapshot.avg_batch_put_ms = Average(batch_put_latencies_ms_);
    snapshot.avg_batch_get_ms = Average(batch_get_latencies_ms_);
    snapshot.p50_batch_put_ms = Percentile(batch_put_latencies_ms_, 50.0);
    snapshot.p95_batch_put_ms = Percentile(batch_put_latencies_ms_, 95.0);
    snapshot.p50_batch_get_ms = Percentile(batch_get_latencies_ms_, 50.0);
    snapshot.p95_batch_get_ms = Percentile(batch_get_latencies_ms_, 95.0);

    const double elapsed_seconds =
        static_cast<double>(snapshot.elapsed_ms) / 1000.0;
    if (elapsed_seconds > 0.0) {
        snapshot.put_ops_per_sec =
            static_cast<double>(snapshot.total_put_ops) / elapsed_seconds;
        snapshot.get_ops_per_sec =
            static_cast<double>(snapshot.total_get_ops) / elapsed_seconds;
        snapshot.write_mib_per_sec =
            static_cast<double>(snapshot.bytes_written) / elapsed_seconds /
            1024.0 / 1024.0;
        snapshot.read_mib_per_sec =
            static_cast<double>(snapshot.bytes_read) / elapsed_seconds / 1024.0 /
            1024.0;
    }

    return snapshot;
}

void BenchmarkRunner::Run(BenchmarkConfig config, std::string run_id) {
    const auto steady_started_at_ms = NowSteadyMs();
    std::vector<std::thread> workers;
    workers.reserve(config.concurrency);

    try {
        for (std::size_t worker_id = 0; worker_id < config.concurrency;
             ++worker_id) {
            workers.emplace_back([this, config, run_id, worker_id]() {
                WorkerMain(config, run_id, worker_id);
            });
        }

        for (auto& worker : workers) {
            worker.join();
        }
    } catch (const std::exception& ex) {
        MarkFatal(ex.what());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    elapsed_ms_ = NowSteadyMs() - steady_started_at_ms;
    if (state_ != BenchmarkState::Failed) {
        state_ = BenchmarkState::Completed;
    }
}

void BenchmarkRunner::WorkerMain(const BenchmarkConfig& config,
                                 const std::string& run_id,
                                 std::size_t worker_id) {
    try {
        auto client = client_factory_();
        if (!client) {
            MarkFatal("failed to create benchmark worker client");
            return;
        }

        const std::size_t total_buffer_size = config.object_size * config.batch_size;
        const auto base_addr =
            reinterpret_cast<void*>(client->alloc_from_mem_pool(total_buffer_size));
        if (base_addr == nullptr) {
            MarkFatal("failed to allocate worker shared memory");
            return;
        }

        if (client->register_buffer(base_addr, total_buffer_size) != 0) {
            MarkFatal("failed to register worker shared memory");
            return;
        }

        const auto cleanup = [&client, base_addr]() {
            (void)client->unregister_buffer(base_addr);
        };

        std::vector<std::string> keys(config.batch_size);
        std::vector<void*> buffers(config.batch_size);
        std::vector<std::size_t> sizes(config.batch_size, config.object_size);
        auto* base = static_cast<char*>(base_addr);

        ReplicateConfig replicate_config;
        replicate_config.replica_num = config.replica_num;

        for (std::size_t iteration = 0; iteration < config.iterations;
             ++iteration) {
            if (stop_requested_.load()) {
                cleanup();
                return;
            }

            for (std::size_t item = 0; item < config.batch_size; ++item) {
                keys[item] = BuildKey(config, run_id, worker_id, iteration, item);
                char* slice = base + (item * config.object_size);
                buffers[item] = slice;
                FillDeterministicBuffer(slice, config.object_size, config.seed,
                                        worker_id, iteration, item);
            }

            const auto put_started = Clock::now();
            auto put_results =
                client->batch_put_from(keys, buffers, sizes, replicate_config);
            const auto put_elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          put_started)
                    .count();
            RecordPutBatch(put_results, sizes, put_elapsed_ms);

            const auto get_started = Clock::now();
            auto get_results = client->batch_get_into(keys, buffers, sizes);
            const auto get_elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          get_started)
                    .count();
            RecordGetBatch(get_results, sizes, get_elapsed_ms);

            std::uint64_t verify_failures = 0;
            std::vector<char> expected(config.object_size);
            for (std::size_t item = 0; item < config.batch_size; ++item) {
                if (item >= get_results.size() ||
                    get_results[item] !=
                        static_cast<int64_t>(config.object_size)) {
                    continue;
                }
                FillDeterministicBuffer(expected.data(), expected.size(),
                                        config.seed, worker_id, iteration, item);
                if (std::memcmp(buffers[item], expected.data(),
                                config.object_size) != 0) {
                    ++verify_failures;
                }
            }
            if (verify_failures != 0) {
                RecordVerifyFailure(verify_failures);
            }

            if (config.cleanup) {
                std::uint64_t cleanup_failures = 0;
                for (const auto& key : keys) {
                    if (client->remove(key) != 0) {
                        ++cleanup_failures;
                    }
                }
                if (cleanup_failures != 0) {
                    RecordCleanupFailure(cleanup_failures);
                }
            }
        }

        cleanup();
    } catch (const std::exception& ex) {
        MarkFatal(ex.what());
    }
}

void BenchmarkRunner::MarkFatal(const std::string& error_message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = BenchmarkState::Failed;
        if (last_error_.empty()) {
            last_error_ = error_message;
        }
    }
    stop_requested_.store(true);
    LOG(ERROR) << "Benchmark failed: " << error_message;
}

void BenchmarkRunner::RecordPutBatch(const std::vector<int>& results,
                                     const std::vector<std::size_t>& sizes,
                                     double latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_put_ops_ += results.size();
    batch_put_latencies_ms_.push_back(latency_ms);
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (results[index] == 0) {
            bytes_written_ += sizes[index];
        } else {
            ++put_failures_;
        }
    }
}

void BenchmarkRunner::RecordGetBatch(const std::vector<int64_t>& results,
                                     const std::vector<std::size_t>& sizes,
                                     double latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_get_ops_ += results.size();
    batch_get_latencies_ms_.push_back(latency_ms);
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (results[index] >= 0) {
            bytes_read_ += static_cast<std::uint64_t>(results[index]);
            if (static_cast<std::size_t>(results[index]) != sizes[index]) {
                ++get_failures_;
            }
        } else {
            ++get_failures_;
        }
    }
}

void BenchmarkRunner::RecordVerifyFailure(std::uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    verify_failures_ += count;
}

void BenchmarkRunner::RecordCleanupFailure(std::uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup_failures_ += count;
}

std::string BenchmarkRunner::MakeRunId() const {
    std::ostringstream oss;
    oss << NowEpochMs() << "-" << run_counter_.fetch_add(1);
    return oss.str();
}

void BenchmarkRunner::JoinControllerThreadIfNeeded() {
    bool should_join = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_join = controller_thread_.joinable() &&
                      state_ != BenchmarkState::Running &&
                      state_ != BenchmarkState::Stopping;
    }
    if (should_join) {
        controller_thread_.join();
    }
}

}  // namespace mooncake::tools
