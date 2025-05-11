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
   In the daytime, the clock will be set to the lowest brightness if the light sensor reads a value less than DIM_THRESHOLD_DAY.
   If the light sensor reads a value greater than BRIGHT_THRESHOLD_DAY, the clock will be set to the highest brightness.
   If the light sensor reads a value between DIM_THRESHOLD_DAY and BRIGHT_THRESHOLD_DAY, the clock will be set to the middle brightness.
   At night, the clock will be turned off if the light sensor reads a value less than OFF_THRESHOLD_NIGHT, and turned on otherwise.

   The ESP32 does not know the current state of the clock, so the clock is expected to be in this state when the ESP is turned on:
     Brightness: Maximum
     Power: Off
 ****/

// Thresholds
#define OFF_THRESHOLD_NIGHT 50
#define DIM_THRESHOLD_DAY 800
#define BRIGHT_THRESHOLD_DAY 1200
#define HYSTERESIS 50

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
  .power = 0xFFFFFFFF,
  .up = 0xDEC228D7,
  .left = 0xDEC26897,
  .down = 0xDEC200FF,
  .right = 0xDEC218E7,
  .clock = 0xDEC2807F,
  .temp = 0xDEC220DF,
  .alarm = 0xDEC2C03F,
  .bright = 0xDEC2609F
};

IRsend irsend(SEND_PIN);
SunSet sun;
int timezone;

const int BUFFER_SIZE = 100;
int lightSensorReadings[BUFFER_SIZE];
int currentIndex = 0;
int lightSensorReading = 0;

void updateAverage(int newReading)
{
  lightSensorReading -= lightSensorReadings[currentIndex];
  lightSensorReadings[currentIndex] = newReading;

  int sum = 0;
  for (int i = 0; i < BUFFER_SIZE; i++)
  {
    sum += lightSensorReadings[i];
  }
  lightSensorReading = sum / BUFFER_SIZE;
  currentIndex = (currentIndex + 1) % BUFFER_SIZE;
}

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
    Serial.println("Start updating " + type);
  })
  .onEnd([]()
  {
    Serial.println("\nEnd");
  })
  .onProgress([](unsigned int progress, unsigned int total)
  {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  })
  .onError([](ota_error_t error)
  {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

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

  // Initialize the circular buffer with initial readings
  for (int i = 0; i < BUFFER_SIZE; i++)
  {
    lightSensorReadings[i] = analogRead(LIGHT_SENSOR_PIN);
    lightSensorReading += lightSensorReadings[i];
  }
  lightSensorReading /= BUFFER_SIZE;

  // Print IP Address
  IPAddress ip = WiFi.localIP();
  Serial.println();
  Serial.println("Connected to WiFi network.");
  Serial.print("Connect with Telnet client to ");
  Serial.println(ip);

  TelnetStream.begin();
}

//bool roomLightsDetected = true;
bool isPoweredOn = false;
bool isChangingBrightness = false;

#define BRIGHT_OFF 0
#define BRIGHT_HIGH 1
#define BRIGHT_MID 2
#define BRIGHT_LOW 3
#define BRIGHT_AUTO 4
#define MAX_BRIGHTNESS 4
#define LOGGING_DELAY 5000

int brightness = 1; // 0: off, 1: high, 2: medium, 3: low, 4: auto (unused)
int requestedBrightness = 1;

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

  // Check at this point of time if our brightness is not requested brightness
  isChangingBrightness = brightness != requestedBrightness;

  // Handle brightness (we can't just hit delay() because we need to handle OTA and Telnet)
  static unsigned long next1;
  if (millis() - next1 > 250)
  {
    next1 = millis();
    if (isChangingBrightness)
    {
      log("Brightness is changing from " + String(brightness) + " / Requested: " + String(requestedBrightness));
      brightness++;
      if (brightness > MAX_BRIGHTNESS)
      {
        brightness = 0;
      }
      log("Brightness changed to " + String(brightness));
      sendKeycode(keyCodes.bright);
    }
  }

  // If brightness is currently being changed, we don't need to do anything else
  if (isChangingBrightness)
  {
    return;
  }

  // Get the latest light sensor reading
  int newReading = analogRead(LIGHT_SENSOR_PIN);

  // Update the average reading
  updateAverage(newReading);

  // Set the adjusted light sensor reading
  int adjustedLightSensorReading = lightSensorReading / 50;
  adjustedLightSensorReading *= 50;


  // Logging data every 5 seconds
  static unsigned long next2;
  if (millis() - next2 > LOGGING_DELAY)
  {
    next2 = millis();
    // log("Rise: " + String(sunrise) + " Set: " + String(sunset) + " MinsPM: " + String(minsPastMidnight));
    log("Light sensor: " + String(lightSensorReading) + " / adj " + String(adjustedLightSensorReading) + " / Brightness: " + brightness);
  }


  // Different logic for day and night
  if (minsPastMidnight > sunrise && minsPastMidnight < sunset)
  {
    // Day
    if (adjustedSensorReading < DIM_THRESHOLD_DAY-HYSTERESIS && brightness != BRIGHT_LOW) {
      setBrightness(BRIGHT_LOW);
      log("Day / Brightness low / Brightness fell below " + String(DIM_THRESHOLD_DAY-HYSTERESIS));
    }
    else if (adjustedLightSensorReading > DIM_THRESHOLD_DAY+HYSTERESIS && brightness != BRIGHT_MID) {
      setBrightness(BRIGHT_MID);
      log("Day / Brightness medium / Brightness rose above " + String(DIM_THRESHOLD_DAY+HYSTERESIS));
    }
    else if (adjustedLightSensorReading > BRIGHT_THRESHOLD_DAY-HYSTERESIS && brightness != BRIGHT_HIGH) {
      setBrightness(BRIGHT_HIGH);
      log("Day / Brightness high / Brightness rose above " + String(BRIGHT_THRESHOLD_DAY-HYSTERESIS));
    } 
    else if (adjustedLightSensorReading < BRIGHT_THRESHOLD_DAY+HYSTERESIS && brightness != BRIGHT_MID) {
      setBrightness(BRIGHT_MID);
      log("Day / Brightness medium / Brightness fell below " + String(BRIGHT_THRESHOLD_DAY+HYSTERESIS));
    }
  }
  else
  {
    // Night
    if (adjustedLightSensorReading > OFF_THRESHOLD_NIGHT+HYSTERESIS && brightness != BRIGHT_LOW)
    {
      setBrightness(BRIGHT_LOW);
      log("Night / Powered on / Brightness rose above " + String(OFF_THRESHOLD_NIGHT+HYSTERESIS));
    }
    else if (adjustedLightSensorReading < OFF_THRESHOLD_NIGHT-HYSTERESIS && brightness != BRIGHT_OFF)
    {
      setBrightness(BRIGHT_OFF);
      log("Night / Powered off / Brightness fell below " + String(OFF_THRESHOLD_NIGHT-HYSTERESIS));
    }
  }
}

void setBrightness(int requestedBrightnessParam)
{
  log("Brightness requested: " + String(requestedBrightnessParam));
  requestedBrightness = requestedBrightnessParam;
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
    case 'r':
      TelnetStream.stop();
      delay(100);
      ESP.restart();
      break;
    case 'c':
      TelnetStream.println("bye bye");
      TelnetStream.stop();
      break;
    case 'b':
      sendKeycode(keyCodes.bright);
      TelnetStream.println("Brightness key sent");
      break;
    case 's':
      int tempRequestedBrightness = requestedBrightness;

      if (++tempRequestedBrightness > MAX_BRIGHTNESS) {
        tempRequestedBrightness = 0;
      }

      setBrightness(tempRequestedBrightness);
      break;
  }
}
