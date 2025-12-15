#pragma once

#include "../third/include/aip-cpp-sdk/speech.h"
#include "logger.hpp"

class ASRClient 
{
public:
    ASRClient(const std::string &appid,
            const std::string &api_key,
            const std::string &secret_key) : _client(appid, api_key, secret_key) {}
    std::string recognize(const std::string &speech_data)
    {
        Json::Value result = _client.recognize(speech_data, "pcm", 16000, aip::null);
        if(result["err_no"].asInt() != 0) {
            LOG_ERROR("语言识别失败: {}", result["err_msg"].asString());
            return std::string();
        }
    }
private:
    aip::Speech _client;
};