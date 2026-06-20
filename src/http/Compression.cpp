// oreshnek/src/http/Compression.cpp
#include "oreshnek/http/Compression.h"

#include <zlib.h>
#ifdef ORESHNEK_HAVE_BROTLI
#include <brotli/encode.h>
#endif

#include <cstdint>

namespace Oreshnek {
namespace Http {

std::string gzip_compress(std::string_view input, int level) {
    z_stream zs{};
    // windowBits 15 + 16 selects the gzip wrapper (instead of raw zlib).
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::string out;
    char buf[32768];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK);
    deflateEnd(&zs);

    if (ret != Z_STREAM_END) return {};
    return out;
}

bool brotli_available() {
#ifdef ORESHNEK_HAVE_BROTLI
    return true;
#else
    return false;
#endif
}

std::string brotli_compress(std::string_view input, int quality) {
#ifdef ORESHNEK_HAVE_BROTLI
    size_t out_size = BrotliEncoderMaxCompressedSize(input.size());
    if (out_size == 0) out_size = input.size() + 1024;
    std::string out(out_size, '\0');
    size_t encoded_size = out_size;
    BROTLI_BOOL ok = BrotliEncoderCompress(
        quality, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_TEXT,
        input.size(), reinterpret_cast<const uint8_t*>(input.data()),
        &encoded_size, reinterpret_cast<uint8_t*>(out.data()));
    if (!ok) return {};
    out.resize(encoded_size);
    return out;
#else
    (void)input;
    (void)quality;
    return {};
#endif
}

}  // namespace Http
}  // namespace Oreshnek
