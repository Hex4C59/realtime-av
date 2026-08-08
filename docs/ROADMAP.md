# 实时音视频通话系统 — 路线图

目标:自研传输层的 1v1/多人音视频通话系统(简历项目,求职音视频开发岗)。

技术决策:FFmpeg 只做 H.264/Opus 编解码;RTP 打包、UDP 传输、JitterBuffer、NACK 全部自研;
客户端 Qt + QOpenGLWidget;信令服务器 epoll + TCP;后期 SFU 转发服务器。
明确不做:NAT 穿透(ICE/STUN/TURN)、自研 AEC。

预计周期:每天 2~3 小时约 4~6 个月。

## 阶段清单

### ✅ 阶段 0:环境搭建 + 前置学习(1~2 周)
- [x] git 仓库 + CMake 骨架 + 目录结构
- [x] hello_ffmpeg:链接 FFmpeg 并检查 H.264/Opus 编解码器
- [x] 安装依赖:`sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavdevice-dev libopus-dev libx264-dev qt6-base-dev libpulse-dev libv4l-dev v4l-utils ffmpeg`
- [x] `cmake -B build && cmake --build build && ./build/examples/hello_ffmpeg` 验收通过(2026-07-26)
- [ ] C++ 补课(限时):RAII、智能指针、thread/mutex/condition_variable、lambda、移动语义
- [ ] 音视频扫盲(≤3 天):YUV、I/P/B 帧、码率/GOP、容器 vs 编码格式
- [ ] 用 ffplay 玩熟摄像头:`ffplay /dev/video0`
- 验收:程序输出 FFmpeg 版本且编解码器齐全;能口算 YUV420P 一帧字节数(w×h×1.5)

### ✅ 阶段 1:本地采集 + 渲染回显(2026-07-26 验收通过)
- [x] `client/capture/` V4L2 采集(ioctl 能力查询 + REQBUFS/mmap 队列,YUYV→YUV420P)
- [x] `client/render/` QOpenGLWidget,Y/U/V 三纹理 + fragment shader 转 RGB(BT.601)
- [x] 有界阻塞队列 `common/util/blocking_queue.h`(阶段 2 编码线程启用)
- [x] 设备枚举 + UI 设备下拉框;调试工具 `examples/capture_dump`
- [x] `--self-test` 自检模式:抓渲染 framebuffer 存 PNG,后续各阶段自动化验收复用
- [x] 验收:BRIO 下 3 秒渲染 94 帧(≈31fps),自检画面颜色/比例正确
- 注:内置摄像头(Bison 5986:2113)被 EC 固件锁死 camera_power=0,sysfs 写入被回弹,
  软件层无法打开;需按 F8 摄像头键或查 BIOS,不排除硬件故障。开发统一用 BRIO(/dev/video2)。

### ✅ 阶段 2:H.264 编解码本地环回(2026-07-26 验收通过)
- [x] `client/codec/` VideoEncoder/VideoDecoder(send_frame/receive_packet 模型)
- [x] 编码参数:preset=ultrafast, tune=zerolatency, max_b_frames=0,
      不设 GLOBAL_HEADER → SPS/PPS 自动在每个 IDR 前重发
- [x] 环回链路:采集线程 → BlockingQueue → 编解码线程(编码→立即解码)→ 渲染;
      UI 加"编码环回"开关 + 实时码率显示;dump loopback.h264
- [x] avclient 加 --loopback 参数,配合 --self-test 自动化验收
- [x] 验收:Release 版环回 30fps 流畅;ffprobe 解码 102/102 帧
      (Constrained Baseline);xxd 可见 00000001 起始码 + 67(SPS)/68(PPS)
- [x] 修复:渲染重绘请求合并(防事件队列被塞满饿死定时器)
- 注:Debug(ASan)构建退出时会停滞约 18 秒——ASan 分配器 × NVIDIA 闭源驱动
  GL 上下文销毁时的大块 realloc 冲突,属环境问题非代码 bug;性能测量用 build-rel

### ✅ 阶段 3:RTP 打包 + UDP 单向传输(2026-07-26 验收通过)
- [x] `common/rtp/` RtpHeader(RFC 3550,手写大端序列化)+
      RtpPacketizer/RtpDepacketizer(RFC 6184:Single NAL / FU-A,MTU 1200)
- [x] `common/net/` UdpSocket 封装(poll 超时接收)
- [x] avclient --send ip:port / --recv port 模式(--recv 不占摄像头)
- [x] 单元测试 tests/rtp_roundtrip_test:打包→解包字节级一致、
      seq/marker/ts 语义、丢包弃帧,ASan 下全过
- [x] 验收:双进程 127.0.0.1 实测,接收端 3 秒解码渲染 60 帧,画面正确;
      tshark 抓包 600 包/64 帧:seq 逐包+1、同帧同 timestamp、PT=96、
      SSRC 恒定、marker 仅帧尾;IDR 帧 FU-A 分片正确(指示字节 0x7c,
      尾片 FU header E 位置 1),SPS/PPS 随每个 IDR 重发
- 注:解包器为按序简化版,乱序/丢包重传由阶段 6 JitterBuffer 接管

### ✅ 阶段 4:信令服务器 + 双向 1v1 通话(2026-07-27 单机验收通过)
- [x] `signal_server/` epoll 单线程 Reactor;TCP 帧协议 `[4字节大端长度][JSON]`
      (裸写粘包/半包处理,见 common/protocol/signal_message.h 的 FrameParser)
- [x] `common/protocol/` 消息:login/user_list/call/answer/media_info/hangup/heartbeat
      + nlohmann/json(third_party)
- [x] 客户端:SignalClient(QTcpSocket + 同一 FrameParser)、
      呼叫状态机(Idle→Calling/Ringing→InCall)、呼叫 UI、通话中本地小窗
- [x] 对称 RTP:收发共用一个 UDP socket,随机绑定端口经 media_info 交换;
      每通新 SSRC 防上一通残包污染
- [x] CLI 自动化:--server/--user/--call/--auto-answer + v4l2loopback 虚拟摄像头
- [x] 验收(单机双实例 + 虚拟摄像头):两轮完整"呼叫→双向视频→挂断→重呼",
      A 端抓图=对端彩条、B 端抓图=对端 BRIO 画面,掉线清理正确
- [ ] 局域网两台真机互通(需第二台机器,手动验证)

### ✅ 阶段 5:音频链路 + 音画同步(2026-07-27 自动化验收通过)
- [x] `client/audio/` PulseAudio Simple API 采集/播放(48kHz/S16LE/单声道,
      960 samples=20ms 对齐 Opus 帧;采集 fragsize/播放 tlength 都收紧降延迟)
- [x] libopus 编解码(VOIP 模式 32kbps);音频独立 SSRC、PT=97,
      复用同一 UDP socket,接收端按 payload type 分流
- [x] 时间戳同源:音视频都用发送端毫秒钟(视频×90、音频×48),
      接收端标题栏实时显示音画偏差
- [x] 自动化验收:PulseAudio null-sink 环回,B 假麦克风喂 440Hz 正弦,
      A 端录制扬声器输出——双向各 ~390 包/8s 无丢,440Hz 窄带电平接近
      全频段、带外低 10dB+,证明音频端到端内容正确
- [ ] 人耳验收:双机戴耳机通话确认"声音清晰、口型对齐"(需第二台机器)

### ✅ 阶段 6:弱网对抗 — JitterBuffer + NACK(2026-07-27 验收通过,核心亮点)
- [x] `client/media/jitter_buffer`:扩展序号(64 位单调,int16 差值消回绕)、
      乱序重排、去重、gap 检测、队头超时(450ms)跳帧
- [x] NACK 调度:首包等 30ms 乱序消解窗口(实测把重复重传从 37% 压到 ~7%)、
      重发间隔 100ms(≥RTT)、单包上限 4 次后放弃等跳帧;RTT 动态估计留作扩展
- [x] 控制包 `common/rtp/rtp_control.h`:PT=127 自定义 NACK/PLI(简化版 RTCP);
      发送端 RtpHistory 环形缓存 1024 包响应重传;PLI→编码器强制 IDR
- [x] 统计:UI 标题栏丢包/恢复/NACK/缓冲深度;通话结束打印完整 jb stats
- [x] --no-nack 对照开关
- [x] 单元测试 6 项(乱序/去重/回绕/NACK 调度/重试上限/超时跳帧)ASan 全过
- [x] 验收(netem loss 10% delay 50ms±20ms,10 秒通话):
      开 NACK:真实丢包 309 全部恢复、0 跳帧、画面清晰锐利;
      关 NACK:288 次跳帧、画面持续拖影破损(对比截图留存)
- [ ] 录屏对比视频(面试素材,建议真人出镜手动录制)
- 注:stats 的 lost_detected 含抖动乱序造成的瞬时缺口(30ms 内自愈不计恢复),
      真实丢包数 ≈ recovered + skipped

### ✅ 阶段 7:SFU 多人会议(2026-07-27 验收通过)
- [x] `sfu_server/` 单线程 UDP 转发器:UDP JOIN 包注册成员(源地址即身份,
      1s 保活/5s 超时清理)、房间内转发 RTP(不解码——SFU 与 MCU 的本质区别)、
      每路视频独立 NACK 缓存(下行重传由 SFU 服务,不打扰发送端)、
      新人入会向老成员发 PLI 快速出图
- [x] 控制协议:NACK/PLI 带目标 SSRC(多路流必须指明是哪路);JOIN 包(PT=126)
- [x] 客户端多路化:按 SSRC 的 RemoteStream 表(每路独立 JitterBuffer/
      解包/解码/小窗),1v1 与房间统一走该路径;宫格布局 UI 线程建窗、
      原子指针发布给接收线程
- [x] 音频多路混音:AudioPlayer 重写为按 SSRC 队列 + 每 20ms 逐样本
      int32 叠加饱和裁剪,pa 阻塞写就是播放时钟
- [x] 信令 join_room→sfu_info;UI 房间号输入 + 进房/离开;--join 自动化
- [x] 验收:3 客户端(BRIO/虚拟摄像头/无摄像头)同房互通,音频每人收到
      另外两路(~1030 包/10s),C 晚 4 秒入会即刻双路出图(入会 PLI 生效),
      整窗抓图确认宫格双路画面;1v1 回归测试通过
- [ ] 公网部署(阶段 8):信令+SFU 放云服务器,客户端主动连 SFU 过 NAT

### 🔶 阶段 8:打磨 + 求职材料(2026-07-27 自动化部分完成)
- [x] 码率自适应:RR 控制包(每秒回报未恢复丢包率)+ 丢包驱动 AIMD
      (>10% 乘 0.8 / <2% 加 50k,简版 GCC),编码器动态重配;
      实测 30% 丢包下 1500→393kbps 逐级退避
- [x] README:架构表、弱网量化数据、验收截图(docs/images/)、构建运行说明
- [x] docs/interview-qa.md 面试问答稿(全部结合本项目真实代码与数据)
- [x] docs/deploy.md 云部署指南
- [ ] 【用户任务】购买/使用云主机,按 deploy.md 部署公网演示
- [ ] 【用户任务】录 5 分钟演示视频(重点:弱网开关 NACK 对比、三人会议)
- [ ] 【用户任务】GitHub 建仓推送(git remote add + push --tags)
- [ ] 【用户任务】双机真人实测(音频戴耳机、口型同步、局域网 1v1)

## 关键坑备忘

1. Annex-B vs AVCC 别搞混:全程坚持 Annex-B
2. x264 不设 zerolatency 会有几百 ms 编码延迟(lookahead + B 帧)
3. 三套时钟(系统 ms / 视频 90kHz / 音频 48kHz)在打包层统一换算
4. 别依赖 IP 分片,超 MTU 必须 FU-A
5. Qt:非主线程碰 UI/OpenGL 会随机崩溃,解码帧走信号槽 QueuedConnection
6. 很多摄像头 720p+ 只出 MJPEG,采集层要做格式协商
7. seq 16 位约 90 秒回绕,比较必须回绕安全;重呼叫换新 SSRC
8. NACK 必须有重试上限,防高丢包下重传风暴
9. 每阶段结束打 git tag + 录演示视频(防退化 + 攒面试素材)
10. 全程开 ASan,buffer 一律 vector/shared_ptr,禁裸 new
11. 本机 ASan × NVIDIA 驱动:GL 上下文销毁时停滞 ~18s(teardown-only,无害);
    测延迟/帧率一律用 build-rel(Release)
12. Qt 高频跨线程投递 update() 必须合并(pending 标志),否则事件队列
    被塞满会饿死定时器等其他事件
13. 自定义命令行参数不能撞 Qt 内置参数(-name/-style/-geometry 等,
    双杠写法同样被 QApplication 吞掉)——曾因 --name 被吃调试半天
14. 双实例自动化互测时,先退出的一方会触发对端挂断切回预览;
    抓图验收必须在双方都存活时进行(自检抓图后延迟 2s 再退出)

## 简历条目(定稿,数字均为实测)

> **实时音视频通话系统(C++17/Qt6/FFmpeg,个人项目)**
> - 从零实现 1v1 通话与 SFU 多人会议:V4L2 mmap 采集、H.264/Opus 低延迟编解码、
>   自研 RTP 打包(RFC 3550/6184,FU-A 分片)与 UDP 传输、OpenGL shader 渲染 YUV
> - 自研弱网对抗:扩展序号 JitterBuffer + NACK/PLI + 丢包驱动码率自适应(AIMD),
>   10% 丢包 + 50ms 抖动下丢包恢复率 100%、零跳帧(对照组跳帧 288 次);
>   通过 30ms 乱序消解窗口把无效重传从 37% 降至 7%
> - 自研 epoll 信令服务器(TCP 长度前缀帧协议、粘包处理)与 SFU 转发服务器
>   (选择性转发不解码、每路下行独立 NACK 缓存、入会 PLI 1~2s 出图)
> - 多线程流水线(采集/编码/传输/解码/混音/渲染),自实现有界阻塞队列 +
>   满丢最旧背压;RTP/JitterBuffer 均有 ASan 单元测试

注意:没真正用过 WebRTC 就不要写"熟悉 WebRTC";但要能按 interview-qa.md
里的映射表把自己的模块对应到 WebRTC 组件(NetEQ/GCC/RTCP)。
