#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "./tdigest-c/tdigest.h"

/// @file perf_collector.h
/// @brief Performance data collector using t-digest for percentile calculations.

constexpr const char* kMixTag = "mix";
constexpr double kDefaultCompression = 200.0;

/// @brief High-resolution timer for measuring elapsed time.
class PerfTimer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    /// @brief Constructs a new PerfTimer and starts timing.
    PerfTimer() : start_(Clock::now()) {}

    PerfTimer(const PerfTimer&) = delete;
    PerfTimer& operator=(const PerfTimer&) = delete;
    PerfTimer(PerfTimer&&) = delete;
    PerfTimer& operator=(PerfTimer&&) = delete;

    /// @brief Returns elapsed time in milliseconds.
    /// @return Elapsed time in milliseconds.
    double ElapsedMs() const {
        auto now = Clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_);
        return static_cast<double>(duration.count()) / 1'000'000.0;
    }

private:
    TimePoint start_;
};

/// @brief Collects performance metrics with t-digest for accurate percentile estimation.
class PerfCollector {
public:
    /// @brief Performance report structure.
    struct Report {
        std::string tag;
        double avg_ms = 0.0;
        double min_ms = 0.0;
        double max_ms = 0.0;
        double p90_ms = 0.0;
        double p95_ms = 0.0;
        double p99_ms = 0.0;
        std::uint64_t total_requests = 0;
        double total_time_span_ms = 0.0;
        double total_duration_ms = 0.0;
        double qps = 0.0;

        /// @brief Checks if the report contains valid data.
        /// @return true if total_requests > 0, false otherwise.
        bool Valid() const { return total_requests > 0; }
    };

    /// @brief Constructs a new PerfCollector.
    /// @param compression Compression factor for t-digest (default: kDefaultCompression).
    explicit PerfCollector(double compression = kDefaultCompression) : compression_(compression) {
        CreateCollector(kMixTag);
    }

    ~PerfCollector() {
        for (auto& [tag, state] : collectors_) {
            if (state.digest) {
                td_free(state.digest);
            }
        }
    }

    PerfCollector(const PerfCollector&) = delete;
    PerfCollector& operator=(const PerfCollector&) = delete;

    /// @brief Records a performance metric.
    /// @param tag Metric tag name.
    /// @param dur_ms Duration in milliseconds.
    void Record(const std::string& tag, double dur_ms) {
        std::lock_guard<std::mutex> lock(mtx_);

        if (collectors_.find(tag) == collectors_.end()) {
            CreateCollector(tag);
        }

        AddToCollector(tag, dur_ms);
        AddToCollector(kMixTag, dur_ms);
    }

    /// @brief Retrieves all performance reports.
    /// @return Vector of Report structures.
    std::vector<Report> GetReports() {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<Report> reports;
        reports.reserve(collectors_.size());

        for (const auto& [tag, state] : collectors_) {
            if (state.total_count == 0) continue;

            Report rep;
            rep.tag = tag;
            rep.total_requests = state.total_count;
            rep.total_time_span_ms = state.time_span_ms;
            rep.total_duration_ms = state.total_duration_ms;
            rep.avg_ms = rep.total_duration_ms / static_cast<double>(rep.total_requests);
            rep.min_ms = state.min_ms;
            rep.max_ms = state.max_ms;

            if (state.total_count == 1) {
                rep.p90_ms = rep.p95_ms = rep.p99_ms = state.total_duration_ms;
            } else {
                rep.p90_ms = td_quantile(state.digest, 0.90);
                rep.p95_ms = td_quantile(state.digest, 0.95);
                rep.p99_ms = td_quantile(state.digest, 0.99);
            }

            if (rep.total_time_span_ms > 0) {
                rep.qps = static_cast<double>(rep.total_requests) / (rep.total_time_span_ms / 1000.0);
            }

            reports.push_back(rep);
        }

        return reports;
    }

    /// @brief Resets all collected metrics.
    void Reset() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [tag, state] : collectors_) {
            td_reset(state.digest);
            state.time_span_ms = 0.0;
            state.total_duration_ms = 0.0;
            state.total_count = 0;
            state.min_ms = std::numeric_limits<double>::max();
            state.max_ms = 0.0;
            state.first_record_time = PerfTimer::TimePoint{};
            state.last_record_time = PerfTimer::TimePoint{};
        }
    }

private:
    /// @brief Internal state for each collector.
    struct CollectorState {
        td_histogram_t* digest = nullptr;
        double time_span_ms = 0.0;
        double total_duration_ms = 0.0;
        std::uint64_t total_count = 0;
        double min_ms = std::numeric_limits<double>::max();
        double max_ms = 0.0;

        PerfTimer::TimePoint first_record_time;
        PerfTimer::TimePoint last_record_time;
    };

    /// @brief Creates a new collector for a tag.
    /// @param tag Tag name.
    void CreateCollector(const std::string& tag) {
        CollectorState state;
        state.digest = td_new(compression_);
        collectors_[tag] = std::move(state);
    }

    /// @brief Adds a duration value to a collector.
    /// @param tag Tag name.
    /// @param dur_ms Duration in milliseconds.
    void AddToCollector(const std::string& tag, double dur_ms) {
        auto& state = collectors_.at(tag);
        auto now = PerfTimer::Clock::now();

        if (state.total_count == 0) {
            state.first_record_time = now;
            state.last_record_time = now;
            state.time_span_ms = dur_ms;
        } else {
            state.last_record_time = now;
            auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    state.last_record_time - state.first_record_time);
            state.time_span_ms = static_cast<double>(duration.count()) / 1000.0;
        }

        state.total_count++;
        state.total_duration_ms += dur_ms;
        td_add(state.digest, dur_ms, 1);
        if (dur_ms < state.min_ms) state.min_ms = dur_ms;
        if (dur_ms > state.max_ms) state.max_ms = dur_ms;
    }

    double compression_;
    std::unordered_map<std::string, CollectorState> collectors_;
    mutable std::mutex mtx_;
};
