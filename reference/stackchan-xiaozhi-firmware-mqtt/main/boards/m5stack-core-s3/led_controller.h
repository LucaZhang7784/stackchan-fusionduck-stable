#pragma once
#include <stdint.h>

class LedController {
public:
    static uint16_t Rgb888To565(uint8_t r, uint8_t g, uint8_t b);
    static void ColorForEmotion(const char* emotion, uint8_t& r, uint8_t& g, uint8_t& b);
};
