# EchoEar 角色表现系统第一阶段验收指南

本文档把产品设计中的功能与稳定性验收项转换为可重复执行的设备测试。

## 1. 准备

构建并烧录 EchoEar 固件后，保存完整串口输出：

```bash
idf.py -p /dev/cu.usbmodem1201 monitor | tee echoear-phase1.log
```

端口名按实际设备修改。测试期间不要输出账号、Cookie 或带鉴权参数的音乐 URL。

## 2. 冒烟验收

依次完成一次：

1. 冷启动并联网；
2. 唤醒、说一句话、等待回答结束；
3. 成功搜索并播放一首歌，然后手动停止；
4. 搜索一首不存在的歌曲，观察失败反馈；
5. 断开并恢复网络；
6. 保持待机，直到五种微动作和 5 分钟困倦均出现；
7. 在微动作和困倦状态中分别使用唤醒词，确认立即响应。

执行日志检查：

```bash
python3 scripts/verify_echoear_phase1.py echoear-phase1.log
```

如果测试被分成多次串口会话，可以一次传入多个日志，脚本会合并统计并正确处理设备时间戳重置：

```bash
python3 scripts/verify_echoear_phase1.py echoear-startup.log echoear-music.log echoear-idle.log
```

## 3. 完整稳定性验收

在一份连续日志中完成：

- 50 轮“唤醒—提问—回答—待机”；
- 20 次音乐播放和停止，至少包含一次自然结束和一次唤醒打断；
- 20 次网络断开和恢复；
- 连续运行至少 8 小时。

然后执行：

```bash
python3 scripts/verify_echoear_phase1.py --full echoear-phase1.log
```

脚本检查状态覆盖、循环次数、崩溃/看门狗错误、最长连续运行时间，以及待机状态 SRAM 首尾下降是否超过 8 KiB。仅比较待机样本，避免把对话或音乐缓冲区的正常占用误判为泄漏。测试者仍需观察有无爆音、明显丢帧和儿童错过说话时机。

## 4. 回退验证

分别关闭以下 Kconfig 后编译一次：

- `CONFIG_ECHOEAR_IDLE_MICRO_MOTIONS`：待机保持稳定中性表情；
- `CONFIG_ECHOEAR_CLOUD_EMOTION`：本地对话状态仍完整；
- `CONFIG_ECHOEAR_EXPRESSION_DIRECTOR`：回退原有 EchoEar 显示逻辑。

所有回退构建都必须保持 WakeNet、语音对话和音乐播放可用。
