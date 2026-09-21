/* service.cpp — 业务服务层：把 HTTP / 缓存 / 存储 / 容灾降级串起来
 *
 * 这是"后台服务"的主干：请求进来 → 限流 → 缓存命中？→ 走业务 → 熔断保护 → 降级兜底 → 记录指标
 *
 * 降级与应急策略（对应 JD 第 3 条）：
 *   · 限流超限      -> 429（保护后端）
 *   · 熔断打开      -> 503 + 降级响应（返回缓存旧值或静态兜底）
 *   · 健康度 Down   -> 只读降级：拒绝写操作，查询走缓存
 *   · 异常          -> 500 并计入熔断统计
 */
#include "bsk.h"
#include <sstream>

namespace bsk {



/* Service 的构造与后端调用模拟（其余成员在头文件声明、此处定义） */
Service::Service(size_t cache_cap, double rate_per_s, double burst,
                 BreakerConfig bcfg, HealthConfig hcfg)
    : cache_(cache_cap), limiter_(rate_per_s, burst),
      breaker_(bcfg), health_(hcfg),
      t_("metrics", {"id", "device", "metric", "value", "ts"}) {}

void Service::backend_call(bool fail_hint)
{
    if (fail_hint) {
        health_.record(false);
        breaker_.on_failure(now_);
    } else {
        health_.record(true);
        breaker_.on_success(now_);
    }
}

static std::string json_escape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else o.push_back(c);
    }
    return o;
}

/* 从 query 串里取参数：a=1&b=2 */
static std::string query_param(const std::string &query, const std::string &key)
{
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos
                                                   ? std::string::npos : amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

Response Service::handle(const Request &req)
{
    m_.requests++;

    /* 1) 限流 */
    if (!limiter_.try_acquire(now_)) {
        m_.rate_limited++;
        Response r = make_json(429, "{\"error\":\"rate limited\"}");
        r.keep_alive = req.keep_alive;
        return r;
    }

    /* 2) 健康度降级：Down 时拒绝写 */
    const Health h = health_.evaluate();
    const bool is_write = (req.method == Method::POST || req.method == Method::PUT);
    if (h == Health::Down && is_write) {
        m_.degraded++;
        Response r = make_json(503, "{\"error\":\"service down, writes rejected\","
                                    "\"degraded\":true}");
        r.keep_alive = req.keep_alive;
        return r;
    }

    /* 3) 路由 */
    Response resp;
    if (req.path == "/health") {
        std::ostringstream os;
        os << "{\"status\":\"" << health_name(h) << "\","
           << "\"breaker\":\"" << breaker_state_name(breaker_.state()) << "\","
           << "\"failure_ratio\":" << health_.failure_ratio() << ","
           << "\"cache_hit_ratio\":" << cache_.stat().hit_ratio() << "}";
        resp = make_json(200, os.str());
    } else if (req.path == "/metrics") {
        std::ostringstream os;
        os << "{\"requests\":" << m_.requests
           << ",\"ok\":" << m_.ok
           << ",\"not_found\":" << m_.not_found
           << ",\"bad_request\":" << m_.bad_request
           << ",\"rate_limited\":" << m_.rate_limited
           << ",\"circuit_rejected\":" << m_.circuit_rejected
           << ",\"degraded\":" << m_.degraded
           << ",\"errors\":" << m_.errors
           << ",\"avg_latency_ms\":" << m_.avg_latency_ms() << "}";
        resp = make_json(200, os.str());
    } else if (req.path == "/metric" && req.method == Method::GET) {
        resp = do_query(req);
    } else if (req.path == "/metric" && req.method == Method::POST) {
        resp = do_write(req);
    } else {
        m_.bad_request++;
        resp = make_json(404, "{\"error\":\"not found\"}");
    }

    resp.keep_alive = req.keep_alive;
    return resp;
}

Response Service::do_query(const Request &req)
{
    const std::string device = query_param(req.query, "device");

    /* 先取缓存（含过期旧值，供后面降级用） */
    std::string cached, stale_val;
    bool has_cached = false, stale_expired = false;
    if (!device.empty()) {
        has_cached = cache_.get("q:" + device, cached, now_);
        if (has_cached) {                      /* 未过期：正常命中 */
            m_.ok++;
            return make_json(200, cached);
        }
        /* 未命中或已过期：把旧值取出来备用（这才能支撑降级） */
        cache_.get_stale("q:" + device, stale_val, &stale_expired, now_);
    }

    /* 熔断打开 -> 降级：有旧值就返回 stale，否则 503 */
    if (!breaker_.allow(now_)) {
        m_.circuit_rejected++;
        m_.degraded++;
        if (!stale_val.empty()) {
            return make_json(200, std::string("{\"degraded\":true,\"stale\":true,")
                                      + "\"data\":" + stale_val + "}");
        }
        return make_json(503, "{\"error\":\"circuit open\",\"degraded\":true}");
    }

    /* 后端查询（这里用本地表模拟） */
    const size_t n = t_.count_where("device", device);
    std::ostringstream os;
    os << "{\"device\":\"" << json_escape(device) << "\",\"count\":" << n
       << ",\"sum\":" << t_.sum("value") << "}";
    const std::string body = os.str();

    m_.ok++;
    if (!device.empty()) cache_.set("q:" + device, body, 5.0, now_);   /* TTL 5s */
    return make_json(200, body);
}

Response Service::do_write(const Request &req)
{
    /* 熔断打开 -> 写操作直接降级拒绝（避免打挂后端） */
    if (!breaker_.allow(now_)) {
        m_.circuit_rejected++;
        m_.degraded++;
        return make_json(503, "{\"error\":\"circuit open, write rejected\","
                              "\"degraded\":true}");
    }

    /* 极简的 JSON 字段提取（避免引入 JSON 库） */
    auto field = [&req](const char *key) -> std::string {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = req.body.find(k);
        if (p == std::string::npos) return "";
        p = req.body.find(':', p + k.size());
        if (p == std::string::npos) return "";
        p++;
        while (p < req.body.size() && (req.body[p] == ' ' || req.body[p] == '"')) p++;
        size_t e = p;
        while (e < req.body.size() && req.body[e] != '"' && req.body[e] != ',' &&
               req.body[e] != '}') e++;
        return req.body.substr(p, e - p);
    };

    const std::string device = field("device");
    const std::string metric = field("metric");
    const std::string value = field("value");
    if (device.empty() || metric.empty() || value.empty()) {
        m_.bad_request++;
        return make_json(400, "{\"error\":\"device/metric/value required\"}");
    }

    if (!t_.insert({{"id", std::to_string(t_.count())},
                    {"device", device}, {"metric", metric},
                    {"value", value}, {"ts", std::to_string(static_cast<long long>(now_))}})) {
        m_.errors++;
        return make_json(500, "{\"error\":\"insert failed\"}");
    }
    /* 写后失效缓存 */
    cache_.del("q:" + device, now_);
    m_.ok++;
    return make_json(201, "{\"ok\":true}");
}

} /* namespace bsk */
