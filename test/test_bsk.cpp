/* test_bsk.cpp — 单元测试（纯 C++17，无第三方框架）
 *
 *  T1 http        请求行/头/体解析、keep-alive、分块到达、坏请求、响应序列化
 *  T2 cache       LRU 淘汰、TTL 过期、命中率统计、EXPIRE/TTL/DEL
 *  T3 resilience  熔断三态与触发条件、令牌桶限流、健康度分级
 *  T4 store       建表/参数化插入（未知列拒绝）/等值查询/聚合/落盘
 *  T5 service     端到端：限流 -> 降级 -> 缓存 -> 熔断 -> 业务 -> 指标
 */
#include "bsk.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace bsk;

static int passed = 0, failed = 0;

#define CHECK(cond, ...) do {                        \
    if (cond) { passed++; std::printf("  [PASS] "); } \
    else      { failed++; std::printf("  [FAIL] "); } \
    std::printf(__VA_ARGS__);                         \
    std::printf("\n");                                \
} while (0)

#define NEAR(a, b, tol) (std::fabs((a) - (b)) <= (tol))

/* ------------------------------------------------------------------ T1 http */
static void t1_http()
{
    std::printf("T1 http（协议解析与生成）\n");

    const std::string raw =
        "POST /metric?device=AUV-01 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"value\":123}";

    Request r;
    size_t used = 0;
    const int rc = http_parse_request(raw, r, &used);
    CHECK(rc == 0, "解析成功（rc=%d）", rc);
    CHECK(r.method == Method::POST, "方法 = %s", method_name(r.method));
    CHECK(r.path == "/metric", "路径 = %s", r.path.c_str());
    CHECK(r.query == "device=AUV-01", "查询串 = %s", r.query.c_str());
    CHECK(r.version == "HTTP/1.1", "版本 = %s", r.version.c_str());
    CHECK(r.body == "{\"value\":123}", "体 = %s（长度 %zu）", r.body.c_str(), r.body.size());
    CHECK(used == raw.size(), "消耗字节 = %zu / %zu", used, raw.size());

    /* 头大小写不敏感 */
    CHECK(r.header("content-length") == "13", "小写取头 Content-Length = %s",
          r.header("content-length").c_str());
    CHECK(r.header("CONTENT-TYPE") == "application/json", "大写取头成功");

    /* keep-alive 默认（HTTP/1.1） */
    CHECK(r.keep_alive, "HTTP/1.1 默认 keep-alive");

    Request r2;
    http_parse_request("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", r2, nullptr);
    CHECK(!r2.keep_alive, "Connection: close -> 短连接");

    Request r3;
    http_parse_request("GET / HTTP/1.0\r\n\r\n", r3, nullptr);
    CHECK(!r3.keep_alive, "HTTP/1.0 默认短连接");

    /* 分块到达：头不完整 */
    std::string partial = raw.substr(0, 20);
    Request r4;
    CHECK(http_parse_request(partial, r4, nullptr) == 1, "头不完整 -> 返回 1（继续收）");

    /* 分块到达：体不完整 */
    partial = raw.substr(0, raw.size() - 4);
    Request r5;
    CHECK(http_parse_request(partial, r5, nullptr) == 1, "体不完整 -> 返回 1（继续收）");

    /* 坏请求 */
    Request r6;
    CHECK(http_parse_request("BADLINE\r\n\r\n", r6, nullptr) == -1, "非法请求行 -> -1");
    Request r7;
    CHECK(http_parse_request("GET / HTTP/1.1\r\nNoColonHere\r\n\r\n", r7, nullptr) == -1,
          "坏头（无冒号）-> -1");
    Request r8;
    CHECK(http_parse_request("GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\nXYZ", r8,
                             nullptr) == -1, "Content-Length 非数字 -> -1");

    /* 无体的 GET */
    Request r9;
    size_t u9 = 0;
    CHECK(http_parse_request("GET /health HTTP/1.1\r\n\r\n", r9, &u9) == 0,
          "无体 GET 解析成功（消耗 %zu）", u9);

    /* 响应序列化 */
    Response resp = make_json(200, "{\"ok\":true}");
    const std::string wire = resp.serialize();
    CHECK(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "状态行正确");
    CHECK(wire.find("Content-Type: application/json") != std::string::npos,
          "自动补 Content-Type");
    CHECK(wire.find("Content-Length: 11") != std::string::npos, "自动补 Content-Length");
    CHECK(wire.find("Connection: keep-alive") != std::string::npos, "自动补 Connection");
    CHECK(wire.size() > 11 && wire.substr(wire.size() - 11) == "{\"ok\":true}",
          "体在报文末尾");

    Response r503 = make_text(503, "down");
    r503.keep_alive = false;
    CHECK(r503.serialize().find("Connection: close") != std::string::npos,
          "短连接响应带 close");
    CHECK(std::strcmp(status_text(429), "Too Many Requests") == 0, "429 状态文本");
    CHECK(std::strcmp(status_text(503), "Service Unavailable") == 0, "503 状态文本");
}

/* ----------------------------------------------------------------- T2 cache */
static void t2_cache()
{
    std::printf("T2 cache（LRU + TTL）\n");

    LruTtlCache c(3);
    c.set("a", "1", 0.0, 0.0);
    c.set("b", "2", 0.0, 0.0);
    c.set("c", "3", 0.0, 0.0);
    CHECK(c.size() == 3, "容量 3 装满");

    std::string v;
    CHECK(c.get("a", v, 0.0) && v == "1", "命中 a = %s", v.c_str());
    CHECK(c.get("b", v, 0.0) && v == "2", "命中 b = %s", v.c_str());
    /* 访问过 a、b，最久未使用的是 c -> 插入 d 应淘汰 c */
    c.set("d", "4", 0.0, 0.0);
    CHECK(c.size() == 3, "淘汰后仍为 3");
    CHECK(!c.get("c", v, 0.0), "c 已被 LRU 淘汰");
    CHECK(c.get("d", v, 0.0) && v == "4", "d 存在");
    CHECK(c.stat().evictions == 1, "淘汰计数 = %llu",
          (unsigned long long)c.stat().evictions);

    /* TTL 过期 */
    LruTtlCache t(8);
    t.set("k", "v", 5.0, 100.0);
    CHECK(t.get("k", v, 104.0) && v == "v", "4s 时未过期");
    CHECK(!t.get("k", v, 106.0), "6s 时已过期");
    CHECK(t.stat().expired >= 1, "过期计数 = %llu",
          (unsigned long long)t.stat().expired);

    /* TTL 查询语义：-2 不存在 / -1 无过期 / 剩余秒数 */
    CHECK(NEAR(t.ttl("nope", 0.0), -2.0, 1e-9), "不存在 -> -2");
    t.set("forever", "x", 0.0, 0.0);
    CHECK(NEAR(t.ttl("forever", 10.0), -1.0, 1e-9), "无过期 -> -1");
    t.set("ttl3", "y", 3.0, 10.0);
    CHECK(NEAR(t.ttl("ttl3", 11.0), 2.0, 1e-9), "剩余 TTL = %.2f", t.ttl("ttl3", 11.0));

    /* EXPIRE 修改过期时间 */
    t.expire("forever", 2.0, 100.0);
    CHECK(NEAR(t.ttl("forever", 100.5), 1.5, 1e-9), "EXPIRE 生效，剩余 %.2f",
          t.ttl("forever", 100.5));
    CHECK(t.expire("nope", 1.0, 0.0) == false, "对不存在的 key EXPIRE -> false");

    /* DEL */
    t.set("del1", "z", 0.0, 0.0);
    CHECK(t.del("del1", 0.0), "DEL 成功");
    CHECK(!t.get("del1", v, 0.0), "DEL 后取不到");
    CHECK(!t.del("del1", 0.0), "重复 DEL -> false");

    /* 命中率 */
    LruTtlCache h(4);
    h.set("x", "1", 0.0, 0.0);
    h.get("x", v, 0.0);
    h.get("x", v, 0.0);
    h.get("miss", v, 0.0);
    CHECK(NEAR(h.stat().hit_ratio(), 2.0 / 3.0, 1e-9), "命中率 = %.3f",
          h.stat().hit_ratio());

    /* clear_expired */
    LruTtlCache e(8);
    e.set("s1", "a", 1.0, 0.0);
    e.set("s2", "b", 100.0, 0.0);
    e.clear_expired(5.0);
    CHECK(e.size() == 1, "清理过期后剩 %zu 个", e.size());
    CHECK(e.get("s2", v, 5.0) && v == "b", "未过期的保留");
}

/* ------------------------------------------------------------ T3 resilience */
static void t3_resilience()
{
    std::printf("T3 resilience（熔断 / 限流 / 健康度）\n");

    /* 熔断：窗口 5，失败率 0.5 */
    BreakerConfig cfg;
    cfg.window = 5;
    cfg.fail_ratio = 0.5;
    cfg.open_seconds = 2.0;
    cfg.half_open_trials = 1;
    CircuitBreaker b(cfg);

    for (int i = 0; i < 5; i++) { CHECK(b.allow(0.0), "窗口未满时放行 (i=%d)", i); b.on_failure(0.1 * i); }
    CHECK(b.state() == BreakerState::Open, "失败率 100%% -> OPEN");
    CHECK(b.trips() == 1, "触发次数 = %u", b.trips());
    CHECK(!b.allow(1.0), "熔断期内拒绝");
    CHECK(b.rejected() == 1, "拒绝计数 = %u", b.rejected());

    /* 到期进入半开 */
    CHECK(b.allow(5.0), "超过 open_seconds -> 半开放行试探");
    CHECK(b.state() == BreakerState::HalfOpen, "状态 = %s",
          breaker_state_name(b.state()));
    CHECK(!b.allow(5.1), "半开试探次数用尽 -> 拒绝");

    /* 试探失败 -> 重新打开 */
    CircuitBreaker b2(cfg);
    for (int i = 0; i < 5; i++) b2.on_failure(0.0);
    b2.allow(10.0);
    b2.on_failure(10.0);
    CHECK(b2.state() == BreakerState::Open, "半开试探失败 -> 重新 OPEN");
    CHECK(b2.trips() == 2, "触发次数 = %u", b2.trips());

    /* 试探成功 -> 恢复 */
    CircuitBreaker b3(cfg);
    for (int i = 0; i < 5; i++) b3.on_failure(0.0);
    b3.allow(10.0);
    b3.on_success(10.0);
    CHECK(b3.state() == BreakerState::Closed, "半开试探成功 -> CLOSED");

    /* 失败率未达阈值不熔断 */
    CircuitBreaker b4(cfg);
    b4.on_success(0.0); b4.on_success(0.0); b4.on_success(0.0);
    b4.on_failure(0.0); b4.on_failure(0.0);      /* 40% < 50% */
    CHECK(b4.state() == BreakerState::Closed, "失败率 40%% 不熔断");

    /* 令牌桶 */
    TokenBucket tb(10.0, 3.0);
    int ok = 0;
    for (int i = 0; i < 5; i++) if (tb.try_acquire(0.0)) ok++;
    CHECK(ok == 3, "burst=3 -> 首次放行 %d 个", ok);
    CHECK(tb.rejected() == 2, "拒绝 = %u", tb.rejected());
    /* 时间必须单调递增：dt<0 不会补令牌 */
    ok = 0;
    for (int i = 0; i < 5; i++) if (tb.try_acquire(0.5)) ok++;
    CHECK(ok == 3, "0.5s 补 5 个令牌但被 burst=3 截顶 -> 放行 %d 个", ok);
    ok = 0;
    for (int i = 0; i < 5; i++) if (tb.try_acquire(0.6)) ok++;
    CHECK(ok == 1, "再过 0.1s 补 1 个令牌 -> 放行 %d 个", ok);

    /* 健康度 */
    HealthConfig hc;
    hc.window = 10;
    hc.degrade_ratio = 0.3;
    hc.down_ratio = 0.7;
    HealthMonitor hm(hc);
    for (int i = 0; i < 5; i++) hm.record(true);
    CHECK(hm.evaluate() == Health::Up, "样本不足 -> UP");
    for (int i = 0; i < 5; i++) hm.record(true);
    CHECK(hm.evaluate() == Health::Up, "全成功 -> UP");

    HealthMonitor hm2(hc);
    for (int i = 0; i < 10; i++) hm2.record(i >= 4);      /* 4 次失败 = 40% */
    CHECK(hm2.evaluate() == Health::Degraded, "失败率 40%% -> DEGRADED（%.1f%%）",
          hm2.failure_ratio() * 100);
    HealthMonitor hm3(hc);
    for (int i = 0; i < 10; i++) hm3.record(i >= 8);      /* 8 次失败 = 80% */
    CHECK(hm3.evaluate() == Health::Down, "失败率 80%% -> DOWN");
    CHECK(std::strcmp(health_name(hm3.evaluate()), "DOWN") == 0, "health_name 正确");
}

/* ----------------------------------------------------------------- T4 store */
static void t4_store()
{
    std::printf("T4 store（数据存储）\n");

    Table t("metrics", {"id", "device", "metric", "value", "ts"});
    CHECK(t.name() == "metrics", "表名 = %s", t.name().c_str());
    CHECK(t.columns().size() == 5, "列数 = %zu", t.columns().size());

    CHECK(t.insert({{"id", "0"}, {"device", "A"}, {"metric", "depth"},
                    {"value", "10"}, {"ts", "1"}}), "插入 1 成功");
    CHECK(t.insert({{"id", "1"}, {"device", "A"}, {"metric", "depth"},
                    {"value", "20"}, {"ts", "2"}}), "插入 2 成功");
    CHECK(t.insert({{"id", "2"}, {"device", "B"}, {"metric", "temp"},
                    {"value", "30"}, {"ts", "3"}}), "插入 3 成功");
    CHECK(t.count() == 3, "总行数 = %zu", t.count());

    /* 参数化语义：未知列应被拒绝（防注入/防拼错） */
    CHECK(!t.insert({{"id", "9"}, {"evil", "x"}}), "未知列 -> 插入被拒绝");
    CHECK(t.count() == 3, "被拒后行数不变 = %zu", t.count());

    /* 等值查询 */
    auto rows = t.select({{"device", "A"}});
    CHECK(rows.size() == 2, "device=A 查到 %zu 行", rows.size());
    CHECK(rows[0].get("value") == "10", "首行 value = %s", rows[0].get("value").c_str());
    CHECK(rows[0].get("nope").empty(), "取不存在的列 -> 空串");

    auto lim = t.select({{"device", "A"}}, 1);
    CHECK(lim.size() == 1, "limit 生效 = %zu", lim.size());

    auto none = t.select({{"device", "ZZZ"}});
    CHECK(none.empty(), "不存在的设备 -> 空结果");

    /* 聚合 */
    CHECK(t.count_where("device", "A") == 2, "count_where = %zu", t.count_where("device", "A"));
    CHECK(t.sum("value") == 60, "sum(value) = %lld", t.sum("value"));
    CHECK(t.count_where("device", "NOPE") == 0, "count_where 空 = 0");

    /* 落盘 */
    CHECK(t.flush("test_table.csv"), "flush 成功");
    std::remove("test_table.csv");
}

/* --------------------------------------------------------------- T5 service */
static void t5_service()
{
    std::printf("T5 service（端到端：限流/降级/缓存/熔断/指标）\n");

    BreakerConfig bc;
    bc.window = 5;
    bc.fail_ratio = 0.5;
    bc.open_seconds = 2.0;
    HealthConfig hc;
    hc.window = 5;
    hc.degrade_ratio = 0.3;
    hc.down_ratio = 0.6;

    Service svc(4, 100.0, 100.0, bc, hc);      /* 容量小、限流宽松，专注业务逻辑 */
    svc.set_now(0.0);

    auto req = [](Method m, const std::string &path, const std::string &q = "",
                  const std::string &b = "") {
        Request r;
        r.method = m; r.path = path; r.query = q; r.body = b;
        r.version = "HTTP/1.1"; r.keep_alive = true;
        return r;
    };

    /* 写 */
    Response w = svc.handle(req(Method::POST, "/metric", "",
                                "{\"device\":\"A\",\"metric\":\"depth\",\"value\":\"10\"}"));
    CHECK(w.status == 201, "POST 新建 -> %d", w.status);
    CHECK(svc.table().count() == 1, "入库 1 行");

    /* 缺字段 -> 400 */
    Response bad = svc.handle(req(Method::POST, "/metric", "", "{\"device\":\"A\"}"));
    CHECK(bad.status == 400, "缺字段 -> %d", bad.status);

    /* 查询未命中 -> 200 并缓存 */
    Response q1 = svc.handle(req(Method::GET, "/metric", "device=A"));
    CHECK(q1.status == 200, "GET 未命中 -> %d", q1.status);
    CHECK(q1.body.find("\"count\":1") != std::string::npos, "返回 count=1");
    CHECK(svc.cache().size() == 1, "查询后写入缓存");

    /* 再查命中 */
    Response q2 = svc.handle(req(Method::GET, "/metric", "device=A"));
    CHECK(q2.status == 200 && q2.body == q1.body, "缓存命中返回一致");
    CHECK(svc.cache().stat().hits == 1, "命中计数 = %llu",
          (unsigned long long)svc.cache().stat().hits);

    /* 写后失效缓存 */
    svc.handle(req(Method::POST, "/metric", "",
                   "{\"device\":\"A\",\"metric\":\"depth\",\"value\":\"20\"}"));
    CHECK(svc.cache().size() == 0, "写后缓存失效");

    /* 未知路由 -> 404 */
    CHECK(svc.handle(req(Method::GET, "/nope")).status == 404, "未知路径 -> 404");

    /* /health 与 /metrics */
    Response h = svc.handle(req(Method::GET, "/health"));
    CHECK(h.status == 200 && h.body.find("CLOSED") != std::string::npos,
          "health 返回熔断状态");
    Response mt = svc.handle(req(Method::GET, "/metrics"));
    CHECK(mt.body.find("\"requests\"") != std::string::npos, "metrics 返回请求数");

    /* ---- 熔断 + 降级 ---- */
    Service s2(4, 100.0, 100.0, bc, hc);
    s2.set_now(0.0);
    s2.handle(req(Method::POST, "/metric", "",
                  "{\"device\":\"B\",\"metric\":\"d\",\"value\":\"5\"}"));
    for (int i = 0; i < 5; i++) s2.backend_call(true);      /* 打满失败窗口 */
    CHECK(s2.breaker().state() == BreakerState::Open, "熔断打开");

    /* 无旧值 -> 503 降级 */
    Response d1 = s2.handle(req(Method::GET, "/metric", "device=B"));
    CHECK(d1.status == 503, "熔断+无旧值 -> %d", d1.status);
    CHECK(d1.body.find("\"degraded\":true") != std::string::npos, "返回降级标记");

    /* 未过期的旧值 -> 正常命中（缓存顶住熔断，不是 stale） */
    s2.cache().set("q:B", "{\"count\":1}", 60.0, 0.0);
    Response d2 = s2.handle(req(Method::GET, "/metric", "device=B"));
    CHECK(d2.status == 200, "熔断+未过期缓存 -> %d", d2.status);
    CHECK(d2.body.find("\"stale\"") == std::string::npos, "未过期不应标记 stale");

    /* 已过期的旧值 -> 200 + stale（这才是降级路径） */
    s2.cache().set("q:B", "{\"count\":1}", 1.0, 0.0);
    /* 时间要卡在"缓存已过期、熔断仍 OPEN"的窗口内：
     * TTL=1s 于 now=1.0 过期；熔断 open_seconds=2.0 于 now=2.0 到期转 HALF_OPEN。
     * 取 now=1.5 —— 若取 2.0，熔断已恢复，请求会走正常路径而不是降级。 */
    s2.set_now(1.5);
    Response d2b = s2.handle(req(Method::GET, "/metric", "device=B"));
    CHECK(d2b.status == 200, "熔断+已过期旧值 -> %d", d2b.status);
    CHECK(d2b.body.find("\"stale\":true") != std::string::npos,
          "过期旧值降级返回 stale 标记");

    /* 写操作被降级拒绝 */
    Response d3 = s2.handle(req(Method::POST, "/metric", "",
                                "{\"device\":\"B\",\"metric\":\"d\",\"value\":\"9\"}"));
    CHECK(d3.status == 503, "健康度 DOWN 时写被拒 -> %d", d3.status);
    CHECK(s2.metrics().degraded >= 3, "降级计数 = %llu",
          (unsigned long long)s2.metrics().degraded);

    /* 恢复：必须走正确状态机 —— 等 open_seconds -> allow() 进 HALF_OPEN -> on_success 才回 CLOSED */
    s2.set_now(10.0);                       /* 超过 open_seconds=2.0 */
    CHECK(s2.breaker().allow(10.0), "熔断到期后放行试探");
    CHECK(s2.breaker().state() == BreakerState::HalfOpen, "进入 HALF_OPEN");
    s2.backend_call(false);                 /* 试探成功 */
    CHECK(s2.breaker().state() == BreakerState::Closed, "试探成功后恢复 CLOSED");

    /* ---- 限流 ---- */
    Service s3(8, 10.0, 3.0, bc, hc);
    s3.set_now(0.0);
    int ok = 0, limited = 0;
    for (int i = 0; i < 6; i++) {
        Response r = s3.handle(req(Method::GET, "/health"));
        if (r.status == 429) limited++; else ok++;
    }
    CHECK(ok == 3, "burst=3 -> 放行 %d 个", ok);
    CHECK(limited == 3, "限流 %d 个", limited);
    CHECK(s3.metrics().rate_limited == 3, "限流计数 = %llu",
          (unsigned long long)s3.metrics().rate_limited);
}

int main()
{
    std::printf("==== bsk 单元测试 ====\n\n");
    t1_http();
    t2_cache();
    t3_resilience();
    t4_store();
    t5_service();
    std::printf("\n==== 结果：%d passed, %d failed ====\n", passed, failed);
    return failed ? 1 : 0;
}
