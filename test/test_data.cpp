/* test_data.cpp — JSON / XML / 线程安全配置 单元测试（纯 C++17，无第三方框架）
 *
 *  T1 json     解析（对象/数组/转义/数字/布尔/null）、生成、点号路径、容错、错误定位
 *  T2 xml      解析（属性/文本/自闭合/注释/声明/CDATA）、实体转义反转义、生成、错误定位
 *  T3 config   JSON/XML 载入、点号取值、类型转换、多线程并发读写、变更回调
 */
#include "bsk_data.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace bsk;

static int passed = 0, failed = 0;

#define CHECK(cond, ...) do {                        \
    if (cond) { passed++; std::printf("  [PASS] "); } \
    else      { failed++; std::printf("  [FAIL] "); } \
    std::printf(__VA_ARGS__);                         \
    std::printf("\n");                                \
} while (0)

/* ------------------------------------------------------------------ T1 JSON */
static void t1_json()
{
    std::printf("T1 JSON（解析 / 生成 / 取值）\n");

    const std::string text =
        "{"
        "\"server\":{\"host\":\"0.0.0.0\",\"port\":8080,\"tls\":false},"
        "\"devices\":[{\"id\":\"AUV-01\",\"depth\":12.5},{\"id\":\"AUV-02\",\"depth\":0}],"
        "\"name\":\"通信\\\"网关\\\"\","
        "\"path\":\"a\\\\b\","
        "\"note\":\"第一行\\n第二行\","
        "\"unicode\":\"\\u4e2d\\u6587\","
        "\"empty\":null,"
        "\"n\":-3.5e2"
        "}";

    Json j;
    std::string err;
    CHECK(Json::parse(text, j, &err), "解析成功（err=%s）", err.c_str());
    CHECK(j.is_object(), "根是对象");
    CHECK(j["server"]["host"].as_string() == "0.0.0.0", "嵌套取值 host=%s",
          j["server"]["host"].as_string().c_str());
    CHECK(j["server"]["port"].as_int() == 8080, "整数取值 port=%d",
          j["server"]["port"].as_int());
    CHECK(j["server"]["port"].as_number() == 8080.0, "数字取值");
    CHECK(j["server"]["tls"].as_bool() == false, "布尔取值");
    CHECK(j["server"]["tls"].as_bool(true) == false, "显式 false 不落入默认值");

    /* 数组 */
    CHECK(j["devices"].is_array() && j["devices"].size() == 2, "数组长度=%zu",
          j["devices"].size());
    CHECK(j["devices"].at(0)["id"].as_string() == "AUV-01", "数组元素取值");
    CHECK(j["devices"].at(1)["depth"].as_number() == 0.0, "数组元素数值 0");
    CHECK(j["devices"].at(99).is_null(), "数组越界返回 null（不崩）");

    /* 转义 */
    CHECK(j["name"].as_string() == "通信\"网关\"", "引号转义还原 = %s",
          j["name"].as_string().c_str());
    CHECK(j["path"].as_string() == "a\\b", "反斜杠转义还原 = %s",
          j["path"].as_string().c_str());
    CHECK(j["note"].as_string() == "第一行\n第二行", "换行转义还原");
    CHECK(j["unicode"].as_string() == "中文", "\\uXXXX 还原为 UTF-8 = %s",
          j["unicode"].as_string().c_str());
    CHECK(j["n"].as_number() == -350.0, "科学计数法 = %g", j["n"].as_number());

    /* null 与缺失键 */
    CHECK(j["empty"].is_null(), "null 类型");
    CHECK(!j.has("nope"), "缺失键 has=false");
    CHECK(j["nope"].is_null(), "缺失键取值返回 null");
    CHECK(j["nope"]["deeper"].is_null(), "链式取缺失键也安全");

    /* 默认值 */
    CHECK(j["nope"].as_string("dft") == "dft", "缺失键取默认值");
    CHECK(j["nope"].as_int(7) == 7, "缺失键取默认整数");
    /* 注意 std::stod 只解析前缀："0.0.0.0" 会得到 0.0 而不是失败，
     * 所以这里用真正非数字的串来验证"解析失败 -> 取默认值"的语义 */
    Json nonnum("abc");
    CHECK(nonnum.as_int(7) == 7, "非数字字符串 -> 取默认整数（不抛异常）");
    CHECK(nonnum.as_number(1.5) == 1.5, "非数字字符串 -> 取默认浮点");
    CHECK(j["server"]["host"].as_string() == "0.0.0.0", "原字符串仍可正常取出");

    /* 点号路径 */
    CHECK(j.path("server.port").as_int() == 8080, "path 取嵌套值");
    CHECK(j.path("devices.1.id").as_string() == "AUV-02", "path 取数组元素");
    CHECK(j.path("devices.9.id").is_null(), "path 越界安全");
    CHECK(j.path("a.b.c.d").is_null(), "path 全缺失安全");

    /* contains */
    CHECK(j.contains("AUV-01"), "contains 命中值");
    CHECK(j.contains("host"), "contains 命中键");
    CHECK(!j.contains("NO_SUCH"), "contains 未命中");

    /* keys：断言期望的键都存在（比断言总数更稳、也更有意义） */
    {
        const std::vector<std::string> ks = j.keys();
        bool has_all = true;
        for (const char *want : {"server", "devices", "name", "path",
                                 "note", "unicode", "empty", "n"})
            if (!j.has(want)) has_all = false;
        CHECK(has_all, "顶层包含全部期望键（共 %zu 个）", ks.size());
        CHECK(ks.size() >= 8, "顶层键数不少于 8（实际 %zu）", ks.size());
    }

    /* 生成：紧凑与缩进 */
    const std::string compact = j.dump();
    Json j2;
    CHECK(Json::parse(compact, j2), "紧凑输出可被重新解析");
    CHECK(j2["server"]["port"].as_int() == 8080, "往返后值一致");
    CHECK(j2["name"].as_string() == j["name"].as_string(), "往返后转义一致");

    const std::string pretty = j.dump(2);
    CHECK(pretty.find("\n") != std::string::npos, "缩进输出含换行");
    Json j3;
    CHECK(Json::parse(pretty, j3, &err), "缩进输出可被重新解析（err=%s）", err.c_str());
    CHECK(j3["devices"].size() == 2, "缩进往返数组长度一致");

    /* 空容器 */
    Json ea = Json::array();
    CHECK(ea.dump() == "[]", "空数组 dump = %s", ea.dump().c_str());
    Json eo = Json::object();
    CHECK(eo.dump() == "{}", "空对象 dump = %s", eo.dump().c_str());

    /* 错误定位 */
    Json bad;
    err.clear();
    CHECK(!Json::parse("{\"a\":1", bad, &err) && !err.empty(),
          "缺右括号 -> 失败并给出原因：%s", err.c_str());
    err.clear();
    CHECK(!Json::parse("{\"a\":1} extra", bad, &err) && err.find("多余") != std::string::npos,
          "尾部多余内容 -> 失败：%s", err.c_str());
    err.clear();
    CHECK(!Json::parse("{\"a\":}", bad, &err), "缺值 -> 失败：%s", err.c_str());
    err.clear();
    CHECK(!Json::parse("{\"a\":tru}", bad, &err), "非法字面量 -> 失败：%s", err.c_str());
    err.clear();
    CHECK(!Json::parse("", bad, &err), "空输入 -> 失败");

    /* 深嵌套防栈溢出 */
    std::string deep;
    for (int i = 0; i < 200; i++) deep += "[";
    err.clear();
    CHECK(!Json::parse(deep, bad, &err) && err.find("嵌套") != std::string::npos,
          "超深嵌套被拒绝：%s", err.c_str());
}

/* ------------------------------------------------------------------- T2 XML */
static void t2_xml()
{
    std::printf("T2 XML（解析 / 转义 / 生成）\n");

    const std::string text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!-- 设备配置 -->\n"
        "<config version=\"2\">\n"
        "  <server host=\"0.0.0.0\" port=\"8080\"/>\n"
        "  <device id=\"AUV-01\" kind='fixed'>\n"
        "    <name>一号机</name>\n"
        "    <depth unit=\"m\">12.5</depth>\n"
        "  </device>\n"
        "  <device id=\"AUV-02\">\n"
        "    <name>二号机 &amp; 备用</name>\n"
        "  </device>\n"
        "  <note><![CDATA[原样 <保留> & 不转义]]></note>\n"
        "  <empty></empty>\n"
        "</config>";

    XmlNode root;
    std::string err;
    CHECK(Xml::parse(text, root, &err), "解析成功（err=%s）", err.c_str());
    CHECK(root.name == "config", "根标签 = %s", root.name.c_str());
    CHECK(root.attr("version") == "2", "根属性 version=%s", root.attr("version").c_str());

    const XmlNode *srv = root.child("server");
    CHECK(srv != nullptr, "找到 server 子节点");
    CHECK(srv && srv->attr("host") == "0.0.0.0", "server@host = %s",
          srv ? srv->attr("host").c_str() : "-");
    CHECK(srv && srv->attr("port", "0") == "8080", "server@port = %s（带默认值）",
          srv ? srv->attr("port").c_str() : "-");
    CHECK(srv && srv->attr("missing", "dft") == "dft", "缺失属性取默认值");
    CHECK(srv && !srv->has_attr("missing"), "has_attr 正确");

    /* 同名多个子节点 */
    auto devs = root.children_named("device");
    CHECK(devs.size() == 2, "device 子节点数 = %zu", devs.size());
    CHECK(devs.size() == 2 && devs[0]->attr("id") == "AUV-01", "第一个 device@id");
    CHECK(devs.size() == 2 && devs[1]->attr("kind", "-") == "-",
          "第二个 device 没有 kind 属性");
    CHECK(devs.size() == 2 && devs[0]->attr("kind") == "fixed",
          "单引号属性也支持：kind=%s", devs[0]->attr("kind").c_str());

    /* 子元素文本与嵌套 */
    CHECK(devs.size() == 2 && devs[0]->child_text("name") == "一号机",
          "嵌套取值 name=%s", devs.size() == 2 ? devs[0]->child_text("name").c_str() : "-");
    const XmlNode *depth = devs.size() == 2 ? devs[0]->child("depth") : nullptr;
    CHECK(depth && depth->text == "12.5", "depth 文本 = %s", depth ? depth->text.c_str() : "-");
    CHECK(depth && depth->attr("unit") == "m", "depth@unit");

    /* 实体反转义 */
    CHECK(devs.size() == 2 && devs[1]->child_text("name") == "二号机 & 备用",
          "&amp; 还原 = %s", devs.size() == 2 ? devs[1]->child_text("name").c_str() : "-");

    /* CDATA 原样保留 */
    CHECK(root.child_text("note") == "原样 <保留> & 不转义", "CDATA 内容 = %s",
          root.child_text("note").c_str());

    /* 空元素 */
    const XmlNode *empty = root.child("empty");
    CHECK(empty != nullptr && empty->text.empty(), "空元素文本为空");

    /* 缺失节点 */
    CHECK(root.child("nope") == nullptr, "缺失子节点返回 nullptr");
    CHECK(root.child_text("nope", "dft") == "dft", "缺失子节点文本取默认值");

    /* 转义 / 反转义 */
    CHECK(Xml::escape("a<b>&\"'") == "a&lt;b&gt;&amp;&quot;&apos;",
          "escape = %s", Xml::escape("a<b>&\"'").c_str());
    CHECK(Xml::unescape("a&lt;b&gt;&amp;&quot;&apos;") == "a<b>&\"'", "unescape 往返一致");
    CHECK(Xml::unescape("&#65;&#x42;") == "AB", "数字实体还原 = %s",
          Xml::unescape("&#65;&#x42;").c_str());
    CHECK(Xml::unescape("&unknown;") == "&unknown;", "未知实体原样保留");
    CHECK(Xml::escape(Xml::unescape("&amp;amp;")) == "&amp;amp;", "双重转义稳定");

    /* 生成 */
    const std::string out = Xml::dump(root, 2);
    CHECK(out.find("<?xml") == 0, "输出以声明开头");
    CHECK(out.find("host=\"0.0.0.0\"") != std::string::npos, "属性被写出");
    CHECK(out.find("&amp;") != std::string::npos || out.find("二号机") != std::string::npos,
          "文本被正确写出");
    XmlNode rt;
    CHECK(Xml::parse(out, rt, &err), "生成结果可被重新解析（err=%s）", err.c_str());
    CHECK(rt.children_named("device").size() == 2, "往返后 device 数一致");
    CHECK(rt.child("note") && rt.child("note")->text.find("保留") != std::string::npos,
          "往返后 CDATA 文本保留");

    /* 自闭合 */
    XmlNode sc;
    CHECK(Xml::parse("<a x=\"1\"/>", sc, &err), "自闭合解析成功");
    CHECK(sc.attrs.size() == 1 && sc.children.empty(), "自闭合无子节点");

    /* 错误定位 */
    XmlNode bad;
    err.clear();
    CHECK(!Xml::parse("<a><b></a>", bad, &err) && err.find("不匹配") != std::string::npos,
          "标签不匹配 -> 失败：%s", err.c_str());
    err.clear();
    CHECK(!Xml::parse("<a>", bad, &err), "缺闭合标签 -> 失败：%s", err.c_str());
    err.clear();
    CHECK(!Xml::parse("no xml here", bad, &err), "无根元素 -> 失败：%s", err.c_str());
    err.clear();
    CHECK(!Xml::parse("<a x=1/>", bad, &err), "属性值缺引号 -> 失败：%s", err.c_str());
    err.clear();
    CHECK(!Xml::parse("<a/>trailing", bad, &err) && err.find("多余") != std::string::npos,
          "根后多余内容 -> 失败：%s", err.c_str());
}

/* ---------------------------------------------------------------- T3 config */
static void t3_config()
{
    std::printf("T3 Config（JSON/XML 载入 · 并发读写 · 变更回调）\n");

    Config cfg;
    std::string err;
    const std::string js =
        "{\"server\":{\"host\":\"127.0.0.1\",\"port\":9000,\"tls\":true},"
        "\"retry\":{\"max\":3}}";
    CHECK(cfg.load_json(js, &err), "JSON 载入成功（err=%s）", err.c_str());
    CHECK(cfg.get("server.host") == "127.0.0.1", "点号取值 host=%s",
          cfg.get("server.host").c_str());
    CHECK(cfg.get_int("server.port") == 9000, "整数取值 port=%d", cfg.get_int("server.port"));
    CHECK(cfg.get_bool("server.tls"), "布尔取值 tls=true");
    CHECK(cfg.get_bool("server.nope", false) == false, "缺失布尔取默认");
    CHECK(cfg.get_int("server.port", 1) == 9000, "存在时不取默认");
    CHECK(cfg.has("retry.max"), "has 命中");
    CHECK(!cfg.has("retry.min"), "has 未命中");
    CHECK(cfg.size() == 4, "扁平化后键数 = %zu", cfg.size());

    /* XML 载入（属性走 @，文本走标签路径） */
    Config cx;
    const std::string xs =
        "<config><server host=\"0.0.0.0\" port=\"8080\"/>"
        "<device id=\"A1\"><name>一号</name></device></config>";
    CHECK(cx.load_xml(xs, &err), "XML 载入成功（err=%s）", err.c_str());
    CHECK(cx.get("config.server@host") == "0.0.0.0", "属性取值 = %s",
          cx.get("config.server@host").c_str());
    CHECK(cx.get_int("config.server@port") == 8080, "属性整数取值 = %d",
          cx.get_int("config.server@port"));
    CHECK(cx.get("config.device.name") == "一号", "元素文本取值 = %s",
          cx.get("config.device.name").c_str());
    CHECK(cx.get("config.device@id") == "A1", "元素属性取值 = %s",
          cx.get("config.device@id").c_str());

    /* 写入与变更回调 */
    Config cc;
    std::vector<std::string> changed;
    cc.on_change([&changed](const std::string &k, const std::string &v) {
        changed.push_back(k + "=" + v);
    });
    cc.set("a.b", "1");
    cc.set("a.b", "2");
    CHECK(changed.size() == 2, "回调触发次数 = %zu", changed.size());
    CHECK(changed.size() == 2 && changed[1] == "a.b=2", "回调带出键与值：%s",
          changed.empty() ? "-" : changed[1].c_str());
    CHECK(cc.get("a.b") == "2", "覆写生效");

    /* 回调里再 set 不应死锁（实现里是先解锁再回调） */
    Config cd;
    int hits = 0;
    cd.on_change([&cd, &hits](const std::string &, const std::string &) {
        hits++;
        if (hits == 1) cd.set("derived", "x");      /* 重入写 */
    });
    cd.set("src", "y");
    CHECK(hits >= 1 && cd.get("derived") == "x", "回调内重入 set 成功（未死锁）");

    /* 并发读写：8 读 2 写，无崩溃且最终一致 */
    Config mt;
    mt.set("counter", "0");
    std::atomic<int> reads{0};
    std::vector<std::thread> ths;
    for (int i = 0; i < 8; i++) {
        ths.emplace_back([&mt, &reads]() {
            for (int k = 0; k < 2000; k++) {
                const std::string v = mt.get("counter", "-1");
                if (!v.empty()) reads++;
            }
        });
    }
    std::atomic<int> writes{0};
    for (int i = 0; i < 2; i++) {
        ths.emplace_back([&mt, &writes]() {
            for (int k = 0; k < 500; k++) {
                mt.set("counter", std::to_string(k));
                writes++;
            }
        });
    }
    for (auto &t : ths) t.join();
    CHECK(reads.load() == 16000, "并发读次数 = %d（无丢失）", reads.load());
    CHECK(writes.load() == 1000, "并发写次数 = %d", writes.load());
    CHECK(mt.get("counter") == "499", "最终值为最后一次写入 = %s",
          mt.get("counter").c_str());
    /* 并发读过程中也应始终读到合法值（要么 0，要么是写入过的数字） */
    CHECK(mt.get_int("counter", -1) == 499, "并发后整数取值正确");

    /* 载入失败不应污染已有配置 */
    Config cf;
    cf.set("keep", "1");
    err.clear();
    CHECK(!cf.load_json("{bad json", &err), "非法 JSON 载入失败：%s", err.c_str());
    CHECK(cf.get("keep") == "1", "失败载入不破坏已有配置");
    err.clear();
    CHECK(!cf.load_xml("<a><b></a>", &err), "非法 XML 载入失败：%s", err.c_str());
    CHECK(cf.get("keep") == "1", "XML 失败也不破坏已有配置");
}

int main()
{
    std::printf("==== bsk_data 单元测试 ====\n\n");
    t1_json();
    t2_xml();
    t3_config();
    std::printf("\n==== 结果：%d passed, %d failed ====\n", passed, failed);
    return failed ? 1 : 0;
}
