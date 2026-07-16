// oreshnek/include/oreshnek/utils/StringUtil.h
#ifndef ORESHNEK_UTILS_STRING_UTIL_H
#define ORESHNEK_UTILS_STRING_UTIL_H

#include <string_view>

namespace Oreshnek {
namespace Utils {

// std::string_view::starts_with/ends_with are C++20-only; these give the same
// behavior under C++17 too, so the framework builds with either standard (see
// ORESHNEK_CXX_STANDARD in CMakeLists.txt).
inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace Utils
}  // namespace Oreshnek

#endif  // ORESHNEK_UTILS_STRING_UTIL_H
