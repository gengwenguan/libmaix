#!/bin/bash
python3 project.py build
adb -s 20080411 push dist/camera /root/maix_dist

#编译项目，将成果物push到开发板上