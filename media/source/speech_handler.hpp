#pragma once

/**
 * SpeechHandler —— 短音频 ASR 占位
 * ---
 * P4 v1 仅保留 RPC + bytes 字段，转发到 ASR 引擎的实际实现
 * 在 P7 完成（spec §3.8）。本类做：
 *   - bytes 长度上限 2MB（speech_content > 2MB 直接拒）
 *   - 返回空 recognition_result
 */

#include <cstdint>
#include <string>

#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "media/media_service.pb.h"

namespace chatnow {

class SpeechHandler {
public:
    explicit SpeechHandler(std::string asr_endpoint)
        : _ep(std::move(asr_endpoint)) {}

    void recognize(const ::chatnow::media::SpeechRecognitionReq& req,
                   ::chatnow::media::SpeechRecognitionRsp* rsp) {
        if (req.speech_content().size() > 2 * 1024 * 1024) {
            throw ServiceError(::chatnow::error::kMediaFileTooLarge, "speech > 2MB");
        }
        // P7：调真实 ASR endpoint；当前仅返回空字符串
        rsp->set_recognition_result("");
    }

private:
    std::string _ep;
};

}  // namespace chatnow
