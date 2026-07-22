#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace chatnow::config {

enum class SecretId {
    JwtConfig,
    IdentityMysqlPassword,
    ConversationMysqlPassword,
    RelationshipMysqlPassword,
    MessageMysqlPassword,
    MediaMysqlPassword,
    TransmiteMqPassword,
    MessageMqPassword,
    PushMqPassword,
    IdentitySmtpPassword,
    MediaS3AccessKey,
    MediaS3SecretKey,
};

struct SecretSpec {
    const char* env_name;
    const char* env_file;
    std::size_t max_bytes;
};

inline SecretSpec secret_spec(SecretId id) {
    constexpr std::size_t kPasswordMaxBytes = 4096;
    constexpr std::size_t kJwtConfigMaxBytes = 64 * 1024;
    switch (id) {
        case SecretId::JwtConfig:
            return {"CHATNOW_JWT_CONFIG", "CHATNOW_JWT_CONFIG_FILE", kJwtConfigMaxBytes};
        case SecretId::IdentityMysqlPassword:
            return {"CHATNOW_IDENTITY_MYSQL_PASSWORD", "CHATNOW_IDENTITY_MYSQL_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::ConversationMysqlPassword:
            return {"CHATNOW_CONVERSATION_MYSQL_PASSWORD", "CHATNOW_CONVERSATION_MYSQL_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::RelationshipMysqlPassword:
            return {"CHATNOW_RELATIONSHIP_MYSQL_PASSWORD", "CHATNOW_RELATIONSHIP_MYSQL_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::MessageMysqlPassword:
            return {"CHATNOW_MESSAGE_MYSQL_PASSWORD", "CHATNOW_MESSAGE_MYSQL_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::MediaMysqlPassword:
            return {"CHATNOW_MEDIA_MYSQL_PASSWORD", "CHATNOW_MEDIA_MYSQL_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::TransmiteMqPassword:
            return {"CHATNOW_TRANSMITE_MQ_PASSWORD", "CHATNOW_TRANSMITE_MQ_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::MessageMqPassword:
            return {"CHATNOW_MESSAGE_MQ_PASSWORD", "CHATNOW_MESSAGE_MQ_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::PushMqPassword:
            return {"CHATNOW_PUSH_MQ_PASSWORD", "CHATNOW_PUSH_MQ_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::IdentitySmtpPassword:
            return {"CHATNOW_IDENTITY_SMTP_PASSWORD", "CHATNOW_IDENTITY_SMTP_PASSWORD_FILE", kPasswordMaxBytes};
        case SecretId::MediaS3AccessKey:
            return {"CHATNOW_MEDIA_S3_ACCESS_KEY", "CHATNOW_MEDIA_S3_ACCESS_KEY_FILE", kPasswordMaxBytes};
        case SecretId::MediaS3SecretKey:
            return {"CHATNOW_MEDIA_S3_SECRET_KEY", "CHATNOW_MEDIA_S3_SECRET_KEY_FILE", kPasswordMaxBytes};
    }
    throw std::runtime_error("SecretId: invalid_secret_id");
}

namespace detail {

[[noreturn]] inline void throw_secret_error(const char* locator, const char* reason) {
    throw std::runtime_error(std::string(locator) + ": " + reason);
}

inline void reject_nul(const std::string& value, const char* locator) {
    if (value.find('\0') != std::string::npos) {
        throw_secret_error(locator, "value_contains_nul");
    }
}

inline void validate_secret_value(const std::string& value,
                                  const SecretSpec& spec,
                                  const char* locator) {
    if (value.empty()) {
        throw_secret_error(locator, "value_empty");
    }
    reject_nul(value, locator);
    if (value.size() > spec.max_bytes) {
        throw_secret_error(locator, "value_too_large");
    }
}

inline void trim_one_trailing_line_ending(std::string& value) {
    if (!value.empty() && value.back() == '\n') {
        value.pop_back();
        if (!value.empty() && value.back() == '\r') {
            value.pop_back();
        }
    } else if (!value.empty() && value.back() == '\r') {
        value.pop_back();
    }
}

class ScopedFd {
public:
    explicit ScopedFd(int fd) noexcept : _fd(fd) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ~ScopedFd() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    int get() const noexcept { return _fd; }

private:
    int _fd;
};

inline std::string read_secret_file(const std::string& path, const SecretSpec& spec) {
    constexpr std::size_t kMaxLocatorBytes = 4096;
    if (path.empty()) {
        throw_secret_error(spec.env_file, "locator_empty");
    }
    if (path.front() != '/') {
        throw_secret_error(spec.env_file, "locator_not_absolute");
    }
    reject_nul(path, spec.env_file);
    if (path.size() > kMaxLocatorBytes) {
        throw_secret_error(spec.env_file, "locator_too_large");
    }

    int raw_fd;
    do {
        raw_fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    } while (raw_fd < 0 && errno == EINTR);
    if (raw_fd < 0) {
        throw_secret_error(spec.env_file, "open_failed");
    }
    ScopedFd fd(raw_fd);

    struct stat status {};
    if (::fstat(fd.get(), &status) != 0) {
        throw_secret_error(spec.env_file, "stat_failed");
    }
    if (!S_ISREG(status.st_mode)) {
        throw_secret_error(spec.env_file, "not_regular_file");
    }
    const uid_t effective_uid = ::geteuid();
    if (status.st_uid != effective_uid && status.st_uid != 0) {
        throw_secret_error(spec.env_file, "owner_not_allowed");
    }
    constexpr mode_t kForbiddenMode = S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
    if ((status.st_mode & kForbiddenMode) != 0) {
        throw_secret_error(spec.env_file, "permissions_too_open");
    }

    constexpr std::size_t kTrailingLineEndingMaxBytes = 2;
    const std::size_t raw_limit = spec.max_bytes + kTrailingLineEndingMaxBytes;
    if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > raw_limit) {
        throw_secret_error(spec.env_file, "value_too_large");
    }

    std::string value;
    value.reserve(std::min<std::size_t>(static_cast<std::size_t>(status.st_size), raw_limit));
    std::array<char, 4096> buffer {};
    while (true) {
        const std::size_t remaining = raw_limit + 1 - value.size();
        const std::size_t request_size = std::min(buffer.size(), remaining);
        ssize_t count;
        do {
            count = ::read(fd.get(), buffer.data(), request_size);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            throw_secret_error(spec.env_file, "read_failed");
        }
        if (count == 0) {
            break;
        }
        value.append(buffer.data(), static_cast<std::size_t>(count));
        if (value.size() > raw_limit) {
            throw_secret_error(spec.env_file, "value_too_large");
        }
    }

    reject_nul(value, spec.env_file);
    trim_one_trailing_line_ending(value);
    validate_secret_value(value, spec, spec.env_file);
    return value;
}

}  // namespace detail

inline std::string resolve_secret(SecretId id) {
    const SecretSpec spec = secret_spec(id);
    const char* env_value = std::getenv(spec.env_name);
    const char* env_file = std::getenv(spec.env_file);

    if (env_value != nullptr && env_file != nullptr) {
        detail::throw_secret_error(spec.env_name, "source_conflict");
    }
    if (env_value == nullptr && env_file == nullptr) {
        detail::throw_secret_error(spec.env_name, "source_missing");
    }
    if (env_value != nullptr) {
        std::string value(env_value);
        detail::validate_secret_value(value, spec, spec.env_name);
        return value;
    }
    return detail::read_secret_file(std::string(env_file), spec);
}

}  // namespace chatnow::config
