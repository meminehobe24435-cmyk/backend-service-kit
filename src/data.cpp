/* data.cpp — 零依赖 JSON / XML 解析与生成 + 线程安全配置
 *
 * 实现要点（也是自己写一遍才会注意到的细节）：
 *   JSON  · 字符串转义要处理 \" \\ \/ \b \f \n \r \t 与 \uXXXX
 *         · 数字走 strtod 保证精度；解析后必须检查剩余输入
 *         · 递归下降，深度限制防栈溢出（恶意深嵌套）
 *   XML   · 自闭合 <a/>、注释 <!-- -->、声明 <?xml ?>、CDATA 都要跳过
 *         · 文本里的实体 &amp; &lt; &gt; &quot; &apos; 必须转义/反转义
 *         · 属性值既可单引号也可双引号
 *   Config· 读多写少 -> shared_mutex（读并发、写独占）
 *         · set 时先取锁释放再回调，避免回调里再 set 造成死锁
 */
#include "bsk_data.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace bsk {

/* ================================================================== JSON */
const Json &Json::null_ref()
{
    static Json n;
    return n;
}

Json Json::array()
{
    Json j;
    j.type_ = JsonType::Array;
    return j;
}

Json Json::object()
{
    Json j;
    j.type_ = JsonType::Object;
    return j;
}

bool Json::as_bool(bool def) const
{
    if (type_ == JsonType::Bool) return bool_;
    if (type_ == JsonType::Number) return num_ != 0.0;
    if (type_ == JsonType::String) return str_ == "true" || str_ == "1";
    return def;
}

double Json::as_number(double def) const
{
    if (type_ == JsonType::Number) return num_;
    if (type_ == JsonType::Bool) return bool_ ? 1.0 : 0.0;
    if (type_ == JsonType::String) {
        try { return std::stod(str_); } catch (...) { return def; }
    }
    return def;
}

int Json::as_int(int def) const
{
    const double d = as_number(static_cast<double>(def));
    return static_cast<int>(d);
}

std::string Json::as_string(const std::string &def) const
{
    switch (type_) {
    case JsonType::String: return str_;
    case JsonType::Number: {
        char buf[32];
        if (num_ == static_cast<double>(static_cast<long long>(num_)))
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
        else
            std::snprintf(buf, sizeof(buf), "%g", num_);
        return buf;
    }
    case JsonType::Bool: return bool_ ? "true" : "false";
    default: return def;
    }
}

const Json &Json::at(size_t i) const
{
    if (type_ != JsonType::Array || i >= arr_.size()) return null_ref();
    return arr_[i];
}

void Json::push_back(const Json &v)
{
    type_ = JsonType::Array;
    arr_.push_back(v);
}

const Json &Json::operator[](const std::string &key) const
{
    auto it = obj_.find(key);
    return it == obj_.end() ? null_ref() : it->second;
}

Json &Json::operator[](const std::string &key)
{
    if (type_ != JsonType::Object) { type_ = JsonType::Object; obj_.clear(); }
    return obj_[key];
}

std::vector<std::string> Json::keys() const
{
    std::vector<std::string> out;
    for (const auto &kv : obj_) out.push_back(kv.first);
    return out;
}

const Json &Json::path(const std::string &dotted) const
{
    const Json *cur = this;
    size_t pos = 0;
    while (pos <= dotted.size()) {
        const size_t dot = dotted.find('.', pos);
        const std::string seg = dotted.substr(pos, dot == std::string::npos
                                                     ? std::string::npos : dot - pos);
        if (!seg.empty()) {
            if (cur->type_ == JsonType::Object) {
                auto it = cur->obj_.find(seg);
                if (it == cur->obj_.end()) return null_ref();
                cur = &it->second;
            } else if (cur->type_ == JsonType::Array) {
                try {
                    const size_t idx = static_cast<size_t>(std::stoul(seg));
                    if (idx >= cur->arr_.size()) return null_ref();
                    cur = &cur->arr_[idx];
                } catch (...) { return null_ref(); }
            } else {
                return null_ref();
            }
        }
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return *cur;
}

bool Json::contains(const std::string &needle) const
{
    if (type_ == JsonType::String) return str_.find(needle) != std::string::npos;
    if (type_ == JsonType::Object) {
        for (const auto &kv : obj_)
            if (kv.first.find(needle) != std::string::npos || kv.second.contains(needle))
                return true;
        return false;
    }
    if (type_ == JsonType::Array) {
        for (const auto &v : arr_) if (v.contains(needle)) return true;
        return false;
    }
    return false;
}

/* ------------------------------------------------------------ JSON 解析 */
namespace {

struct JParser {
    const std::string &s;
    size_t i = 0;
    std::string err;
    int depth = 0;
    static constexpr int kMaxDepth = 64;

    explicit JParser(const std::string &text) : s(text) {}

    void skip_ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                                s[i] == '\n' || s[i] == '\r'))
            i++;
    }

    bool fail(const std::string &msg)
    {
        if (err.empty())
            err = msg + "（位置 " + std::to_string(i) + "）";
        return false;
    }

    bool parse_string(std::string &out)
    {
        if (i >= s.size() || s[i] != '"') return fail("期望字符串");
        i++;
        out.clear();
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') { i++; return true; }
            if (c == '\\') {
                i++;
                if (i >= s.size()) return fail("字符串转义未结束");
                const char e = s[i++];
                switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("\\u 后面不足 4 位");
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        const char h = s[i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("\\u 中出现非十六进制字符");
                    }
                    /* 只做基础码位处理：ASCII 直出，其余按 UTF-8 编码 */
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return fail(std::string("未知转义 \\") + e);
                }
            } else {
                out.push_back(c);
                i++;
            }
        }
        return fail("字符串未闭合");
    }

    bool parse_value(Json &out)
    {
        if (++depth > kMaxDepth) return fail("嵌套过深");
        skip_ws();
        if (i >= s.size()) return fail("输入意外结束");

        const char c = s[i];
        bool ok = false;
        if (c == '{') {
            i++;
            out = Json::object();
            skip_ws();
            if (i < s.size() && s[i] == '}') { i++; depth--; return true; }
            while (true) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) { depth--; return false; }
                skip_ws();
                if (i >= s.size() || s[i] != ':') { fail("键后缺少 :"); depth--; return false; }
                i++;
                Json v;
                if (!parse_value(v)) { depth--; return false; }
                out[key] = v;
                skip_ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; ok = true; break; }
                fail("对象里期望 , 或 }");
                depth--;
                return false;
            }
        } else if (c == '[') {
            i++;
            out = Json::array();
            skip_ws();
            if (i < s.size() && s[i] == ']') { i++; depth--; return true; }
            while (true) {
                Json v;
                if (!parse_value(v)) { depth--; return false; }
                out.push_back(v);
                skip_ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; ok = true; break; }
                fail("数组里期望 , 或 ]");
                depth--;
                return false;
            }
        } else if (c == '"') {
            std::string tmp;
            if (!parse_string(tmp)) { depth--; return false; }
            out = Json(tmp);
            ok = true;
        } else if (s.compare(i, 4, "true") == 0) {
            i += 4; out = Json(true); ok = true;
        } else if (s.compare(i, 5, "false") == 0) {
            i += 5; out = Json(false); ok = true;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4; out = Json(); ok = true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char *start = s.c_str() + i;
            char *end = nullptr;
            const double d = std::strtod(start, &end);
            if (end == start) { fail("数字格式错误"); depth--; return false; }
            i += static_cast<size_t>(end - start);
            out = Json(d);
            ok = true;
        } else {
            fail("无法识别的值起始字符");
        }
        depth--;
        return ok;
    }
};

void dump_json(const Json &j, std::ostringstream &os, int indent, int level)
{
    const bool pretty = indent >= 0;
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent) * (level + 1), ' ') : "";
    const std::string pad1 = pretty ? std::string(static_cast<size_t>(indent) * level, ' ') : "";

    switch (j.type()) {
    case JsonType::Null:   os << "null"; break;
    case JsonType::Bool:   os << (j.as_bool() ? "true" : "false"); break;
    case JsonType::Number: os << j.as_string("0"); break;
    case JsonType::String: {
        os << '"';
        for (char ch : j.as_string()) {
            switch (ch) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    os << buf;
                } else {
                    os << ch;
                }
            }
        }
        os << '"';
        break;
    }
    case JsonType::Array: {
        if (j.items().empty()) { os << "[]"; break; }
        os << "[";
        if (pretty) os << "\n";
        for (size_t k = 0; k < j.items().size(); k++) {
            if (pretty) os << pad;
            dump_json(j.items()[k], os, indent, level + 1);
            if (k + 1 < j.items().size()) os << ",";
            if (pretty) os << "\n";
        }
        if (pretty) os << pad1;
        os << "]";
        break;
    }
    case JsonType::Object: {
        if (j.fields().empty()) { os << "{}"; break; }
        os << "{";
        if (pretty) os << "\n";
        size_t k = 0;
        for (const auto &kv : j.fields()) {
            if (pretty) os << pad;
            os << '"' << kv.first << "\":";
            if (pretty) os << " ";
            dump_json(kv.second, os, indent, level + 1);
            if (++k < j.fields().size()) os << ",";
            if (pretty) os << "\n";
        }
        if (pretty) os << pad1;
        os << "}";
        break;
    }
    }
}

}  /* namespace */

bool Json::parse(const std::string &text, Json &out, std::string *err)
{
    JParser p(text);
    Json v;
    if (!p.parse_value(v)) {
        if (err) *err = p.err.empty() ? "解析失败" : p.err;
        return false;
    }
    p.skip_ws();
    if (p.i != text.size()) {          /* 尾部还有垃圾 -> 不算合法 JSON */
        if (err) *err = "JSON 结束后仍有多余内容（位置 " + std::to_string(p.i) + "）";
        return false;
    }
    out = v;
    return true;
}

std::string Json::dump(int indent) const
{
    std::ostringstream os;
    dump_json(*this, os, indent, 0);
    return os.str();
}

/* =================================================================== XML */
std::string XmlNode::attr(const std::string &k, const std::string &def) const
{
    auto it = attrs.find(k);
    return it == attrs.end() ? def : it->second;
}

const XmlNode *XmlNode::child(const std::string &n) const
{
    for (const auto &c : children) if (c.name == n) return &c;
    return nullptr;
}

std::vector<const XmlNode *> XmlNode::children_named(const std::string &n) const
{
    std::vector<const XmlNode *> out;
    for (const auto &c : children) if (c.name == n) out.push_back(&c);
    return out;
}

std::string XmlNode::child_text(const std::string &n, const std::string &def) const
{
    const XmlNode *c = child(n);
    return c ? c->text : def;
}

std::string Xml::escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':  o += "&amp;";  break;
        case '<':  o += "&lt;";   break;
        case '>':  o += "&gt;";   break;
        case '"':  o += "&quot;"; break;
        case '\'': o += "&apos;"; break;
        default:   o += c;
        }
    }
    return o;
}

std::string Xml::unescape(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') { o += s[i++]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos) { o += s[i++]; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if      (ent == "amp")  o += '&';
        else if (ent == "lt")   o += '<';
        else if (ent == "gt")   o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            try {
                const unsigned cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    ? static_cast<unsigned>(std::stoul(ent.substr(2), nullptr, 16))
                    : static_cast<unsigned>(std::stoul(ent.substr(1)));
                if (cp < 0x80) o += static_cast<char>(cp);
                else if (cp < 0x800) {
                    o += static_cast<char>(0xC0 | (cp >> 6));
                    o += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    o += static_cast<char>(0xE0 | (cp >> 12));
                    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    o += static_cast<char>(0x80 | (cp & 0x3F));
                }
            } catch (...) { o += s.substr(i, semi - i + 1); }
        } else {
            o += s.substr(i, semi - i + 1);      /* 未知实体原样保留 */
        }
        i = semi + 1;
    }
    return o;
}

namespace {

bool is_name_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
           c == ':' || c == '.';
}

struct XParser {
    const std::string &s;
    size_t i = 0;
    std::string err;

    explicit XParser(const std::string &t) : s(t) {}

    bool fail(const std::string &m)
    {
        if (err.empty()) err = m + "（位置 " + std::to_string(i) + "）";
        return false;
    }

    void skip_misc()
    {
        while (i < s.size()) {
            if (std::isspace(static_cast<unsigned char>(s[i]))) { i++; continue; }
            if (s.compare(i, 4, "<!--") == 0) {          /* 注释 */
                const size_t e = s.find("-->", i);
                i = (e == std::string::npos) ? s.size() : e + 3;
                continue;
            }
            if (s.compare(i, 2, "<?") == 0) {            /* 声明 / 处理指令 */
                const size_t e = s.find("?>", i);
                i = (e == std::string::npos) ? s.size() : e + 2;
                continue;
            }
            if (s.compare(i, 9, "<![CDATA[") == 0) {     /* CDATA 由 parse_text 处理 */
                return;
            }
            if (s.compare(i, 2, "<!") == 0) {            /* DOCTYPE 等 */
                const size_t e = s.find('>', i);
                i = (e == std::string::npos) ? s.size() : e + 1;
                continue;
            }
            return;
        }
    }

    bool parse_name(std::string &out)
    {
        out.clear();
        while (i < s.size() && is_name_char(s[i])) out += s[i++];
        return !out.empty();
    }

    /* 解析一个元素（假定 s[i]=='<' 且不是注释/声明） */
    bool parse_element(XmlNode &node)
    {
        if (s[i] != '<') return fail("期望 <");
        i++;
        if (!parse_name(node.name)) return fail("标签名非法");

        /* 属性 */
        while (true) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
            if (i >= s.size()) return fail("标签未闭合");
            if (s[i] == '/') {
                if (i + 1 >= s.size() || s[i + 1] != '>') return fail("自闭合标签格式错误");
                i += 2;
                return true;                     /* <a/> */
            }
            if (s[i] == '>') { i++; break; }
            std::string key;
            if (!parse_name(key)) return fail("属性名非法");
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
            std::string val;
            if (i < s.size() && s[i] == '=') {
                i++;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                if (i >= s.size() || (s[i] != '"' && s[i] != '\''))
                    return fail("属性值必须用引号");
                const char q = s[i++];
                const size_t e = s.find(q, i);
                if (e == std::string::npos) return fail("属性值引号未闭合");
                val = Xml::unescape(s.substr(i, e - i));
                i = e + 1;
            }
            node.attrs[key] = val;
        }

        /* 内容：文本 + 子元素，直到 </name> */
        std::string text;
        while (i < s.size()) {
            if (s.compare(i, 9, "<![CDATA[") == 0) {
                const size_t e = s.find("]]>", i);
                if (e == std::string::npos) return fail("CDATA 未闭合");
                text += s.substr(i + 9, e - i - 9);
                i = e + 3;
                continue;
            }
            if (s.compare(i, 4, "<!--") == 0) {
                const size_t e = s.find("-->", i);
                if (e == std::string::npos) return fail("注释未闭合");
                i = e + 3;
                continue;
            }
            if (s.compare(i, 2, "</") == 0) {
                i += 2;
                std::string close;
                if (!parse_name(close)) return fail("闭合标签名非法");
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                if (i >= s.size() || s[i] != '>') return fail("闭合标签未结束");
                i++;
                if (close != node.name)
                    return fail("标签不匹配：<" + node.name + "> 被 </" + close + "> 关闭");
                node.text = Xml::unescape(text);
                return true;
            }
            if (s[i] == '<') {
                XmlNode child;
                if (!parse_element(child)) return false;
                node.children.push_back(child);
                continue;
            }
            text += s[i++];
        }
        return fail("缺少闭合标签 </" + node.name + ">");
    }
};

void dump_xml(const XmlNode &n, std::ostringstream &os, int indent, int level)
{
    const bool pretty = indent >= 0;
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent) * level, ' ') : "";
    os << pad << "<" << n.name;
    for (const auto &kv : n.attrs)
        os << " " << kv.first << "=\"" << Xml::escape(kv.second) << "\"";
    if (n.children.empty() && n.text.empty()) {
        os << "/>";
        if (pretty) os << "\n";
        return;
    }
    os << ">";
    if (n.children.empty()) {
        os << Xml::escape(n.text);
    } else {
        if (!n.text.empty()) os << Xml::escape(n.text);
        if (pretty) os << "\n";
        for (const auto &c : n.children) dump_xml(c, os, indent, level + 1);
        if (pretty) os << pad;
    }
    os << "</" << n.name << ">";
    if (pretty) os << "\n";
}

}  /* namespace */

bool Xml::parse(const std::string &text, XmlNode &root, std::string *err)
{
    XParser p(text);
    p.skip_misc();
    if (p.i >= text.size() || text[p.i] != '<') {
        if (err) *err = "未找到根元素";
        return false;
    }
    if (!p.parse_element(root)) {
        if (err) *err = p.err.empty() ? "解析失败" : p.err;
        return false;
    }
    /* 根之后只允许空白与注释 */
    const size_t save = p.i;
    p.skip_misc();
    while (p.i < text.size() && std::isspace(static_cast<unsigned char>(text[p.i]))) p.i++;
    if (p.i != text.size()) {
        if (err) *err = "根元素之后仍有多余内容（位置 " + std::to_string(p.i) + "）";
        return false;
    }
    (void)save;
    return true;
}

std::string Xml::dump(const XmlNode &root, int indent, const std::string &decl)
{
    std::ostringstream os;
    if (!decl.empty()) os << decl << "\n";
    dump_xml(root, os, indent, 0);
    return os.str();
}

/* ====================================================== 线程安全配置 */
std::string Config::get(const std::string &dotted, const std::string &def) const
{
    std::shared_lock<std::shared_mutex> lk(mu_);       /* 读并发 */
    auto it = kv_.find(dotted);
    return it == kv_.end() ? def : it->second;
}

int Config::get_int(const std::string &dotted, int def) const
{
    const std::string v = get(dotted);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

bool Config::get_bool(const std::string &dotted, bool def) const
{
    const std::string v = get(dotted);
    if (v.empty()) return def;
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

bool Config::has(const std::string &dotted) const
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    return kv_.count(dotted) > 0;
}

void Config::set(const std::string &dotted, const std::string &value)
{
    std::function<void(const std::string &, const std::string &)> cb;
    {
        std::unique_lock<std::shared_mutex> lk(mu_);   /* 写独占 */
        kv_[dotted] = value;
        cb = cb_;                       /* 复制出来，避免持锁回调 -> 死锁 */
    }
    if (cb) cb(dotted, value);          /* 锁已释放再回调 */
}

size_t Config::size() const
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    return kv_.size();
}

std::vector<std::string> Config::keys() const
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<std::string> out;
    for (const auto &kv : kv_) out.push_back(kv.first);
    return out;
}

void Config::on_change(std::function<void(const std::string &, const std::string &)> cb)
{
    std::unique_lock<std::shared_mutex> lk(mu_);
    cb_ = std::move(cb);
}

void Config::flatten(const Json &j, const std::string &prefix)
{
    if (j.is_object()) {
        for (const auto &kv : j.fields())
            flatten(kv.second, prefix.empty() ? kv.first : prefix + "." + kv.first);
    } else if (j.is_array()) {
        for (size_t k = 0; k < j.size(); k++)
            flatten(j.at(k), prefix + "." + std::to_string(k));
    } else {
        if (!prefix.empty()) kv_[prefix] = j.as_string();
    }
}

void Config::flatten(const XmlNode &n, const std::string &prefix)
{
    const std::string base = prefix.empty() ? n.name : prefix + "." + n.name;
    for (const auto &kv : n.attrs) kv_[base + "@" + kv.first] = kv.second;
    if (n.children.empty()) {
        kv_[base] = n.text;
        return;
    }
    for (const auto &c : n.children) flatten(c, base);
}

bool Config::load_json(const std::string &text, std::string *err)
{
    Json j;
    if (!Json::parse(text, j, err)) return false;
    std::unique_lock<std::shared_mutex> lk(mu_);
    kv_.clear();
    flatten(j, "");
    return true;
}

bool Config::load_xml(const std::string &text, std::string *err)
{
    XmlNode root;
    if (!Xml::parse(text, root, err)) return false;
    std::unique_lock<std::shared_mutex> lk(mu_);
    kv_.clear();
    flatten(root, "");
    return true;
}

} /* namespace bsk */
