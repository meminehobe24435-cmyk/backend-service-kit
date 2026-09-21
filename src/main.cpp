/* main.cpp — 服务仿真主程序
 *
 * 场景（对应 JD 的三条职责）：
 *   normal    正常流量：写数据 -> 查询命中缓存 -> 指标
 *   degrade   后端故障：失败率上升 -> 熔断打开 -> 读请求返回降级(旧值) / 写请求被拒
 *   recover   故障恢复：熔断进入半开 -> 试探成功 -> 恢复 Closed
 *   ratelimit 突发流量：令牌桶限流 -> 429
 */
#include "bsk.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

using namespace bsk;

static Request mk(Method m, const std::string &path,
                  const std::string &query = "",
                  const std::string &body = "")
{
    Request r;
    r.method = m;
    r.path = path;
    r.query = query;
    r.body = body;
    r.version = "HTTP/1.1";
    r.keep_alive = true;
    return r;
}

static std::string post_body(const std::string &dev, const std::string &metric,
                             int value)
{
    std::ostringstream os;
    os << "{\"device\":\"" << dev << "\",\"metric\":\"" << metric
       << "\",\"value\":\"" << value << "\"}";
    return os.str();
}

static void show(const char *tag, const Response &r)
{
    std::printf("  [%s] %d %s | %s\n", tag, r.status, status_text(r.status),
                r.body.substr(0, 96).c_str());
}

int main(int argc, char **argv)
{
    const std::string scen = (argc > 1) ? argv[1] : "normal";

    BreakerConfig bc;
    bc.window = 10;
    bc.fail_ratio = 0.5;
    bc.open_seconds = 3.0;
    bc.half_open_trials = 2;

    HealthConfig hc;
    hc.window = 10;
    hc.degrade_ratio = 0.3;
    hc.down_ratio = 0.7;

    Service svc(/*cache 容量*/ 128, /*限流*/ 50.0, /*突发*/ 10.0, bc, hc);
    double now = 0.0;
    svc.set_now(now);

    auto step = [&](double dt) { now += dt; svc.set_now(now); };

    std::printf("==== 后台服务仿真：场景=%s ====\n\n", scen.c_str());

    if (scen == "normal") {
        std::puts("1) 写入 3 台设备各 2 条指标");
        for (int i = 0; i < 3; i++) {
            const std::string dev = "AUV-0" + std::to_string(i + 1);
            for (int k = 0; k < 2; k++) {
                Response r = svc.handle(mk(Method::POST, "/metric", "",
                                           post_body(dev, "depth", 10 + k)));
                if (i == 0 && k == 0) show("POST 首次", r);
            }
            step(0.1);
        }
        std::puts("\n2) 查询同一设备两次（第二次应命中缓存）");
        Response r1 = svc.handle(mk(Method::GET, "/metric", "device=AUV-01"));
        show("GET 第1次(未命中)", r1);
        Response r2 = svc.handle(mk(Method::GET, "/metric", "device=AUV-01"));
        show("GET 第2次(命中)", r2);

        std::puts("\n3) 健康与指标");
        show("GET /health", svc.handle(mk(Method::GET, "/health")));
        show("GET /metrics", svc.handle(mk(Method::GET, "/metrics")));

        std::fprintf(stderr, "\n[汇总] 缓存 命中=%llu 未命中=%llu 命中率=%.1f%% 淘汰=%llu\n",
                     (unsigned long long)svc.cache().stat().hits,
                     (unsigned long long)svc.cache().stat().misses,
                     svc.cache().stat().hit_ratio() * 100.0,
                     (unsigned long long)svc.cache().stat().evictions);
        std::fprintf(stderr, "[汇总] 熔断状态=%s 触发次数=%u\n",
                     breaker_state_name(svc.breaker().state()), svc.breaker().trips());
        std::fprintf(stderr, "[汇总] 健康度=%s（失败率 %.1f%%）\n",
                     health_name(svc.health().evaluate()),
                     svc.health().failure_ratio() * 100.0);
        return 0;
    }

    if (scen == "degrade") {
        std::puts("1) 先写入并查询一次，让缓存里有可用旧值");
        svc.handle(mk(Method::POST, "/metric", "", post_body("AUV-01", "depth", 12)));
        show("GET 预热", svc.handle(mk(Method::GET, "/metric", "device=AUV-01")));
        step(6.0);                    /* 让缓存 TTL(5s) 过期，专门验证"无旧值"分支 */
        show("GET TTL 过期后", svc.handle(mk(Method::GET, "/metric", "device=AUV-01")));

        std::puts("\n2) 注入后端故障（连续 10 次失败，把窗口打满）");
        for (int i = 0; i < 10; i++) { svc.backend_call(true); step(0.05); }
        std::fprintf(stderr, "   失败率=%.1f%% 熔断=%s 健康度=%s\n",
                     svc.health().failure_ratio() * 100.0,
                     breaker_state_name(svc.breaker().state()),
                     health_name(svc.health().evaluate()));

        std::puts("\n3) 熔断打开后的读请求");
        std::puts("   注：缓存里若有值会**直接顶住**熔断（这正是缓存的价值），"
                  "所以先清掉缓存才能看到降级路径");
        svc.cache().del("q:AUV-01", now);
        show("GET 降级(无旧值)", svc.handle(mk(Method::GET, "/metric", "device=AUV-01")));

        std::puts("\n3b) 塞一个**未过期**的缓存值，再请求");
        std::puts("   结果会是 200 正常返回 —— 缓存直接顶住了熔断，请求根本没到后端。");
        std::puts("   （真正的 stale 降级发生在"缓存已过期 + 熔断打开"时，由单元测试覆盖）");
        svc.cache().set("q:AUV-01", "{\"device\":\"AUV-01\",\"count\":1,\"sum\":12}",
                        30.0, now);
        show("GET 降级(有旧值)", svc.handle(mk(Method::GET, "/metric", "device=AUV-01")));
        std::puts("\n4) 熔断打开后的写请求（应被拒绝）");
        show("POST 被拒", svc.handle(mk(Method::POST, "/metric", "",
                                       post_body("AUV-02", "depth", 20))));
        show("GET /health", svc.handle(mk(Method::GET, "/health")));
        return 0;
    }

    if (scen == "recover") {
        std::puts("1) 打满失败窗口 -> 熔断打开");
        for (int i = 0; i < 10; i++) { svc.backend_call(true); step(0.05); }
        std::fprintf(stderr, "   熔断=%s\n", breaker_state_name(svc.breaker().state()));

        std::puts("2) 熔断期内请求被拒");
        show("GET 熔断中", svc.handle(mk(Method::GET, "/metric", "device=AUV-01")));

        std::puts("3) 等待 open_seconds 后进入半开，试探成功");
        step(3.5);
        svc.handle(mk(Method::GET, "/metric", "device=AUV-01"));   /* 触发半开 */
        std::fprintf(stderr, "   状态=%s\n", breaker_state_name(svc.breaker().state()));
        for (int i = 0; i < 3; i++) { svc.backend_call(false); step(0.05); }

        std::puts("4) 恢复后的请求");
        show("GET 恢复后", svc.handle(mk(Method::GET, "/metric", "device=AUV-01")));
        show("GET /health", svc.handle(mk(Method::GET, "/health")));
        std::fprintf(stderr, "\n[汇总] 熔断触发=%u 半开试探后状态=%s\n",
                     svc.breaker().trips(), breaker_state_name(svc.breaker().state()));
        return 0;
    }

    if (scen == "ratelimit") {
        std::puts("1) 令牌桶 rate=50/s burst=10：连续发 25 个请求");
        int ok = 0, limited = 0;
        for (int i = 0; i < 25; i++) {
            Response r = svc.handle(mk(Method::GET, "/health"));
            if (r.status == 429) limited++; else ok++;
            if (i < 3 || r.status == 429) show(i == 0 ? "第1个" : "限流", r);
        }
        std::fprintf(stderr, "\n[汇总] 放行=%d 限流=%d（令牌桶 burst=10）\n", ok, limited);

        std::puts("\n2) 等待 0.5 s 后补充令牌，再发 20 个");
        step(0.5);
        ok = limited = 0;
        for (int i = 0; i < 20; i++) {
            Response r = svc.handle(mk(Method::GET, "/health"));
            if (r.status == 429) limited++; else ok++;
        }
        std::fprintf(stderr, "[汇总] 0.5s 后 放行=%d 限流=%d"
                             "（0.5s×50/s=25 个令牌，但桶容量 burst=10 截顶，故放行 10 个）\n",
                     ok, limited);
        return 0;
    }

    std::fprintf(stderr, "未知场景: %s（可选 normal/degrade/recover/ratelimit）\n",
                 scen.c_str());
    return 2;
}
