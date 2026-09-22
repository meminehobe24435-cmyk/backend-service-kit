/*
 * bsk_data.h — 零依赖的数据交换与配置模块（JSON / XML / 线程安全配置）
 *
 * 对应 JD「掌握 Linux/Android 下 XML 和 JSON 操作，多线程操作」：
 *   Json   手写 JSON 解析与生成（对象/数组/字符串/数字/布尔/null，含转义）
 *   Xml    手写 XML 解析与生成（声明/注释/标签/属性/文本/自闭合，含实体转义）
 *   Config 线程安全配置容器（读写用 shared_mutex；支持从 JSON/XML 载入、
 *          点号路径取值、变更通知）—— 这是"多线程下用配置"的正确做法
 *
 * 为什么自己写而不引第三方库：
 *   · 服务/固件侧经常不允许引入大依赖，解析器本身也不复杂
 *   · 自己写一遍才能真正说清"JSON 的转义规则"和"XML 为什么要转义实体"
 */
#ifndef BSK_DATA_H
#define BSK_DATA_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace bsk {

/* ================================================================== JSON */
enum class JsonType { Null, Bool, Number, String, Array, Object };

class Json {
public:
    Json() = default;
    Json(bool b) : type_(JsonType::Bool), num_(b ? 1 : 0), bool_(b) {}
    Json(double d) : type_(JsonType::Number), num_(d) {}
    Json(int i) : type_(JsonType::Number), num_(static_cast<double>(i)) {}
    Json(const char *s) : type_(JsonType::String), str_(s) {}
    Json(const std::string &s) : type_(JsonType::String), str_(s) {}

    static Json array();
    static Json object();

    JsonType type() const { return type_; }
    bool is_null()   const { return type_ == JsonType::Null; }
    bool is_bool()   const { return type_ == JsonType::Bool; }
    bool is_number() const { return type_ == JsonType::Number; }
    bool is_string() const { return type_ == JsonType::String; }
    bool is_array()  const { return type_ == JsonType::Array; }
    bool is_object() const { return type_ == JsonType::Object; }

    /* 取值：类型不符时返回默认值（不抛异常 —— 服务侧更希望"容错"而不是"崩"） */
    bool        as_bool(bool def = false) const;
    double      as_number(double def = 0.0) const;
    int         as_int(int def = 0) const;
    std::string as_string(const std::string &def = "") const;

    /* 数组 */
    size_t size() const { return arr_.size(); }
    const Json &at(size_t i) const;
    void push_back(const Json &v);
    const std::vector<Json> &items() const { return arr_; }

    /* 对象 */
    bool has(const std::string &key) const { return obj_.count(key) > 0; }
    const Json &operator[](const std::string &key) const;
    Json &operator[](const std::string &key);
    const std::map<std::string, Json> &fields() const { return obj_; }
    std::vector<std::string> keys() const;

    /* 点号路径取值：cfg["server"]["port"] 等价于 path("server.port") */
    const Json &path(const std::string &dotted) const;

    bool contains(const std::string &needle) const;   /* 值里是否包含该子串 */

    /* 解析 / 生成 */
    static bool parse(const std::string &text, Json &out, std::string *err = nullptr);
    std::string dump(int indent = -1) const;

private:
    JsonType type_ = JsonType::Null;
    double num_ = 0.0;
    bool   bool_ = false;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
    static const Json &null_ref();
};

/* =================================================================== XML */
struct XmlNode {
    std::string name;
    std::string text;
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> children;

    std::string attr(const std::string &k, const std::string &def = "") const;
    bool has_attr(const std::string &k) const { return attrs.count(k) > 0; }
    const XmlNode *child(const std::string &n) const;
    std::vector<const XmlNode *> children_named(const std::string &n) const;
    std::string child_text(const std::string &n, const std::string &def = "") const;
    void add_child(const XmlNode &c) { children.push_back(c); }
};

class Xml {
public:
    /* 解析：返回是否成功；err 带出错误原因与位置 */
    static bool parse(const std::string &text, XmlNode &root, std::string *err = nullptr);
    static std::string dump(const XmlNode &root, int indent = -1,
                            const std::string &decl = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    /* 实体转义/反转义（& < > " '）—— 手写解析器最容易漏的部分 */
    static std::string escape(const std::string &s);
    static std::string unescape(const std::string &s);
};

/* ====================================================== 线程安全配置容器 */
class Config {
public:
    Config() = default;

    /* 读取：加共享锁，可并发读 */
    std::string get(const std::string &dotted, const std::string &def = "") const;
    int         get_int(const std::string &dotted, int def = 0) const;
    bool        get_bool(const std::string &dotted, bool def = false) const;
    bool        has(const std::string &dotted) const;

    /* 写入：加独占锁，并触发变更回调 */
    void set(const std::string &dotted, const std::string &value);

    /* 批量载入：JSON 或 XML（按首字符自动判断），成功返回 true */
    bool load_json(const std::string &text, std::string *err = nullptr);
    bool load_xml(const std::string &text, std::string *err = nullptr);

    size_t size() const;
    std::vector<std::string> keys() const;

    /* 变更通知：回调里**不要**再调用 set，否则会死锁 */
    void on_change(std::function<void(const std::string &, const std::string &)> cb);

private:
    mutable std::shared_mutex mu_;
    std::map<std::string, std::string> kv_;
    std::function<void(const std::string &, const std::string &)> cb_;

    void flatten(const Json &j, const std::string &prefix);
    void flatten(const XmlNode &n, const std::string &prefix);
};

} /* namespace bsk */

#endif /* BSK_DATA_H */
