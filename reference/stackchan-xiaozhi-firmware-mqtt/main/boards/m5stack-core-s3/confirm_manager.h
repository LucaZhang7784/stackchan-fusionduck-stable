#pragma once

#include <esp_timer.h>
#include <string>

class Display;
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

class ConfirmManager {
public:
    using TimeoutCallback = void (*)(void* context);

    ConfirmManager(TimeoutCallback callback, void* context);
    ~ConfirmManager();
    void Show(Display* display, const std::string& text, const std::string& msg_uid);
    void Dismiss(Display* display);
    bool IsActive() const;
    const char* HitTest(int x, int y) const;
    const std::string& GetUid() const;

private:
    static void TimerCallback(void* arg);
    void StopTimer();

    TimeoutCallback timeout_callback_ = nullptr;
    void* timeout_context_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* allow_button_ = nullptr;
    lv_obj_t* deny_button_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    std::string uid_;
    int allow_x0_ = 0, allow_y0_ = 0, allow_x1_ = 0, allow_y1_ = 0;
    int deny_x0_ = 0, deny_y0_ = 0, deny_x1_ = 0, deny_y1_ = 0;
};
