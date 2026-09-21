/*
 * bsk.h — 后台服务基础组件库（Backend Service Kit）
 *
 * 对准后台/业务系统研发的四个核心问题：
 *   http        HTTP/1.1 请求解析与响应生成（方法/路径/头/体、keep-alive、状态码）
 *   server      epoll + 非阻塞 + 线程池 + 连接管理
 *   cache       LRU + TTL 缓存（GET/SET/EXPIRE/TTL 语义、命中率统计、淘汰）
 *   resilience  ★ 容灾 / 降级 / 应急：熔断器、令牌桶限流、降级返回、健康检查
 *   store       业务数据落盘（journal 格式；接口按 SQLite 语义设计）
 *
 * 纯 C++17，零第三方依赖。
 */
#ifndef BSK_H
#define BSK_H

#include <cstdint>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bsk {

/* ================================================================ HTTP */
enum class Method { GET, POST, PUT, DELETE_, HEAD, UNKNOWN };

const char *method_name(Method m);
Method parse_method(const std::string &s);

struct Request {
    Method method = Method::UNKNOWN;
    std::string path;
    std::string query;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool keep_alive = true;

    std::string header(const std::string &k) const;
};

struct Response {
    int status = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool keep_alive = true;

    std::string serialize() const;
};

const char *status_text(int code);

/* 解析 HTTP 请求；返回：0 成功、1 数据不完整（需继续收）、-1 协议错误 */
int http_parse_request(const std::string &raw, Request &out, size_t *consumed);

/* 构造常见响应 */
Response make_text(int status, const std::string &body);
Response make_json(int status, const std::string &json);

/* ================================================================ 缓存 */
struct CacheStat {
    uint64_t hits = 0, misses = 0, sets = 0, evictions = 0, expired = 0;
    double hit_ratio() const {
        uint64_t t = hits + misses;
        return t ? static_cast<double>(hits) / t : 0.0;
    }
};

/* LRU + TTL 缓存：容量满时淘汰最久未使用；支持过期时间 */
class LruTtlCache {
public:
    explicit LruTtlCache(size_t capacity) : cap_(capacity ? capacity : 1) {}

    void   set(const std::string &key, const std::string &value,
               double ttl_s = 0.0, double now = 0.0);   /* ttl<=0 表示不过期 */
    /* 正常读取：过期视为未命中（但**不删除**值，留给降级用） */
    bool   get(const std::string &key, std::string &out, double now = 0.0);
    /* 降级读取：过期也返回旧值，并通过 *expired 告知是否已过期 */
    bool   get_stale(const std::string &key, std::string &out, bool *expired = nullptr,
                     double now = 0.0);
    bool   del(const std::string &key, double now = 0.0);
    bool   expire(const std::string &key, double ttl_s, double now = 0.0);
    double ttl(const std::string &key, double now = 0.0);  /* -2 不存在，-1 无过期 */
    size_t size() const { return map_.size(); }
    const CacheStat &stat() const { return stat_; }
    void   clear_expired(double now);
    std::vector<std::string> keys() const;

private:
    struct Entry {
        std::string value;
        double expire_at;          /* <=0 表示不过期 */
        std::deque<std::string>::iterator lru_it;
    };
    size_t cap_;
    std::unordered_map<std::string, Entry> map_;
    std::deque<std::string> lru_;   /* 队尾最新 */
    CacheStat stat_;
    void touch(const std::string &key);
    bool is_expired(const Entry &e, double now) const {
        return e.expire_at > 0.0 && now >= e.expire_at;
    }
};

/* ========================================================= 容灾与降级 */
/* 熔断器三态：CLOSED 正常 / OPEN 熔断 / HALF_OPEN 试探 */
enum class BreakerState { Closed, Open, HalfOpen };
const char *breaker_state_name(BreakerState s);

struct BreakerConfig {
    uint32_t window = 20;           /* 统计窗口（最近 N 次调用） */
    double   fail_ratio = 0.5;      /* 失败率阈值 */
    double   open_seconds = 5.0;    /* 熔断持续时间 */
    uint32_t half_open_trials = 3;  /* 半开态放行试探次数 */
};

class CircuitBreaker {
public:
    explicit CircuitBreaker(BreakerConfig cfg = {}) : cfg_(cfg) {}

    /* 调用前询问是否允许（OPEN 且未到时间 -> 拒绝，触发降级） */
    bool allow(double now);
    /* 上报调用结果 */
    void on_success(double now);
    void on_failure(double now);

    BreakerState state() const { return state_; }
    double failure_ratio() const;
    uint32_t rejected() const { return rejected_; }
    uint32_t trips() const { return trips_; }
    BreakerConfig &config() { return cfg_; }

private:
    BreakerConfig cfg_;
    BreakerState state_ = BreakerState::Closed;
    std::deque<bool> window_;       /* true=成功 */
    double opened_at_ = 0.0;
    uint32_t half_open_left_ = 0;
    uint32_t rejected_ = 0, trips_ = 0;
};

/* 令牌桶限流 */
class TokenBucket {
public:
    /* EPS：令牌数是浮点累加出来的，0.5s*10/s 可能得到 0.9999999999999998，
     * 直接与 1.0 比较会"明明够却判不够"。比较时必须带容差。 */
    static constexpr double EPS = 1e-9;

    TokenBucket(double rate_per_s, double burst)
        : rate_(rate_per_s > 0 ? rate_per_s : 1.0),
          burst_(burst > 0 ? burst : 1.0), tokens_(burst > 0 ? burst : 1.0) {}
    /* 尝试取 1 个令牌；返回是否放行 */
    bool try_acquire(double now);
    double tokens(double now);
    uint32_t rejected() const { return rejected_; }

private:
    double rate_, burst_, tokens_, last_ = -1.0;
    uint32_t rejected_ = 0;
    void refill(double now);
};

/* 健康检查与降级策略 */
enum class Health { Up, Degraded, Down };
const char *health_name(Health h);

struct HealthConfig {
    uint32_t window = 10;
    double   degrade_ratio = 0.3;   /* 失败率超此值 -> Degraded */
    double   down_ratio = 0.7;      /* 超此值 -> Down */
};

class HealthMonitor {
public:
    explicit HealthMonitor(HealthConfig cfg = {}) : cfg_(cfg) {}
    void record(bool ok);
    Health evaluate() const;
    double failure_ratio() const;
    uint32_t samples() const { return static_cast<uint32_t>(window_.size()); }

private:
    HealthConfig cfg_;
    std::deque<bool> window_;
};

/* ============================================================ 数据存储 */
struct Row {
    std::map<std::string, std::string> cols;
    std::string get(const std::string &k) const {
        auto it = cols.find(k);
        return it == cols.end() ? std::string() : it->second;
    }
};

/* 极简关系表：接口按 SQLite 用法设计（建表/插入/参数化查询/聚合） */
class Table {
public:
    explicit Table(std::string name, std::vector<std::string> columns)
        : name_(std::move(name)), columns_(std::move(columns)) {}

    const std::string &name() const { return name_; }
    const std::vector<std::string> &columns() const { return columns_; }

    bool insert(const std::map<std::string, std::string> &values);
    /* 简单等值过滤（参数化语义：不拼字符串） */
    std::vector<Row> select(
        const std::map<std::string, std::string> &where = {},
        size_t limit = 0) const;
    size_t count() const { return rows_.size(); }
    size_t count_where(const std::string &col, const std::string &val) const;
    long long sum(const std::string &col) const;
    bool flush(const std::string &path) const;

private:
    std::string name_;
    std::vector<std::string> columns_;
    std::vector<Row> rows_;
};

/* ============================================================ 业务服务 */
struct ServiceMetrics {
    uint64_t requests = 0;
    uint64_t ok = 0, not_found = 0, bad_request = 0;
    uint64_t rate_limited = 0, circuit_rejected = 0, degraded = 0, errors = 0;
    double   total_latency_ms = 0.0;
    double avg_latency_ms() const {
        return requests ? total_latency_ms / requests : 0.0;
    }
};

/* 把 HTTP / 缓存 / 存储 / 容灾降级串成一条请求链路：
 *   请求 -> 限流 -> 健康度降级 -> 缓存 -> 熔断 -> 业务 -> 指标 */
class Service {
public:
    Service(size_t cache_cap, double rate_per_s, double burst,
            BreakerConfig bcfg = {}, HealthConfig hcfg = {});

    Response handle(const Request &req);

    LruTtlCache    &cache()   { return cache_; }
    TokenBucket    &limiter() { return limiter_; }
    CircuitBreaker &breaker() { return breaker_; }
    HealthMonitor  &health()  { return health_; }
    Table          &table()   { return t_; }
    const ServiceMetrics &metrics() const { return m_; }

    void set_now(double t) { now_ = t; }
    /* 模拟一次后端调用结果（供测试与仿真驱动熔断/健康度） */
    void backend_call(bool fail_hint);

private:
    LruTtlCache    cache_;
    TokenBucket    limiter_;
    CircuitBreaker breaker_;
    HealthMonitor  health_;
    Table          t_;
    ServiceMetrics m_;
    double         now_ = 0.0;

    Response do_query(const Request &req);
    Response do_write(const Request &req);
};

} /* namespace bsk */

#endif /* BSK_H */
