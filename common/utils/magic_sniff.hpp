#pragma once

/**
 * magic_sniff —— 极简 magic-number 嗅探
 * ---
 * v1 仅识别"非用即拒"的几类格式：
 *   - 危险：PE (MZ)、ELF、Mach-O 32/64 + 大小端 → 一律拒绝
 *   - 图像：JPEG / PNG / GIF / WebP (RIFF...WEBP)
 *
 * 设计准则：
 *   - 头部 8~12 字节即可判定，调用方传 Range GET 前 512B 进来
 *   - 检测不出 → 返回空 mime 串；matches_claimed() 视为 OK（保守）
 *   - 检测到危险类（可执行）→ 不论客户端声明什么 mime，一律返回 false
 *
 * 不做的事：
 *   - 不调 libmagic（避免引入运行期 .so 依赖）
 *   - 不做完整 mime 数据库（那是 cleanup worker 异步嗅探的范围，
 *     真出问题就 quarantined）
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace chatnow::magic_sniff {

inline std::string sniff(std::string_view buf) {
    const auto* p = reinterpret_cast<const unsigned char*>(buf.data());
    auto n = buf.size();
    if (n < 4) return {};

    // JPEG: FF D8 FF
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image/jpeg";
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (n >= 8 && p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47
               && p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A) {
        return "image/png";
    }
    // GIF87a / GIF89a
    if (n >= 6 && std::memcmp(p, "GIF87a", 6) == 0) return "image/gif";
    if (n >= 6 && std::memcmp(p, "GIF89a", 6) == 0) return "image/gif";
    // WebP: "RIFF" + 4B size + "WEBP"
    if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }
    // PE/Win EXE: MZ
    if (n >= 2 && p[0] == 'M' && p[1] == 'Z') return "application/x-dosexec";
    // ELF
    if (n >= 4 && p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') return "application/x-elf";
    // Mach-O 32/64 + 大小端
    if (n >= 4) {
        uint32_t m = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                   | (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
        if (m == 0xFEEDFACE || m == 0xFEEDFACF || m == 0xCEFAEDFE || m == 0xCFFAEDFE) {
            return "application/x-mach-binary";
        }
    }
    return {};
}

/**
 * @brief 检测出的 mime 是否与客户端声明 mime 一致
 *
 * @return  true  检测不到（未知格式，保守放行） 或 检测到的 mime 与声明一致
 *          false 检测到危险可执行格式（PE/ELF/Mach-O，无视声明）
 *                或 检测到的 mime 与声明不同
 */
inline bool matches_claimed(std::string_view buf, std::string_view claimed_mime) {
    auto detected = sniff(buf);
    if (detected.empty()) return true;
    if (detected == "application/x-dosexec" ||
        detected == "application/x-elf" ||
        detected == "application/x-mach-binary") {
        return false;
    }
    return detected == claimed_mime;
}

}  // namespace chatnow::magic_sniff
