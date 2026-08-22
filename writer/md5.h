// md5.h — RFC 1321 MD5 参考实现 (公共域形态, 无外部依赖)。
// 用途: writer PDF 缓存键 = 源文件内容 MD5 (与 /tmp/NPOfficeCache 的
// GlobalDataSet::GetFileMD5ValueW 同算法, 同一文件必同值, 两处缓存键互通,
// 经验 38 落地决策③)。仅用于内容寻址, 非安全用途。
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// 计算 data 的 MD5 (32 位小写十六进制)
std::string Md5Hex(const void* data, size_t len);

// 计算文件内容的 MD5 (整文件读入; 失败返回空串)
std::string Md5FileHex(const std::string& path);
