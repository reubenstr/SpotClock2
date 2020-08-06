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
Adafruit_NeoPixel strip1 = Adafruit_NeoPixel(42, PIN_STRIP_1, NEO_RGB + NEO_KHZ800);
Adafruit_NeoPixel strip2 = Adafruit_NeoPixel(47, PIN_STRIP_2, NEO_RGB + NEO_KHZ800);
Adafruit_NeoPixel strip3 = Adafruit_NeoPixel(34, PIN_STRIP_3, NEO_RGB + NEO_KHZ800);

Button buttonSelect(PIN_BUTTON_SELECT, 25, false, true);

// SD card parameters.
String ssid, password, brightness;

const char *wifiFilePath = "/wifi.txt";

const int chipSelect = D8;

float openAu, openAg, openPt;
float spotAu, spotAg, spotPt;

const int blankSegment = 10;




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
        strip3.setPixelColor(i, Wheel(wheelPos + i * 6));
    }

    // Set metal indicators;
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
        uint32_t color = toggle ? Color(127, 127, 0) : Color(0, 0, 0);
        strip2.setPixelColor(stripStatusIndicatorIndex, color);
    }
    else if (indicatorStatus == wifiConnecting)
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
    else if (indicatorStatus == wifiConnected)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, Color(0, 255, 0));
    }
    else if (indicatorStatus == wifiDisconnected)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, Color(255, 0, 0));
    }
    else if (indicatorStatus == fetchingData)
    {
        strip2.setPixelColor(stripStatusIndicatorIndex, Color(0, 0, 255));
    }
    else if (indicatorStatus == fetchFailed)
    {
        static msTimer timer(1000);
        static bool toggle;
        if (timer.elapsed())
        {
            toggle = !toggle;
        }
        uint32_t color = toggle ? Color(255, 0, 0) : Color(0, 255, 0);
        strip2.setPixelColor(stripStatusIndicatorIndex, color);
    }
    else if (indicatorStatus == fetchSuccess)
    {
        static msTimer timer(1000);
        static bool toggle;
        if (timer.elapsed())
        {
            toggle = !toggle;
        }
        uint32_t color = toggle ? Color(0, 255, 0) : Color(0, 0, 255);
        strip2.setPixelColor(stripStatusIndicatorIndex, color);
    }

    strip2.show();
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

    static int metalSelectionIndex = 0;
    if (++metalSelectionIndex > 2)
    {
        metalSelectionIndex = 0;
    }

   float price;

    if (!FetchDataFromInternet(&price, "close", metals[metalSelectionIndex]))
    {
        return false;
    }  

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

void IncrementMetalSelection()
{
    if (selectedMetal == au)
        selectedMetal = ag;
    else if (selectedMetal == ag)
        selectedMetal = pt;
    else if (selectedMetal == pt)
        selectedMetal = au;
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

    ESP.wdtFeed();

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
        timerFetch.setDelay(300000);

        indicatorStatus = fetchingData;
        UpdateConnectionIndicator(indicatorStatus);

        bool success = GetUpdatedSpot();
        indicatorStatus = success ? fetchSuccess : fetchFailed;
    }

    static msTimer timerMetalSelection(3000);
    if (timerMetalSelection.elapsed())
    {
        IncrementMetalSelection();
    }

    buttonSelect.read();
    if (buttonSelect.wasPressed())
    {
        IncrementMetalSelection();
    }

    int numbers[5];
    int dot;
    float spot = selectedMetal == au ? spotAu : selectedMetal == ag ? spotAg : selectedMetal == pt ? spotPt : 0;

    GenerateNumbers(spot, numbers, &dot);
    SetSegments(numbers, 0x0000ff);
    SetDots(dot, 0x0000ff);
    SetIndicators(0x0000ff);
    UpdateStrips();
}