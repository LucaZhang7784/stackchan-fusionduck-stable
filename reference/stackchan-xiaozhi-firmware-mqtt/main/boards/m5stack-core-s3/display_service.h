#pragma once
#include <functional>
#include "display/lcd_display.h"
class DisplayService {
public:
    struct Config { std::function<void()> reset_panel; };
    static LcdDisplay* InitializeDisplay(const Config& config);
};
