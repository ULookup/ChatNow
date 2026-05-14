#pragma once

/**
 * ===========================================================================
 * 语音识别封装（基于百度 AIP cpp-sdk）
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. 抽出 SpeechRecognizer 接口，便于后续替换不同 ASR 提供商
 *   2. recognize() 同时返回结果与错误信息，调用方按 res.empty() 判错
 *   3. 默认参数：PCM / 16kHz；如需扩展请在 recognize() 增加重载
 *   4. 错误日志统一脱敏：不输出原始 speech_data 二进制
 * ===========================================================================
 */

#include <memory>
#include <string>
#include "../third/include/aip-cpp-sdk/speech.h"
#include "infra/logger.hpp"

namespace chatnow
{

/* brief: 语音识别接口（解耦 ASR 提供商） */
class SpeechRecognizer
{
public:
    using ptr = std::shared_ptr<SpeechRecognizer>;
    virtual ~SpeechRecognizer() = default;
    /* brief: 识别一段 PCM 16kHz 数据为文字
     *  @param speech_data 二进制语音数据
     *  @param err         返回错误信息（仅失败时填充）
     *  @return 文字结果；空字符串表示失败
     */
    virtual std::string recognize(const std::string &speech_data, std::string &err) = 0;
};

class ASRClient : public SpeechRecognizer
{
public:
    using ptr = std::shared_ptr<ASRClient>;

    ASRClient(const std::string &appid,
              const std::string &api_key,
              const std::string &secret_key)
        : _client(appid, api_key, secret_key) {}

    std::string recognize(const std::string &speech_data, std::string &err) override {
        try {
            Json::Value result = _client.recognize(speech_data, "pcm", 16000, aip::null);
            if(result["err_no"].asInt() != 0) {
                err = result["err_msg"].asString();
                LOG_ERROR("语音识别失败 err_no={} msg={}", result["err_no"].asInt(), err);
                return std::string();
            }
            // 百度 SDK 偶发返回空数组，做防御
            if(!result["result"].isArray() || result["result"].size() == 0) {
                err = "结果为空";
                LOG_ERROR("语音识别返回空结果");
                return std::string();
            }
            return result["result"][0].asString();
        } catch(const std::exception &e) {
            err = e.what();
            LOG_ERROR("语音识别异常: {}", e.what());
            return std::string();
        }
    }

private:
    aip::Speech _client;
};

} // namespace chatnow
