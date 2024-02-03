#include <IRremoteESP8266.h>
#include <IRSend.h>
#include <pins_arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <TimeLib.h>
#include <TelnetStream.h>
#include <sunset.h>

/****
 * In the daytime, the clock will be set to the lowest brightness if the light sensor reads a value less than DIM_THRESHOLD_DAY.
 * If the light sensor reads a value greater than BRIGHT_THRESHOLD_DAY, the clock will be set to the highest brightness.
 * If the light sensor reads a value between DIM_THRESHOLD_DAY and BRIGHT_THRESHOLD_DAY, the clock will be set to the middle brightness.
 * At night, the clock will be turned off if the light sensor reads a value less than OFF_THRESHOLD_NIGHT, and turned on otherwise.
 *
 * The ESP32 does not know the current state of the clock, so the clock is expected to be in this state when the ESP is turned on:
 *   Brightness: Maximum
 *   Power: Off
 ****/

// Thresholds
#define OFF_THRESHOLD_NIGHT 50
#define DIM_THRESHOLD_DAY 800
#define BRIGHT_THRESHOLD_DAY 1200

// Timezone
#define GMTOFFSET_SEC -18000 // UTC-5
#define DAYLIGHTOFFSET_SEC 3600

// Pins
#define RECV_PIN 27
#define SEND_PIN 4
#define LIGHT_SENSOR_PIN 34

// Ensure WiFi details and location are configured in secrets.h
// Define the following: WIFISSID, WIFIPW, LATITUDE, LONGITUDE
#include "secrets.h"

///////// End of Configuration /////////

struct
{
    uint64_t power;
    uint64_t up;
    uint64_t left;
    uint64_t down;
    uint64_t right;
    uint64_t clock;
    uint64_t temp;
    uint64_t alarm;
    uint64_t bright;
} keyCodes = {
    .power = 0x1FE7887,
    .up = 0x1FE40BF,
    .left = 0x1FE20DF,
    .down = 0x1FE10EF,
    .right = 0x1FE609F,
    .clock = 0x1FE50AF,
    .temp = 0x1FE30CF,
    .alarm = 0x1FEF807,
    .bright = 0x1FE708F};

IRsend irsend(SEND_PIN);
SunSet sun;
int timezone;

void setup()
{
    // Set up IRSend
    irsend.begin();

    // Set up serial
    Serial.begin(115200);
    Serial.println("Booting");

    // Connect to WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFISSID, WIFIPW);
    while (WiFi.waitForConnectResult() != WL_CONNECTED)
    {
        Serial.println("Connection Failed! Rebooting...");
        delay(5000);
        ESP.restart();
    }

    // Set up ArduinoOTA
    ArduinoOTA
        .onStart([]()
                 {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type); })
        .onEnd([]()
               { Serial.println("\nEnd"); })
        .onProgress([](unsigned int progress, unsigned int total)
                    { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
        .onError([](ota_error_t error)
                 {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed"); });

    ArduinoOTA.begin();

    // Set up NTP
    configTime(GMTOFFSET_SEC, DAYLIGHTOFFSET_SEC, "pool.ntp.org");
    time_t now = time(nullptr);
    while (now < SECS_YR_2000)
    {
        delay(100);
        now = time(nullptr);
    }
    setTime(now);

    // Set up adjusted timezone
    timezone = GMTOFFSET_SEC / 3600;
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    if (timeinfo.tm_isdst)
    {
        timezone++;
    }

    // Set up SunSet
    sun.setPosition(LATITUDE, LONGITUDE, timezone);

    // Print IP Address
    IPAddress ip = WiFi.localIP();
    Serial.println();
    Serial.println("Connected to WiFi network.");
    Serial.print("Connect with Telnet client to ");
    Serial.println(ip);

    TelnetStream.begin();
}

bool roomLightsDetected = true;
bool isPoweredOn = false;
int brightness = 3; // 0: auto (unused), 1: low, 2: medium, 3: high

void loop()
{
    // Handle OTA and Telnet
    ArduinoOTA.handle();
    handleTelnetRead();

    // Set SunSet parameters
    sun.setCurrentDate(year(), month(), day());
    sun.setTZOffset(timezone);
    double sunrise = sun.calcSunrise();
    double sunset = sun.calcSunset();

    // sunrise/sunset are in minutes past midnight, so we need to get ours too
    double minsPastMidnight = hour() * 60 + minute() + GMTOFFSET_SEC / 60;

    // Logging
    static unsigned long next;
    if (millis() - next > 1000)
    {
        next = millis();
        log("Sunrise: " + String(sunrise));
        log("Sunset: " + String(sunset));
        log("Mins past midnight: " + String(minsPastMidnight));
    }

    int lightSensorReading = analogRead(LIGHT_SENSOR_PIN);
    // Different logic for day and night
    if (minsPastMidnight > sunrise && minsPastMidnight < sunset)
    {
        if (!isPoweredOn) {
            sendKeycode(keyCodes.power);
            isPoweredOn = true;
            log("Day / Powered on");
        }
        
        // Day
        if (lightSensorReading < DIM_THRESHOLD_DAY)
        {
            if (brightness != 1)
            {

                setBrightness(1);
                log("Day / Brightness low");
            }
        }
        else if (lightSensorReading > BRIGHT_THRESHOLD_DAY)
        {
            if (brightness != 3)
            {
                setBrightness(3);
                log("Day / Brightness high");
            }
        }
        else
        {
            if (brightness != 2)
            {
                setBrightness(2);
                log("Day / Brightness medium");
            }
        }
    }
    else
    {
        // Night
        if (lightSensorReading > OFF_THRESHOLD_NIGHT && !isPoweredOn)
        {
            sendKeycode(keyCodes.power);
            isPoweredOn = true;
            log("Night / Powered on");
        }
        else if (lightSensorReading < OFF_THRESHOLD_NIGHT && isPoweredOn)
        {
            sendKeycode(keyCodes.power);
            isPoweredOn = false;
            log("Night / Powered off");
        }
    }

    // int lightSensorReading = analogRead(LIGHT_SENSOR_PIN);
    // if (lightSensorReading > OFF_THRESHOLD_NIGHT && roomLightsDetected)
    // {
    //     sendKeycode(keyCodes.power);
    //     roomLightsDetected = false;
    //     log("Room lights off");
    // }
    // else if (lightSensorReading < OFF_THRESHOLD_NIGHT && !roomLightsDetected)
    // {
    //     sendKeycode(keyCodes.power);
    //     roomLightsDetected = true;
    //     log("Room lights on");
    // }

    // static unsigned long next;
    // if (millis() - next > 1000)
    // {
    //     struct tm timeinfo;
    //     getLocalTime(&timeinfo);
    //     log("Is DST? " + String(timeinfo.tm_isdst));
    //     next = millis();
    //     char reading[20];
    //     sprintf(reading, "Light: %d", analogRead(LIGHT_SENSOR_PIN));
    //     log(reading);
    // }
}

void setBrightness(int requestedBrightness)
{
    while (brightness != requestedBrightness)
    {
        brightness--;
        if (brightness < 0)
        {
            brightness = 3;
        }
        sendKeycode(keyCodes.bright);
        delay(250);
    }
}

void sendKeycode(uint64_t code)
{
    irsend.sendNEC(code, 32, 2);
}

void log(String msg)
{
    static int i = 0;

    char timeStr[20];
    sprintf(timeStr, "%02d-%02d-%02d %02d:%02d:%02d", year(), month(), day(), hour(), minute(), second());

    TelnetStream.print(i++);
    TelnetStream.print(" ");
    TelnetStream.print(timeStr);
    TelnetStream.print(" ");
    TelnetStream.println(msg);
}

void handleTelnetRead()
{
    switch (TelnetStream.read())
    {
    case 'R':
        TelnetStream.stop();
        delay(100);
        ESP.restart();
        break;
    case 'C':
        TelnetStream.println("bye bye");
        TelnetStream.stop();
        break;
    }
}