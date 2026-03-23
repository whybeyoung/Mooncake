#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gflags {

using FlagSetter = std::function<void(const std::string&)>;

inline std::unordered_map<std::string, FlagSetter>& Registry() {
    static std::unordered_map<std::string, FlagSetter> registry;
    return registry;
}

inline void RegisterFlag(const std::string& name, FlagSetter setter) {
    Registry()[name] = std::move(setter);
}

inline void ParseCommandLineFlags(int* argc, char*** argv, bool remove_flags) {
    if (argc == nullptr || argv == nullptr || *argv == nullptr) {
        return;
    }

    std::vector<char*> positional;
    positional.reserve(*argc);
    positional.push_back((*argv)[0]);

    for (int index = 1; index < *argc; ++index) {
        std::string argument((*argv)[index]);
        if (argument.rfind("--", 0) != 0) {
            positional.push_back((*argv)[index]);
            continue;
        }

        argument = argument.substr(2);
        auto equal_pos = argument.find('=');
        std::string name =
            equal_pos == std::string::npos ? argument : argument.substr(0, equal_pos);
        std::string value =
            equal_pos == std::string::npos ? "true" : argument.substr(equal_pos + 1);

        auto it = Registry().find(name);
        if (it == Registry().end()) {
            positional.push_back((*argv)[index]);
            continue;
        }
        it->second(value);
    }

    if (remove_flags) {
        for (std::size_t index = 0; index < positional.size(); ++index) {
            (*argv)[index] = positional[index];
        }
        *argc = static_cast<int>(positional.size());
    }
}

namespace detail {

template <typename T>
struct FlagParser;

template <>
struct FlagParser<bool> {
    static bool Parse(const std::string& value) {
        return !(value == "0" || value == "false" || value == "False");
    }
};

template <>
struct FlagParser<int32_t> {
    static int32_t Parse(const std::string& value) {
        return static_cast<int32_t>(std::stol(value));
    }
};

template <>
struct FlagParser<uint64_t> {
    static uint64_t Parse(const std::string& value) {
        return static_cast<uint64_t>(std::stoull(value));
    }
};

template <>
struct FlagParser<std::string> {
    static std::string Parse(const std::string& value) { return value; }
};

template <typename T>
struct FlagRegister {
    FlagRegister(const std::string& name, T& flag_ref) {
        RegisterFlag(name, [&flag_ref](const std::string& value) {
            flag_ref = FlagParser<T>::Parse(value);
        });
    }
};

}  // namespace detail
}  // namespace gflags

#define DEFINE_bool(name, default_value, description)                        \
    inline bool FLAGS_##name = (default_value);                              \
    [[maybe_unused]] static ::gflags::detail::FlagRegister<bool>             \
        gflags_registrar_##name(#name, FLAGS_##name)

#define DEFINE_int32(name, default_value, description)                       \
    inline int32_t FLAGS_##name = (default_value);                           \
    [[maybe_unused]] static ::gflags::detail::FlagRegister<int32_t>          \
        gflags_registrar_##name(#name, FLAGS_##name)

#define DEFINE_uint64(name, default_value, description)                      \
    inline uint64_t FLAGS_##name = (default_value);                          \
    [[maybe_unused]] static ::gflags::detail::FlagRegister<uint64_t>         \
        gflags_registrar_##name(#name, FLAGS_##name)

#define DEFINE_string(name, default_value, description)                      \
    inline std::string FLAGS_##name = (default_value);                       \
    [[maybe_unused]] static ::gflags::detail::FlagRegister<std::string>      \
        gflags_registrar_##name(#name, FLAGS_##name)

#define DECLARE_bool(name) extern bool FLAGS_##name
#define DECLARE_int32(name) extern int32_t FLAGS_##name
#define DECLARE_uint64(name) extern uint64_t FLAGS_##name
#define DECLARE_string(name) extern std::string FLAGS_##name
