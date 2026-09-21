/* cache.cpp — LRU + TTL 缓存（对标 Redis 的核心语义）
 * resilience.cpp 内容合并在此文件（熔断 / 限流 / 健康检查）
 * store.cpp 内容合并在此文件（极简关系表 + journal 落盘）
 *
 * 合并原因：三者都是"服务运行期的基础设施"，放在一起便于对照阅读；
 * 模块边界仍清晰（各自独立的类与职责）。
 */
#include "bsk.h"
#include <algorithm>
#include <cstdio>

namespace bsk {

/* ================================================================= 缓存 */
void LruTtlCache::touch(const std::string &key)
{
    auto &e = map_[key];
    /* 从 LRU 链上摘掉旧的迭代器位置 */
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
        if (*it == key) { lru_.erase(it); break; }
    }
    lru_.push_back(key);
    e.lru_it = std::prev(lru_.end());
}

void LruTtlCache::set(const std::string &key, const std::string &value,
                      double ttl_s, double now)
{
    auto it = map_.find(key);
    if (it == map_.end() && map_.size() >= cap_) {
        /* 淘汰最久未使用（队首） */
        while (!lru_.empty()) {
            const std::string victim = lru_.front();
            lru_.pop_front();
            auto vit = map_.find(victim);
            if (vit != map_.end()) {
                map_.erase(vit);
                stat_.evictions++;
                break;
            }
        }
    }
    map_[key].value = value;
    map_[key].expire_at = ttl_s > 0.0 ? now + ttl_s : 0.0;
    touch(key);
    stat_.sets++;
}

bool LruTtlCache::get(const std::string &key, std::string &out, double now)
{
    auto it = map_.find(key);
    if (it == map_.end()) { stat_.misses++; return false; }
    if (is_expired(it->second, now)) {
        /* 关键：**不删除**过期值 —— 留给降级路径返回 stale 数据 */
        stat_.expired++;
        stat_.misses++;
        return false;
    }
    out = it->second.value;
    touch(key);
    stat_.hits++;
    return true;
}

bool LruTtlCache::get_stale(const std::string &key, std::string &out,
                            bool *expired, double now)
{
    auto it = map_.find(key);
    if (it == map_.end()) { if (expired) *expired = false; return false; }
    const bool exp = is_expired(it->second, now);
    out = it->second.value;
    if (expired) *expired = exp;
    if (!exp) { touch(key); stat_.hits++; }
    else      { stat_.expired++; }
    return true;
}

bool LruTtlCache::del(const std::string &key, double now)
{
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    (void)now;
    for (auto li = lru_.begin(); li != lru_.end(); ++li) {
        if (*li == key) { lru_.erase(li); break; }
    }
    map_.erase(it);
    return true;
}

bool LruTtlCache::expire(const std::string &key, double ttl_s, double now)
{
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    it->second.expire_at = ttl_s > 0.0 ? now + ttl_s : 0.0;
    return true;
}

double LruTtlCache::ttl(const std::string &key, double now)
{
    auto it = map_.find(key);
    if (it == map_.end()) return -2.0;              /* 不存在 */
    if (it->second.expire_at <= 0.0) return -1.0;   /* 无过期 */
    const double left = it->second.expire_at - now;
    if (left <= 0.0) { map_.erase(it); stat_.expired++; return -2.0; }
    return left;
}

void LruTtlCache::clear_expired(double now)
{
    for (auto it = map_.begin(); it != map_.end();) {
        if (is_expired(it->second, now)) {
            for (auto li = lru_.begin(); li != lru_.end(); ++li) {
                if (*li == it->first) { lru_.erase(li); break; }
            }
            it = map_.erase(it);
            stat_.expired++;
        } else {
            ++it;
        }
    }
}

std::vector<std::string> LruTtlCache::keys() const
{
    std::vector<std::string> out;
    for (const auto &kv : map_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

/* ========================================================= 容灾与降级 */
const char *breaker_state_name(BreakerState s)
{
    switch (s) {
    case BreakerState::Closed:   return "CLOSED";
    case BreakerState::Open:     return "OPEN";
    case BreakerState::HalfOpen: return "HALF_OPEN";
    }
    return "?";
}

double CircuitBreaker::failure_ratio() const
{
    if (window_.empty()) return 0.0;
    size_t fail = 0;
    for (bool ok : window_) if (!ok) fail++;
    return static_cast<double>(fail) / window_.size();
}

bool CircuitBreaker::allow(double now)
{
    if (state_ == BreakerState::Open) {
        if (now - opened_at_ >= cfg_.open_seconds) {
            state_ = BreakerState::HalfOpen;
            half_open_left_ = cfg_.half_open_trials;
        } else {
            rejected_++;
            return false;                 /* 熔断中：拒绝，触发降级 */
        }
    }
    if (state_ == BreakerState::HalfOpen) {
        if (half_open_left_ == 0) { rejected_++; return false; }
        half_open_left_--;
    }
    return true;
}

void CircuitBreaker::on_success(double now)
{
    if (state_ == BreakerState::HalfOpen) {
        /* 试探成功则恢复 */
        state_ = BreakerState::Closed;
        window_.clear();
        return;
    }
    (void)now;
    window_.push_back(true);
    if (window_.size() > cfg_.window) window_.pop_front();
}

void CircuitBreaker::on_failure(double now)
{
    window_.push_back(false);
    if (window_.size() > cfg_.window) window_.pop_front();

    if (state_ == BreakerState::HalfOpen) {
        state_ = BreakerState::Open;
        opened_at_ = now;
        trips_++;
        return;
    }
    if (window_.size() >= cfg_.window && failure_ratio() >= cfg_.fail_ratio) {
        state_ = BreakerState::Open;
        opened_at_ = now;
        trips_++;
    }
}

double TokenBucket::tokens(double now)
{
    refill(now);
    return tokens_;
}

void TokenBucket::refill(double now)
{
    if (last_ < 0.0) { last_ = now; return; }
    const double dt = now - last_;
    if (dt <= 0.0) return;
    tokens_ = std::min(burst_, tokens_ + dt * rate_);
    last_ = now;
}

bool TokenBucket::try_acquire(double now)
{
    refill(now);
    /* 带容差比较：浮点累加会得到 0.9999999999999998 这种值 */
    if (tokens_ + EPS >= 1.0) {
        tokens_ -= 1.0;
        if (tokens_ < 0.0) tokens_ = 0.0;      /* 夹到 0，避免负数累积 */
        return true;
    }
    rejected_++;
    return false;
}

const char *health_name(Health h)
{
    switch (h) {
    case Health::Up:       return "UP";
    case Health::Degraded: return "DEGRADED";
    case Health::Down:     return "DOWN";
    }
    return "?";
}

void HealthMonitor::record(bool ok)
{
    window_.push_back(ok);
    if (window_.size() > cfg_.window) window_.pop_front();
}

double HealthMonitor::failure_ratio() const
{
    if (window_.empty()) return 0.0;
    size_t fail = 0;
    for (bool ok : window_) if (!ok) fail++;
    return static_cast<double>(fail) / window_.size();
}

Health HealthMonitor::evaluate() const
{
    if (window_.size() < cfg_.window) return Health::Up;   /* 样本不足先按 Up */
    const double r = failure_ratio();
    if (r >= cfg_.down_ratio) return Health::Down;
    if (r >= cfg_.degrade_ratio) return Health::Degraded;
    return Health::Up;
}

/* ============================================================== 存储 */
bool Table::insert(const std::map<std::string, std::string> &values)
{
    /* 参数化语义：列名必须在表定义里，值不做任何拼接 */
    for (const auto &kv : values) {
        if (std::find(columns_.begin(), columns_.end(), kv.first) == columns_.end()) {
            return false;                       /* 未知列 -> 拒绝 */
        }
    }
    Row r;
    r.cols = values;
    rows_.push_back(r);
    return true;
}

std::vector<Row> Table::select(const std::map<std::string, std::string> &where,
                               size_t limit) const
{
    std::vector<Row> out;
    for (const auto &r : rows_) {
        bool ok = true;
        for (const auto &w : where) {
            if (r.get(w.first) != w.second) { ok = false; break; }
        }
        if (!ok) continue;
        out.push_back(r);
        if (limit && out.size() >= limit) break;
    }
    return out;
}

size_t Table::count_where(const std::string &col, const std::string &val) const
{
    size_t n = 0;
    for (const auto &r : rows_) if (r.get(col) == val) n++;
    return n;
}

long long Table::sum(const std::string &col) const
{
    long long s = 0;
    for (const auto &r : rows_) {
        const std::string v = r.get(col);
        if (v.empty()) continue;
        try { s += std::stoll(v); } catch (...) { /* 非数值列忽略 */ }
    }
    return s;
}

bool Table::flush(const std::string &path) const
{
    FILE *fp = std::fopen(path.c_str(), "w");
    if (!fp) return false;
    std::fprintf(fp, "# table %s\n", name_.c_str());
    std::fprintf(fp, "# columns:");
    for (const auto &c : columns_) std::fprintf(fp, ",%s", c.c_str());
    std::fprintf(fp, "\n");
    for (const auto &r : rows_) {
        for (size_t i = 0; i < columns_.size(); i++) {
            std::fprintf(fp, "%s%s", i ? "," : "", r.get(columns_[i]).c_str());
        }
        std::fprintf(fp, "\n");
    }
    std::fclose(fp);
    return true;
}

} /* namespace bsk */
