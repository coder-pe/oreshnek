// oreshnek/include/oreshnek/http/Compression.h
#ifndef ORESHNEK_HTTP_COMPRESSION_H
#define ORESHNEK_HTTP_COMPRESSION_H

#include <string>
#include <string_view>

namespace Oreshnek {
namespace Http {

// Negotiated content codings.
enum class Encoding { None, Gzip, Brotli };

// gzip-wrapped DEFLATE (Content-Encoding: gzip) via zlib. Returns an empty
// string on failure. zlib is always available.
std::string gzip_compress(std::string_view input, int level = 6);

// Brotli (Content-Encoding: br). Compiled in only when the brotli library is
// present; brotli_available() reports whether it is. Returns empty if
// unavailable or on failure.
bool brotli_available();
std::string brotli_compress(std::string_view input, int quality = 5);

}  // namespace Http
}  // namespace Oreshnek

#endif  // ORESHNEK_HTTP_COMPRESSION_H
