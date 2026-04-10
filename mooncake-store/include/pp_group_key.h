#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace mooncake {

/// Parsed PP group info extracted from a storage key.
///
/// Key format:  ..._pp_size_{pp_size}_pp_rank_{pp_rank}_...
/// Example:     abc123_pp_size_2_pp_rank_0_k
struct PPGroupInfo {
    size_t rank_value_pos;   // position of the rank digit(s) in the key
    size_t rank_value_len;   // length of the rank digit(s) string
    int pp_rank;
    int pp_size;
};

/// Try to parse "_pp_size_{N}" and "_pp_rank_{R}" from a key string.
/// The two tags can appear in any order and with arbitrary content between
/// them (e.g. cp, tp, or other future dimensions).
/// Returns std::nullopt if either tag is missing or pp_size <= 1.
inline std::optional<PPGroupInfo> parsePPGroupKey(const std::string &key) {
    static const std::string kPPSizeTag = "_pp_size_";
    static const std::string kPPRankTag = "_pp_rank_";

    // Find "_pp_size_" anywhere in the key
    auto size_pos = key.find(kPPSizeTag);
    if (size_pos == std::string::npos) return std::nullopt;

    size_t i = size_pos + kPPSizeTag.size();
    if (i >= key.size() || !std::isdigit(static_cast<unsigned char>(key[i])))
        return std::nullopt;
    size_t size_digit_start = i;
    while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i])))
        ++i;
    int pp_size = std::stoi(key.substr(size_digit_start, i - size_digit_start));
    if (pp_size <= 1) return std::nullopt;

    // Find "_pp_rank_" anywhere in the key (independent of pp_size position)
    auto rank_pos = key.find(kPPRankTag);
    if (rank_pos == std::string::npos) return std::nullopt;

    size_t j = rank_pos + kPPRankTag.size();
    if (j >= key.size() || !std::isdigit(static_cast<unsigned char>(key[j])))
        return std::nullopt;
    size_t rank_digit_start = j;
    while (j < key.size() && std::isdigit(static_cast<unsigned char>(key[j])))
        ++j;
    int pp_rank = std::stoi(key.substr(rank_digit_start, j - rank_digit_start));
    if (pp_rank < 0 || pp_rank >= pp_size) return std::nullopt;

    return PPGroupInfo{rank_digit_start, j - rank_digit_start, pp_rank,
                       pp_size};
}

/// Generate a sibling key by replacing the pp_rank digits with target_rank.
inline std::string makePPSiblingKey(const std::string &key,
                                    const PPGroupInfo &info,
                                    int target_rank) {
    std::string rank_str = std::to_string(target_rank);
    std::string result = key;
    result.replace(info.rank_value_pos, info.rank_value_len, rank_str);
    return result;
}

/// Expand a batch of keys with PP group semantics.
///
/// For each input key that contains "_pp_size_{N}_pp_rank_{R}", generates
/// N sibling keys (one per pp_rank 0..N-1).  Keys without the pattern are
/// kept as-is.
///
/// @param[in]  keys          Original key list.
/// @param[out] expanded      All expanded keys (for a single batch RPC).
/// @param[out] expand_count  Per-input-key: how many expanded keys it produced.
/// @return true if any key was expanded (pp_size > 1).
inline bool expandPPGroupKeys(
    const std::vector<std::string> &keys,
    std::vector<std::string> &expanded,
    std::vector<int> &expand_count) {
    bool has_pp = false;
    expanded.reserve(keys.size());  // at least this many
    expand_count.reserve(keys.size());

    for (const auto &key : keys) {
        auto info = parsePPGroupKey(key);
        if (info) {
            has_pp = true;
            expand_count.push_back(info->pp_size);
            for (int r = 0; r < info->pp_size; ++r) {
                expanded.push_back(makePPSiblingKey(key, *info, r));
            }
        } else {
            expand_count.push_back(1);
            expanded.push_back(key);
        }
    }
    return has_pp;
}

}  // namespace mooncake
