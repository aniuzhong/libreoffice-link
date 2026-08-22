// md5.cpp — RFC 1321 MD5 参考实现 (公共域形态, 见 md5.h)。
#include "md5.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <base/link_utils.h> // link_utils::u2w (UTF-8 -> UTF-16, 中文路径)
#endif

namespace {

struct MD5Ctx {
    uint32_t a, b, c, d;
    uint64_t len;      // 已处理字节数
    uint8_t buf[64];   // 未满块缓冲
    size_t buflen;
};

constexpr uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void ProcessBlock(MD5Ctx* ctx, const uint8_t p[64]) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    uint32_t a = ctx->a, b = ctx->b, c = ctx->c, d = ctx->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);      g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);      g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d;               g = (3 * i + 5) % 16; }
        else             { f = c ^ (b | ~d);            g = (7 * i) % 16; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + K[i] + w[g], S[i]);
        a = tmp;
    }
    ctx->a += a; ctx->b += b; ctx->c += c; ctx->d += d;
}

void Md5Init(MD5Ctx* ctx) {
    ctx->a = 0x67452301; ctx->b = 0xefcdab89; ctx->c = 0x98badcfe; ctx->d = 0x10325476;
    ctx->len = 0; ctx->buflen = 0;
}

void Md5Update(MD5Ctx* ctx, const uint8_t* data, size_t len) {
    ctx->len += len;
    if (ctx->buflen) {
        size_t need = 64 - ctx->buflen;
        size_t take = len < need ? len : need;
        memcpy(ctx->buf + ctx->buflen, data, take);
        ctx->buflen += take;
        data += take;
        len -= take;
        if (ctx->buflen == 64) {
            ProcessBlock(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
    while (len >= 64) {
        ProcessBlock(ctx, data);
        data += 64;
        len -= 64;
    }
    if (len) {
        memcpy(ctx->buf, data, len);
        ctx->buflen = len;
    }
}

void Md5Final(MD5Ctx* ctx, uint8_t out[16]) {
    uint64_t bits = ctx->len * 8;
    uint8_t pad = 0x80;
    Md5Update(ctx, &pad, 1);
    uint8_t zero = 0;
    while (ctx->buflen != 56)
        Md5Update(ctx, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(bits >> (8 * i));
    Md5Update(ctx, lenbuf, 8);
    uint32_t v[4] = {ctx->a, ctx->b, ctx->c, ctx->d};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (uint8_t)(v[i] >> (8 * j));
}

} // namespace

std::string Md5Hex(const void* data, size_t len) {
    MD5Ctx ctx;
    Md5Init(&ctx);
    Md5Update(&ctx, static_cast<const uint8_t*>(data), len);
    uint8_t digest[16];
    Md5Final(&ctx, digest);
    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 32);
}

std::string Md5FileHex(const std::string& path) {
#ifdef _WIN32
    std::ifstream f(link_utils::u2w(path), std::ios::binary); // 窄路径按 ANSI 码页, UTF-8 中文会失败
#else
    std::ifstream f(path, std::ios::binary);
#endif
    if (!f)
        return std::string();
    std::vector<uint8_t> buf(1 << 20);
    MD5Ctx ctx;
    Md5Init(&ctx);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), buf.size());
        Md5Update(&ctx, buf.data(), static_cast<size_t>(f.gcount()));
    }
    uint8_t digest[16];
    Md5Final(&ctx, digest);
    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 32);
}
