@echo off
REM 快速检查服务器端口
REM 使用方法：双击运行或命令行执行

echo ========================================
echo   服务器端口快速检查
echo ========================================
echo.

set SERVER_IP=124.220.224.189

echo 目标服务器: %SERVER_IP%
echo.

REM 检查视频服务器端口
echo [1/3] 检查视频服务器端口 (8890)...
powershell -Command "Test-NetConnection -ComputerName %SERVER_IP% -Port 8890 -InformationLevel Quiet" >nul 2>&1
if %errorlevel% equ 0 (
    echo   ✅ 端口 8890 开放
) else (
    echo   ❌ 端口 8890 关闭
)

REM 检查信令服务器端口
echo [2/3] 检查信令服务器端口 (7777)...
powershell -Command "Test-NetConnection -ComputerName %SERVER_IP% -Port 7777 -InformationLevel Quiet" >nul 2>&1
if %errorlevel% equ 0 (
    echo   ✅ 端口 7777 开放
) else (
    echo   ❌ 端口 7777 关闭
)

REM 检查文件上传端口
echo [3/3] 检查文件上传端口 (5000)...
powershell -Command "Test-NetConnection -ComputerName %SERVER_IP% -Port 5000 -InformationLevel Quiet" >nul 2>&1
if %errorlevel% equ 0 (
    echo   ✅ 端口 5000 开放
) else (
    echo   ❌ 端口 5000 关闭
)

echo.
echo ========================================
echo   检查完成
echo ========================================
echo.
pause
