#pragma once
template<class... Args> inline void TestLog(Args...) {}
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
