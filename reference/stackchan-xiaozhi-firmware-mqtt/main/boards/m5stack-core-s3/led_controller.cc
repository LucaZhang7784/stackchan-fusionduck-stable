#include "led_controller.h"
#include <string.h>

uint16_t LedController::Rgb888To565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

void LedController::ColorForEmotion(const char* e, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!e) e = "neutral";
    if (!strcmp(e,"happy")||!strcmp(e,"laughing")||!strcmp(e,"funny")){r=255;g=180;b=0;}
    else if (!strcmp(e,"loving")||!strcmp(e,"kissy")){r=255;g=0;b=100;}
    else if (!strcmp(e,"sad")||!strcmp(e,"crying")){r=0;g=50;b=255;}
    else if (!strcmp(e,"angry")){r=255;g=0;b=0;}
    else if (!strcmp(e,"surprised")||!strcmp(e,"shocked")){r=200;g=0;b=255;}
    else if (!strcmp(e,"thinking")||!strcmp(e,"confused")){r=0;g=100;b=255;}
    else if (!strcmp(e,"winking")){r=255;g=120;b=0;}
    else if (!strcmp(e,"cool")){r=0;g=180;b=255;}
    else if (!strcmp(e,"relaxed")){r=180;g=255;b=100;}
    else if (!strcmp(e,"delicious")){r=255;g=80;b=0;}
    else if (!strcmp(e,"confident")){r=255;g=200;b=0;}
    else if (!strcmp(e,"sleepy")){r=10;g=5;b=30;}
    else if (!strcmp(e,"embarrassed")){r=255;g=80;b=120;}
    else if (!strcmp(e,"silly")){r=100;g=255;b=0;}
    else if (!strcmp(e,"listening")){r=0;g=120;b=255;}
    else if (!strcmp(e,"speaking")){r=0;g=255;b=90;}
    else if (!strcmp(e,"connecting")){r=255;g=190;b=0;}
    else {r=60;g=35;b=10;}
}
