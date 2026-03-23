#pragma once

#include <string>

namespace YAML {

class Node {
   public:
    Node() = default;
};

inline Node LoadFile(const std::string&) { return Node{}; }

}  // namespace YAML
