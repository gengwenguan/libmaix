#!/bin/bash
adb -s 20080411 pull /root/maix_dist/core dist/
adb -s 20080411 pull /root/maix_dist/run.log dist/
/opt/toolchain-sunxi-musl/toolchain/bin/arm-openwrt-linux-muslgnueabi-gdb dist/camera dist/core

#gdb调试开发板上的崩溃core文件