# EchoEar 角色表现系统第一阶段技术设计

## 1. 文档目的

本文档把《EchoEar 角色表现系统第一阶段产品设计》转化为可实施的软件方案，重点回答：

- 表情导演放在哪里；
- 业务模块如何发送语义化事件；
- 状态、情绪和临时事件如何抢占与恢复；
- 定时器和显示线程如何保证安全；
- 如何分批落地并保持现有功能可回退。

本文档完成评审后，再进入运行逻辑编码。

## 2. 当前实现问题

当前显示链路为：

```text
Application/MCP/Music
        │
        ├── SetStatus(本地化字符串)
        ├── SetEmotion(云端字符串)
        └── SetMusicInfo(歌曲信息)
                    │
                    ▼
              EmoteDisplay
                    │
                    ▼
              EmoteEngine/GFX
```

存在以下技术问题：

1. `Application::SetDeviceState()` 同时设置状态文字和表情，显示决策散落在业务代码中。
2. `EmoteDisplay::SetStatus()` 通过比较“聆听中...”等中文文本判断状态，无法可靠支持其他语言。
3. `SetEmotion()` 不区分设备基础状态和云端情绪，没有统一优先级。
4. TTS、MCP、音乐和网络事件没有共同的表现事件模型。
5. 表情播放接口只有“立刻换资源”，没有持续时间、抢占、恢复和去重机制。
6. 音乐播放线程和 MCP 工具线程可能从非主任务发起显示更新，后续引入定时行为时容易产生竞态。

## 3. 总体架构

### 3.1 分层

```text
┌─────────────────────────────────────────────┐
│ 业务事件源                                  │
│ Application / Protocol / MCP / Music        │
└──────────────────────┬──────────────────────┘
                       │ DisplayBehaviorRequest
                       ▼
┌─────────────────────────────────────────────┐
│ Display 语义接口                            │
│ 默认实现保持兼容，EchoEar 实现进入导演       │
└──────────────────────┬──────────────────────┘
                       ▼
┌─────────────────────────────────────────────┐
│ ExpressionDirector                         │
│ 状态、优先级、抢占、超时、恢复、随机微动作   │
└──────────────────────┬──────────────────────┘
                       │ RenderModel
                       ▼
┌─────────────────────────────────────────────┐
│ EmoteDisplay / EmoteEngine                  │
│ AAF、图标、文字、可见性和资源缓存             │
└─────────────────────────────────────────────┘
```

### 3.2 作用域

导演作为 EchoEar 板级能力实现，不强迫其他开发板采用 AAF 动画。但语义事件类型和 `Display` 虚接口放在公共显示层，方便未来其他带屏设备复用。

第一阶段不建立全局通用 UI 框架，也不改写现有 LVGL 显示体系。

## 4. 文件组织

计划新增：

```text
main/display/display_behavior.h
main/boards/echoear/expression_director.h
main/boards/echoear/expression_director.cc
```

计划修改：

```text
main/display/display.h
main/display/display.cc
main/application.cc
main/boards/echoear/emote_display.h
main/boards/echoear/emote_display.cc
main/boards/common/esp32_music.cc
main/mcp_server.cc
main/CMakeLists.txt
```

如果构建系统已经自动收集 EchoEar 板级 `.cc` 文件，则不额外修改根级源文件列表。

## 5. 语义事件接口

### 5.1 行为枚举

建议新增：

```cpp
enum class DisplayBehavior {
    kStartup,
    kConnecting,
    kIdle,
    kWakeAcknowledged,
    kListening,
    kThinking,
    kToolRunning,
    kSpeaking,
    kSuccess,
    kRecoverableError,
    kFatalError,
    kMusicBuffering,
    kMusicPlaying,
    kMusicPaused,
};
```

行为枚举表达“设备正在做什么”，不表达具体动画文件。

### 5.2 事件来源

```cpp
enum class DisplayBehaviorSource {
    kDeviceState,
    kWakeWord,
    kTts,
    kMcp,
    kMusic,
    kNetwork,
    kSystem,
    kTimer,
};
```

来源仅用于日志和决策追踪，不参与业务显示文案。

### 5.3 请求结构

```cpp
struct DisplayBehaviorRequest {
    DisplayBehavior behavior;
    DisplayBehaviorSource source;
    std::string detail;
    int duration_ms = 0;
};
```

约束：

- `detail` 是可选短文案，不参与状态判断。
- `duration_ms == 0` 表示使用导演中的行为默认值。
- 请求对象持有自己的 `detail` 字符串，可安全复制到主事件队列。

### 5.4 Display 接口

在 `Display` 中新增默认无操作虚方法：

```cpp
virtual void SetBehavior(const DisplayBehaviorRequest& request);
```

采用默认无操作实现，避免一次性修改全部显示驱动。EchoEar 的 `EmoteDisplay` 覆盖该方法。

保留现有 `SetStatus()`、`SetEmotion()` 和 `SetMusicInfo()`：

- `SetStatus()` 继续负责用户可见文字；
- `SetEmotion()` 在 EchoEar 中转换为低优先级云端情绪请求；
- `SetMusicInfo()` 保持现有音乐 UI 行为；
- 新代码不得再从 `SetStatus()` 的本地化字符串推断业务状态。

## 6. ExpressionDirector

### 6.1 职责

`ExpressionDirector` 只负责决策，不直接依赖 Application、Protocol、MCP 或 Music：

- 接收基础状态和临时事件；
- 接收云端情绪；
- 计算优先级；
- 决定是否抢占、忽略或去重；
- 管理最短展示时间和超时；
- 临时行为结束后根据当前基础状态重新计算；
- 调度待机微动作；
- 生成最终 `RenderModel` 交给 `EmoteDisplay`。

### 6.2 内部模型

```cpp
enum class ExpressionPriority : uint8_t {
    kIdle = 0,
    kEmotion = 1,
    kMedia = 2,
    kTask = 3,
    kOutput = 4,
    kInput = 5,
    kCritical = 6,
};

struct ActiveBehavior {
    DisplayBehavior behavior;
    DisplayBehaviorSource source;
    ExpressionPriority priority;
    int64_t started_at_us;
    int64_t expires_at_us;
    bool persistent;
};
```

导演维护：

- `base_behavior_`：由设备状态映射出的持久行为；
- `active_behavior_`：当前真正显示的行为；
- `cloud_emotion_` 及过期时间；
- `last_interaction_at_us_`；
- `last_idle_action_`；
- `enabled_` 和 `idle_motion_enabled_`；
- 一个单次软件定时器。

### 6.3 公共方法

建议接口：

```cpp
class ExpressionDirector {
public:
    using RenderCallback = std::function<void(const RenderModel&)>;

    explicit ExpressionDirector(RenderCallback render_callback);
    ~ExpressionDirector();

    void SetBaseBehavior(const DisplayBehaviorRequest& request);
    void PostTransientBehavior(const DisplayBehaviorRequest& request);
    void SetCloudEmotion(const char* emotion);
    void NotifyInteraction();
    void SetEnabled(bool enabled);
    void SetIdleMotionEnabled(bool enabled);

private:
    void Recompute(const char* reason);
    void ScheduleNextDeadline();
    void OnTimer();
};
```

基础行为和临时行为分开，防止“成功动画结束后永远回到 idle”，而设备实际上已经进入 listening。

## 7. 状态映射

### 7.1 DeviceState

| DeviceState | DisplayBehavior |
| --- | --- |
| `Unknown` / `Starting` | `kStartup` |
| `WifiConfiguring` | `kConnecting`，文字仍由原界面负责 |
| `Idle` | `kIdle` |
| `Connecting` | `kConnecting` |
| `Listening` | `kListening` |
| `Speaking` | `kSpeaking` |
| `Upgrading` | `kStartup`，保留升级文字 |
| `Activating` | `kConnecting` |
| `AudioTesting` | `kSpeaking` 或专用测试表现 |
| `FatalError` | `kFatalError` |

`Application::SetDeviceState()` 在状态更新后调用一次 `SetBehavior()`。原有 `SetStatus()` 保留，原有面向 EchoEar 的固定 `SetEmotion("neutral")` 在导演启用后不再承担状态控制。

### 7.2 WakeNet

`OnWakeWordDetected()` 在成功命中、开始打开音频通道前发送 `kWakeAcknowledged`，默认持续 400 ms。

如果连接过程超过唤醒确认时长，由 `kConnecting` 接管；进入 listening 后由更高优先级的 `kListening` 接管。

唤醒失败或音频通道打开失败时，不保留唤醒动画，按实际连接/错误状态重算。

### 7.3 TTS

- `tts.start`：基础状态通常已切换到 `Speaking`，补发 `kSpeaking` 以降低消息时序差异。
- `tts.stop`：不直接指定返回动画，由新的 DeviceState 触发重算。
- abort：协议确认前可以结束说话临时行为，但最终以 DeviceState 为准。
- `tts.sentence_start` 只更新字幕，不重启动画。

### 7.4 MCP

`McpServer::DoToolCall()` 在工具线程启动前发送 `kToolRunning`，工具名转换为安全的本地短文案：

- 音乐工具：`正在找歌`；
- 其他工具：`正在处理`。

工具完成：

- 正常返回：发送 `kSuccess`，默认 800 ms；
- 抛出异常或返回协议错误：发送 `kRecoverableError`，默认 1800 ms；
- 完成后由导演恢复当前基础状态。

不能把工具参数、音乐 URL 或账号信息放入显示文案和日志。

第一阶段允许多个 MCP 请求覆盖为一个“工具执行中”状态，但导演应维护活动工具计数，防止先完成的请求错误结束仍在运行的工具状态。

### 7.5 音乐

第一阶段接入以下生命周期：

- `StartStreaming()` 已创建播放任务：`kMusicBuffering`；
- 首个有效音频帧开始输出：`kMusicPlaying`；
- `StopStreaming()`：结束音乐行为；
- 下载、解析或解码失败：`kRecoverableError`；
- 自然播放结束：结束音乐行为并恢复实际基础状态。

音乐状态不能依赖 `is_playing_` 单个布尔值推断，因为当前该标志同时覆盖下载、缓冲和播放生命周期。

第一阶段只发送语义事件，不重做歌词/频谱 UI。

## 8. 行为渲染映射

导演输出与动画资源解耦：

```cpp
struct RenderModel {
    int animation_asset_id;
    int fps;
    bool repeat;
    int icon_asset_id;
    bool show_top_animation;
    std::string text;
};
```

第一版映射建议：

| 行为 | 现有资源 |
| --- | --- |
| 启动 | idle |
| 连接 | thinking |
| 待机 | idle |
| 唤醒确认 | happy |
| 聆听 | listen 顶部动画＋idle/专用聆听眼神 |
| 思考 | thinking |
| 工具执行 | thinking |
| 说话 | happy，后续可添加自然说话资源 |
| 成功 | enjoy/happy |
| 一般失败 | dizzy/sad |
| 严重错误 | shocked/angry |
| 音乐缓冲 | thinking |
| 音乐播放 | 保持现有音乐 UI |

导演需要记录上一份 `RenderModel`，完全相同则不重新设置动画源。

## 9. 云端情绪

`EmoteDisplay::SetEmotion()` 将标准字符串交给 `ExpressionDirector::SetCloudEmotion()`，不再直接调用 `setEyes()`。

云端情绪：

- 优先级固定为 Emotion；
- 默认有效期 5 秒；
- 只在 idle 或允许情绪叠加的 speaking 风格中生效；
- 未知值只记录一次限频日志；
- `crying -> sad`、`sleepy -> idle`、`surprised -> shocked`；
- 不支持的细分情绪使用最接近的非冲突资源，不统一映射为 happy。

第一阶段 speaking 仍以“正在说话”为主，是否使用云端情绪选择说话风格作为后续增强，不放入首个实现提交。

## 10. 定时与线程模型

### 10.1 原则

- 只有 Application 主事件循环执行表现决策和最终渲染。
- MCP、音乐下载和播放线程不得直接操作导演内部状态或 GFX 对象。
- `esp_timer` 回调只投递任务，不直接渲染。
- `EmoteEngine` 现有锁继续保护 GFX 操作，但它不是业务状态同步手段。

### 10.2 投递方式

非主线程调用 `Display::SetBehavior()` 时，EchoEar 实现将请求复制后通过 `Application::Schedule()` 投递到主事件循环。

为避免递归投递，`EmoteDisplay` 提供一个只在主循环内部使用的私有处理方法，例如 `ApplyBehaviorOnMainTask()`。

如果后续发现 Display 不应依赖 Application，则第二阶段提取独立 UI Event Queue；第一阶段避免扩大改造范围。

### 10.3 单定时器

导演只使用一个单次定时器，始终指向最近期限：

- 临时行为过期；
- 云端情绪过期；
- 下一次待机微动作；
- 困倦阈值。

每次状态变化重新计算最近期限，避免为每种行为创建不同定时器。

## 11. 待机微动作

首个代码版本不引入外部动画资产，先实现调度框架和可验证行为：

- idle 稳定 5 秒后才启动微动作调度；
- 8～25 秒随机间隔；
- 使用现有 idle 动画的重播、速度变化或可用片段；
- 同一动作不连续执行；
- 30 秒内最多 3 次；
- 5 分钟后进入困倦候选，但在没有合适资源时继续使用慢 idle；
- 任意 P0～P5 事件立即取消微动作定时。

引入 Brookesia 的 blink/sleep AAF 资源单独作为后续提交，附带来源、提交号和许可证说明，不与状态机首个提交混合。

## 12. 开关与回退

新增 EchoEar Kconfig：

```text
CONFIG_ECHOEAR_EXPRESSION_DIRECTOR=y
CONFIG_ECHOEAR_IDLE_MICRO_MOTIONS=y
CONFIG_ECHOEAR_CLOUD_EMOTION=y
```

要求：

- 导演关闭时保留当前 `SetStatus()`/`SetEmotion()` 行为；
- 微动作可单独关闭；
- 云端情绪可单独关闭；
- 新增逻辑只在 EchoEar 编译配置中启用，不增加其他板子的运行开销。

## 13. 日志

使用 `ESP_LOGI/W/D/E`，TAG 建议为 `Expression`。

状态切换日志：

```text
idle -> listening source=device_state priority=input preempt=true
cloud_happy ignored active=listening reason=lower_priority
tool_success expired restore=listening
```

降噪规则：

- 相同请求去重，不重复打印 INFO；
- 未知云端情绪按名称限频；
- 待机微动作使用 DEBUG；
- 资源加载失败使用 ERROR，并包含资源枚举，不打印地址或用户数据。

## 14. 测试设计

### 14.1 主机侧逻辑测试

如果现有 ESP-IDF 测试结构不便直接运行，先将导演的优先级和状态计算设计成不依赖 GFX 的纯 C++ 逻辑，以便增加最小测试程序。

必须覆盖：

- 高优先级抢占低优先级；
- 低优先级请求被忽略；
- 临时行为过期后恢复最新基础状态；
- 云端情绪过期；
- 相同请求去重；
- MCP 活动计数；
- 状态变化取消待机微动作；
- 未知情绪不改变当前行为。

### 14.2 设备侧验证

串口日志与实际屏幕同时验证：

1. 冷启动、联网、待机；
2. 唤醒、聆听、思考、说话、待机完整循环；
3. TTS 中途触摸或唤醒中止；
4. 搜歌成功、失败和超时；
5. 音乐自然结束、手动停止、被唤醒打断；
6. 断网与自动重连；
7. 待机 10 分钟并随机唤醒；
8. 连续 50 轮对话与 20 次音乐播放。

重点观察：

- WakeNet 是否始终恢复；
- 动画切换是否卡死；
- 音频是否出现爆音或丢帧；
- 空闲堆内存是否持续下降；
- 非主线程是否触发 GFX 错误。

## 15. 提交拆分

建议按以下顺序形成独立、可回退提交：

1. `refactor: add semantic display behavior interface`
   - 新增公共事件类型和 Display 默认接口；
   - Application 发送基础状态；
   - 不改变现有画面。
2. `feat(echoear): add expression director`
   - 优先级、超时、恢复、日志；
   - 修正云端情绪映射；
   - 接入关键对话状态。
3. `feat(echoear): integrate tool and music expression events`
   - MCP 活动计数；
   - 音乐缓冲、播放、结束和失败。
4. `feat(echoear): add idle micro motions`
   - 单定时器和受约束随机调度；
   - 不引入新资产。
5. `feat(echoear): add licensed blink and sleep assets`
   - 可选提交；
   - 单独记录来源和许可证。

每个提交都应能够独立编译。完成第 2、3、4 个提交后分别进行设备冒烟测试，避免最后一次性定位状态问题。

## 16. 已确认的第一轮实现决策

以下决策已经产品确认：

1. 允许从 Apache-2.0 许可的乐鑫资源中引入少量慢眨眼、快速眨眼、观察和睡眠动画；状态机稳定后以独立提交引入，并记录来源、版本和许可证。首个代码提交不引入新 AAF 资源。
2. 工具文案按类别区分：音乐显示“正在找歌”，网络显示“正在连接”，其他 MCP 显示“正在处理”。不显示内部工具名、URL 或参数。
3. 困倦候选默认 5 分钟；唤醒、触摸和对话重新计时，音乐播放期间不进入困倦，且困倦状态不关闭 WakeNet、不主动说话。
4. 第一阶段先使用固定说话动画，不加入音频能量驱动；待状态机稳定后再评估 PCM/RMS 驱动方案。
5. 表情导演默认开启，同时提供 Kconfig 回退开关。

## 17. 完成定义

技术阶段完成需满足：

- 所有行为入口均为语义事件，不再依赖中文状态字符串决策；
- 状态优先级、超时、恢复和线程模型在代码中可追踪；
- EchoEar 可通过开关回退原实现；
- 其他开发板编译与行为不受影响；
- 产品文档中的第一阶段功能验收项全部有对应测试步骤；
- 实机完成唤醒、对话、MCP 搜歌、音乐结束和断网恢复验证。
