#include <Arduino.h>
#include "msTimer.h"

#include <Adafruit_NeoPixel.h> // https://github.com/adafruit/Adafruit_NeoPixel

#define PIN_STRIP_1 5
#define PIN_STRIP_2 4
#define PIN_STRIP_3 0

const int stripMaxBrightness = 50;
// Due to hardware limitations of the ESP8266 long WS2812b strips are not possible.
// Therefore segments, indicators, and dots are combined in a awkward combination to prevent flickering.
Adafruit_NeoPixel strip1 = Adafruit_NeoPixel(42, PIN_STRIP_1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip2 = Adafruit_NeoPixel(47, PIN_STRIP_2, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip3 = Adafruit_NeoPixel(34, PIN_STRIP_3, NEO_GRB + NEO_KHZ800);

// Pack color data into 32 bit unsigned int (copied from Neopixel library).
uint32_t Color(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Fade color by specified amount.
uint32_t Fade(uint32_t color, int amt)
{
    signed int r, g, b;

    r = (color & 0x00FF0000) >> 16;
    g = (color & 0x0000FF00) >> 8;
    b = color & 0x000000FF;

    r -= amt;
    g -= amt;
    b -= amt;

    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;

    return Color(r, g, b);
}

// Input a value 0 to 255 to get a color value (of a pseudo-rainbow).
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos)
{
    WheelPos = 255 - WheelPos;
    if (WheelPos < 85)
    {
        return Color(255 - WheelPos * 3, 0, WheelPos * 3);
    }
    if (WheelPos < 170)
    {
        WheelPos -= 85;
        return Color(0, WheelPos * 3, 255 - WheelPos * 3);
    }
    WheelPos -= 170;
    return Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

// Convert decimal value to segments (hardware does not follow 7-segment display convention).
const int decimalToSegmentValues[10][7] = {{1, 1, 1, 1, 1, 1, 0},  // 0
                                           {0, 0, 0, 1, 1, 0, 0},  // 1
                                           {1, 0, 1, 1, 0, 1, 1},  // 2
                                           {0, 0, 1, 1, 1, 1, 1},  // 3
                                           {0, 1, 0, 1, 1, 0, 1},  // 4
                                           {0, 1, 1, 0, 1, 1, 1},  // 5
                                           {1, 1, 0, 0, 1, 1, 1},  // 6
                                           {0, 0, 1, 1, 1, 0, 0},  // 7
                                           {1, 1, 1, 1, 1, 1, 1},  // 8
                                           {0, 1, 1, 1, 1, 0, 1}}; // 9

void SetSegments(int numbers[5], uint32_t color)
{

    for (int i = 0; i < 21; i++)
    {
        // Segment 1.
        strip1.setPixelColor(i, decimalToSegmentValues[numbers[4]][i / 3] ? color : 0);

        // Segment 2.
        strip1.setPixelColor(i + 21, decimalToSegmentValues[numbers[3]][i / 3] ? color : 0);

        // Segment 3.
        strip2.setPixelColor(i + 5, decimalToSegmentValues[numbers[2]][i / 3] ? color : 0);

        // Segment 4.
        strip2.setPixelColor(i + 5 + 21, decimalToSegmentValues[numbers[1]][i / 3] ? color : 0);

        // Segment 5.
        strip3.setPixelColor(i + 13, decimalToSegmentValues[numbers[0]][i / 3] ? color : 0);
    }

    strip1.show();
    strip2.show();
    strip3.show();
}

void setup()
{
    strip1.setBrightness(stripMaxBrightness);
    strip2.setBrightness(stripMaxBrightness);
    strip3.setBrightness(stripMaxBrightness);

    strip1.begin();
    strip2.begin();
    strip3.begin();
}

void loop()
{

    static int numbers[5];

    static msTimer timer(100);
    static int n = 0;

    if (timer.elapsed())
    {
        n++;

        int ones = n % 10;
        int tens = (n / 10) % 10;
        int hundreds = (n / 100) % 10;
        int thousands = (n / 1000) % 10;
        int tenthousands = (n / 10000);

        numbers[0] = ones;
        numbers[1] = tens;
        numbers[2] = hundreds;
        numbers[3] = thousands;
        numbers[4] = tenthousands;

        SetSegments(numbers, 0x00000f);
    }
}