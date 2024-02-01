#include <IRremoteESP8266.h>
#include <IRSend.h>
#include <pins_arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <TimeLib.h>
#include <TelnetStream.h>
// #include <sunset.h>
#include "secrets.h"

const long gmtOffset_sec = -18000; // UTC-5
const int daylightOffset_sec = 3600;

const int RECV_PIN = A17;
const int SEND_PIN = 4;
const int LIGHT_SENSOR_PIN = 34;

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

void setup()
{
    irsend.begin();
    Serial.begin(115200);
    Serial.println("Booting");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFISSID, WIFIPW);
    while (WiFi.waitForConnectResult() != WL_CONNECTED)
    {
        Serial.println("Connection Failed! Rebooting...");
        delay(5000);
        ESP.restart();
    }

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

    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
    time_t now = time(nullptr);
    while (now < SECS_YR_2000)
    {
        delay(100);
        now = time(nullptr);
    }
    setTime(now);

    IPAddress ip = WiFi.localIP();
    Serial.println();
    Serial.println("Connected to WiFi network.");
    Serial.print("Connect with Telnet client to ");
    Serial.println(ip);

    TelnetStream.begin();
}

bool roomLightsDetected = true;
const int LIGHT_SENSOR_THRESHOLD = 150;

void loop()
{
    ArduinoOTA.handle();
    handleTelnetRead();

    int lightSensorReading = analogRead(LIGHT_SENSOR_PIN);
    if (lightSensorReading > LIGHT_SENSOR_THRESHOLD && roomLightsDetected)
    {
        sendKeycode(keyCodes.power);
        roomLightsDetected = false;
        log("Room lights off");
    }
    else if (lightSensorReading < LIGHT_SENSOR_THRESHOLD && !roomLightsDetected)
    {
        sendKeycode(keyCodes.power);
        roomLightsDetected = true;
        log("Room lights on");
    }

    // static unsigned long next;
    // if (millis() - next > 1000) {
    // next = millis();
    // char reading[20];
    // sprintf(reading, "Light: %d", analogRead(LIGHT_SENSOR_PIN));
    // log(reading);
    // }

    // for (int i = 0; i < 9; i++) {
    //   irsend.sendNEC(datas[i], 32, 2);
    //   // delay(800);
    // }
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