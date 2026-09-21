# bsk — 后台服务基础组件库（Backend Service Kit）

> 把**后台服务与业务系统**里最容易出问题的四件事做成可复用、可测试的组件：
> **HTTP 协议解析** · **LRU+TTL 缓存** · **★ 容灾/降级/应急（熔断·限流·健康度）** · **数据存储**。
>
> 纯 **C++17**，零第三方依赖。

[![ci](https://github.com/meminehobe24435-cmyk/backend-service-kit/actions/workflows/ci.yml/badge.svg)](https://github.com/meminehobe24435-cmyk/backend-service-kit/actions/workflows/ci.yml)
![cpp](https://img.shields.io/badge/C%2B%2B-17-blue)
![deps](https://img.shields.io/badge/dependencies-none-green)
![tests](https://img.shields.io/badge/tests-116%20passed-brightgreen)

---

## 1. 为什么是这四块

后台服务的日常绕不开这些事，而它们恰好也是最容易"看起来能跑、出事就崩"的地方：

| 组件 | 解决什么 | 关键点 |
|---|---|---|
| **http** | 请求解析与响应生成 | 请求行/头/体逐层解析、**分块到达**（半包）返回"继续收"、keep-alive、自动补 `Content-Length`/`Content-Type`/`Connection` |
| **cache** | 热点数据免打后端 | **LRU 淘汰** + **TTL 过期** + 命中率统计；★ 过期值**保留不删**，专门用于降级返回旧数据 |
| **resilience** | ★ **容灾 / 降级 / 应急** | **熔断器**三态（CLOSED/OPEN/HALF_OPEN）、**令牌桶限流**、**健康度分级**（UP/DEGRADED/DOWN） |
| **store** | 业务数据落盘 | 列校验（未知列拒绝，防拼错/防注入）、等值查询、聚合、journal 导出 |

请求链路：

```
请求 ──► 限流(令牌桶) ──► 健康度降级 ──► 缓存 ──► 熔断器 ──► 业务 ──► 指标
         429 保护后端      DOWN 拒写     命中即返回   OPEN 则降级
```

---

## 2. 快速开始

```bash
make test      # 编译并跑 116 项单元测试
make sim       # 跑 4 个仿真场景
# 或直接：
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
    src/http.cpp src/cache.cpp src/service.cpp test/test_bsk.cpp -o build/bsk_test && ./build/bsk_test
./build/bsk_sim normal | degrade | recover | ratelimit
```

---

## 3. 目录结构

```
include/bsk.h        公共接口（HTTP / 缓存 / 容灾 / 存储 / 服务）
src/http.cpp         HTTP/1.1 解析与响应生成
src/cache.cpp        LRU+TTL 缓存 + 熔断器 + 令牌桶 + 健康度 + 存储
src/service.cpp      业务服务层：把上述组件串成一条请求链路
src/main.cpp         四个仿真场景
test/test_bsk.cpp    116 项单元测试
```

---

## 4. 容灾与降级策略（核心）

| 触发条件 | 动作 | 返回 |
|---|---|---|
| 令牌不足 | 限流，保护后端 | **429** + `{"error":"rate limited"}` |
| 失败率 ≥ `degrade_ratio` | 健康度降为 DEGRADED（只观测，不影响请求） | — |
| 失败率 ≥ `down_ratio` | 健康度 DOWN：**拒绝写操作**（只读降级） | **503** + `degraded:true` |
| 熔断器 OPEN（窗口内失败率 ≥ 阈值） | 拒绝调用，**返回缓存旧值**兜底 | **200** + `stale:true`（有旧值）／**503**（无旧值） |
| 熔断 OPEN 超过 `open_seconds` | 转 HALF_OPEN，放行少量试探 | — |
| 试探成功 / 失败 | 恢复 CLOSED / 重新 OPEN 并重新计时 | — |

**两个容易被做错、本项目专门测过的点**：

1. **过期值不能删** —— 若缓存一过期就把值删掉，"熔断时返回旧值"这条**最关键的降级路径永远走不到**。
   本项目的 `get()` 过期时只计数不删除，另有 `get_stale()` 专门给降级用。
2. **浮点令牌数要带容差** —— `0.6s − 0.5s = 0.09999999999999998`，乘速率得
   `0.9999999999999998`，直接与 `1.0` 比较会"明明够却判不够"。比较必须加 `1e-9` 容差。

---

## 5. 实测结果

### 单元测试

```
==== 结果：116 passed, 0 failed ====
```

覆盖：请求行/头/体解析、**头不完整与体不完整的分块到达**、坏请求三种形态、
响应自动补头、**LRU 淘汰顺序**、TTL 过期与 `EXPIRE`/`TTL`/`DEL` 语义、命中率、
**熔断三态与触发/恢复条件**、失败率未达阈值不熔断、**令牌桶 burst 截顶**、
健康度三级、存储列校验与聚合、**端到端请求链路的 9 种响应形态**。

### 四个仿真场景（真实输出节选）

**normal** —— 缓存命中率 50%、熔断 CLOSED、健康度 UP：

```
[POST 首次] 201 Created | {"ok":true}
[GET 第1次(未命中)] 200 OK | {"device":"AUV-01","count":2,"sum":63}
[GET 第2次(命中)]   200 OK | {"device":"AUV-01","count":2,"sum":63}
[GET /health] 200 OK | {"status":"UP","breaker":"CLOSED","cache_hit_ratio":0.5}
```

**degrade** —— 后端故障 → 熔断打开 → 写被拒：

```
失败率=100.0% 熔断=OPEN 健康度=DOWN
[GET 降级(无旧值)] 503 Service Unavailable | {"error":"circuit open","degraded":true}
[POST 被拒]        503 Service Unavailable | {"error":"service down, writes rejected"}
[GET /health]      200 OK | {"status":"DOWN","breaker":"OPEN","failure_ratio":1}
```

**recover** —— 熔断 → 半开 → 试探成功 → 恢复：

```
[GET 熔断中] 503 Service Unavailable | {"error":"circuit open","degraded":true}
（等待 open_seconds 后进入 HALF_OPEN，试探成功）
[GET 恢复后] 200 OK
[汇总] 熔断触发=1 半开试探后状态=CLOSED
```

**ratelimit** —— 令牌桶精确限流：

```
[汇总] 放行=10 限流=15（burst=10）
[汇总] 0.5s 后 放行=10 限流=10（0.5s×50/s=25 个令牌，但桶容量 burst=10 截顶）
```

---

## 6. 开发过程中被测试抓出来的问题

| # | 问题 | 定位方式 | 修复 |
|---|---|---|---|
| 1 | **stale 降级路径不可达（真设计缺陷）**：`get()` 发现过期就 `erase`，值被删掉，于是"熔断时返回旧值"的分支**永远进不去** | 单测 3 项失败（stale 标记 / 降级计数 / 缓存命中路径） | `get()` 过期时**只计数不删除**；新增 `get_stale()` 专供降级；`do_query` 先取旧值备用 |
| 2 | **令牌桶浮点精度**：`0.6−0.5` 得 `0.09999999999999998`，×10 得 `0.9999999999999998 < 1.0`，明明补够 1 个令牌却判不足 | 单测令牌桶用例失败 | 比较加 `1e-9` 容差，扣减后夹到 0 |
| 3 | **Service/ServiceMetrics 重复定义**（先写进 .cpp，后移到 .h） | 编译报 `redefinition` | 删除 .cpp 内定义；并把构造与 `backend_call` 的定义补回（否则链接 `undefined reference`） |
| 4 | **Service 构造函数与 `backend_call` 漏定义** | 链接 `undefined reference` | 在 `service.cpp` 补定义 |

> 另有 3 处是**测试自身写错**（时间倒流、缓存未过期却断言 stale、熔断到期后还想走降级），
> 都逐一核对后修正了测试而不是去改正确的实现。

---

## 7. 边界说明（重要）

- 本项目是**仿真与自建实践**，**未部署为线上服务**，不构成线上运维经验
- **未实现真实网络层**：`epoll`/非阻塞/线程池在头文件里预留了 `tick/poll` 式接口，
  但当前 `main` 是**单线程事件循环 + 直接调用 `handle()`**，不含并发连接管理
- **存储是内存表 + journal 落盘**：列校验/查询/聚合接口按 SQLite 语义设计，但**未接真实数据库**
- **未实现**：TLS、HTTP/2、chunked 请求体、持久化缓存、分布式限流（当前限流是单机的）
- **无分布式/微服务**经验：本项目是单进程组件库，不含服务注册、RPC、链路追踪

**下一步可做**：接 epoll + 线程池做真实并发服务；把存储换成 SQLite（连接池 + 事务）；
限流改为基于共享存储的分布式实现；加 Prometheus 指标导出。

---

## 8. 许可

MIT
