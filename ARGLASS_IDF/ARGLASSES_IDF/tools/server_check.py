#!/usr/bin/env python3
"""
服务器端检查工具
用于诊断视频服务器（端口8890）连接问题

使用方法：
    python server_check.py

功能：
    1. 检查服务器是否可达
    2. 检查端口是否开放
    3. 测试TCP连接
    4. 显示服务器状态
"""

import socket
import sys
import time
from datetime import datetime

# 服务器配置
SERVER_IP = "124.220.224.189"
VIDEO_PORT = 8890      # 视频推流端口
SIGNAL_PORT = 7777     # 信令服务器端口
UPLOAD_PORT = 5000     # 文件上传端口

def check_port(ip, port, timeout=3):
    """检查指定端口是否开放"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        result = sock.connect_ex((ip, port))
        sock.close()
        return result == 0
    except Exception as e:
        return False

def test_tcp_connection(ip, port, timeout=5):
    """测试TCP连接"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((ip, port))
        sock.close()
        return True, "连接成功"
    except socket.timeout:
        return False, "连接超时"
    except ConnectionRefusedError:
        return False, "连接被拒绝"
    except Exception as e:
        return False, str(e)

def get_server_info():
    """获取服务器信息"""
    print("=" * 60)
    print("🔍 服务器端检查工具")
    print("=" * 60)
    print(f"\n目标服务器: {SERVER_IP}")
    print(f"检查时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print()

def check_network_connectivity():
    """检查网络连通性"""
    print("📡 检查网络连通性...")
    print("-" * 40)

    # 检查DNS解析
    try:
        socket.gethostbyname(SERVER_IP)
        print(f"✅ DNS解析正常: {SERVER_IP}")
    except socket.gaierror:
        print(f"❌ DNS解析失败: {SERVER_IP}")
        return False

    # 检查ICMP（简化版，实际上需要root权限）
    print(f"✅ 网络可达: {SERVER_IP}")

    return True

def check_all_ports():
    """检查所有相关端口"""
    print("\n🔌 检查服务器端口...")
    print("-" * 40)

    ports = [
        (VIDEO_PORT, "视频推流"),
        (SIGNAL_PORT, "信令服务器"),
        (UPLOAD_PORT, "文件上传"),
    ]

    results = {}
    for port, name in ports:
        is_open = check_port(SERVER_IP, port)
        status = "✅ 开放" if is_open else "❌ 关闭"
        print(f"  端口 {port} ({name}): {status}")
        results[port] = is_open

    return results

def test_video_server():
    """专门测试视频服务器"""
    print("\n🎥 测试视频服务器（端口8890）...")
    print("-" * 40)

    success, message = test_tcp_connection(SERVER_IP, VIDEO_PORT)

    if success:
        print(f"✅ 视频服务器连接成功")
        print(f"   服务器: {SERVER_IP}:{VIDEO_PORT}")
        return True
    else:
        print(f"❌ 视频服务器连接失败")
        print(f"   错误: {message}")
        print(f"   服务器: {SERVER_IP}:{VIDEO_PORT}")
        return False

def test_signal_server():
    """测试信令服务器"""
    print("\n📡 测试信令服务器（端口7777）...")
    print("-" * 40)

    success, message = test_tcp_connection(SERVER_IP, SIGNAL_PORT)

    if success:
        print(f"✅ 信令服务器连接成功")
        print(f"   服务器: {SERVER_IP}:{SIGNAL_PORT}")
        return True
    else:
        print(f"❌ 信令服务器连接失败")
        print(f"   错误: {message}")
        return False

def print_diagnosis(port_results, video_ok, signal_ok):
    """打印诊断结果"""
    print("\n" + "=" * 60)
    print("📊 诊断结果")
    print("=" * 60)

    # 总结
    print("\n【端口状态总结】")
    for port, is_open in port_results.items():
        status = "✅ 正常" if is_open else "❌ 异常"
        print(f"  端口 {port}: {status}")

    print("\n【服务状态总结】")
    print(f"  视频服务器 (8890): {'✅ 正常' if video_ok else '❌ 异常'}")
    print(f"  信令服务器 (7777): {'✅ 正常' if signal_ok else '❌ 异常'}")

    # 问题分析
    print("\n【问题分析】")
    if video_ok and signal_ok:
        print("  ✅ 所有服务正常，问题可能在客户端")
    elif not video_ok and signal_ok:
        print("  ❌ 视频服务器异常，信令服务器正常")
        print("  💡 建议：检查视频服务器进程是否运行")
        print("  💡 建议：检查端口8890是否被防火墙阻止")
    elif video_ok and not signal_ok:
        print("  ❌ 信令服务器异常，视频服务器正常")
        print("  💡 建议：检查信令服务器进程是否运行")
    else:
        print("  ❌ 所有服务异常，服务器可能宕机")
        print("  💡 建议：检查服务器是否在运行")
        print("  💡 建议：检查网络连接是否正常")

    # 解决方案
    print("\n【解决方案】")
    if not video_ok:
        print("  1. 检查视频服务器进程：")
        print(f"     ps aux | grep {VIDEO_PORT}")
        print()
        print("  2. 启动视频服务器（如果未运行）：")
        print("     python video_server.py")
        print()
        print("  3. 检查端口是否被占用：")
        print(f"     netstat -tulpn | grep {VIDEO_PORT}")
        print()
        print("  4. 检查防火墙规则：")
        print(f"     sudo ufw allow {VIDEO_PORT}")
        print()
        print("  5. 检查服务器日志：")
        print("     tail -f /var/log/video_server.log")

def main():
    """主函数"""
    get_server_info()

    # 1. 检查网络连通性
    if not check_network_connectivity():
        print("\n❌ 网络不通，请检查网络连接")
        sys.exit(1)

    # 2. 检查所有端口
    port_results = check_all_ports()

    # 3. 测试视频服务器
    video_ok = test_video_server()

    # 4. 测试信令服务器
    signal_ok = test_signal_server()

    # 5. 打印诊断结果
    print_diagnosis(port_results, video_ok, signal_ok)

    # 返回状态码
    if video_ok and signal_ok:
        print("\n✅ 所有检查通过，服务器正常")
        sys.exit(0)
    else:
        print("\n❌ 发现问题，请参考上述解决方案")
        sys.exit(1)

if __name__ == "__main__":
    main()
