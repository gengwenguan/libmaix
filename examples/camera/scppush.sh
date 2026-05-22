#!/bin/bash
python3 project.py build
sshpass -p "root" scp -r -O ~/work/libmaix/examples/camera/dist/camera root@192.168.1.11:/root/maix_dist

#停止旧的camera进程
sshpass -p "root" ssh root@192.168.1.11 "killall camera 2>/dev/null || true"

#创建启动脚本
sshpass -p "root" ssh root@192.168.1.11 "cd /root/maix_dist && ./start_app.sh"

#编译项目，将成果物push到开发板上，杀掉老进程启动新进程