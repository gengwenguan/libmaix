::使用该脚本调用客户端程序可以更方便连接服务器和服务器进行交互

@echo off 
::设置CMD窗口字体颜色为0a 在CMD中输入命令 color /? 可查看颜色列表
color 0a
::设置CMD窗口显示模式为100列宽 20行高
::MODE con: COLS=100 LINES=20
MODE con: COLS=200 LINES=99999
::设置窗口标题
title buildsystem_1_0_0

set gNetAddr=127.0.0.1:19123
set gInputCmd=NetAdaptHelp
::编译出的可执行文件的名称
set gExeName=netadaptshellnetclient.exe


::第一次启动先配置服务器地址
call:funcServerConfig

::第一次启动先打印一次服务器所有支持的命令
call:funcShowServerSupportCmd

:start
  
  @Rem help show all supported shell cmd.
  set /p gInputCmd=输入命令(NetAdaptHelp):
  
  echo  --- %gExeName% %gNetAddr% %gInputCmd%

  :: 可执行文件名称  服务器地址:端口  命令 参数[可选]
  cmd /c %gExeName% %gNetAddr% %gInputCmd%

  echo.
  
  goto :start

::退出
:end



::
:: 服务器地址端口配置
::
:funcServerConfig
  echo.
  set /p serverIp=请输入服务器ip:
  set serverPort=19123
  set gNetAddr=%serverIp%:%serverPort%

  echo ------------------------------------------
  echo connect server: %gNetAddr%
  echo ------------------------------------------
  echo.
goto:eof


::
:: 显示服务器所有支持的命令
::
:funcShowServerSupportCmd
  echo.
  echo  -----------------------------------------------------
  echo    ALL Support Shell Cmd:
  echo  -----------------------------------------------------

  ::向服务器发送NetAdaptHelp命令获取服务器所有支持的命令
  cmd /c %gExeName% %gNetAddr% "NetAdaptHelp"
  
  echo  -----------------------------------------------------
goto:eof


