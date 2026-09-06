# 服务器端问题排查指南

## 📊 问题现象

从你的日志看：
```
W (45131) CAMERA_APP: ⚠️ 连接视频服务器失败，重试 1/3
W (65631) CAMERA_APP: ⚠️ 连接视频服务器失败，重试 2/3
I (62122) SIG: ✅ 已连接到云服务器信令基站！  ← 信令服务器正常
```

**关键发现**：
- ✅ 信令服务器（端口7777）：正常连接
- ❌ 视频服务器（端口8890）：连接失败
- ⚠️ MQTT：发布失败

## 🔍 问题诊断

### 1. 网络是否通畅？

**证据**：信令服务器可以正常连接
```
I (62122) SIG: ✅ 已连接到云服务器信令基站！
```

**结论**：✅ 网络是通的，不是网络问题

### 2. 视频服务器是否运行？

**可能原因**：
1. ❌ 视频服务器进程未运行
2. ❌ 视频服务器端口8890未开放
3. ❌ 防火墙阻止了端口8890
4. ❌ 视频服务器崩溃或死锁

### 3. 端口状态检查

| 端口 | 服务 | 状态 | 说明 |
|------|------|------|------|
| 8890 | 视频推流 | ❌ 关闭 | **问题所在** |
| 7777 | 信令服务器 | ✅ 开放 | 正常 |
| 5000 | 文件上传 | ❓ 未知 | 需要检查 |

## 🛠️ 解决方案

### 方案1：检查视频服务器进程

```bash
# 登录服务器
ssh root@124.220.224.189

# 检查视频服务器进程
ps aux | grep video_server

# 或者检查所有Python进程
ps aux | grep python

# 如果没有运行，启动视频服务器
python video_server.py
```

### 方案2：检查端口是否被占用

```bash
# 检查端口8890
netstat -tulpn | grep 8890

# 或者使用lsof
lsof -i :8890

# 如果端口被占用，杀死进程
kill -9 <PID>
```

### 方案3：检查防火墙规则

```bash
# 查看防火墙状态
sudo ufw status

# 允许端口8890
sudo ufw allow 8890

# 或者使用iptables
sudo iptables -A INPUT -p tcp --dport 8890 -j ACCEPT

# 重启防火墙
sudo ufw reload
```

### 方案4：检查服务器日志

```bash
# 查看视频服务器日志
tail -f /var/log/video_server.log

# 或者查看系统日志
journalctl -u video_server

# 查看最近的错误
grep -i error /var/log/video_server.log
```

### 方案5：重启视频服务器

```bash
# 停止视频服务器
pkill -f video_server.py

# 等待2秒
sleep 2

# 启动视频服务器
nohup python video_server.py > /var/log/video_server.log 2>&1 &

# 验证启动
ps aux | grep video_server
```

## 📋 快速检查命令

### 在服务器上执行

```bash
# 1. 检查端口监听状态
netstat -tulpn | grep -E "8890|7777|5000"

# 2. 检查进程状态
ps aux | grep -E "video_server|signal_server|upload_server"

# 3. 检查防火墙
sudo ufw status | grep -E "8890|7777|5000"

# 4. 检查服务器日志
tail -20 /var/log/video_server.log

# 5. 测试端口连通性
telnet localhost 8890
```

### 在客户端（ESP32）上执行

```bash
# 使用Python检查脚本
python tools/server_check.py

# 或者使用快速检查
tools/quick_check.bat  # Windows
```

## 🔧 常见问题及解决

### 问题1：视频服务器未启动

**现象**：
```
❌ 无法连接视频服务器
```

**解决**：
```bash
# 启动视频服务器
cd /path/to/server
python video_server.py
```

### 问题2：端口被防火墙阻止

**现象**：
```
⚠️ 连接视频服务器失败，重试 1/3
```

**解决**：
```bash
# 允许端口
sudo ufw allow 8890

# 或者临时关闭防火墙测试
sudo ufw disable
```

### 问题3：服务器资源不足

**现象**：
```
连接超时
连接被拒绝
```

**解决**：
```bash
# 检查CPU和内存
top
htop

# 检查磁盘空间
df -h

# 清理日志文件
sudo journalctl --vacuum-time=7d
```

### 问题4：网络配置错误

**现象**：
```
DNS解析失败
网络不可达
```

**解决**：
```bash
# 检查网络配置
ifconfig
ip addr

# 检查路由
route -n

# 测试网络连通性
ping 8.8.8.8
```

## 📊 诊断流程图

```
开始诊断
    ↓
检查网络连通性
    ├─ 失败 → 检查网络配置
    └─ 成功 → 检查端口状态
                ↓
          端口8890状态
          ├─ 关闭 → 启动服务器/检查防火墙
          └─ 开放 → 检查服务器日志
                    ↓
              查看错误日志
              ├─ 有错误 → 根据错误修复
              └─ 无错误 → 检查资源使用
                          ↓
                    CPU/内存/磁盘
                    ├─ 过高 → 清理资源
                    └─ 正常 → 检查代码逻辑
```

## 🎯 最佳实践

### 1. 监控脚本

```bash
#!/bin/bash
# save as monitor.sh

while true; do
    # 检查视频服务器
    if ! netstat -tulpn | grep -q ":8890"; then
        echo "⚠️ 视频服务器未运行，正在启动..."
        nohup python /path/to/video_server.py > /var/log/video_server.log 2>&1 &
    fi

    # 检查端口
    if ! nc -z localhost 8890; then
        echo "❌ 端口8890不通"
    fi

    sleep 60
done
```

### 2. 自动重启服务

```bash
# 使用systemd创建服务
sudo tee /etc/systemd/system/video-server.service <<EOF
[Unit]
Description=Video Streaming Server
After=network.target

[Service]
ExecStart=/usr/bin/python3 /path/to/video_server.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

# 启用服务
sudo systemctl enable video-server
sudo systemctl start video-server
```

### 3. 日志轮转

```bash
# /etc/logrotate.d/video-server
/var/log/video_server.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
}
```

## 📞 联系支持

如果以上方法都无法解决问题，请提供：

1. **服务器日志**：`/var/log/video_server.log`
2. **端口状态**：`netstat -tulpn | grep -E "8890|7777|5000"`
3. **进程状态**：`ps aux | grep python`
4. **防火墙状态**：`sudo ufw status`

---

**服务器端检查工具已准备好！** 🛠️
