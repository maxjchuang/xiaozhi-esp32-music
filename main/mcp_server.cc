/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

 #include "mcp_server.h"
 #include <esp_log.h>
 #include <esp_app_desc.h>
 #include <algorithm>
 #include <cstring>
 #include <cctype>
 #include <esp_pthread.h>
 
 #include "application.h"
 #include "display.h"
 #include "board.h"
 #include "boards/common/esp32_music.h"
 
 #define TAG "MCP"
 
 #define DEFAULT_TOOLCALL_STACK_SIZE 6144

 namespace {

 bool IsPrivateAudioProxyUrl(const std::string& url) {
     constexpr char kPrefix[] = "http://";
     constexpr char kStreamMarker[] = ":8765/stream/";
     if (url.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0 || url.size() > 2048) {
         return false;
     }

     const auto marker = url.find(kStreamMarker, sizeof(kPrefix) - 1);
     if (marker == std::string::npos ||
         url.find(kStreamMarker, marker + 1) != std::string::npos) {
         return false;
     }

     const auto token = url.substr(marker + sizeof(kStreamMarker) - 1);
     if (token.size() < 16 || token.size() > 128 ||
         !std::all_of(token.begin(), token.end(), [](unsigned char ch) {
             return std::isalnum(ch) || ch == '-' || ch == '_';
         })) {
         return false;
     }

     const auto address = url.substr(sizeof(kPrefix) - 1, marker - (sizeof(kPrefix) - 1));
     int octets[4] = {};
     size_t start = 0;
     for (size_t index = 0; index < 4; ++index) {
         const auto end = index == 3 ? address.size() : address.find('.', start);
         if (end == std::string::npos || end <= start || end - start > 3) {
             return false;
         }
         int value = 0;
         for (size_t cursor = start; cursor < end; ++cursor) {
             const auto ch = static_cast<unsigned char>(address[cursor]);
             if (!std::isdigit(ch)) {
                 return false;
             }
             value = value * 10 + (ch - '0');
         }
         if (value > 255) {
             return false;
         }
         octets[index] = value;
         start = end + 1;
     }

     return octets[0] == 10 ||
            (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
            (octets[0] == 192 && octets[1] == 168);
 }

 bool IsApprovedAudioUrl(const std::string& url) {
     constexpr char kApprovedPrefix[] = "https://dl.espressif.com/";
     if (url.empty() || url.size() > 2048 ||
         (url.compare(0, sizeof(kApprovedPrefix) - 1, kApprovedPrefix) != 0 &&
          !IsPrivateAudioProxyUrl(url))) {
         return false;
     }
     return std::none_of(url.begin(), url.end(), [](unsigned char ch) {
         return ch < 0x20 || ch == 0x7f;
     });
 }

 bool ContainsControlCharacter(const std::string& value) {
     return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
         return ch < 0x20 || ch == 0x7f;
     });
 }

 void PostMcpBehavior(DisplayBehavior behavior, const std::string& detail, int duration_ms) {
     Application::GetInstance().Schedule([behavior, detail, duration_ms]() {
         auto display = Board::GetInstance().GetDisplay();
         if (display) {
             display->SetBehavior({
                 behavior,
                 DisplayBehaviorSource::kMcp,
                 detail,
                 duration_ms,
             });
         }
     });
 }

 }  // namespace
 
 McpServer::McpServer() {
 }
 
 McpServer::~McpServer() {
     for (auto tool : tools_) {
         delete tool;
     }
     tools_.clear();
 }
 
 void McpServer::AddCommonTools() {
     // To speed up the response time, we add the common tools to the beginning of
     // the tools list to utilize the prompt cache.
     // Backup the original tools list and restore it after adding the common tools.
     auto original_tools = std::move(tools_);
     auto& board = Board::GetInstance();
 
     AddTool("self.get_device_status",
         "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
         "Use this tool for: \n"
         "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
         "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
         PropertyList(),
         [&board](const PropertyList& properties) -> ReturnValue {
             return board.GetDeviceStatusJson();
         });
 
     AddTool("self.audio_speaker.set_volume", 
         "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
         PropertyList({
             Property("volume", kPropertyTypeInteger, 0, 100)
         }), 
         [&board](const PropertyList& properties) -> ReturnValue {
             auto codec = board.GetAudioCodec();
             codec->SetOutputVolume(properties["volume"].value<int>());
             return true;
         });
     
     auto backlight = board.GetBacklight();
     if (backlight) {
         AddTool("self.screen.set_brightness",
             "Set the brightness of the screen.",
             PropertyList({
                 Property("brightness", kPropertyTypeInteger, 0, 100)
             }),
             [backlight](const PropertyList& properties) -> ReturnValue {
                 uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                 backlight->SetBrightness(brightness, true);
                 return true;
             });
     }
 
    auto display = board.GetDisplay();
    if (display && display->SupportsExpressionTest()) {
        AddTool("self.screen.test_expressions",
            "Run the device's deterministic expression self-test. You MUST use this tool when the user asks to test, preview, cycle, or inspect all facial expressions. The screen displays each expression name while cycling and restores the normal state automatically.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                if (!display->StartExpressionTest()) {
                    return "{\"success\": false, \"message\": \"表情自检已在运行或启动失败\"}";
                }
                return "{\"success\": true, \"message\": \"表情自检已开始，将依次展示全部表情\"}";
            });
    }

    if (display && !display->GetTheme().empty()) {
         AddTool("self.screen.set_theme",
             "Set the theme of the screen. The theme can be `light` or `dark`.",
             PropertyList({
                 Property("theme", kPropertyTypeString)
             }),
             [display](const PropertyList& properties) -> ReturnValue {
                 display->SetTheme(properties["theme"].value<std::string>().c_str());
                 return true;
             });
     }
 
     auto camera = board.GetCamera();
     if (camera) {
         AddTool("self.camera.take_photo",
             "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
             "Args:\n"
             "  `question`: The question that you want to ask about the photo.\n"
             "Return:\n"
             "  A JSON object that provides the photo information.",
             PropertyList({
                 Property("question", kPropertyTypeString)
             }),
             [camera](const PropertyList& properties) -> ReturnValue {
                 if (!camera->Capture()) {
                     return "{\"success\": false, \"message\": \"Failed to capture photo\"}";
                 }
                 auto question = properties["question"].value<std::string>();
                 return camera->Explain(question);
             });
     }
 
     auto music = board.GetMusic();
     if (music) {
         AddTool("self.online_music.play_music",
             "播放由外部 MCP 解析得到的音频。只允许乐鑫官方测试音频或本机 MCP 生成的短期局域网代理地址。\n"
             "参数:\n"
             "  `play_type`: 必须为 'url'。\n"
             "  `url`: 要播放的 HTTP(S) MP3 地址。\n"
             "  `url_song_name`: 屏幕显示的歌曲名称。\n"
             "返回:\n"
             "  播放状态信息。",
             PropertyList({
                 Property("play_type", kPropertyTypeString, "url"),
                 Property("url", kPropertyTypeString),
                 Property("url_song_name", kPropertyTypeString, "在线音频")
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto play_type = properties["play_type"].value<std::string>();
                 auto url = properties["url"].value<std::string>();
                 auto song_name = properties["url_song_name"].value<std::string>();

                 std::transform(play_type.begin(), play_type.end(), play_type.begin(), [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                 });
                 if (play_type != "url") {
                     return "{\"success\": false, \"message\": \"play_type 必须为 url\"}";
                 }
                 if (!IsApprovedAudioUrl(url)) {
                     return "{\"success\": false, \"message\": \"仅允许乐鑫官方测试音频或安全的局域网音乐代理\"}";
                 }
                 if (song_name.size() > 192 || ContainsControlCharacter(song_name)) {
                     return "{\"success\": false, \"message\": \"歌曲名称无效\"}";
                 }
                 if (!music->PlayUrl(url, song_name)) {
                     return "{\"success\": false, \"message\": \"启动音频流失败\"}";
                 }
                 return "{\"success\": true, \"message\": \"音频开始播放\"}";
             });
 
         AddTool("self.music.set_display_mode",
             "设置音乐播放时的显示模式。可以选择显示频谱或歌词，比如用户说‘打开频谱’或者‘显示频谱’，‘打开歌词’或者‘显示歌词’就设置对应的显示模式。\n"
             "参数:\n"
             "  `mode`: 显示模式，可选值为 'spectrum'（频谱）或 'lyrics'（歌词）。\n"
             "返回:\n"
             "  设置结果信息。",
             PropertyList({
                 Property("mode", kPropertyTypeString)//显示模式: "spectrum" 或 "lyrics"
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto mode_str = properties["mode"].value<std::string>();
                 
                 // 转换为小写以便比较
                 std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);
                 
                 if (mode_str == "spectrum" || mode_str == "频谱") {
                     // 设置为频谱显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_SPECTRUM);
                     return "{\"success\": true, \"message\": \"已切换到频谱显示模式\"}";
                 } else if (mode_str == "lyrics" || mode_str == "歌词") {
                     // 设置为歌词显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_LYRICS);
                     return "{\"success\": true, \"message\": \"已切换到歌词显示模式\"}";
                 } else {
                     return "{\"success\": false, \"message\": \"无效的显示模式，请使用 'spectrum' 或 'lyrics'\"}";
                 }
                 
                 return "{\"success\": false, \"message\": \"设置显示模式失败\"}";
             });
     }
 
     // Restore the original tools list to the end of the tools list
     tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
 }
 
 void McpServer::AddTool(McpTool* tool) {
     // Prevent adding duplicate tools
     if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
         ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
         return;
     }
 
     ESP_LOGI(TAG, "Add tool: %s", tool->name().c_str());
     tools_.push_back(tool);
 }
 
 void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
     AddTool(new McpTool(name, description, properties, callback));
 }
 
 void McpServer::ParseMessage(const std::string& message) {
     cJSON* json = cJSON_Parse(message.c_str());
     if (json == nullptr) {
         ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
         return;
     }
     ParseMessage(json);
     cJSON_Delete(json);
 }
 
 void McpServer::ParseCapabilities(const cJSON* capabilities) {
     auto vision = cJSON_GetObjectItem(capabilities, "vision");
     if (cJSON_IsObject(vision)) {
         auto url = cJSON_GetObjectItem(vision, "url");
         auto token = cJSON_GetObjectItem(vision, "token");
         if (cJSON_IsString(url)) {
             auto camera = Board::GetInstance().GetCamera();
             if (camera) {
                 std::string url_str = std::string(url->valuestring);
                 std::string token_str;
                 if (cJSON_IsString(token)) {
                     token_str = std::string(token->valuestring);
                 }
                 camera->SetExplainUrl(url_str, token_str);
             }
         }
     }
 }
 
 void McpServer::ParseMessage(const cJSON* json) {
     // Check JSONRPC version
     auto version = cJSON_GetObjectItem(json, "jsonrpc");
     if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
         ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
         return;
     }
     
     // Check method
     auto method = cJSON_GetObjectItem(json, "method");
     if (method == nullptr || !cJSON_IsString(method)) {
         ESP_LOGE(TAG, "Missing method");
         return;
     }
     
     auto method_str = std::string(method->valuestring);
     if (method_str.find("notifications") == 0) {
         return;
     }
     
     // Check params
     auto params = cJSON_GetObjectItem(json, "params");
     if (params != nullptr && !cJSON_IsObject(params)) {
         ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
         return;
     }
 
     auto id = cJSON_GetObjectItem(json, "id");
     if (id == nullptr || !cJSON_IsNumber(id)) {
         ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
         return;
     }
     auto id_int = id->valueint;
     
     if (method_str == "initialize") {
         if (cJSON_IsObject(params)) {
             auto capabilities = cJSON_GetObjectItem(params, "capabilities");
             if (cJSON_IsObject(capabilities)) {
                 ParseCapabilities(capabilities);
             }
         }
         auto app_desc = esp_app_get_description();
         std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
         message += app_desc->version;
         message += "\"}}";
         ReplyResult(id_int, message);
     } else if (method_str == "tools/list") {
         std::string cursor_str = "";
         if (params != nullptr) {
             auto cursor = cJSON_GetObjectItem(params, "cursor");
             if (cJSON_IsString(cursor)) {
                 cursor_str = std::string(cursor->valuestring);
             }
         }
         GetToolsList(id_int, cursor_str);
     } else if (method_str == "tools/call") {
         if (!cJSON_IsObject(params)) {
             ESP_LOGE(TAG, "tools/call: Missing params");
             ReplyError(id_int, "Missing params");
             return;
         }
         auto tool_name = cJSON_GetObjectItem(params, "name");
         if (!cJSON_IsString(tool_name)) {
             ESP_LOGE(TAG, "tools/call: Missing name");
             ReplyError(id_int, "Missing name");
             return;
         }
         auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
         if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
             ESP_LOGE(TAG, "tools/call: Invalid arguments");
             ReplyError(id_int, "Invalid arguments");
             return;
         }
         auto stack_size = cJSON_GetObjectItem(params, "stackSize");
         if (stack_size != nullptr && !cJSON_IsNumber(stack_size)) {
             ESP_LOGE(TAG, "tools/call: Invalid stackSize");
             ReplyError(id_int, "Invalid stackSize");
             return;
         }
         DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, stack_size ? stack_size->valueint : DEFAULT_TOOLCALL_STACK_SIZE);
     } else {
         ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
         ReplyError(id_int, "Method not implemented: " + method_str);
     }
 }
 
 void McpServer::ReplyResult(int id, const std::string& result) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id) + ",\"result\":";
     payload += result;
     payload += "}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::ReplyError(int id, const std::string& message) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id);
     payload += ",\"error\":{\"message\":\"";
     payload += message;
     payload += "\"}}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::GetToolsList(int id, const std::string& cursor) {
     const int max_payload_size = 8000;
     std::string json = "{\"tools\":[";
     
     bool found_cursor = cursor.empty();
     auto it = tools_.begin();
     std::string next_cursor = "";
     
     while (it != tools_.end()) {
         // 如果我们还没有找到起始位置，继续搜索
         if (!found_cursor) {
             if ((*it)->name() == cursor) {
                 found_cursor = true;
             } else {
                 ++it;
                 continue;
             }
         }
         
         // 添加tool前检查大小
         std::string tool_json = (*it)->to_json() + ",";
         if (json.length() + tool_json.length() + 30 > max_payload_size) {
             // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
             next_cursor = (*it)->name();
             break;
         }
         
         json += tool_json;
         ++it;
     }
     
     if (json.back() == ',') {
         json.pop_back();
     }
     
     if (json.back() == '[' && !tools_.empty()) {
         // 如果没有添加任何tool，返回错误
         ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
         ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
         return;
     }
 
     if (next_cursor.empty()) {
         json += "]}";
     } else {
         json += "],\"nextCursor\":\"" + next_cursor + "\"}";
     }
     
     ReplyResult(id, json);
 }
 
 void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments, int stack_size) {
     auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                  [&tool_name](const McpTool* tool) { 
                                      return tool->name() == tool_name; 
                                  });
     
     if (tool_iter == tools_.end()) {
         ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
         ReplyError(id, "Unknown tool: " + tool_name);
         PostMcpBehavior(DisplayBehavior::kRecoverableError, tool_name, 1800);
         return;
     }
 
     PropertyList arguments = (*tool_iter)->properties();
     try {
         for (auto& argument : arguments) {
             bool found = false;
             if (cJSON_IsObject(tool_arguments)) {
                 auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                 if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                     argument.set_value<bool>(value->valueint == 1);
                     found = true;
                 } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                     argument.set_value<int>(value->valueint);
                     found = true;
                 } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                     argument.set_value<std::string>(value->valuestring);
                     found = true;
                 }
             }
 
             if (!argument.has_default_value() && !found) {
                 ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                 ReplyError(id, "Missing valid argument: " + argument.name());
                 PostMcpBehavior(DisplayBehavior::kRecoverableError, tool_name, 1800);
                 return;
             }
         }
     } catch (const std::exception& e) {
         ESP_LOGE(TAG, "tools/call: %s", e.what());
         ReplyError(id, e.what());
         PostMcpBehavior(DisplayBehavior::kRecoverableError, tool_name, 1800);
         return;
     }

     PostMcpBehavior(DisplayBehavior::kToolRunning, tool_name, 30000);
 
     // Start a task to receive data with stack size
     esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
     cfg.thread_name = "tool_call";
     cfg.stack_size = stack_size;
     cfg.prio = 1;
     esp_pthread_set_cfg(&cfg);
 
     // Use a thread to call the tool to avoid blocking the main thread
     tool_call_thread_ = std::thread([this, id, tool_iter, tool_name,
                                     arguments = std::move(arguments)]() {
         try {
             auto result = (*tool_iter)->Call(arguments);
             ReplyResult(id, result);
             PostMcpBehavior(DisplayBehavior::kSuccess, tool_name, 900);
         } catch (const std::exception& e) {
             ESP_LOGE(TAG, "tools/call: %s", e.what());
             ReplyError(id, e.what());
             PostMcpBehavior(DisplayBehavior::kRecoverableError, tool_name, 1800);
         }
     });
     tool_call_thread_.detach();
 }
