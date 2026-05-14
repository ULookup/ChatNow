#pragma once

/**
 * ===========================================================================
 * 邮件 / 验证码下发封装（基于 libcurl SMTP）
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. CodeClient 抽象接口保留 — 后续可换成第三方短信 / 邮件 SaaS
 *   2. send() 旧实现存在资源泄漏：错误路径只 return 不 cleanup curl handle、
 *      不 free curl_slist；本版本统一在函数末尾 cleanup
 *   3. curl handle 改为 RAII（CurlHandle / SListHandle）避免漏 free
 *   4. 增加 connect timeout / total timeout，避免 SMTP 阻塞调用线程
 *   5. 邮件正文模板抽出，便于扩展找回密码 / 异地登录通知等场景
 * ===========================================================================
 */

#include <curl/curl.h>
#include <memory>
#include <sstream>
#include <string>
#include "infra/logger.hpp"

namespace chatnow
{

struct mail_settings {
    std::string username;
    std::string password;
    std::string url;        // smtps://smtp.xxx:465
    std::string from;
};

/* brief: 验证码下发抽象接口（邮箱 / 短信 / 第三方 SaaS） */
class CodeClient
{
public:
    CodeClient() = default;
    virtual ~CodeClient() = default;
    virtual bool send(const std::string &to, const std::string &code) = 0;
};

/* brief: libcurl 资源 RAII */
struct CurlHandle {
    CURL *h = nullptr;
    CurlHandle() : h(curl_easy_init()) {}
    ~CurlHandle() { if(h) curl_easy_cleanup(h); }
    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;
};

struct SListHandle {
    curl_slist *l = nullptr;
    void append(const std::string &s) { l = curl_slist_append(l, s.c_str()); }
    ~SListHandle() { if(l) curl_slist_free_all(l); }
};

class MailClient : public CodeClient
{
public:
    using ptr = std::shared_ptr<MailClient>;

    explicit MailClient(const mail_settings &settings) : _settings(settings) {
        auto ret = curl_global_init(CURL_GLOBAL_DEFAULT);
        if(ret != CURLE_OK) {
            LOG_ERROR("初始化 CURL 全局配置失败: {}", curl_easy_strerror(ret));
            abort();
        }
    }
    ~MailClient() override { curl_global_cleanup(); }

    /* brief: 发送验证码邮件 */
    bool send(const std::string &to, const std::string &code) override {
        CurlHandle curl;
        if(!curl.h) {
            LOG_ERROR("构造 CURL 操作句柄失败");
            return false;
        }

        if(!setopt(curl.h, CURLOPT_URL, _settings.url.c_str(),       "URL")) return false;
        if(!setopt(curl.h, CURLOPT_USERNAME, _settings.username.c_str(), "USERNAME")) return false;
        if(!setopt(curl.h, CURLOPT_PASSWORD, _settings.password.c_str(), "PASSWORD")) return false;
        if(!setopt(curl.h, CURLOPT_MAIL_FROM, _settings.from.c_str(),    "FROM")) return false;

        SListHandle rcpt;
        rcpt.append(to);
        if(!setopt(curl.h, CURLOPT_MAIL_RCPT, rcpt.l, "RCPT")) return false;

        auto body = codeBody(to, code);
        if(!setopt(curl.h, CURLOPT_READDATA, (void*)&body, "READDATA")) return false;
        if(!setopt(curl.h, CURLOPT_READFUNCTION, &callback, "READFUNCTION")) return false;
        if(!setopt(curl.h, CURLOPT_UPLOAD, 1L, "UPLOAD")) return false;

        // 增加超时，避免 SMTP 卡死调用线程
        if(!setopt(curl.h, CURLOPT_CONNECTTIMEOUT, 5L,  "CONNECTTIMEOUT")) return false;
        if(!setopt(curl.h, CURLOPT_TIMEOUT,        15L, "TIMEOUT")) return false;

        auto ret = curl_easy_perform(curl.h);
        if(ret != CURLE_OK) {
            LOG_ERROR("请求邮件服务器失败: {}", curl_easy_strerror(ret));
            return false;
        }
        LOG_DEBUG("发送邮件成功: {}-{}", to, code);
        return true;
    }

    mail_settings settings() const { return _settings; }

private:
    /* brief: setopt 包装：错误路径走日志而非裸 return；模板支持任意值类型 */
    template <class T>
    static bool setopt(CURL *c, CURLoption opt, T val, const char *what) {
        auto ret = curl_easy_setopt(c, opt, val);
        if(ret != CURLE_OK) {
            LOG_ERROR("CURL setopt {} 失败: {}", what, curl_easy_strerror(ret));
            return false;
        }
        return true;
    }

    /* brief: 构造邮件正文（HTML 验证码模板） */
    std::stringstream codeBody(const std::string &to, const std::string &code) const {
        (void)to;
        std::stringstream ss;
        ss << "Subject: " << _title << "\r\n"
           << "Content-Type: text/html\r\n"
           << "\r\n"
           << "<html><body><p>你的验证码: <b>" << code
           << "</b></p><p>验证码将在 5 分钟后失效.</p></body></html>\r\n";
        return ss;
    }

    static size_t callback(char *buffer, size_t size, size_t nitems, void *userdata) {
        auto *ss = static_cast<std::stringstream*>(userdata);
        ss->read(buffer, static_cast<std::streamsize>(size * nitems));
        return static_cast<size_t>(ss->gcount());
    }

    const std::string _title = "验证码";
    mail_settings _settings;
};

} // namespace chatnow
