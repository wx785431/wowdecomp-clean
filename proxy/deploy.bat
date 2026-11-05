@echo off
chcp 936 >nul
setlocal

rem 把编译好的 DINPUT8.dll 部署到客户端目录
rem 用法：把本 bat 和 DINPUT8.dll 放在一起，双击，或  deploy.bat "D:\World of for"

set GAME=%~1
if "%GAME%"=="" set GAME=D:\World of for

if not exist "%~dp0DINPUT8.dll" (
    echo [x] 没找到 DINPUT8.dll，请先把云编译的产物放到本 bat 同目录
    pause
    exit /b 1
)
if not exist "%GAME%\Wow.exe" (
    echo [x] 目录里没有 Wow.exe：%GAME%
    pause
    exit /b 1
)

if exist "%GAME%\dinput8.dll" (
    if not exist "%GAME%\dinput8_x86.dll.bak" (
        echo [1/2] 把旧的 32 位 dinput8.dll 改名为 dinput8_x86.dll.bak
        ren "%GAME%\dinput8.dll" "dinput8_x86.dll.bak"
    ) else (
        echo [1/2] 32 位 dinput8.dll 已经备份过，跳过
    )
) else (
    echo [1/2] 目录里没有旧 dinput8.dll，跳过
)

echo [2/2] 复制 x64 代理 DLL
copy /y "%~dp0DINPUT8.dll" "%GAME%\DINPUT8.dll" >nul
if errorlevel 1 (
    echo [x] 复制失败，可能游戏正在运行
    pause
    exit /b 1
)

echo.
echo 完成。启动 Wow.exe 即可，客户端目录会出现 WowProxy64.log 供排查。
pause
