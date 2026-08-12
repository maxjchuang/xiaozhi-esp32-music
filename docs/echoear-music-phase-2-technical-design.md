# EchoEar 第二阶段：沉浸式音乐界面技术设计

## 协议

设备工具 `self.online_music.play_music` 新增可选字符串参数 `metadata_url`。旧参数保持不变。设备仅接受 `http://私网IPv4:8765/media/<token>/manifest.json`，音频同时兼容旧 `/stream/<token>` 与新 `/media/<token>/audio`。

清单版本为 `schema_version: 1`，可包含标题、歌手、专辑、时长、两张 JPEG 资源和 LRC 歌词。清单内资源必须与清单同源、同令牌并使用固定文件名，避免设备读取任意 URL。

## 播放链路

1. `MusicPlaybackRequest` 将音频 URL、显示名称和可选清单 URL 传给 `Esp32Music`。
2. 音频下载和解码线程立即启动，音乐场景先显示默认主题。
3. 低优先级元数据线程读取不超过 64 KiB 的清单，并校验版本和字段长度。
4. 歌词沿用 LRC 解析器；显示层接收上一句、当前句和下一句。
5. 背景和唱片 JPEG 各限制 512 KiB，必须分别解码为 360 × 360 和 192 × 192 RGB565。
6. 播放代次编号使上一首歌的异步结果失效。停止或自然结束时释放场景资源。

## 显示实现

`Display` 增加默认无操作/兼容方法，其他开发板不受影响。EchoEar 在既有 `esp_emote_gfx` 引擎中创建音乐图片和文本对象，不初始化第二套 LVGL 显示驱动。

图片缓冲位于 PSRAM。唱片保留源 RGB565 和 RGB565A8 帧缓冲，200 ms 定时器使用定点逆向映射和最近邻采样生成旋转帧，圆外 alpha 为零。背景由 MCP 端预先裁剪、模糊并暗化，设备只负责 JPEG 解码。

配置开关为 `CONFIG_ECHOEAR_MUSIC_SCENE`，默认开启。关闭时结构化播放和歌词链路仍可工作，显示回退到原有通用接口。

## 资源上限

- 清单：64 KiB
- 单张 JPEG：512 KiB
- 背景：360 × 360 RGB565A8，约 380 KiB
- 唱片源和旋转帧：约 180 KiB
- MCP 上游原图：5 MiB、最大 4096 × 4096

