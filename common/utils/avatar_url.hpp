#pragma once

/**
 * avatar_url —— 头像公开 URL 拼装工具
 * ---
 * 头像走 MediaService 公共 bucket（chatnow-media-public），bucket 已设
 * anonymous download，因此 file_id 可直接拼成永久可达的 GET URL，
 * 不必每次 ApplyDownload 签 presigned URL。
 *
 * 使用契约：
 *   1. 客户端 ApplyUpload(purpose=AVATAR) → PUT bytes → CompleteUpload，
 *      拿到 file_id（mime ∈ {image/jpeg, image/png, image/webp}）。
 *   2. 客户端 IdentityService.UpdateProfile(avatar_file_id=file_id)。
 *   3. IdentityService 收到后：
 *        a. 调 MediaService.GetFileInfo(file_id) 验证 mime；非 image 类拒绝。
 *        b. 调 avatar_url::of(public_prefix, file_id) 计算 URL 写入 user.avatar_url。
 *        c. 响应 UserInfo.avatar_url 为该 URL。
 *
 * 输入参数：
 *   - public_url_prefix：来自 conf/media.json `media.public_url_prefix`，
 *     例如 "http://127.0.0.1:9000/chatnow-media-public" 或 "https://cdn.example.com"。
 *   - file_id：snowflake 16 hex（见 file/source/upload_handler.hpp）。
 */

#include <string>
#include <string_view>

namespace chatnow::avatar_url {

/* brief: 把 file_id 拼成 <prefix>/avatar/<file_id> 形式 */
inline std::string of(std::string_view public_url_prefix, std::string_view file_id) {
    std::string out;
    out.reserve(public_url_prefix.size() + 8 + file_id.size());
    out.append(public_url_prefix);
    if (!out.empty() && out.back() == '/') out.pop_back();
    out.append("/avatar/").append(file_id);
    return out;
}

}  // namespace chatnow::avatar_url
