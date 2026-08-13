# EchoEar 第二阶段：验收清单

## 功能验收

- 新 MCP 歌曲在音频开始后 3 秒内显示歌名、歌手和可用封面。
- 唱片平滑缓慢旋转，播放音频无明显卡顿。
- LRC 显示上一句、当前句、下一句，当前句与声音偏差不超过 ±700 ms。
- 缺少封面、歌词、清单或网络中断时仍能播放，并显示规定的降级界面。
- 旧 `/stream/<token>` 参数仍可播放。
- 唤醒、聆听、说话和错误状态能覆盖音乐界面，回到空闲后恢复；播放结束后恢复表情系统。

## 自动检查

- 开启和关闭 `CONFIG_ECHOEAR_MUSIC_SCENE` 均可编译。
- MCP Provider、媒体代理、标准 MCP 与 WebSocket 桥接测试全部通过。
- 清单版本、令牌路由、歌词大小、图片大小与尺寸限制均有测试覆盖。

保存串口日志后执行基础检查：

```bash
python3 scripts/verify_echoear_phase2.py echoear-music-phase2.log
```

结构化媒体加载耗时和交互覆盖/恢复检查：

```bash
python3 scripts/verify_echoear_phase2.py \
  --structured --interaction echoear-music-phase2.log
```

完整稳定性门槛：

```bash
python3 scripts/verify_echoear_phase2.py --full echoear-music-phase2-soak.log
```

固件会输出统一的 `MUSIC_METRIC` 日志，包含元数据、歌词、封面/兜底耗时、断流续传、音频欠载、音乐层覆盖切换，以及退出 5 秒后的内存差值。歌词与声音的 ±700 ms 偏差和旋转观感仍需人工确认。

主动停止时，如果下载缓冲中仍有未播放数据，还会输出
`MUSIC_METRIC audio_buffer_release`；其 `bytes` 和 `chunks` 应大于零，随后
应出现 `MUSIC_METRIC workers_reaped`，且 `resources released` 的内部 SRAM
与 PSRAM 差值均应不低于 -8 KiB。

## 合入后稳定性门槛

- 连续 50 次对话。
- 连续 20 次点播、结束或主动停止。
- 连续 20 次网络断开和恢复。
- 持续运行 8 小时，无崩溃、看门狗、掉电复位或 WakeNet 无法恢复。
- 停止后 5 秒内释放音乐图片资源；空闲内部 SRAM 相比基线下降不超过 8 KiB。
