#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include "logger.hpp"

namespace chatnow
{

struct mail_settings {
    std::string username;
    std::string password;
    std::string url;
    std::string from;
};

class CodeClient
{
public:
    CodeClient() = default;
    ~CodeClient() = default;
    virtual bool send(const std::string &to, const std::string &code) = 0;
};

class MailClient : public CodeClient
{
public:
    /* brief: 初始化全局配置，保存服务配置信息 */
    MailClient(const mail_settings &settings) : _settings(settings) {
        auto ret = curl_global_init(CURL_GLOBAL_DEFAULT);
        if(ret != CURLE_OK) {
            LOG_ERROR("初始化CURL全局配置失败: {}", curl_easy_strerror(ret));
            abort();
        }
    }
    /* brief: 释放全局配置资源 */
    ~MailClient() { curl_global_cleanup(); }
    /* brief: 发送邮件 */
    virtual bool send(const std::string &to, const std::string &code) override {
        //1. 构造操作句柄
        auto curl = curl_easy_init();
        if(curl == nullptr) {
            LOG_ERROR("构造操作句柄失败");
            return false;
        }
        //2. 设置请求参数
        auto ret = curl_easy_setopt(curl, CURLOPT_URL, _settings.url.c_str());
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的URL请求参数失败: {}", curl_easy_strerror(ret));
            return false;
        }
        ret = curl_easy_setopt(curl, CURLOPT_USERNAME, _settings.username.c_str());
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的Username请求参数失败: {}", curl_easy_strerror(ret));
            return false;
        }
        ret = curl_easy_setopt(curl, CURLOPT_PASSWORD, _settings.password.c_str());
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的Password请求参数失败: {}", curl_easy_strerror(ret));
            return false;
        }
        ret = curl_easy_setopt(curl, CURLOPT_MAIL_FROM, _settings.from.c_str());
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的From请求参数失败: {}", curl_easy_strerror(ret));
            return false;
        }
        struct curl_slist *cs = nullptr;
        cs = curl_slist_append(cs, to.c_str());
        ret = curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, cs);
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的RCPT失败: {}", curl_easy_strerror(ret));
            return false;
        }
        auto body = codeBody(to, code);
        //3. 设置正文
        ret = curl_easy_setopt(curl, CURLOPT_READDATA, (void*)&body);
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的READDATA失败: {}", curl_easy_strerror(ret));
            return false;
        }
        //4. 设置回调
        ret = curl_easy_setopt(curl, CURLOPT_READFUNCTION, &callback);
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的READFUNCTION失败: {}", curl_easy_strerror(ret));
            return false;
        }
        //5. 设置发送模式
        ret = curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        if(ret != CURLE_OK) {
            LOG_ERROR("设置CURL的发送模式失败: {}", curl_easy_strerror(ret));
            return false;
        }
        //6. 执行请求
        ret = curl_easy_perform(curl);
        if(ret != CURLE_OK) {
            LOG_ERROR("请求邮件服务器失败: {}", curl_easy_strerror(ret));
            return false;
        }
        //7. 清理资源
        curl_slist_free_all(cs);
        curl_easy_cleanup(curl);
        LOG_DEBUG("发送邮件成功: {}-{}", to, code);
        return true;
    }
private:
    /* brief: 构造邮件正文 */
    std::stringstream codeBody(const std::string &to, const std::string &code) {
        std::stringstream ss;
        ss << "Subject: " << _title << "\r\n"; //邮件标题
        ss << "Content-Type: text/html\r\n";
        ss << "\r\n";
        ss << "<html><body><p>你的验证码: <b>" << code << "</b></p><p>验证码将在5分钟后失效.</p></body></html>\r\n";
        return ss;
    }
    /* brief: curl请求处理回调 */
    static size_t callback(char *buffer, size_t size, size_t nitems, void *userdata) {
        std::stringstream *ss = (std::stringstream*)userdata;
        ss->read(buffer, size * nitems);
        return ss->gcount();
    }
private:
    const std::string _title = "验证码";
    mail_settings _settings;
};

}