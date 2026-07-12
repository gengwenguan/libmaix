/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  mqttClient.h
  *Author:    gengwenguan
  *Date:      2026-07-12
  *Description:  极简 MQTT 3.1.1 客户端（over 明文 TCP），只实现"发布一条消息"。
  *
  *  设计取舍（64MB / arm musl 板）：
  *    - 不引第三方库（mosquitto / paho），纯 libc socket + 手写报文，二进制零膨胀。
  *    - 只支持 QoS0 发布：一次 PublishOnce() 完成
  *        connect TCP → 发 CONNECT → 读 CONNACK → 发 PUBLISH → 发 DISCONNECT → close。
  *      "按需短连接"语义：不维持长连接、不做心跳、不订阅、不支持遗嘱/TLS。
  *    - 全程带超时，任何一步失败都返回 false，不抛异常、不残留 fd。
  *
  *  适用场景：低频（秒级以上）状态上报，例如 IPv6 地址变化通知。
  *  broker 地址支持域名 / IPv4 / IPv6（内部走 getaddrinfo）。
**********************************************************************************/
#pragma once
#include <string>

namespace MqttClient {

// 向 broker 发布一条 QoS0 消息，成功返回 true。
//   host        : broker 域名或 IP（v4/v6 均可）
//   port        : broker 端口（明文 MQTT 通常 1883）
//   clientId    : MQTT client id（同 broker 下需唯一，多设备请区分）
//   topic       : 发布主题
//   payload     : 消息内容（本项目用纯 IPv6 字符串）
//   retain      : 是否让 broker 保留该消息（新订阅者一连上即收到最后一条）
//   keepAliveSec: CONNECT 里声明的 keepalive（短连接下仅占位，默认 60）
//   timeoutMs   : 单次网络操作（connect / send / recv）超时，默认 5000ms
bool PublishOnce(const std::string& host, int port,
                 const std::string& clientId,
                 const std::string& topic,
                 const std::string& payload,
                 bool retain      = false,
                 int keepAliveSec = 60,
                 int timeoutMs    = 5000);

} // namespace MqttClient
