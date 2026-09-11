#include "confirm_manager.h"

#include "display/display.h"
#include <lvgl.h>

ConfirmManager::ConfirmManager(TimeoutCallback callback, void* context)
    : timeout_callback_(callback), timeout_context_(context) {}

ConfirmManager::~ConfirmManager() {
    StopTimer();
}

void ConfirmManager::StopTimer() {
    if (timer_ != nullptr) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void ConfirmManager::TimerCallback(void* arg) {
    auto* self = static_cast<ConfirmManager*>(arg);
    if (self != nullptr && self->timeout_callback_ != nullptr) {
        self->timeout_callback_(self->timeout_context_);
    }
}

void ConfirmManager::Dismiss(Display* display) {
    if (display == nullptr) return;
    DisplayLockGuard lock(display);
    StopTimer();
    if (panel_ != nullptr) {
        lv_obj_delete(panel_);
        panel_ = nullptr;
    }
    allow_button_ = nullptr;
    deny_button_ = nullptr;
    uid_.clear();
}

void ConfirmManager::Show(Display* display, const std::string& text, const std::string& msg_uid) {
    if (display == nullptr) return;
    DisplayLockGuard lock(display);
    StopTimer();
    if (panel_ != nullptr) {
        lv_obj_delete(panel_);
        panel_ = nullptr;
        allow_button_ = nullptr;
        deny_button_ = nullptr;
    }
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) return;
    uid_ = msg_uid;
    panel_ = lv_obj_create(screen);
    lv_obj_set_size(panel_, 260, 150);
    lv_obj_center(panel_);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(panel_, 2, 0);
    lv_obj_set_style_border_color(panel_, lv_color_hex(0x999999), 0);
    lv_obj_set_style_radius(panel_, 8, 0);
    lv_obj_t* title = lv_label_create(panel_);
    lv_label_set_text(title, "需要确认");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    std::string body = text;
    if (body.size() > 42) body = body.substr(0, 42) + "...";
    lv_obj_t* label = lv_label_create(panel_);
    lv_label_set_text(label, body.c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 236);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 34);
    allow_button_ = lv_button_create(panel_);
    lv_obj_set_size(allow_button_, 96, 40);
    lv_obj_align(allow_button_, LV_ALIGN_BOTTOM_LEFT, 18, -12);
    lv_obj_t* allow_label = lv_label_create(allow_button_);
    lv_label_set_text(allow_label, "允许");
    lv_obj_center(allow_label);
    deny_button_ = lv_button_create(panel_);
    lv_obj_set_size(deny_button_, 96, 40);
    lv_obj_align(deny_button_, LV_ALIGN_BOTTOM_RIGHT, -18, -12);
    lv_obj_t* deny_label = lv_label_create(deny_button_);
    lv_label_set_text(deny_label, "拒绝");
    lv_obj_center(deny_label);
    lv_obj_update_layout(panel_);
    int px = lv_obj_get_x(panel_), py = lv_obj_get_y(panel_);
    allow_x0_ = px + lv_obj_get_x(allow_button_);
    allow_y0_ = py + lv_obj_get_y(allow_button_);
    allow_x1_ = allow_x0_ + 96;
    allow_y1_ = allow_y0_ + 40;
    deny_x0_ = px + lv_obj_get_x(deny_button_);
    deny_y0_ = py + lv_obj_get_y(deny_button_);
    deny_x1_ = deny_x0_ + 96;
    deny_y1_ = deny_y0_ + 40;
    esp_timer_create_args_t args = {};
    args.callback = &ConfirmManager::TimerCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "confirm_timeout";
    if (esp_timer_create(&args, &timer_) == ESP_OK) {
        esp_timer_start_once(timer_, 15 * 1000 * 1000);
    }
}

bool ConfirmManager::IsActive() const { return panel_ != nullptr; }
const char* ConfirmManager::HitTest(int x, int y) const {
    if (!IsActive()) return nullptr;
    if (x >= allow_x0_ && x <= allow_x1_ && y >= allow_y0_ && y <= allow_y1_) return "allow";
    if (x >= deny_x0_ && x <= deny_x1_ && y >= deny_y0_ && y <= deny_y1_) return "deny";
    return nullptr;
}
const std::string& ConfirmManager::GetUid() const { return uid_; }
