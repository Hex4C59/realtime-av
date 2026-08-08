# 云服务器部署(公网演示)

目标:信令 + SFU 放公网云主机,两台 NAT 后的电脑通过房间互通。
客户端全部**主动外连** SFU(UDP 打出去,回包走同一 NAT 映射),无需 ICE/STUN。

## 服务器侧(Ubuntu 22.04+)

```bash
# 只需要两个服务端二进制,无 GUI 依赖(不用装 Qt/FFmpeg 全家桶)
sudo apt install -y build-essential cmake
git clone <你的仓库> && cd realtime-av
# 只编服务端目标
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target signal_server sfu_server -j

# 运行(建议 tmux/systemd)
./build/sfu_server/sfu_server 7000 &
./build/signal_server/signal_server 6000 <云主机公网IP>:7000 &
```

注意:signal_server 的第二个参数是**告诉客户端的 SFU 地址**,必须填公网 IP。

## 安全组 / 防火墙放行

| 端口 | 协议 | 用途 |
|------|------|------|
| 6000 | TCP | 信令 |
| 7000 | UDP | SFU 媒体 |

## 客户端侧

GUI 里服务器填云主机公网 IP → 登录 → 输入房间号进房。
1v1 直呼模式在双方都在 NAT 后时不可用(media_info 交换的是内网地址),
公网场景请统一用房间(SFU 中转)。

## 验证

- SFU 日志应出现两条 join;
- 客户端标题栏丢包/恢复计数正常增长;
- 公网丢包是真实的,NACK 恢复数 > 0 属正常现象,正好当演示素材。
