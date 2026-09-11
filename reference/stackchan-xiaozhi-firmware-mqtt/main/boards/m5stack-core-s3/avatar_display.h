#pragma once
#include <functional>
#include <memory>
#include <string>
#include <esp_timer.h>
#include "display/lcd_display.h"
#include "lvgl.h"
class FaceTracker;
class StackChanServo;
namespace shizhou_avatar {


// Face profile compiled from D:\ProcessCenter\StackChan\Avatar\Cat.json.
// The xiaozhi assets partition stores image packs, while this CoreS3 face is a
// live LVGL canvas; keeping the profile here preserves its blink, gaze and lip
// animation without introducing a second runtime asset format.
constexpr int kCatCanvasWidth = 320;
constexpr int kCatCanvasHeight = 240;
constexpr int kCatEyeWidth = 30;
constexpr int kCatEyeHeight = 54;
constexpr int kCatEyeLeftX = 228;
constexpr int kCatEyeRightX = 92;
constexpr int kCatEyeY = 98;
constexpr int kCatMouthX = 160;
constexpr int kCatMouthY = 150;
constexpr uint32_t kCatBlinkIntervalMs = 4500;
constexpr uint32_t kCatBlinkDurationMs = 120;

enum class Expression {
    Neutral, Happy, Angry, Sad, Sleepy,
    Loving, Crying,
    Kissy, Cool, Confident,
    Shocked, Thinking, Surprised, Confused,
    Embarrassed, Silly, Winking, Laughing, Funny, Relaxed, Delicious
};

struct Overlay {
    bool tear = false;
    bool heart_eyes = false;
    bool kiss_heart = false;
    bool cheek_blush = false;
    bool cool_glasses = false;
    bool excl_mark = false;
    bool think_bubble = false;
    bool star_burst = false;
    bool wave_squiggle = false;
    bool drool = false;
    bool laugh_lines = false;
    bool question_mark = false;
    bool zzz = false;
};

class LvglAvatar {
public:
    LvglAvatar() = default;
    ~LvglAvatar() { Destroy(); }
bool Init(lv_obj_t* parent, int w, int h);
void Destroy();

    bool IsReady() const { return canvas_ != nullptr; }

    void SetExpression(Expression e) { expression_ = e; }
    void SetOverlay(const Overlay& o) { overlay_ = o; }
void StartSpeaking(uint32_t duration_ms);
void StopSpeaking();
private:
    static void TimerCb(lv_timer_t* t);
void UpdateBreathParams();
bool BlinkAllowed() const;
bool SlowBlink() const;
bool SaccadeEnabled() const;
void GetGazeOverride(float* gh, float* gv) const;
void OnTick();
void Draw();
void FillRect(lv_layer_t* layer, int x, int y, int w, int h, lv_color_t c);
void FillCircle(lv_layer_t* layer, int cx, int cy, int r, lv_color_t c);
void FillTriangle(lv_layer_t* layer, int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t c);
void FillRoundRect(lv_layer_t* layer, int x, int y, int w, int h, int radius, lv_color_t c);
void DrawArc(lv_layer_t* layer, int cx, int cy, int r, int start_deg, int end_deg, int width, lv_color_t c, bool rounded = false);
void DrawLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, int width, bool round, lv_color_t c);
void DrawCatMouth(lv_layer_t* layer, lv_color_t fg);
void DrawMouth(lv_layer_t* layer, lv_color_t fg, lv_color_t bg);
bool DrawCatEye(lv_layer_t* layer, lv_color_t fg, lv_color_t bg, bool is_left);
void DrawEye(lv_layer_t* layer, lv_color_t fg, lv_color_t bg, bool is_left);
void DrawOverlay(lv_layer_t* layer, lv_color_t fg, lv_color_t bg);

    lv_obj_t* canvas_ = nullptr;
    uint8_t* buf_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    int w_ = 320, h_ = 240;

    Expression expression_ = Expression::Neutral;
    Overlay overlay_;

    uint32_t tick_count_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t last_saccade_ms_ = 0;
    uint32_t speaking_until_ms_ = 0;
    bool eye_closed_ = false;
    float eye_open_ratio_ = 1.0f;
    float mouth_open_ = 0.0f;
    float breath_ = 0.0f;
    float gaze_h_ = 0.0f;
    float gaze_v_ = 0.0f;

    float breath_amp_ = 3.0f;
    uint32_t breath_period_steps_ = 100;
    bool breath_paused_ = false;
};
Expression MapEmotion(const char* e);
Overlay OverlayFor(const char* e);
}  // namespace shizhou_avatar
class M5StackAvatarDisplay : public SpiLcdDisplay {
public:
    M5StackAvatarDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                          int width, int height, int offset_x, int offset_y,
                          bool mirror_x, bool mirror_y, bool swap_xy);
void SetFaceTracker(FaceTracker* ft);
void SetServo(StackChanServo* s);
void SetLedUpdater(std::function<void(const char*)> fn);
void OnPetted();
void SetEmotion(const char* emotion) override;
void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
void SetChatMessage(const char* role, const char* content) override;
void SetStatus(const char* status) override;

private:
    shizhou_avatar::LvglAvatar avatar_;
    FaceTracker* face_tracker_ = nullptr;
    StackChanServo* servo_ = nullptr;
    std::function<void(const char*)> led_updater_;
    esp_timer_handle_t avatar_init_timer_ = nullptr;
    esp_timer_handle_t idle_timer_ = nullptr;
    bool active_mode_ = false;
    int canvas_w_ = 320;
    int canvas_h_ = 240;
    static constexpr uint64_t IDLE_TIMEOUT_US = 8 * 1000 * 1000;
void HideEmojiBoxLocked();
void SetActiveLocked(bool active);
void BumpIdleTimerLocked();
static void IdleTimerCallback(void* arg);
static void InitTimerCallback(void* arg);
void TryInitAvatar();
};
