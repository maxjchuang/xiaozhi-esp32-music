#pragma once
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    int dispatch_method;
    const char* name;
    bool skip_unhandled_events;
};
struct TestTimer { esp_timer_create_args_t args{}; int64_t deadline = INT64_MAX; };
using esp_timer_handle_t = TestTimer*;
inline int64_t test_now_us = 1000;
inline TestTimer* test_timer = nullptr;
inline bool test_timer_fail = false;
inline int64_t esp_timer_get_time() { return test_now_us; }
inline esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* timer) {
    if (test_timer_fail) return ESP_ERR_INVALID_STATE;
    *timer = test_timer = new TestTimer{*args, INT64_MAX}; return ESP_OK;
}
inline esp_err_t esp_timer_stop(esp_timer_handle_t timer) { timer->deadline = INT64_MAX; return ESP_OK; }
inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, int64_t delay) {
    timer->deadline = test_now_us + delay; return ESP_OK;
}
inline esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    delete timer; test_timer = nullptr; return ESP_OK;
}
inline const char* esp_err_to_name(esp_err_t) { return "test_error"; }
inline void TestAdvance(int64_t milliseconds) {
    const int64_t target = test_now_us + milliseconds * 1000;
    unsigned callbacks = 0;
    while (test_timer && test_timer->deadline <= target) {
        if (++callbacks > 10000) __builtin_trap();
        test_now_us = test_timer->deadline;
        test_timer->deadline = INT64_MAX;
        test_timer->args.callback(test_timer->args.arg);
    }
    test_now_us = target;
}
