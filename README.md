# realtime-av — 自研传输层的实时音视频通话系统

C++17 实现的 1v1 / 多人实时音视频通话系统。**传输层完全自研**:RTP 打包(RFC 3550/6184)、
UDP 传输、JitterBuffer、NACK 重传、码率自适应全部手写;FFmpeg 仅用于 H.264/Opus 编解码。

| 组件 | 技术 |
|------|------|
| 视频链路 | V4L2 (mmap) 采集 → libswscale → x264 (zerolatency) → 自研 RTP/FU-A → UDP |
| 音频链路 | PulseAudio 采集 → libopus (VOIP 32k) → RTP → 多路混音播放 |
| 渲染 | Qt6 QOpenGLWidget,三纹理 + BT.601 shader |
| 弱网对抗 | 自研 JitterBuffer(扩展序号消回绕)+ NACK/PLI + 丢包率码率自适应 |
| 信令 | epoll 单线程 Reactor,TCP `[4B长度][JSON]` 帧协议(裸写粘包处理) |
| 多人会议 | 自研 SFU:选择性转发不解码、每路下行独立 NACK 缓存、入会 PLI 快速出图 |

## 实测数据(tc netem 弱网注入)

**10% 丢包 + 50±20ms 抖动**,10 秒通话:

| 指标 | 开 NACK | 关 NACK(对照) |
|------|---------|----------------|
| 真实丢包恢复率 | **100%**(309/309) | 0 |
| 跳帧次数 | **0** | 288 |
| 重复重传开销 | ~7%(30ms 乱序消解窗口优化前为 37%) | — |

| 开 NACK:画面清晰 | 关 NACK:参考帧破损 |
|---|---|
| ![with-nack](docs/images/netem-with-nack.png) | ![without-nack](docs/images/netem-without-nack.png) |

**30% 持续丢包**下码率自适应(丢包驱动 AIMD,简版 GCC 思想):
`1500 → 1200 → 960 → 768 → 614 → 491 → 393 kbps` 逐级退避,网络恢复后加性回升。

**SFU 三人会议**(BRIO 摄像头 + 虚拟摄像头 + 纯接收端,第三人中途入会即刻出图):

![room](docs/images/sfu-room-grid.png)

## 构建与运行

```bash
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libavdevice-dev libopus-dev libx264-dev qt6-base-dev libpulse-dev \
    libv4l-dev v4l-utils ffmpeg
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j
```

```bash
# 服务端(同机或云主机)
./build-rel/signal_server/signal_server 6000 <SFU_IP>:7000
./build-rel/sfu_server/sfu_server 7000

# 客户端(GUI):填服务器 IP → 登录 → 呼叫在线用户,或输入房间号进房
./build-rel/client/avclient

# 弱网演示
sudo tc qdisc add dev lo root netem loss 10% delay 50ms 20ms   # 注入
sudo tc qdisc del dev lo root                                   # 恢复
```

常用调试参数:`--loopback`(本地编解码环回)、`--no-nack`(弱网对照)、
`--server ip --user 名字 --call 对端 / --join 房间`(自动化)、
`--self-test out.png`(无人值守验收抓图)。

## 目录结构

```
common/    rtp/(RTP 打包/解包/NACK/历史缓存) net/(UDP/epoll) protocol/(信令帧)
client/    capture/ codec/ media/(JitterBuffer) audio/ render/ signal/ ui/
signal_server/   epoll 信令服务器
sfu_server/      SFU 转发服务器
tests/           RTP 往返、JitterBuffer 单元测试(ASan)
docs/            ROADMAP.md(8 阶段开发记录)、interview-qa.md、deploy.md
```

## 设计决策与边界

- **坚持 Annex-B 码流**,SPS/PPS 随每个 IDR 重发(中途加入者可解码)
- **MTU 1200,超限 FU-A 分片**——绝不依赖 IP 分片(丢一片废整包)
- **对称 RTP**:收发共用一个 socket;公网走 SFU 中转,客户端主动外连天然过 NAT,
  因此**明确不做** ICE/STUN/TURN
- 音频不重传(20ms 一帧,重传到达即过时),靠有界队列丢旧帧防延迟累积;
  回声消除未自研(测试请戴耳机),可接 speexdsp
- 每阶段验收记录与已踩坑清单见 [docs/ROADMAP.md](docs/ROADMAP.md)
