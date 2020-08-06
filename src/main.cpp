#include <Arduino.h>
#include "msTimer.h" // Local libary.
#include "flasher.h" // Local libary.
#include <SPI.h>
#include <SD.h>
#include "ESP8266WiFi.h"
#include <esp8266httpclient.h>
#include "ArduinoJson.h"
#include <Adafruit_NeoPixel.h> // https://github.com/adafruit/Adafruit_NeoPixel
#include <JC_Button.h>         // https://github.com/JChristensen/JC_Button

#define PIN_STRIP_1 5       // GPIO PIN NUMBER
#define PIN_STRIP_2 4       // GPIO PIN NUMBER
#define PIN_STRIP_3 0       // GPIO PIN NUMBER
#define PIN_BUTTON_SELECT 2 // GPIO PIN NUMBER

const int stripMaxBrightness = 50;
const int stripStatusIndicatorIndex = 4;
// Due to hardware limitations of the ESP8266 long WS2812b strips are not possible.
// Therefore segments, indicators, and dots are combined in a awkward combination to prevent flickering.
Adafruit_NeoPixel strip1 = Adafruit_NeoPixel(42, PIN_STRIP_1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip2 = Adafruit_NeoPixel(47, PIN_STRIP_2, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip3 = Adafruit_NeoPixel(34, PIN_STRIP_3, NEO_GRB + NEO_KHZ800);

Button buttonSelect(PIN_BUTTON_SELECT, 25, false, true);

// SD card parameters.
String ssid, password, brightness;
int cycleDelay;

const char *wifiFilePath = "/wifi.txt";
const int chipSelect = D8;
const int blankSegment = 10;

struct MetalSpot
{
    float open;
    float close;
} metalSpot[3];

enum IndicatorStatus
{
    sdCardFailure,
    wifiConnecting,
    wifiConnected,
    wifiDisconnected,
    fetchingData,
    fetchFailed,
    fetchSuccess
} indicatorStatus;

int selectedMetal; // 0 = Au, 1 = Ag, 2 = Pt

const uint32_t OFF = 0x0000000;
const uint32_t RED = 0x00FF0000;
const uint32_t GREEN = 0x0000FF00;
const uint32_t BLUE = 0x000000FF;
const uint32_t YELLOW = 0x00F0F000;

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

// Dot (5mm WS2812b LED) red and green colors are swapped.
uint32_t SwapRG(uint32_t color)
{
    int r, g, b;

    r = (color & 0x00FF0000) >> 16;
    g = (color & 0x0000FF00) >> 8;
    b = color & 0x000000FF;

    return Color(g, r, b);
}

// Convert decimal value to segments (hardware does not follow 7-segment display convention).
const int decimalToSegmentValues[11][7] = {{1, 1, 1, 1, 1, 1, 0},  // 0
                                           {0, 0, 0, 1, 1, 0, 0},  // 1
                                           {1, 0, 1, 1, 0, 1, 1},  // 2
                                           {0, 0, 1, 1, 1, 1, 1},  // 3
                                           {0, 1, 0, 1, 1, 0, 1},  // 4
                                           {0, 1, 1, 0, 1, 1, 1},  // 5
                                           {1, 1, 0, 0, 1, 1, 1},  // 6
                                           {0, 0, 1, 1, 1, 0, 0},  // 7
                                           {1, 1, 1, 1, 1, 1, 1},  // 8
                                           {0, 1, 1, 1, 1, 0, 1},  // 9
                                           {0, 0, 0, 0, 0, 0, 0}}; // 10 / ALL OFF

bool InitSDCard()
{
    int count = 0;

    Serial.println("Attempting to mount SD card...");

    while (!SD.begin(chipSelect))
    {
        if (++count > 5)
        {
            Serial.println("Card Mount Failed.");
            return false;
        }
        delay(250);
    }

    Serial.println("SD card mounted.");
    return true;
}

void GetParametersFromSDCard()
{
    File file = SD.open(wifiFilePath);

    Serial.println("Attempting to fetch parameters from SD card...");

    if (!file)
    {
        Serial.printf("Failed to open file: %s\n", wifiFilePath);
        Serial.printf("Creating file with path: %s\n", wifiFilePath);
        File file = SD.open(wifiFilePath, FILE_WRITE);
        file.print("SSID: \"your SSID inside quotations\"\nPassword: \"your password inside quotations\"");
        file.close();
    }

    if (file.find("SSID: \""))
    {
        ssid = file.readStringUntil('"');
    }

    if (file.find("Password: \""))
    {
        password = file.readStringUntil('"');
    }

    if (file.find("Brightness: \""))
    {
        brightness = file.readStringUntil('"');
    }

    if (file.find("Cycle delay: \""))
    {
        cycleDelay = file.readStringUntil('"').toInt();
    }
}

void GenerateNumbers(float value, int *numbers, int *dot)
{
    // Split the value into an integer part and a fractional part.
    int iPart = (int)value;
    int fPart = (int)((value - iPart) * 100 + .5);

    int ones = iPart % 10;
    int tens = (iPart / 10) % 10;
    int hundreds = (iPart / 100) % 10;
    int thousands = (iPart / 1000) % 10;
    int tenthousands = (iPart / 10000);

    int fOnes = fPart % 10;
    int fTens = (fPart / 10) % 10;

    if (iPart == 0)
    {
        numbers[4] = blankSegment;
        numbers[3] = blankSegment;
        numbers[2] = blankSegment;
        numbers[1] = blankSegment;
        numbers[0] = blankSegment;
        *dot = blankSegment;
    }
    else if (iPart < 100)
    {
        numbers[4] = blankSegment;
        numbers[3] = tens;
        numbers[2] = ones;
        numbers[1] = fTens;
        numbers[0] = fOnes;
        *dot = 2;
    }
    else if (iPart < 1000)
    {
        numbers[4] = hundreds;
        numbers[3] = tens;
        numbers[2] = ones;
        numbers[1] = fTens;
        numbers[0] = fOnes;
        *dot = 2;
    }
    else if (iPart < 10000)
    {
        numbers[4] = thousands;
        numbers[3] = hundreds;
        numbers[2] = tens;
        numbers[1] = ones;
        numbers[0] = fTens;
        *dot = 3;
    }
    else if (iPart < 100000)
    {
        numbers[4] = tenthousands;
        numbers[3] = thousands;
        numbers[2] = hundreds;
        numbers[1] = tens;
        numbers[0] = ones;
        *dot = blankSegment;
    }
}

void SetDots(int dot, uint32_t color)
{
    for (int i = 0; i < 4; i++)
    {
        strip2.setPixelColor(i, 0);
    }

    if (dot != blankSegment)
    {
        strip2.setPixelColor(dot, SwapRG(color));
    }
}

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
}

void SetIndicators(uint32_t color)
{
    // Set Spot Clock text indicator;
    static msTimer timer(25);
    static byte wheelPos;
    if (timer.elapsed())
    {
        wheelPos++;
    }
    for (int i = 0; i < 7; i++)
    {
        //strip3.setPixelColor(i, Wheel((255 / 7) * i) + wheelPos);
        strip3.setPixelColor(i, Wheel(wheelPos + i * 10));
    }

    // Set metal indicators;
    strip3.setPixelColor(7, selectedMetal == 0 ? color : 0);
    strip3.setPixelColor(8, selectedMetal == 0 ? color : 0);
    strip3.setPixelColor(9, selectedMetal == 1 ? color : 0);
    strip3.setPixelColor(10, selectedMetal == 1 ? color : 0);
    strip3.setPixelColor(11, selectedMetal == 2 ? color : 0);
    strip3.setPixelColor(12, selectedMetal == 2 ? color : 0);
}

void UpdateConnectionIndicator(IndicatorStatus indicatorStatus)
{
    if (indicatorStatus == sdCardFailure)
    {
        static msTimer timer(250);
        static bool toggle;
        if (timer.elapsed())
        {
            toggle = !toggle;
        }
        uint32_t color = toggle ? YELLOW : OFF;
        strip2.setPixelColor(stripStatusIndicatorIndex, SwapRG(color));
    }
    else if (indicatorStatus == wifiConnecting)
    {
        static msTimer timer(250);
        static bool toggle;
        if (timer.elapsed())
        {
            toggle = !toggle;
        }
        uint32_t color = toggle ? RED : OFF;
        strip2.setPixelColor(stripStatusIndicatorIndex, SwapRG(color));
    }
    else if (indicatorStatus == wifiConnected)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, SwapRG(GREEN));
    }
    else if (indicatorStatus == wifiDisconnected)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, SwapRG(RED));
    }
    else if (indicatorStatus == fetchingData)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, SwapRG(BLUE));
    }
    else if (indicatorStatus == fetchFailed)
    {
        static msTimer timer(1000);
        static bool toggle;
        if (timer.elapsed())
        {
            toggle = !toggle;
        }
        uint32_t color = toggle ? RED : GREEN;
        strip2.setPixelColor(stripStatusIndicatorIndex, SwapRG(color));
    }
    else if (indicatorStatus == fetchSuccess)
    {
        static msTimer timer(1000);
        static bool toggle;
        if (timer.elapsed())
        {
            toggle = !toggle;
        }
        uint32_t color = toggle ? GREEN : BLUE;
        strip2.setPixelColor(stripStatusIndicatorIndex, SwapRG(color));
    }

    strip2.show();
}

void UpdateStrips()
{
    strip1.show();
    strip2.show();
    strip3.show();
}

void sdFailure()
{
    // Halt system.
    while (1)
    {
        UpdateConnectionIndicator(sdCardFailure);
    }
}

bool FetchDataFromInternet(float *price, String expression, String instrument)
{
    String payload;
    String host = "http://api.fxhistoricaldata.com/indicators?timeframe=day&item_count=1&expression=" + expression + "&instruments=" + instrument;

    Serial.print("Connecting to ");
    Serial.println(host);

    HTTPClient http;
    http.begin(host);
    int httpCode = http.GET();

    if (httpCode > 0)
    {
        Serial.print("HTTP code: ");
        Serial.println(httpCode);
        Serial.println("[RESPONSE]");
        payload = http.getString();
        Serial.println(payload);
        http.end();
    }
    else
    {
        Serial.print("Connection failed, HTTP client code: ");
        Serial.println(httpCode);
        http.end();
        return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);

    if (error)
    {
        Serial.print(F("DeserializeJson() failed: "));
        Serial.println(error.c_str());
        return false;
    }

    *price = doc["results"][instrument]["data"][0][1];

    return true;
}

bool GetUpdatedSpot()
{
    const String metals[] = {"XAU_USD", "XAG_USD", "XPT_USD"};

    static int metalIndex = 0;
    if (++metalIndex > 2)
    {
        metalIndex = 0;
    }

    float price;

    String host = "http://worldclockapi.com/api/json/est/now";

    Serial.print("Connecting to ");
    Serial.println(host);
    /*
    HTTPClient http;
    http.begin(host);
    int httpCode = http.GET();

    if (httpCode > 0)
    {
        Serial.print("HTTP code: ");
        Serial.println(httpCode);
        Serial.println("[RESPONSE]");
        String p = http.getString();
        Serial.println(p);
        http.end();
    }
    else
    {
        Serial.print("Connection failed, HTTP client code: ");
        Serial.println(httpCode);
        http.end();
        return false;
    }
    */

    if (!FetchDataFromInternet(&price, "open", metals[metalIndex]))
    {
        return false;
    }

    metalSpot[metalIndex].open = price;

    if (!FetchDataFromInternet(&price, "close", metals[metalIndex]))
    {
        return false;
    }

    metalSpot[metalIndex].close = price;

    Serial.print(metals[metalIndex] + " | ");
    Serial.print(("Open : " + (String)metalSpot[metalIndex].open) + ", ");
    Serial.println(("Close : " + (String)metalSpot[metalIndex].close));

    return true;
}

void IncrementMetalSelection()
{
    if (++selectedMetal > 2)
    {
        selectedMetal = 0;
    }
}

void setup()
{
    Serial.begin(74880);
    Serial.println("Spot Clock 2 starting up...");

    strip1.setBrightness(stripMaxBrightness);
    strip2.setBrightness(stripMaxBrightness);
    strip3.setBrightness(stripMaxBrightness);
    strip1.begin();
    strip2.begin();
    strip3.begin();
    UpdateStrips();

    buttonSelect.begin();

    if (!InitSDCard())
    {
        sdFailure();
    }

    GetParametersFromSDCard();

    Serial.print("SSID: ");
    Serial.println(ssid);
    Serial.print("Password: ");
    Serial.println(password);

    // Set strip brightness from SD card parameter.
    int b = brightness.toInt();
    if (b >= 50 && b <= 255)
    {
        strip1.setBrightness(b);
        strip2.setBrightness(b);
        strip3.setBrightness(b);
    }
    Serial.print("Brightness: ");
    Serial.println(b);

    Serial.print("CycleDelay: ");
    Serial.println(cycleDelay);

    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi...");

    msTimer timer(250);
    while (WiFi.status() != WL_CONNECTED)
    {
        UpdateConnectionIndicator(wifiConnecting);
        Serial.print(".");
        delay(250);
    }

    Serial.println();
    Serial.println("Connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

void loop()
{

    static msTimer timerYield(25);
    if (timerYield.elapsed())
    {
        // ESP8266 watchdog timer reset work around.
        ESP.wdtFeed();
        yield();
    }

    UpdateConnectionIndicator(indicatorStatus);

    static wl_status_t previousWifiStatus = WL_NO_SHIELD;
    if (previousWifiStatus != WiFi.status())
    {
        previousWifiStatus = WiFi.status();
        if (WiFi.status() == WL_CONNECTED)
        {
            indicatorStatus = wifiConnected;
        }
        else if (WiFi.status() != WL_CONNECTED)
        {
            indicatorStatus = wifiDisconnected;
        }
    }

    static msTimer timerFetch(0);
    if (timerFetch.elapsed())
    {
        timerFetch.setDelay(20000);

        indicatorStatus = fetchingData;
        UpdateConnectionIndicator(indicatorStatus);

        bool success = GetUpdatedSpot();
        indicatorStatus = success ? fetchSuccess : fetchFailed;
    }

    static msTimer timerMetalSelection(cycleDelay);
    if (timerMetalSelection.elapsed())
    {
        IncrementMetalSelection();
    }

    buttonSelect.read();
    if (buttonSelect.wasPressed())
    {
        timerMetalSelection.resetDelay();
        IncrementMetalSelection();
    }

    int numbers[5];
    int dot;
    uint32_t color = (metalSpot[selectedMetal].close > metalSpot[selectedMetal].open) ? GREEN : RED;

    GenerateNumbers(metalSpot[selectedMetal].close, numbers, &dot);
    SetSegments(numbers, color);
    SetDots(dot, color);
    SetIndicators(BLUE);
    UpdateStrips();
}