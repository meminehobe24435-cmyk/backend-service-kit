/* http.cpp — HTTP/1.1 请求解析与响应生成
 *
 * 实现范围（这是"看得见协议"的部分）：
 *   · 请求行：METHOD SP PATH[?QUERY] SP HTTP/x.y
 *   · 头部：逐行解析，处理重复头、大小写不敏感、值首尾空白
 *   · 体：按 Content-Length 读取；支持分块到达（返回"不完整"）
 *   · keep-alive：HTTP/1.1 默认长连接，Connection: close 关闭
 *   · 响应：状态行 + 头 + 体，自动补 Content-Length / Content-Type / Connection
 *
 * 不做：chunked 请求体、HTTP/2、TLS（这些属于更上层或外部组件）
 */
#include "bsk.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace bsk {

const char *method_name(Method m)
{
    switch (m) {
    case Method::GET:     return "GET";
    case Method::POST:    return "POST";
    case Method::PUT:     return "PUT";
    case Method::DELETE_: return "DELETE";
    case Method::HEAD:    return "HEAD";
    default:              return "UNKNOWN";
    }
}

Method parse_method(const std::string &s)
{
    if (s == "GET")     return Method::GET;
    if (s == "POST")    return Method::POST;
    if (s == "PUT")     return Method::PUT;
    if (s == "DELETE")  return Method::DELETE_;
    if (s == "HEAD")    return Method::HEAD;
    return Method::UNKNOWN;
}

const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Request::header(const std::string &k) const
{
    auto it = headers.find(lower(k));
    return it == headers.end() ? std::string() : it->second;
}

int http_parse_request(const std::string &raw, Request &out, size_t *consumed)
{
    if (consumed) *consumed = 0;

    /* 头部结束标志：\r\n\r\n */
    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        /* 容错：只给了 \n\n 的情况 */
        const size_t alt = raw.find("\n\n");
        if (alt == std::string::npos) return 1;      /* 头还没收完 */
        return http_parse_request(raw.substr(0, alt) + "\r\n\r\n" + raw.substr(alt + 2),
                                  out, consumed);
    }

    const std::string head = raw.substr(0, head_end);
    std::istringstream is(head);
    std::string line;

    if (!std::getline(is, line)) return -1;
    {
        std::istringstream ls(trim(line));
        std::string m, target, ver;
        ls >> m >> target >> ver;
        if (m.empty() || target.empty() || ver.rfind("HTTP/", 0) != 0) return -1;
        out.method = parse_method(m);
        if (out.method == Method::UNKNOWN) return -1;
        const size_t q = target.find('?');
        if (q == std::string::npos) {
            out.path = target;
        } else {
            out.path = target.substr(0, q);
            out.query = target.substr(q + 1);
        }
        out.version = ver;
    }

    out.headers.clear();
    while (std::getline(is, line)) {
        const std::string l = trim(line);
        if (l.empty()) continue;
        const size_t c = l.find(':');
        if (c == std::string::npos) return -1;       /* 坏头 */
        const std::string k = lower(trim(l.substr(0, c)));
        out.headers[k] = trim(l.substr(c + 1));
    }

    /* 连接方式 */
    const std::string conn = out.header("connection");
    if (!conn.empty()) {
        out.keep_alive = lower(conn) != "close";
    } else {
        out.keep_alive = out.version != "HTTP/1.0";
    }

    /* 体长度 */
    size_t body_len = 0;
    const std::string cl = out.header("content-length");
    if (!cl.empty()) {
        try {
            long v = std::stol(cl);
            if (v < 0) return -1;
            body_len = static_cast<size_t>(v);
        } catch (...) {
            return -1;
        }
    }

    const size_t body_start = head_end + 4;
    if (raw.size() < body_start + body_len) return 1;   /* 体还没收完 */
    out.body = raw.substr(body_start, body_len);
    if (consumed) *consumed = body_start + body_len;
    return 0;
}

std::string Response::serialize() const
{
    std::ostringstream os;
    os << "HTTP/1.1 " << status << " " << status_text(status) << "\r\n";

    bool has_type = false, has_len = false;
    for (const auto &kv : headers) {
        const std::string k = lower(kv.first);
        if (k == "content-type") has_type = true;
        if (k == "content-length") has_len = true;
        os << kv.first << ": " << kv.second << "\r\n";
    }
    if (!has_type) os << "Content-Type: text/plain; charset=utf-8\r\n";
    if (!has_len)  os << "Content-Length: " << body.size() << "\r\n";
    os << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    os << "\r\n" << body;
    return os.str();
}

Response make_text(int status, const std::string &body)
{
    Response r;
    r.status = status;
    r.body = body;
    r.headers["Content-Type"] = "text/plain; charset=utf-8";
    return r;
}

Response make_json(int status, const std::string &json)
{
    Response r;
    r.status = status;
    r.body = json;
    r.headers["Content-Type"] = "application/json; charset=utf-8";
    return r;
}

} /* namespace bsk */
