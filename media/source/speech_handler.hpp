#pragma once

// The ASR backend is not wired yet. Validate the PCM16 payload and report
// unavailability instead of acknowledging audio that was never processed.

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
        if (req.speech_content().empty() || req.speech_content().size() % 2 != 0) {
            throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                               "non-empty PCM16 content required");
        }
        // No ASR engine is wired. Do not acknowledge unprocessed audio as success.
        throw ServiceError(::chatnow::error::kSystemUnavailable,
                           "speech recognition unavailable");
    }

private:
    std::string _ep;
};

}  // namespace chatnow
