#include <Arduino.h>
#include "msTimer.h"
#include "flasher.h"

#include <SPI.h>
#include <SD.h>

#include "ESP8266WiFi.h"

#include <esp8266httpclient.h>

#include "ArduinoJson.h"
#include <Adafruit_NeoPixel.h> // https://github.com/adafruit/Adafruit_NeoPixel

#define PIN_STRIP_1 5
#define PIN_STRIP_2 4
#define PIN_STRIP_3 0

const int stripMaxBrightness = 50;
const int stripStatusIndicatorIndex = 4;
// Due to hardware limitations of the ESP8266 long WS2812b strips are not possible.
// Therefore segments, indicators, and dots are combined in a awkward combination to prevent flickering.
Adafruit_NeoPixel strip1 = Adafruit_NeoPixel(42, PIN_STRIP_1, NEO_RGB + NEO_KHZ800);
Adafruit_NeoPixel strip2 = Adafruit_NeoPixel(47, PIN_STRIP_2, NEO_RGB + NEO_KHZ800);
Adafruit_NeoPixel strip3 = Adafruit_NeoPixel(34, PIN_STRIP_3, NEO_RGB + NEO_KHZ800);

String ssid;
String password;
const char *wifiFilePath = "/wifi.txt";

const int chipSelect = D8;

float spotAu, spotAg, spotPt;

const int blankSegment = 10;

enum WifiStatus
{
    disconnected,
    connected,
    connecting
} wifiStatus;

bool sdCardMounted;
bool isFetchingData;

enum Metals
{
    au,
    ag,
    pt
} selectedMetal;

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

bool GetWifiCredentialsFromSDCard()
{
    File file = SD.open(wifiFilePath);

    Serial.println("Attempting to fetch WIFI parameters...");

    if (!file)
    {
        Serial.printf("Failed to open file: %s\n", wifiFilePath);
        Serial.printf("Creating file with path: %s\n", wifiFilePath);
        File file = SD.open(wifiFilePath, FILE_WRITE);
        file.print("SSID: \"your SSID inside quotations\"\nPassword: \"your password inside quotations\"");
        file.close();
    }
    else
    {
        if (file.find("SSID: \""))
        {
            ssid = file.readStringUntil('"');
            if (file.find("Password: \""))
            {
                password = file.readStringUntil('"');

                Serial.print("SSID: ");
                Serial.println(ssid);
                Serial.print("Password: ");
                Serial.println(password);
                return true;
            }
        }
    }

    return false;
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

    if (iPart < 100)
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
    for (int i = 0; i < 3; i++)
    {
        strip2.setPixelColor(i, 0);
    }

    if (dot != blankSegment)
    {
        strip2.setPixelColor(dot, color);
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
    strip3.setPixelColor(7, selectedMetal == au ? color : 0);
    strip3.setPixelColor(8, selectedMetal == au ? color : 0);
    strip3.setPixelColor(9, selectedMetal == ag ? color : 0);
    strip3.setPixelColor(10, selectedMetal == ag ? color : 0);
    strip3.setPixelColor(11, selectedMetal == pt ? color : 0);
    strip3.setPixelColor(12, selectedMetal == pt ? color : 0);
}

void UpdateStrips()
{
    strip1.show();
    strip2.show();
    strip3.show();
}

void UpdateStatusIndicator(bool isFetchingData = false)
{
    if (isFetchingData)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, Color(0, 0, 255));
    }
    else if (sdCardMounted == false)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, Color(127, 127, 0));
    }
    else if (wifiStatus == connected)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, Color(0, 255, 0));
    }
    else if (wifiStatus == disconnected)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, Color(255, 0, 0));
    }
    else if (wifiStatus == connecting)
    {
        static msTimer timer(250);
        static bool toggle;
        if (timer.elapsed())
        {
            toggle = !toggle;
        }
        uint32_t color = toggle ? Color(255, 0, 0) : Color(0, 0, 0);
        strip2.setPixelColor(stripStatusIndicatorIndex, color);
    }
    strip2.show();
}

void sdFailure()
{
    sdCardMounted = false;
    UpdateStatusIndicator();

    while (1)
    {
        // Halt system.
    }
}

bool FetchDataFromInternet()
{

    static int metalSelectionIndex = 0;
    if (++metalSelectionIndex > 2)
    {
        metalSelectionIndex = 0;
    }

    String metalSelection = metalSelectionIndex == 0 ? "XAU_USD" : metalSelectionIndex == 1 ? "XAG_USD" : metalSelectionIndex == 2 ? "XPT_USD" : "";

    String host = "http://api.fxhistoricaldata.com/indicators?timeframe=5minute&item_count=1&expression=close&instruments=" + metalSelection;

    Serial.print("Connecting to ");
    Serial.println(host);

    HTTPClient http;
    http.begin(host);
    int httpCode = http.GET();

    String payload;

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

    float price = doc["results"][metalSelection]["data"][0][1];

    if (metalSelectionIndex == 0)
    {
        spotAu = price;
    }
    else if (metalSelectionIndex == 1)
    {
        spotAg = price;
    }
    else if (metalSelectionIndex == 2)
    {
        spotPt = price;
    }

    return true;
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

    if (!InitSDCard())
    {
        sdFailure();
    }

    if (!GetWifiCredentialsFromSDCard())
    {
        sdFailure();
    }
    sdCardMounted = true;

    WiFi.begin(ssid, password);
    /*   */

    Serial.print("Connecting to WiFi...");

    msTimer timer(250);
    while (WiFi.status() != WL_CONNECTED)
    {
        wifiStatus = connecting;
        UpdateStatusIndicator();
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

    // static int numbers[5];
    static msTimer timer(100);
    // static int n = 0;

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiStatus = connected;
    }
    else if (WiFi.status() != WL_CONNECTED)
    {
        wifiStatus = disconnected;
    }

    UpdateStatusIndicator();

    static msTimer timerFetch(0);
    if (timerFetch.elapsed())
    {
        UpdateStatusIndicator(true);
        FetchDataFromInternet();

        timerFetch.setDelay(300000);
    }

    int numbers[5];
    int dot;

    GenerateNumbers(spotAu, numbers, &dot);

    SetSegments(numbers, 0x0000ff);
    SetDots(dot, 0x0000ff);
    SetIndicators(0x0000ff);
    UpdateStrips();
}