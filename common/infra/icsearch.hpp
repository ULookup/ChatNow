#pragma once

/**
 * ===========================================================================
 * Elasticsearch 客户端轻封装（基于 elasticlient）
 * ---------------------------------------------------------------------------
 * 设计要点（在原版基础上的修订）：
 *   1. 自由函数 Serialize / UnSerialize 加 inline，避免多 TU include 链接重复
 *   2. 移除 ESIndex 构造里调试遗留的 std::cout
 *   3. 新增 ESUpdate：upsert 语义，IM 频繁写覆盖场景
 *   4. ESSearch:
 *      - 同时存在 must 与 should 时自动加 minimum_should_match=1
 *        否则 ES bool query 行为会让 should 仅作打分加权，不参与命中
 *      - 增加 size / from 分页与 sort 排序设置
 *   5. 错误日志统一 LOG_ERROR；脱敏 body（仅在 trace 等级输出）
 *   6. ES 7.x+ 已不需要 type 参数；保留 "_doc" 兼容旧索引
 * ===========================================================================
 */

#include <elasticlient/client.h>
#include <cpr/cpr.h>
#include <jsoncpp/json/json.h>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "infra/logger.hpp"

namespace chatnow
{

/* brief: JSON -> string */
inline bool Serialize(const Json::Value &val, std::string &dst) {
    Json::StreamWriterBuilder swb;
    swb.settings_["emitUTF8"] = true;
    std::unique_ptr<Json::StreamWriter> sw(swb.newStreamWriter());
    std::stringstream ss;
    if(sw->write(val, &ss) != 0) {
        LOG_WARN("Json 序列化失败");
        return false;
    }
    dst = ss.str();
    return true;
}

/* brief: string -> JSON */
inline bool UnSerialize(const std::string &src, Json::Value &val) {
    Json::CharReaderBuilder crb;
    std::unique_ptr<Json::CharReader> cr(crb.newCharReader());
    std::string err;
    if(!cr->parse(src.c_str(), src.c_str() + src.size(), &val, &err)) {
        LOG_WARN("Json 反序列化失败: {}", err);
        return false;
    }
    return true;
}

/* brief: 索引 builder — 链式声明 mapping 字段后 create()
 *  - settings 内置 ik 分词器配置（中文分词）
 *  - 调用方按字段类型 / 是否参与检索逐个 append
 */
class ESIndex
{
public:
    ESIndex(std::shared_ptr<elasticlient::Client> &client,
            const std::string &name,
            const std::string &type = "_doc")
        : _name(name), _type(type), _client(client)
    {
        Json::Value tokenizer;
        tokenizer["tokenizer"] = "ik_max_word";
        Json::Value ik;       ik["ik"] = tokenizer;
        Json::Value analyzer; analyzer["analyzer"] = ik;
        Json::Value analysis; analysis["analysis"] = analyzer;
        _index["settings"] = analysis;
    }

    ESIndex& append(const std::string &key,
                    const std::string &type = "text",
                    const std::string &analyzer = "ik_max_word",
                    bool enabled = true)
    {
        Json::Value f;
        f["type"] = type;
        f["analyzer"] = analyzer;
        if(!enabled) f["enabled"] = enabled;
        _properties[key] = f;
        return *this;
    }

    bool create(const std::string &index_id = "default_index_id") {
        Json::Value mappings;
        mappings["dynamic"] = true;
        mappings["properties"] = _properties;
        _index["mappings"] = mappings;

        std::string body;
        if(!Serialize(_index, body)) {
            LOG_ERROR("索引序列化失败");
            return false;
        }
        LOG_TRACE("ES create index {}: {}", _name, body);
        try {
            auto rsp = _client->index(_name, _type, index_id, body);
            if(rsp.status_code < 200 || rsp.status_code >= 300) {
                LOG_ERROR("创建 ES 索引 {} 失败: status={}", _name, rsp.status_code);
                return false;
            }
        } catch(std::exception &e) {
            LOG_ERROR("创建 ES 索引 {} 异常: {}", _name, e.what());
            return false;
        }
        return true;
    }

private:
    std::string _name;
    std::string _type;
    Json::Value _properties;
    Json::Value _index;
    std::shared_ptr<elasticlient::Client> _client;
};

/* brief: 文档插入 builder（id 相同则覆盖，等价 upsert） */
class ESInsert
{
public:
    ESInsert(std::shared_ptr<elasticlient::Client> &client,
             const std::string &name,
             const std::string &type = "_doc")
        : _name(name), _type(type), _client(client) {}

    template <typename T>
    ESInsert &append(const std::string &key, const T &val) { _item[key] = val; return *this; }

    bool insert(const std::string &id = "") {
        std::string body;
        if(!Serialize(_item, body)) {
            LOG_ERROR("插入数据序列化失败");
            return false;
        }
        LOG_TRACE("ES insert {}: {}", _name, body);
        try {
            auto rsp = _client->index(_name, _type, id, body);
            if(rsp.status_code < 200 || rsp.status_code >= 300) {
                LOG_ERROR("新增 ES 数据失败 idx={} id={} status={}", _name, id, rsp.status_code);
                return false;
            }
        } catch(std::exception &e) {
            LOG_ERROR("新增 ES 数据异常 idx={} id={}: {}", _name, id, e.what());
            return false;
        }
        return true;
    }

private:
    std::string _name;
    std::string _type;
    Json::Value _item;
    std::shared_ptr<elasticlient::Client> _client;
};

/* brief: 部分字段更新 — IM 资料修改场景常用（无需重传整文档）
 *  - 通过 _update API 实现，POST /{index}/_update/{id}
 *  - elasticlient 没直接提供 update，使用 performRequest 走原始 HTTP
 */
class ESUpdate
{
public:
    ESUpdate(std::shared_ptr<elasticlient::Client> &client,
             const std::string &name)
        : _name(name), _client(client) {}

    template <typename T>
    ESUpdate &set(const std::string &key, const T &val) { _doc[key] = val; return *this; }

    /* brief: doc-as-upsert 即如果文档不存在则插入 */
    bool update(const std::string &id, bool upsert = true) {
        Json::Value root;
        root["doc"] = _doc;
        if(upsert) root["doc_as_upsert"] = true;

        std::string body;
        if(!Serialize(root, body)) {
            LOG_ERROR("更新数据序列化失败");
            return false;
        }
        LOG_TRACE("ES update {}/{}: {}", _name, id, body);
        try {
            auto rsp = _client->performRequest(
                elasticlient::Client::HTTPMethod::POST,
                _name + "/_update/" + id, body);
            if(rsp.status_code < 200 || rsp.status_code >= 300) {
                LOG_ERROR("更新 ES 数据失败 idx={} id={} status={}", _name, id, rsp.status_code);
                return false;
            }
        } catch(std::exception &e) {
            LOG_ERROR("更新 ES 数据异常 idx={} id={}: {}", _name, id, e.what());
            return false;
        }
        return true;
    }

private:
    std::string _name;
    Json::Value _doc;
    std::shared_ptr<elasticlient::Client> _client;
};

class ESRemove
{
public:
    ESRemove(std::shared_ptr<elasticlient::Client> &client,
             const std::string &name,
             const std::string &type = "_doc")
        : _name(name), _type(type), _client(client) {}

    bool remove(const std::string &id) {
        try {
            auto rsp = _client->remove(_name, _type, id);
            if(rsp.status_code < 200 || rsp.status_code >= 300) {
                LOG_ERROR("删除 ES 数据失败 idx={} id={} status={}", _name, id, rsp.status_code);
                return false;
            }
        } catch(std::exception &e) {
            LOG_ERROR("删除 ES 数据异常 idx={} id={}: {}", _name, id, e.what());
            return false;
        }
        return true;
    }

private:
    std::string _name;
    std::string _type;
    std::shared_ptr<elasticlient::Client> _client;
};

/* brief: 查询 builder
 *  - must / must_not / should 链式追加
 *  - 支持分页 size/from、排序 sort、最小 should 命中数
 */
class ESSearch
{
public:
    ESSearch(std::shared_ptr<elasticlient::Client> &client,
             const std::string &name,
             const std::string &type = "_doc")
        : _name(name), _type(type), _client(client) {}

    ESSearch &append_must_not_terms(const std::string &key, const std::vector<std::string> &vals) {
        Json::Value fields;
        for(const auto &v : vals) fields[key].append(v);
        Json::Value terms; terms["terms"] = fields;
        _must_not.append(terms);
        return *this;
    }
    ESSearch &append_should_match(const std::string &key, const std::string &val) {
        Json::Value f; f[key] = val;
        Json::Value m; m["match"] = f;
        _should.append(m);
        return *this;
    }
    ESSearch &append_must_term(const std::string &key, const std::string &val) {
        Json::Value f; f[key] = val;
        Json::Value t; t["term"] = f;
        _must.append(t);
        return *this;
    }
    ESSearch &append_must_match(const std::string &key, const std::string &val) {
        Json::Value f; f[key] = val;
        Json::Value m; m["match"] = f;
        _must.append(m);
        return *this;
    }

    /* brief: 分页 */
    ESSearch &page(int from, int size) { _from = from; _size = size; return *this; }

    /* brief: 排序（多字段调用多次） */
    ESSearch &sort_by(const std::string &field, const std::string &order = "desc") {
        Json::Value f; f[field] = order;
        _sort.append(f);
        return *this;
    }

    Json::Value search() {
        Json::Value cond;
        if(!_must_not.empty()) cond["must_not"] = _must_not;
        if(!_should.empty())   cond["should"]   = _should;
        if(!_must.empty())     cond["must"]     = _must;
        // 同时有 must 和 should 时，should 默认仅作打分加权 — 这里强制要求至少 1 项命中
        if(!_should.empty() && !_must.empty()) cond["minimum_should_match"] = 1;

        Json::Value query; query["bool"] = cond;
        Json::Value root;  root["query"] = query;
        if(_size > 0) root["size"] = _size;
        if(_from >= 0) root["from"] = _from;
        if(!_sort.empty()) root["sort"] = _sort;

        std::string body;
        if(!Serialize(root, body)) {
            LOG_ERROR("查询 DSL 序列化失败");
            return Json::Value();
        }
        LOG_TRACE("ES search {}: {}", _name, body);

        cpr::Response rsp;
        try {
            rsp = _client->search(_name, _type, body);
            if(rsp.status_code < 200 || rsp.status_code >= 300) {
                LOG_ERROR("检索 ES 数据失败 idx={} status={}", _name, rsp.status_code);
                return Json::Value();
            }
        } catch(std::exception &e) {
            LOG_ERROR("检索 ES 数据异常 idx={}: {}", _name, e.what());
            return Json::Value();
        }
        Json::Value json_res;
        if(!UnSerialize(rsp.text, json_res)) {
            LOG_ERROR("ES 查询结果反序列化失败: {}", rsp.text);
        }
        return json_res["hits"]["hits"];
    }

private:
    std::string _name;
    std::string _type;
    Json::Value _must_not;
    Json::Value _must;
    Json::Value _should;
    Json::Value _sort;
    int _size = 0;     // 0 表示用 ES 默认（一般是 10）
    int _from = -1;    // <0 表示不显式传
    std::shared_ptr<elasticlient::Client> _client;
};

} // namespace chatnow
