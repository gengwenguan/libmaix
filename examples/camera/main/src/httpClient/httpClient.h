/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  httpClient.h
  *Author:    gengwenguan
  *Date:      2026-07-18
  *Description:  极简出站 HTTP/1.0 POST 客户端（仅明文 http://）。
  *
  *  设计取舍（64MB / arm musl 板，参照 mqttClient 同款套路）：
  *    - 不引第三方库（curl / libevent），纯 libc socket + 手写请求行，二进制零膨胀。
  *    - 只实现"发一条 POST 拿回状态码"：
  *        解析 URL → getaddrinfo → 超时 connect → 发请求头(+body) → 读状态行 → close。
  *      "按需短连接"语义：不复用连接、不跟随重定向、不解析响应体、不支持 https/TLS。
  *    - 全程带超时，任何一步失败都返回结构化错误，不抛异常、不残留 fd。
  *
  *  适用场景：把 web 上配置的"设备动作"（开门 / 开灯 / 关灯）代理成一次到
  *  局域网设备（http://）的 POST 触发。https:// 目标不支持（会返回 err）。
**********************************************************************************/
#pragma once
#include <string>

namespace HttpClient {

struct PostResult {
    bool        ok         = false;  // 网络链路成功且拿到了 HTTP 状态行
    int         status     = 0;      // 对端返回的 HTTP 状态码（ok=true 时有效）
    std::string err;                 // ok=false 时的原因（英文短语）
};

// 向 url 发一次 POST。body 为空则 Content-Length: 0。
//   url        : 仅支持 http://host[:port][/path]，host 支持域名 / IPv4 / [IPv6]
//   body       : 请求体（可空）
//   contentType: 请求体 MIME（body 非空时写入头；空 body 时忽略）
//   timeoutMs  : 单次网络操作（connect / send / recv）超时，默认 5000ms
// 返回 ok=true 表示成功收到状态行（status 有效，可能是 2xx/4xx/5xx）；
// 返回 ok=false 表示 URL 非法 / DNS 失败 / 连接超时 / 发送失败等链路级错误。
PostResult Post(const std::string& url,
                const std::string& body        = std::string(),
                const std::string& contentType = "application/json",
                int                timeoutMs    = 5000);

} // namespace HttpClient
