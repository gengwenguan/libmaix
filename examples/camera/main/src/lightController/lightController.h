/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  lightController.h
  *Author:    gengwenguan
  *Date:      2026-07-26
  *Description:  外接补光灯控制器（GPIO 开关，默认 PH13 = 237）。
  *
  *  背景：监控摄像头本身无补光，外接一路 LED 灯板（VCC/GND/DO），GPIO 只输出
  *        3.3V 控制信号到 DO，灯板另行 5V 供电、与开发板共地。
  *
  *  角色：常驻一条后台线程，按 AppConfig.light_* 每 ~500ms 评估"当前是否该亮"，
  *        通过 sysfs /sys/class/gpio/gpioN/value 写电平。像 mqttReporter 一样
  *        "始终 Start、内部按配置决定是否动作"——light_enabled=false 时空转并
  *        确保灯灭，零额外设备占用。
  *
  *  触发（每 tick 现拉 AppConfig，pull-on-demand，改完即时生效）：
  *    - 未启用 或 不在时段内             -> 灭
  *    - 时段内 & mode=0(常亮)            -> 亮
  *    - 时段内 & mode=1(声控)            -> 麦克风响度 >= 阈值时刷新点亮截止时刻，
  *                                         now < 截止 -> 亮，否则灭
  *    时段支持跨零点（如 22 -> 6 表示北京时间 22:00~次日 06:00）。
  *
  *  GPIO：sysfs 方式。Start 时 export + direction=out + 初始灭；持久打开 value
  *        fd，切换只 write 一字节。Stop 时先灭灯、关 fd、unexport。
  *        编号可配置（light_gpio），不硬编码；light_active_low 兼容低电平点亮灯板。
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class C_LightController
{
public:
    C_LightController() = default;
    ~C_LightController();

    // 启动常驻评估线程。幂等；重复调用只第一次生效。
    void Start();
    // 停止并 join 线程：先灭灯、释放 GPIO。幂等。
    void Stop();

private:
    void Loop();

    // 根据当前 AppConfig + 北京时间(UTC+8) + 麦克风响度，计算"此刻是否该亮"。
    bool EvaluateShouldOn();

    // ---- GPIO (sysfs) ----
    // 按 gpioNum 导出并设为输出；成功后 m_valueFd 持有 value 文件 fd。
    // 已导出/换脚时先释放旧的。返回是否就绪。
    bool EnsureGpioReady(int gpioNum);
    void ReleaseGpio();
    void WriteLevel(bool on);   // 按 m_activeLow 决定写 '1'/'0'

    std::atomic<bool>        m_running{false};
    std::thread              m_thread;
    std::mutex               m_mu;
    std::condition_variable  m_cv;

    // 已就绪的 GPIO 状态（仅评估线程访问，无需锁）
    int   m_gpioNum   = -1;      // 当前已 export 的编号；-1 未就绪
    int   m_valueFd   = -1;      // /sys/class/gpio/gpioN/value 的持久 fd
    bool  m_activeLow = false;   // 当前灯板极性
    int   m_curLevel  = -1;      // 最近写入的逻辑电平（1=亮,0=灭,-1=未知），避免重复写

    // 声控：最近一次"响度超阈值"后应保持点亮到的时刻（steady_clock 毫秒）。
    long long m_soundHoldUntilMs = 0;
};
