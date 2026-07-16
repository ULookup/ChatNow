#include "dao/data_redis.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using chatnow::RedisClient;
using chatnow::RedisClusterFactory;
using chatnow::UnackedPush;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

std::string unique_component(const std::string &prefix) {
    static unsigned long counter = 0;
    return prefix + "-" + std::to_string(getpid()) + "-" +
           std::to_string(static_cast<unsigned long>(std::time(nullptr))) + "-" +
           std::to_string(++counter);
}

long long zcard(const RedisClient::ptr &redis, const std::string &key) {
    static const std::string script = "return redis.call('ZCARD', KEYS[1])";
    std::vector<std::string> keys = {key};
    std::vector<std::string> args;
    return redis->eval<long long>(script, keys.begin(), keys.end(),
                                  args.begin(), args.end());
}

std::string zscore(const RedisClient::ptr &redis, const std::string &key,
                   const std::string &member) {
    static const std::string script = "return redis.call('ZSCORE', KEYS[1], ARGV[1])";
    std::vector<std::string> keys = {key};
    std::vector<std::string> args = {member};
    return redis->eval<std::string>(script, keys.begin(), keys.end(),
                                    args.begin(), args.end());
}

long long ttl(const RedisClient::ptr &redis, const std::string &key) {
    static const std::string script = "return redis.call('TTL', KEYS[1])";
    std::vector<std::string> keys = {key};
    std::vector<std::string> args;
    return redis->eval<long long>(script, keys.begin(), keys.end(),
                                  args.begin(), args.end());
}

struct Fixture {
    explicit Fixture(const RedisClient::ptr &client, std::string test_name)
        : redis(client), ledger(client), uid(unique_component(std::move(test_name))),
          device("device-" + std::to_string(getpid())),
          pending_key(UnackedPush::key_for(uid, device)),
          payload_key(UnackedPush::idx_key_for(uid, device)),
          repair_key(UnackedPush::repair_key_for(uid, device)) {}

    ~Fixture() {
        try {
            redis->del(pending_key);
            redis->del(payload_key);
            redis->del(repair_key);
        } catch (...) {
        }
    }

    RedisClient::ptr redis;
    UnackedPush ledger;
    std::string uid;
    std::string device;
    std::string pending_key;
    std::string payload_key;
    std::string repair_key;
};

void test_push_replaces_payload_without_changing_identity(const RedisClient::ptr &redis) {
    Fixture fixture(redis, "push-replace");
    const auto due_score = static_cast<long long>(std::time(nullptr)) - 60;

    fixture.ledger.push(fixture.uid, fixture.device, 41, "payload-A", due_score);
    fixture.ledger.push(fixture.uid, fixture.device, 41, "payload-B", due_score);

    std::vector<std::string> members;
    redis->zrange(fixture.pending_key, 0, -1, std::back_inserter(members));
    require(zcard(redis, fixture.pending_key) == 1,
            "re-push with changed payload must leave ZCARD == 1");
    require(members.size() == 1, "the pending ZSET must contain one member");
    require(members.front() == "41", "ZSET member must be the decimal user_seq only");

    const auto due = fixture.ledger.peek_due(fixture.uid, fixture.device, 10, 0);
    require(due.size() == 1, "re-pushed sequence must be returned exactly once");
    require(due.front().first == 41 && due.front().second == "payload-B",
            "due-read must return the replacement payload from the HASH");
}

void test_ack_removes_zset_and_hash_entries(const RedisClient::ptr &redis) {
    Fixture fixture(redis, "ack-both");
    fixture.ledger.push(fixture.uid, fixture.device, 52, "payload-A", 1);
    fixture.ledger.push(fixture.uid, fixture.device, 52, "payload-B", 1);

    fixture.ledger.ack(fixture.uid, fixture.device, 52);

    std::vector<std::string> members;
    redis->zrange(fixture.pending_key, 0, -1, std::back_inserter(members));
    require(members.empty(),
            "ACK must remove the ZSET member");
    require(!redis->hget(fixture.payload_key, "52"),
            "ACK must remove the HASH field");
}

void test_due_read_removes_both_orphan_directions(const RedisClient::ptr &redis) {
    Fixture fixture(redis, "due-heal");
    redis->zadd(fixture.pending_key, "61", 1);
    redis->hset(fixture.payload_key, "62", "hash-only");

    const auto due = fixture.ledger.peek_due(fixture.uid, fixture.device, 10, 0);

    require(due.empty(), "orphan entries must never be returned as due");
    std::vector<std::string> members;
    redis->zrange(fixture.pending_key, 0, -1, std::back_inserter(members));
    require(members.empty(),
            "due-read must remove a ZSET-only orphan");
    require(!redis->hget(fixture.payload_key, "62"),
            "due-read bounded consistency pass must remove a HASH-only orphan");
}

void test_ack_wrong_type_does_not_partially_remove_zset(const RedisClient::ptr &redis) {
    Fixture fixture(redis, "ack-wrong-type");
    fixture.ledger.push(fixture.uid, fixture.device, 63, "payload", 1);
    redis->del(fixture.payload_key);
    redis->set(fixture.payload_key, "not-a-hash", std::chrono::seconds(300));

    fixture.ledger.ack(fixture.uid, fixture.device, 63);

    require(zcard(redis, fixture.pending_key) == 1,
            "ACK must validate both key types before removing the ZSET member");
}

void test_due_read_progresses_across_hash_orphan_pages(const RedisClient::ptr &redis) {
    Fixture fixture(redis, "due-progressive-heal");
    fixture.ledger.push(fixture.uid, fixture.device, 9000, "complete",
                        static_cast<long long>(std::time(nullptr)) + 3600,
                        std::chrono::seconds(300));
    for (unsigned long seq = 10000; seq < 10700; ++seq) {
        redis->hset(fixture.payload_key, std::to_string(seq), "hash-only");
    }

    fixture.ledger.peek_due(fixture.uid, fixture.device, 5, 0);
    auto cursor = redis->get(fixture.repair_key);
    require(cursor && *cursor != "0",
            "a bounded HASH repair pass must persist its non-zero HSCAN cursor");
    const auto repair_ttl = ttl(redis, fixture.repair_key);
    const auto payload_ttl = ttl(redis, fixture.payload_key);
    require(repair_ttl > 0 && repair_ttl <= payload_ttl,
            "repair cursor must expire with, and never outlive, the pending ledger");

    for (int pass = 0; pass < 1000; ++pass) {
        if (redis->hlen(fixture.payload_key) == 1 && !redis->get(fixture.repair_key)) break;
        fixture.ledger.peek_due(fixture.uid, fixture.device, 5, 0);
    }
    require(redis->hlen(fixture.payload_key) == 1,
            "successive bounded due-reads must eventually remove every HASH-only orphan");
    require(!redis->get(fixture.repair_key),
            "repair cursor must be deleted after a full stable scan completes");
}

void test_bump_updates_only_complete_entries(const RedisClient::ptr &redis) {
    Fixture fixture(redis, "bump-complete");
    fixture.ledger.push(fixture.uid, fixture.device, 71, "complete", 1);
    redis->zadd(fixture.pending_key, "72", 1);
    redis->expire(fixture.pending_key, std::chrono::seconds(10));
    redis->expire(fixture.payload_key, std::chrono::seconds(10));

    const auto before = static_cast<long long>(std::time(nullptr));
    fixture.ledger.bump_score(fixture.uid, fixture.device, {71, 72},
                              std::chrono::seconds(300));
    const auto after = static_cast<long long>(std::time(nullptr));

    const auto bumped_score = std::stoll(zscore(redis, fixture.pending_key, "71"));
    require(bumped_score >= before && bumped_score <= after,
            "bump must set the complete entry score to the current time");
    const auto pending_ttl = ttl(redis, fixture.pending_key);
    const auto payload_ttl = ttl(redis, fixture.payload_key);
    require(pending_ttl >= 200 && payload_ttl >= 200,
            "bump must renew both ledger keys from their forced short TTL");
    require(std::llabs(pending_ttl - payload_ttl) <= 1,
            "bump must apply the same randomized TTL sample to both ledger keys");

    const auto complete = fixture.ledger.peek_due(fixture.uid, fixture.device, 10, -1);
    require(complete.size() == 1 && complete.front().first == 71,
            "bump must retain and update the complete entry");
    std::vector<std::string> members;
    redis->zrange(fixture.pending_key, 0, -1, std::back_inserter(members));
    require(members.size() == 1 && members.front() == "71",
            "bump must remove a ZSET entry whose HASH payload is missing");
}

} // namespace

int main() {
    try {
        const char *configured = std::getenv("CHATNOW_REDIS_CLUSTER_SEEDS");
        const std::string seeds = configured ? configured : "redis-node1:6379";
        auto cluster = RedisClusterFactory::create(seeds, 2);
        auto redis = std::make_shared<RedisClient>(std::move(cluster));

        const std::vector<std::pair<std::string, void (*)(const RedisClient::ptr &)>> tests = {
            {"push replaces payload", test_push_replaces_payload_without_changing_identity},
            {"ack removes both", test_ack_removes_zset_and_hash_entries},
            {"ack wrong type is atomic", test_ack_wrong_type_does_not_partially_remove_zset},
            {"due-read heals both directions", test_due_read_removes_both_orphan_directions},
            {"due-read repair is progressive", test_due_read_progresses_across_hash_orphan_pages},
            {"bump updates complete entries", test_bump_updates_only_complete_entries},
        };
        int failures = 0;
        for (const auto &[name, test] : tests) {
            try {
                test(redis);
            } catch (const std::exception &error) {
                ++failures;
                std::cerr << "FAILED " << name << ": " << error.what() << '\n';
            }
        }
        if (failures != 0) {
            std::cerr << failures << " unacked pending ledger cluster test(s) failed\n";
            return 1;
        }
        std::cout << "unacked pending ledger cluster tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "unacked pending ledger cluster test failed: " << error.what() << '\n';
        return 1;
    }
}
