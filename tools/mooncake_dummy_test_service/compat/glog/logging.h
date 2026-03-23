#pragma once

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace google {

enum Severity {
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    FATAL = 3,
    GLOG_FATAL = 3,
};

inline int FLAGS_minloglevel = 0;
inline bool FLAGS_logtostderr = true;
inline bool FLAGS_stop_logging_if_full_disk = false;
inline std::string FLAGS_log_dir;

inline void InitGoogleLogging(const char*) {}
inline void ShutdownGoogleLogging() {}

}  // namespace google

inline int& FLAGS_minloglevel = ::google::FLAGS_minloglevel;
inline bool& FLAGS_logtostderr = ::google::FLAGS_logtostderr;
inline bool& FLAGS_stop_logging_if_full_disk =
    ::google::FLAGS_stop_logging_if_full_disk;
inline std::string& FLAGS_log_dir = ::google::FLAGS_log_dir;

namespace glog_shim {

class LogMessage {
   public:
    LogMessage(google::Severity severity, const char* file, int line)
        : severity_(severity) {
        stream_ << "[" << file << ":" << line << "] ";
    }

    ~LogMessage() {
        if (severity_ >= static_cast<google::Severity>(google::FLAGS_minloglevel)) {
            if (severity_ >= google::ERROR) {
                std::cerr << stream_.str() << std::endl;
            } else {
                std::clog << stream_.str() << std::endl;
            }
        }
        if (severity_ == google::FATAL) {
            std::abort();
        }
    }

    template <typename T>
    LogMessage& operator<<(T&& value) {
        stream_ << std::forward<T>(value);
        return *this;
    }

   private:
    google::Severity severity_;
    std::ostringstream stream_;
};

}  // namespace glog_shim

#define LOG(severity) \
    ::glog_shim::LogMessage(::google::severity, __FILE__, __LINE__)
#define PLOG(severity) LOG(severity)
#define VLOG(verbose_level) LOG(INFO)
#define DLOG(severity) LOG(severity)

#define CHECK(condition)                                                     \
    if (!(condition))                                                        \
    LOG(FATAL) << "Check failed: " #condition " "
#define CHECK_EQ(lhs, rhs) CHECK((lhs) == (rhs))
#define CHECK_NE(lhs, rhs) CHECK((lhs) != (rhs))
#define CHECK_LT(lhs, rhs) CHECK((lhs) < (rhs))
#define CHECK_LE(lhs, rhs) CHECK((lhs) <= (rhs))
#define CHECK_GT(lhs, rhs) CHECK((lhs) > (rhs))
#define CHECK_GE(lhs, rhs) CHECK((lhs) >= (rhs))
