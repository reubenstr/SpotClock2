/*
*   Spot Clock 2
*
*   Fetches then displays current gold, silver, and platium prices via 
*   custom WS2812b 7-segment displays.
*
*   Reuben Strangelove
*   Summer 2020
*
*   MCU: ESP8266 
*   MCU Hardware: hw-628 NODEMCU V3
*
*   WiFi credentials and other parameters stored on SD card.
*   
*   Notes:
*   NeoPixel strips connect across multiple pins in order to reduce strip length.
*   Long strips cause flickering.
*   NeoPixel are only updated when necessary as constant updates likey causes the
*   watchdog timer to trigger.
*
*/

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

const int stripStatusIndicatorIndex = 4;

// Due to hardware limitations of the ESP8266 long WS2812b strips are not possible.
// Therefore segments, indicators, and dots are combined in a awkward combination to prevent flickering.
Adafruit_NeoPixel strip1 = Adafruit_NeoPixel(42, PIN_STRIP_1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip2 = Adafruit_NeoPixel(47, PIN_STRIP_2, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip3 = Adafruit_NeoPixel(34, PIN_STRIP_3, NEO_GRB + NEO_KHZ800);

Button buttonSelect(PIN_BUTTON_SELECT, 25, false, true);

// SD card parameters.
String ssid, password, timeZone;
int brightness, cycleDelay;

const char *wifiFilePath = "/wifi.txt";
const int chipSelect = D8;
const int blankSegment = 10;
const int dashSegment = 11;

struct MetalSpot
{
    float open;
    float close;
    float percentage;
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

struct TimeDate
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
} curTimeDate;

int selectedMetal; // 0 = Au, 1 = Ag, 2 = Pt

const uint32_t OFF = 0x0000000;
const uint32_t RED = 0x00FF0000;
const uint32_t GREEN = 0x0000FF00;
const uint32_t BLUE = 0x000000FF;
const uint32_t YELLOW = 0x00F0F000;
const uint32_t MAGENTA = 0x00F000F0;

const uint32_t RED_DIM = 0x00300000;
const uint32_t GREEN_DIM = 0x00003000;
const uint32_t BLUE_DIM = 0x00000030;
const uint32_t YELLOW_DIM = 0x002F2F00;
const uint32_t MAGENTA_DIM = 0x002F002F;

// Convert decimal value to segments (hardware does not follow 7-segment display convention).
const int decimalToSegmentValues[12][7] = {{1, 1, 1, 1, 1, 1, 0},  // 0
                                           {0, 0, 0, 1, 1, 0, 0},  // 1
                                           {1, 0, 1, 1, 0, 1, 1},  // 2
                                           {0, 0, 1, 1, 1, 1, 1},  // 3
                                           {0, 1, 0, 1, 1, 0, 1},  // 4
                                           {0, 1, 1, 0, 1, 1, 1},  // 5
                                           {1, 1, 0, 0, 1, 1, 1},  // 6
                                           {0, 0, 1, 1, 1, 0, 0},  // 7
                                           {1, 1, 1, 1, 1, 1, 1},  // 8
                                           {0, 1, 1, 1, 1, 0, 1},  // 9
                                           {0, 0, 0, 0, 0, 0, 0},  // 10 / ALL OFF
                                           {0, 0, 0, 0, 0, 0, 1}}; // 11 / Center dash

// https://www.geeksforgeeks.org/find-day-of-the-week-for-a-given-date/
int dayofweek(int d, int m, int y)
{
    static int t[] = {0, 3, 2, 5, 0, 3,
                      5, 1, 4, 6, 2, 4};
    y -= m < 3;
    return (y + y / 4 - y / 100 +
            y / 400 + t[m - 1] + d) %
           7;
}

// Pack color data into 32 bit unsigned int (copied from Neopixel library).
uint32_t Color(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
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

bool GetParametersFromSDCard()
{
    File file = SD.open(wifiFilePath);

    Serial.println("Attempting to fetch parameters from SD card...");

    if (!file)
    {
        Serial.printf("Failed to open file: %s\n", wifiFilePath);
        file.close();
        return false;
    }
    else
    {
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, file.readString());

        if (error)
        {
            Serial.print(F("DeserializeJson() failed: "));
            Serial.println(error.c_str());
            return false;
        }

        ssid = doc["ssid"].as<String>();
        password = doc["password"].as<String>();
        timeZone = doc["time zone"].as<String>();
        brightness = doc["brightness"].as<int>();
        cycleDelay = doc["cycle delay"].as<int>();

        metalSpot[0].percentage = doc["au alert percentage"].as<float>();
        metalSpot[1].percentage = doc["ag alert percentage"].as<float>();
        metalSpot[2].percentage = doc["pt alert percentage"].as<float>();
    }
    file.close();
    return true;
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

    if (value == 0)
    {
        numbers[4] = dashSegment;
        numbers[3] = dashSegment;
        numbers[2] = dashSegment;
        numbers[1] = dashSegment;
        numbers[0] = dashSegment;
        *dot = blankSegment;
    }
    else if (iPart == 0)
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
    // Brightness fix (since segments share strips of dots and indicators).
    if (color == RED)
    {
        color = Color(brightness, 0, 0);
    }
    else if (color == GREEN)
    {
        color = Color(0, brightness, 0);
    }

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

void UpdateConnectionIndicator()
{
    static uint32_t oldIndicatorValue;

    oldIndicatorValue = strip2.getPixelColor(stripStatusIndicatorIndex);

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

    if (oldIndicatorValue != strip2.getPixelColor(stripStatusIndicatorIndex))
    {
        strip2.show();
    }
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
        indicatorStatus = sdCardFailure;
        UpdateConnectionIndicator();
        yield();
    }
}

bool UpdateTime()
{
    String payload;
    String host = "http://worldclockapi.com/api/json/" + timeZone + "/now";

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

    String dateTime = doc["currentDateTime"];

    //Expected time string: "2020-08-07T10:08-04:00"
    curTimeDate.hour = dateTime.substring(11, 13).toInt();
    curTimeDate.minute = dateTime.substring(14, 16).toInt();

    curTimeDate.year = dateTime.substring(0, 4).toInt();
    curTimeDate.month = dateTime.substring(5, 7).toInt();
    curTimeDate.day = dateTime.substring(8, 10).toInt();

    Serial.printf("Current date: %u:%u:%u", curTimeDate.year, curTimeDate.month, curTimeDate.day);
    Serial.printf("Current time: %u:%u", curTimeDate.hour, curTimeDate.minute);

    return true;
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

    // uint32_t free = system_get_free_heap_size();
    // Serial.print("\nFree RAM: ");
    // Serial.println(free);

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

void UpdateDisplay()
{
    int numbers[5];
    int dot;
    uint32_t color = BLUE;

    int dayOfTheWeek = dayofweek(curTimeDate.day, curTimeDate.month, curTimeDate.year);

    if (dayOfTheWeek == 0 || dayOfTheWeek == 6) // Sunday or Saturday
    {
        color = MAGENTA;
    }
    else
    {
        if (metalSpot[selectedMetal].close + (metalSpot[selectedMetal].close * metalSpot[selectedMetal].percentage) > metalSpot[selectedMetal].open)
        {
            color = GREEN;
        }

        if (metalSpot[selectedMetal].close - (metalSpot[selectedMetal].close * metalSpot[selectedMetal].percentage) < metalSpot[selectedMetal].open)
        {
            color = RED;
        }
    }

    // Dots need dimmed due to physical  characteristics of physical LED housings.
    uint32_t dotColor = color == RED ? RED_DIM : color == GREEN ? GREEN_DIM : color == MAGENTA ? MAGENTA_DIM : OFF;

    GenerateNumbers(metalSpot[selectedMetal].close, numbers, &dot);
    SetSegments(numbers, color);
    SetDots(dot, dotColor);
    SetIndicators(BLUE);
    UpdateStrips();
}

void setup()
{
    Serial.begin(74880); // BAUD is default ESP8266 debug BAUD.

    Serial.println("Spot Clock 2 starting up...");

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

    /*
   // Development parameters when bypassing SD card.
    ssid = "RedSky";
    password = "happyredcat";
    timeZone = "EST";
    brightness = 100;
    cycleDelay = 3000;
    */

    Serial.print("SSID: ");
    Serial.println(ssid);
    Serial.print("Password: ");
    Serial.println(password);
    Serial.print("Time zone: ");
    Serial.println(timeZone);
    Serial.printf("Brightness: %u\n", brightness);
    Serial.printf("CycleDelay: %u\n", cycleDelay);

    Serial.print("Connecting to WiFi...");
    WiFi.begin(ssid, password);

    msTimer timer(250);
    while (WiFi.status() != WL_CONNECTED)
    {
        indicatorStatus = wifiConnecting;
        UpdateConnectionIndicator();
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
    // Check for WiFi status change.
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

    // Update spot values on timer.
    static msTimer timerFetch(0);
    if (timerFetch.elapsed())
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            timerFetch.setDelay(60000);

            indicatorStatus = fetchingData;
            UpdateConnectionIndicator();

            bool success = false;
            success = UpdateTime();
            success = success | GetUpdatedSpot();
            indicatorStatus = success ? wifiConnected : fetchFailed;

            UpdateDisplay();
        }
    }

    // Automatically change metal selection on elasped timer.
    static msTimer timerMetalSelection(cycleDelay);
    static bool holdFlag = false;
    if (timerMetalSelection.elapsed())
    {
        if (!holdFlag)
        {
            IncrementMetalSelection();
            UpdateDisplay();
        }
    }

    // Change metal selection on pressed select button.
    buttonSelect.read();
    if (buttonSelect.wasPressed())
    {
        timerMetalSelection.resetDelay();
        IncrementMetalSelection();
        UpdateDisplay();
    }
    if (buttonSelect.pressedFor(1000))
    {
        // TODO: indicate to the user that a hold is placed/removed.
        holdFlag = !holdFlag;
    }

    // Update status indicator.
    UpdateConnectionIndicator();
}