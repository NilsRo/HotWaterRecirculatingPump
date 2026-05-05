#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <arduino-timer.h>
#include <AsyncMqttClient.h>
#include <map>
#include <SSD1306Wire.h>
#include <ArduinoOTA.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h>
#include <IotWebConfTParameter.h>
#include <IotWebConfESP32HTTPUpdateServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <uptime.h>
#include <algorithm>
#include <langu.h>
#include <esp_core_dump.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <OneButton.h>
#include "average.h"

#define STRING_LEN 128
#define DALLASADRESS_LEN 17
#define TEMP_QUEUE_LENGTH 1
#define nils_length(x) ((sizeof(x) / sizeof(0 [x])) / ((size_t)(!(sizeof(x) % sizeof(0 [x])))))
// #define nils_length( x ) ( sizeof(x) )

static const char *TAG = "HotWaterPump";

// Syslog levels
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_INFO 6
#define LOG_DEBUG 7

// watchdog
RTC_DATA_ATTR int crashCounter = 0;
RTC_DATA_ATTR uint32_t lastCrashTime = 0;

// syslog
char syslogServer[STRING_LEN];
char syslogPortStr[6];
int syslogPort = 514; // default syslog port

// ports
const int ONEWIREPIN = D8;
const int PUMPPIN = D3;
const int VALVEPIN = D4;
const int DISPLAYPIN = D5;
const int WIFICONFIGPIN = D7;

// button
OneButton userBtn(DISPLAYPIN, true);
OneButton resetBtn(DISPLAYPIN, true);

// OTA
TaskHandle_t otaTaskHandle = NULL;

bool manualMode = false;

// pump
static float t[] = {255.0, 255.0, 255.0, 255.0, 255.0}; // letzten 5 Temepraturwerte speichern
bool pumpRunning = false;
unsigned long pumpBlock;
unsigned long pumpStartedAt;
String pump[20] = {"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};
unsigned int pumpCnt;
bool pumpCntInit = true;
bool pumpFirstCall = true;

// valve
float valvePressure;
bool valveState = false;
unsigned long valveOpenAt;
unsigned long valveOpenAtTs;
unsigned long valveCloseAt;
unsigned long valveCloseAtTs;
unsigned long valveSecToRefill;
int valveMaxOpen;
char valveMaxOpenStr[4];
bool valveError = false;
bool valveInitFill = false;
SimpleAverage valvePressureAvg(10);
String valveHist[20] = {"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};
unsigned int valveHistCnt;
bool valveHistCntInit = true;

// temps
// Queue Handle
TaskHandle_t tempTaskHandle = NULL;
QueueHandle_t tempQueue = NULL;
SemaphoreHandle_t tempSemaphore = NULL;
// Typen
typedef uint8_t DeviceAddress_t[8];
typedef struct
{
  DeviceAddress_t out;
  DeviceAddress_t ret;
  DeviceAddress_t intl;
} SensorIds_t;
typedef struct
{
  float tempOut;
  float tempRet;
  float tempInt;
  bool outConnected;
  bool retConnected;
  bool intConnected;
  TickType_t timestamp; // optional
} TempReport_t;

OneWire oneWire(ONEWIREPIN);
DallasTemperature sensors(&oneWire);
DeviceAddress sensorOut_id;
DeviceAddress sensorRet_id;
DeviceAddress sensorInt_id;
TempReport_t sensorData;

bool sensorError = false;
float tempDiff;
float tempDiffTrigger;
char tempDiffTriggerStr[32];
float mqttTempOut;
float mqttTempRet;
float mqttTempInt;
float mqttTempDiff;
unsigned int checkCnt;

// OLED Display
SSD1306Wire display(0x3C, SDA, SCL); // ADDRESS, SDA, SCL  -  SDA and SCL usually populate automatically based on your board's pins_arduino.h e.g. https://github.com/esp8266/Arduino/blob/master/variants/nodemcu/pins_arduino.h
unsigned int displayPage;
unsigned int displayPageLastRuns = 1;
bool networksPageFirstCall = true;
bool displayOn = true;
long displayOnAt;
bool needReset = false;

#define MQTT_PORT 1883
#define MQTT_PUB_TEMP_OUT "dhw_Tflow_measured"
#define MQTT_PUB_TEMP_RET "dhw_Treturn"
#define MQTT_PUB_TEMP_DIFF "dhw_Tdelta"
#define MQTT_PUB_TEMP_INT "Tint"
#define MQTT_PUB_PUMP "dhw_pump_circulation"
#define MQTT_PUB_VALVE_OPENED "dhw_valve_opened"
#define MQTT_PUB_VALVE_SEC_OPENED "dhw_valve_sec_opened"
#define MQTT_PUB_VALVE_SEC_TO_REFILL "dhw_valve_sec_to_refill"
#define MQTT_PUB_VALVE_PRESSURE_AVG "dhw_valve_pressure_avg"
#define MQTT_PUB_STATUS "status"
#define MQTT_PUB_WIFI "log/wifi"
#define MQTT_PUB_INFO "log/info"
#define MQTT_PUB_SYSINFO "log/sysinfo"
AsyncMqttClient mqttClient;
String mqttDisconnectReason;
char mqttDisconnectTime[20];
unsigned long mqttDisconnectTimestamp;
char mqttServer[STRING_LEN];
char mqttUser[STRING_LEN];
char mqttPassword[STRING_LEN];
char mqttTopicPath[STRING_LEN];
static char mqttWillTopic[STRING_LEN];
char mqttHeaterStatusTopic[STRING_LEN];
char mqttHeaterStatusValue[STRING_LEN];
bool mqttHeaterStatus = true;
char mqttPumpTopic[STRING_LEN];
char mqttPumpValue[STRING_LEN];
bool mqttPump;
char mqttValvePressureTopic[STRING_LEN];
String mqttStatus = "";
char mqttThermalDesinfectionTopic[STRING_LEN];
char mqttThermalDesinfectionValue[STRING_LEN];
bool mqttThermalDesinfection = false;

Ticker mqttReconnectTimer;
auto timer = timer_create_default();
Ticker displayTimer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
WiFiUDP syslogUDP;
char ntpServer[STRING_LEN];
char ntpTimezone[STRING_LEN];
time_t now;
struct tm localTime;

IPAddress localIP;
char hostname[STRING_LEN];
unsigned long lastScan;
int networksFound;
int networksPage;
unsigned int networksPageTotal;
unsigned long displayPageSubChange;
unsigned int langu;

#define CONFIG_VERSION "7"
Preferences preferences;
int iotWebConfPinState = HIGH;
unsigned long iotWebConfPinChanged;
DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
static char chooserValues[][DALLASADRESS_LEN] = {"0", "0", "0"};
static char chooserNames[][STRING_LEN] = {"Sensor 1 NA", "Sensor 2 NA", "Sensor 3 NA"};
static const char languValues[][STRING_LEN] = {"0", "1"};
static const char languNames[][STRING_LEN] = {"German", "English"};
static const char syslogLevelValues[][STRING_LEN] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
static const char syslogLevelNames[][STRING_LEN] = {"None", "Error", "Warn", "Info", "Debug", "Verbose"};
IotWebConf iotWebConf("Zirkulationspumpe", &dnsServer, &server, "", CONFIG_VERSION);
IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("mqtt", "MQTT");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("server", "mqttServer", mqttServer, STRING_LEN);
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("user", "mqttUser", mqttUser, STRING_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("password", "mqttPassword", mqttPassword, STRING_LEN);
IotWebConfTextParameter mqttTopicPathParam = IotWebConfTextParameter("topicpath", "mqttTopicPath", mqttTopicPath, STRING_LEN, "ww/ht/");
IotWebConfTextParameter mqttHeaterStatusTopicParam = IotWebConfTextParameter("heater status topic", "mqttHeaterStatusTopic", mqttHeaterStatusTopic, STRING_LEN, "ht3/hometop/ht/hc1_Tniveau");
IotWebConfTextParameter mqttHeaterStatusValueParam = IotWebConfTextParameter("heater status value", "mqttHeaterStatusValue", mqttHeaterStatusValue, STRING_LEN, "3");
IotWebConfTextParameter mqttPumpTopicParam = IotWebConfTextParameter("external pump start topic", "mqttPumpTopic", mqttPumpTopic, STRING_LEN, "ht3/hometop/ht/dhw_pump_circulation");
IotWebConfTextParameter mqttPumpValueParam = IotWebConfTextParameter("external pump start Value", "mqttPumpValue", mqttPumpValue, STRING_LEN, "1");
IotWebConfTextParameter mqttThermalDesinfectionTopicParam = IotWebConfTextParameter("thermal desinfection topic", "mqttThermalDesinfectionTopic", mqttThermalDesinfectionTopic, STRING_LEN, "ht3/hometop/ht/dhw_thermal_desinfection");
IotWebConfTextParameter mqttThermalDesinfectionValueParam = IotWebConfTextParameter("thermal desinfection Value", "mqttThermalDesinfectionValue", mqttThermalDesinfectionValue, STRING_LEN, "1");
IotWebConfTextParameter mqttValvePressureTopicParam = IotWebConfTextParameter("system pressure", "mqttValvePressureTopic", mqttValvePressureTopic, STRING_LEN, "ht3/hometop/ht/ch_system_pressure");
IotWebConfParameterGroup ntpGroup = IotWebConfParameterGroup("ntp", "NTP");
IotWebConfTextParameter ntpServerParam = IotWebConfTextParameter("server", "ntpServer", ntpServer, STRING_LEN, "de.pool.ntp.org");
IotWebConfTextParameter ntpTimezoneParam = IotWebConfTextParameter("timezone", "ntpTimezone", ntpTimezone, STRING_LEN, "CET-1CEST,M3.5.0/02,M10.5.0/03");
IotWebConfParameterGroup valveGroup = IotWebConfParameterGroup("valve", "Valve");
iotwebconf::FloatTParameter valvePressureLowParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("valuePressureLowParam").label("pressure low").defaultValue(1.4).step(0.1).placeholder("e.g. 1.4").build();
iotwebconf::FloatTParameter valvePressureHighParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("valuePressureHighParam").label("pressure high").defaultValue(1.6).step(0.1).placeholder("e.g. 1.6").build();
IotWebConfNumberParameter valveMaxOpenParam = IotWebConfNumberParameter("maximal valve open minutes", "valveMaxOpen", valveMaxOpenStr, 4, "0");
IotWebConfParameterGroup tempGroup = IotWebConfParameterGroup("temp", "Temperature");
iotwebconf::SelectTParameter<DALLASADRESS_LEN> tempOutParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<DALLASADRESS_LEN>>("tempOutParam").label("out").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / DALLASADRESS_LEN).nameLength(STRING_LEN).build();
iotwebconf::SelectTParameter<DALLASADRESS_LEN> tempRetParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<DALLASADRESS_LEN>>("tempRetParam").label("return").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / DALLASADRESS_LEN).nameLength(STRING_LEN).build();
iotwebconf::SelectTParameter<DALLASADRESS_LEN> tempIntParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<DALLASADRESS_LEN>>("tempIntParam").label("internal").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / DALLASADRESS_LEN).nameLength(STRING_LEN).build();
iotwebconf::FloatTParameter tempRetDiffParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("tempRetDiffParam").label("return off diff.").defaultValue(10.0).step(0.5).placeholder("e.g. 23.4").build();
// iotwebconf::FloatTParameter tempDiffTriggerParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("tempDiffTriggerParam").label("temperature trigger").defaultValue(0.125).step(0.0625).placeholder("e.g. 0.12").build();
IotWebConfNumberParameter tempDiffTriggerParam = IotWebConfNumberParameter("temperature trigger", "tempDiffTriggerParam", tempDiffTriggerStr, 32, "0.125", "e.g. 0.125", "step='0.0625'");

IotWebConfParameterGroup miscGroup = IotWebConfParameterGroup("misc", "misc.");
iotwebconf::SelectTParameter<STRING_LEN> languParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("languParam").label("language").optionValues((const char *)languValues).optionNames((const char *)languNames).optionCount(sizeof(languValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("0").build();

IotWebConfParameterGroup syslogGroup = IotWebConfParameterGroup("syslog", "Syslog");
IotWebConfTextParameter syslogServerParam = IotWebConfTextParameter("server", "syslogServer", syslogServer, STRING_LEN, "");
IotWebConfTextParameter syslogPortParam = IotWebConfTextParameter("port", "syslogPort", syslogPortStr, 6, "514");
iotwebconf::SelectTParameter<STRING_LEN> syslogLogLevelParam = iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("syslogLogLevelParam").label("ESP_LOG level").optionValues((const char *)syslogLevelValues).optionNames((const char *)syslogLevelNames).optionCount(sizeof(syslogLevelValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("INFO").build();

/* #region common */
int mod(int x, int y)
{
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
}

float roundTo(float value, int decimals)
{
  float factor = pow(10, decimals);
  return roundf(value * factor) / factor;
}
/* #endregion */

/* #region Necessary forward declarations */
String getStatus();
String getWifiJson();
String getSysinfoJson();
void mqttPublish(const char *topic, const char *payload, bool force, bool jsonAddTimstamp);
void mqttSendTopics(bool mqttInit = false);
/* #endregion */

/* #region coredump */
String verbose_print_reset_reason(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_UNKNOWN:
    return (" Reset reason can not be determined");
  case ESP_RST_POWERON:
    return ("Reset due to power-on event");
  case ESP_RST_EXT:
    return ("Reset by external pin (not applicable for ESP32)");
  case ESP_RST_SW:
    return ("Software reset via esp_restart");
  case ESP_RST_PANIC:
    return ("Software reset due to exception/panic");
  case ESP_RST_INT_WDT:
    return ("Reset (software or hardware) due to interrupt watchdog");
  case ESP_RST_TASK_WDT:
    return ("Reset due to task watchdog");
  case ESP_RST_WDT:
    return ("Reset due to other watchdogs");
  case ESP_RST_DEEPSLEEP:
    return ("Reset after exiting deep sleep mode");
  case ESP_RST_BROWNOUT:
    return ("Brownout reset (software or hardware)");
  case ESP_RST_SDIO:
    return ("Reset over SDIO");
  default:
    return ("NO_MEAN");
  }
}

// void initCoreDumpFlash()
// {
//   if (esp_core_dump_image_check() != ESP_OK)
//   {
//     esp_core_dump_image_erase();
//     Serial.println("Invalid core dump deleted!");
//   }
// }

// bool checkCoreDump()
// {
//   size_t size = 0;
//   size_t address = 0;
//   if (esp_core_dump_image_get(&address, &size) == ESP_OK)
//   {
//     const esp_partition_t *pt = NULL;
//     pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");
//     if (pt != NULL)
//       return true;
//     else
//       return false;
//   }
//   else
//     return false;
// }

String readCoreDump()
{
  size_t size = 0;
  size_t address = 0;
  if (esp_core_dump_image_get(&address, &size) == ESP_OK)
  {
    const esp_partition_t *pt = NULL;
    pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");

    if (pt != NULL)
    {
      uint8_t bf[256];
      char str_dst[640];
      int16_t toRead;
      String return_str;

      for (int16_t i = 0; i < (size / 256) + 1; i++)
      {
        strcpy(str_dst, "");
        toRead = (size - i * 256) > 256 ? 256 : (size - i * 256);

        esp_err_t er = esp_partition_read(pt, i * 256, bf, toRead);
        if (er != ESP_OK)
        {
          Serial.printf("FAIL [%x]\n", er);
          break;
        }

        for (int16_t j = 0; j < 256; j++)
        {
          char str_tmp[3];

          sprintf(str_tmp, "%02x", bf[j]);
          strcat(str_dst, str_tmp);
        }

        return_str += str_dst;
      }
      return return_str;
    }
    else
    {
      return "Partition NULL";
    }
  }
  else
  {
    return "esp_core_dump_image_get() FAIL";
  }
}

void crash_me_hard()
{
  // provoke crash through writing to a nullpointer
  volatile uint32_t *aPtr = (uint32_t *)0x00000000;
  *aPtr = 0x1234567; // goodnight
}

void startCrashTimer(int secs)
{
  for (int i = 0; i <= secs; i++)
  {
    printf("Crashing in %d seconds..\n", secs - i);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  printf("Crashing..\n");
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  crash_me_hard();
}

void startCrash()
{
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Warmwater Recirculation Pump</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  s += "Crashing in 5 seconds...!";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
  startCrashTimer(5);
}

void handleCoreDump()
{
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=coredump.bin");
  server.sendHeader("Connection", "close");
  server.send(200, "application/octet-stream", readCoreDump());
}

void handleDeleteCoreDump()
{
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Warmwater Recirculation Pump</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  if (esp_core_dump_image_erase() == ESP_OK)
    s += "Core dump deleted";
  else
    s += "No core dump found!";
  s += "<p><button type=\"button\" onclick=\"javascript:history.back()\">Back</button>";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
}
/* #endregion */

/* #region watchdog */
void handleCrashCounter()
{
  uint32_t now = millis() / 1000; // Sekunden seit Boot

  // Reset-Gründe prüfen
  esp_reset_reason_t reason = esp_reset_reason();

  bool isCrash =
      reason == ESP_RST_TASK_WDT ||
      reason == ESP_RST_WDT ||
      reason == ESP_RST_PANIC ||
      reason == ESP_RST_BROWNOUT;

  if (isCrash)
  {
    // Wenn letzter Crash zu lange her → Counter verfallen lassen
    if (now - lastCrashTime > 600)
    { // 600 Sekunden = 10 Minuten
      crashCounter = 0;
    }

    crashCounter++;
    lastCrashTime = now;
  }
  else
  {
    // Normaler Boot → Counter verfallen lassen
    crashCounter = 0;
    lastCrashTime = now;
  }
}

void checkForFailover()
{
  if (crashCounter >= 3)
  {
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_set_boot_partition(next);
    esp_restart();
  }
}
/* #endregion */

/* #region NVS handling*/
void changeNvsMode(bool readOnly)
{
  static bool nvsStatus = false;
  if (nvsStatus)
  {
    preferences.end();
  }
  if (preferences.begin("settings", readOnly))
    nvsStatus = true;
  else
  {
    ESP_LOGE(TAG, "Error opening NVS-Namespace");
    for (;;)
      ; // leere Dauerschleife -> Ende
  }
}

void saveValveTimestampNvs(const char *prefKey, unsigned long value)
{
  changeNvsMode(false);
  preferences.putULong(prefKey, value);
  changeNvsMode(true);
}
/* #endregion */

/* #region WIFI Manager */
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  char tempStr[128];

  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Warmwater Recirculation Pump</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  s += "<fieldset id=" + String(mqttGroup.getId()) + ">";
  s += "<legend>" + String(mqttGroup.label) + "</legend>";
  s += "<table border = \"0\"><tr>";
  s += "<td>" + String(mqttServerParam.label) + ": </td>";
  s += "<td>" + String(mqttServer) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(mqttTopicPathParam.label) + ": </td>";
  s += "<td>" + String(mqttTopicPath) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(mqttHeaterStatusTopicParam.label) + ": </td>";
  s += "<td>" + String(mqttHeaterStatusTopic) + " - " + String(mqttHeaterStatusValue) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(mqttPumpTopicParam.label) + ": </td>";
  s += "<td>" + String(mqttPumpTopic) + " - " + String(mqttPumpValue) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(mqttThermalDesinfectionTopicParam.label) + ": </td>";
  s += "<td>" + String(mqttThermalDesinfectionTopic) + " - " + String(mqttThermalDesinfectionValue) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(mqttValvePressureTopicParam.label) + ": </td>";
  s += "<td>" + String(mqttValvePressureTopic) + "</td>";
  s += "</tr><tr>";
  s += "<td>status: </td>";
  if (mqttClient.connected())
    s += "<td>connected</td>";
  else
    s += "<td>disconnected</td>";
  s += "</tr><tr>";
  s += "<td>last disconnect reason: </td>";
  s += "<td>" + mqttDisconnectReason + "</td>";
  s += "</tr><tr>";
  s += "<td>last disconnect: </td>";
  s += "<td>" + String(mqttDisconnectTime) + "</td>";
  s += "</tr><tr>";
  s += "</tr></table></fieldset>";

  s += "<fieldset id=" + String(ntpGroup.getId()) + ">";
  s += "<legend>" + String(ntpGroup.label) + "</legend>";
  s += "<table border = \"0\"><tr>";
  s += "<td>" + String(ntpServerParam.label) + ": </td>";
  s += "<td>" + String(ntpServer) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(ntpTimezoneParam.label) + ": </td>";
  s += "<td>" + String(ntpTimezone) + "</td>";
  s += "</tr><tr>";
  s += "<td>actual local time: </td>";
  strftime(tempStr, 40, "%d.%m.%Y %T", &localTime);
  s += "<td>" + String(tempStr) + "</td>";
  s += "</tr></table></fieldset>";

  s += "<fieldset id=" + String(tempGroup.getId()) + ">";
  s += "<legend>" + String(tempGroup.label) + "</legend>";
  s += "<table border = \"0\"><tr>";
  s += "<td>" + String(tempOutParam.label) + ": </td>";
  s += "<td>";
  s += tempOutParam.value();
  dtostrf(sensorData.tempOut, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(tempRetParam.label) + ": </td>";
  s += "<td>";
  s += tempRetParam.value();
  dtostrf(sensorData.tempRet, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(tempIntParam.label) + ": </td>";
  s += "<td>";
  s += tempIntParam.value();
  dtostrf(sensorData.tempInt, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(tempRetDiffParam.label) + ": </td>";
  s += "<td>";
  s += tempRetDiffParam.value();
  s += "&#8451;</td>";
  s += "</tr><tr>";
  s += "<td>" + String(tempDiffTriggerParam.label) + ": </td>";
  s += "<td>";
  dtostrf(tempDiffTrigger, 2, 3, tempStr);
  s += tempStr;
  s += "&#8451;</td>";
  s += "</tr></table></fieldset>";

  s += "<fieldset id=" + String(valveGroup.getId()) + ">";
  s += "<legend>" + String(valveGroup.label) + "</legend>";
  s += "<table border = \"0\"><tr>";
  s += "<td>" + String(valvePressureHighParam.label) + ": </td>";
  s += "<td>" + String(valvePressureHighParam.value()) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(valvePressureLowParam.label) + ": </td>";
  s += "<td>" + String(valvePressureLowParam.value()) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(mqttValvePressureTopicParam.label) + ": </td>";
  s += "<td>" + String(valvePressure) + " / " + String(valvePressureAvg) + " avg. </td>";
  s += "</tr></table></fieldset>";

  s += "<fieldset id=\"status\">";
  s += "<legend>Status</legend>";
  s += "<p>status pump: ";
  s += getStatus();
  if (pumpRunning)
    s += "<p>pump: running";
  else
    s += "<p>pump: stopped";
  if (valveError)
    s += "<p>status valve : error (<a href=/resetValveError>reset</a>)";
  else
    s += "<p>status valve : ok";
  if (valveState)
  {
    s += "<p>valve: open (";
    dtostrf((millis() - valveOpenAt) / 60000.0, 4, 0, tempStr);
    s += tempStr;
    s += " min)";
  }
  else
  {
    s += "<p>valve: closed (opened for ";
    dtostrf((valveCloseAt - valveOpenAt) / 60000.0, 4, 0, tempStr);
    s += tempStr;
    s += " min)";
  }
  s += "<p><h3>" + String(nils_length(valveHist)) + " Last valve actions</h3>";
  for (int i = 0; i < nils_length(valveHist); i++)
  { // display last pumpOn Events in right order
    byte arrIndex = mod((((int)valveHistCnt) - i), nils_length(valveHist));
    sprintf(tempStr, "%02d", i + 1);
    s += String(tempStr) + ": " + valveHist[arrIndex] + "<br>";
  }
  s += "<p><h3>" + String(nils_length(pump)) + " Last pump actions</h3>";
  for (int i = 0; i < nils_length(pump); i++)
  { // display last pumpOn Events in right order
    byte arrIndex = mod((((int)pumpCnt) - i), nils_length(pump));
    sprintf(tempStr, "%02d", i + 1);
    s += String(tempStr) + ": " + pump[arrIndex] + "<br>";
  }
  uptime::calculateUptime();
  sprintf(tempStr, "%04u Tage %02u:%02u:%02u", uptime::getDays(), uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  s += "<p>uptime: " + String(tempStr);
  s += "<p>last reset reason: " + verbose_print_reset_reason(esp_reset_reason());
  // s += "<p>";
  // switch (esp_core_dump_image_check())
  // {
  // case ESP_OK:
  //   s += "<a href=/coredump>core dump found</a> - <a href=/deletecoredump>delete core dump</a>";
  //   break;
  // case ESP_ERR_NOT_FOUND:
  //   s += "no core dump found";
  //   break;
  // case ESP_ERR_INVALID_SIZE:
  //   s += "core dump with invalid size - <a href=/deletecoredump>delete core dump</a>";
  //   break;
  // case ESP_ERR_INVALID_CRC:
  //   s += "core dump with invalid CRC - <a href=/deletecoredump>delete core dump</a>";
  // }
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  s += "<p>Firmware: running  " + String(running->label) + " - OTA updates " + String(next->label) + "</p>";

  s += "</fieldset>";

  s += "<p>Go to <a href='config'>Configuration</a>";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
}

void configSaved()
{
  // check if Wifi configration has changed - if yes, restart
  changeNvsMode(false);
  if (preferences.isKey("apPassword"))
  {
    if (strcmp(iotWebConf.getApPasswordParameter()->valueBuffer, preferences.getString("apPassword").c_str()) != 0)
      needReset = true;
  }
  else
    needReset = true;
  if (preferences.isKey("wifiSsid"))
  {
    if (strcmp(iotWebConf.getWifiSsidParameter()->valueBuffer, preferences.getString("wifiSsid").c_str()) != 0)
      needReset = true;
  }
  else
    needReset = true;

  if (preferences.isKey("wifiPassword"))
  {
    if (strcmp(iotWebConf.getWifiPasswordParameter()->valueBuffer, preferences.getString("wifiPassword").c_str()) != 0)
      needReset = true;
  }
  else
    needReset = true;

  // TODO: Funktioniert mit apPasswort noch nicht immer....
  ESP_LOGI(TAG, "AP Password length > 0: %d", iotWebConf.getApPasswordParameter()->getLength() > 0);
  if (iotWebConf.getApPasswordParameter()->getLength() > 0)
    preferences.putString("apPassword", String(iotWebConf.getApPasswordParameter()->valueBuffer));
  preferences.putString("wifiSsid", String(iotWebConf.getWifiAuthInfo().ssid));
  preferences.putString("wifiPassword", String(iotWebConf.getWifiAuthInfo().password));
  ESP_LOGI(TAG, "Configuration saved.");
  // TODO: Neustart bei normalen Parametern vermeiden
  needReset = true;
}

bool formValidator(iotwebconf::WebRequestWrapper *webRequestWrapper)
{
  ESP_LOGI(TAG, "Validating form.");
  bool valid = true;

  // int l = webRequestWrapper->arg(mqttServerParam.getId()).length();
  // if (l < 3)
  // {
  //   mqttServerParam.errorMessage = "Please enter at least 3 chars!";
  //   valid = false;
  // }

  return valid;
}

void setTimezone(String timezone)
{
  ESP_LOGI(TAG, "Setting Timezone to %s", ntpTimezone);
  setenv("TZ", ntpTimezone, 1); //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}
/* #endregion */

/* #region connection handling */
void connectToMqtt()
{
  if (strlen(mqttServer) > 0)
  {
    ESP_LOGI(TAG, "Connecting to MQTT...");
    mqttClient.connect();
  }
}

void onWifiConnected()
{
  ESP_LOGI(TAG, "Connected to Wi-Fi. Local IP: %s", WiFi.localIP().toString().c_str());
  connectToMqtt();
  timeClient.begin();
  ArduinoOTA.begin();
  ESP_LOGI(TAG, "TCP services started.");

  // Setup syslog if configured
  if (strlen(syslogServer) > 0 && syslogPort > 0) {
    syslogUDP.begin(0); // Use random local port
    ESP_LOGI(TAG, "Syslog configured to %s:%d", syslogServer, syslogPort);
  }
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
  ESP_LOGI(TAG, "Disconnected from Wi-Fi.");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  timeClient.end();
  // ArduinoOTA.end();  TODO: Fehler untersuchen, dumped.
  ESP_LOGI(TAG, "TCP services stopped.");
}
/* #endregion */

/* #region MQTT */
void onMqttConnect(bool sessionPresent)
{
  ESP_LOGI(TAG, "Connected to MQTT.");
  ESP_LOGI(TAG, "Session present: %s", sessionPresent ? "true" : "false");
  mqttPublish(MQTT_PUB_STATUS, "Online", true, false);
  mqttPublish(MQTT_PUB_WIFI, getWifiJson().c_str(), true, true);
  uint16_t packetIdSub;
  if (strlen(mqttHeaterStatusTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttHeaterStatusTopic, 2);
    ESP_LOGI(TAG, "Subscribed to topic: %s - %u", mqttHeaterStatusTopic, packetIdSub);
  }
  if (strlen(mqttPumpTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttPumpTopic, 2);
    ESP_LOGI(TAG, "Subscribed to topic: %s - %u", mqttPumpTopic, packetIdSub);
  }
  if (strlen(mqttThermalDesinfectionTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttThermalDesinfectionTopic, 2);
    ESP_LOGI(TAG, "Subscribed to topic: %s - %u", mqttThermalDesinfectionTopic, packetIdSub);
  }
  if (strlen(mqttValvePressureTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttValvePressureTopic, 2);
    ESP_LOGI(TAG, "Subscribed to topic: %s - %u", mqttValvePressureTopic, packetIdSub);
  }
  digitalWrite(LED_BUILTIN, HIGH);
  mqttSendTopics(true);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  switch (reason)
  {
  case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:
    mqttDisconnectReason = "TCP_DISCONNECTED";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
    mqttDisconnectReason = "MQTT_UNACCEPTABLE_PROTOCOL_VERSION";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:
    mqttDisconnectReason = "MQTT_IDENTIFIER_REJECTED";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:
    mqttDisconnectReason = "MQTT_SERVER_UNAVAILABLE";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:
    mqttDisconnectReason = "MQTT_MALFORMED_CREDENTIALS";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:
    mqttDisconnectReason = "MQTT_NOT_AUTHORIZED";
    break;
  }
  strftime(mqttDisconnectTime, 20, "%d.%m.%Y %T", &localTime);
  mqttDisconnectTimestamp = timeClient.getEpochTime();

  ESP_LOGI(TAG, " [%8u] Disconnected from the broker reason = %s", millis(), mqttDisconnectReason.c_str());
  digitalWrite(LED_BUILTIN, LOW);

  if (WiFi.isConnected())
  {
    ESP_LOGI(TAG, " [%8u] Reconnecting to MQTT..", millis());
    mqttReconnectTimer.once(5, connectToMqtt);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos)
{
  ESP_LOGI(TAG, " [%8u] Subscribe acknowledged id: %u, qos: %u", millis(), packetId, qos);
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
  char new_payload[len + 1];
  strncpy(new_payload, payload, len);
  new_payload[len] = '\0';
  if (strcmp(topic, mqttPumpTopic) == 0)
  {
    ESP_LOGI(TAG, "MQTT pump: %s", new_payload);
    mqttPump = (strcmp(mqttPumpValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttThermalDesinfectionTopic) == 0)
  {
    ESP_LOGI(TAG, "MQTT thermal desinfection: %s", new_payload);
    mqttThermalDesinfection = (strcmp(mqttThermalDesinfectionValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttHeaterStatusTopic) == 0)
  {
    ESP_LOGI(TAG, "MQTT heater status: %s", new_payload);
    mqttHeaterStatus = (strcmp(mqttHeaterStatusValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttValvePressureTopic) == 0)
  {
    ESP_LOGI(TAG, "MQTT pressure: %s", new_payload);
    valvePressure = atof(new_payload);
  }
}

bool getMqttActive()
{
  return String(mqttServer).length() > 0;
}

void mqttPublish(const char *topic, const char *payload, bool force, bool jsonAddTimstamp)
{
  static std::map<String, String> mqttLastMessage;
  if (getMqttActive())
  {
    String topicStr = String(topic);
    String payloadStr = String(payload);
    String newPayloadStr = String(payload);
    String tempTopicStr = String(mqttTopicPath) + String(topic);

    if (mqttClient.connected())
    {
      if (mqttLastMessage[topicStr] != payloadStr || force)
      {
        if (jsonAddTimstamp && timeClient.isTimeSet())
        {
          JsonDocument object;
          char timeStr[20];
          strftime(timeStr, 20, "%d.%m.%Y %T", &localTime);
          deserializeJson(object, payload);
          object["timestamp"] = timeClient.getEpochTime();
          object["date"] = timeStr;
          serializeJson(object, newPayloadStr);
        }
        ESP_LOGD(TAG, "MQTT send: %s = %s", tempTopicStr.c_str(), newPayloadStr.c_str());
        if (mqttClient.publish(tempTopicStr.c_str(), 0, true, newPayloadStr.c_str()) > 0)
          // TODO: Statt dem String ggf. einen Hash wegspeichern zur Optimierung der Speichernutzung
          mqttLastMessage[topicStr] = payloadStr;
        else
          ESP_LOGE(TAG, "MQTT (error) not send: %s = %s", tempTopicStr.c_str(), newPayloadStr.c_str());
      }
    }
    else
    {
      ESP_LOGW(TAG, "MQTT not send: %s = %s", tempTopicStr.c_str(), payloadStr.c_str());
    }
  }
}

void mqttPublishUptime()
{
  char msg_out[20];
  uptime::calculateUptime();
  sprintf(msg_out, "%04u %s %02u:%02u:%02u", uptime::getDays(), txtDays[langu], uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  // Serial.println(msg_out);
  mqttPublish(MQTT_PUB_INFO, msg_out, false, false);
}

void mqttSendTopics(bool mqttInit)
{
  char msg_out[20];
  dtostrf(sensorData.tempOut, 2, 2, msg_out);
  mqttPublish(MQTT_PUB_TEMP_OUT, msg_out, mqttInit, false);
  dtostrf(sensorData.tempRet, 2, 2, msg_out);
  mqttPublish(MQTT_PUB_TEMP_RET, msg_out, mqttInit, false);
  dtostrf(sensorData.tempInt, 2, 2, msg_out);
  mqttPublish(MQTT_PUB_TEMP_INT, msg_out, mqttInit, false);
  dtostrf(tempDiff, 2, 4, msg_out);
  mqttPublish(MQTT_PUB_TEMP_DIFF, msg_out, mqttInit, false);
  if (valveState)
    mqttPublish(MQTT_PUB_VALVE_OPENED, "1", mqttInit, false);
  else
    mqttPublish(MQTT_PUB_VALVE_OPENED, "0", mqttInit, false);
  if (pumpRunning)
    mqttPublish(MQTT_PUB_PUMP, "1", mqttInit, false);
  else
    mqttPublish(MQTT_PUB_PUMP, "0", mqttInit, false);
  mqttPublish(MQTT_PUB_SYSINFO, getSysinfoJson().c_str(), mqttInit, true);
}

String getStatus()
{
  String status;
  if (sensorError)
    status = "emergency";
  else if (mqttThermalDesinfection)
    status = "desinfection";
  else if (manualMode)
    status = "manual";
  else if (mqttHeaterStatus)
    status = "heater on";
  else
    status = "heater off";
  return status;
}

String getWifiJson()
{
  JsonDocument object;
  String jsonString;

  object["ssid"] = WiFi.SSID();
  object["sta_ip"] = WiFi.localIP().toString();
  object["rssi"] = WiFi.RSSI();
  object["mac"] = WiFi.macAddress();
  serializeJson(object, jsonString);
  return jsonString;
}

String getSysinfoJson()
{
  JsonDocument object;
  String jsonString;
  // TODO: Struktur mit Ordnern versehen und optimieren
  if (manualMode)
    object["dhw"]["mode"] = "manual";
  else
  {
    object["dhw"]["mode"] = "auto";
    if (sensorError)
      object["dhw"]["state"] = "emergency";
    else if (mqttThermalDesinfection)
      object["dhw"]["state"] = "desinfection";
    else if (mqttHeaterStatus)
      object["dhw"]["state"] = "heater on";
    else
      object["dhw"]["state"] = "heater off";
  }
  object["dhw"]["sensor_out_connected"] = sensorData.outConnected;
  object["dhw"]["sensor_ret_connected"] = sensorData.retConnected;
  object["dhw"]["sensor_int_connected"] = sensorData.intConnected;

  object["valve"]["valve_error"] = valveError;
  object["valve"]["valve_initial_fill"] = valveInitFill;

  object["sys"]["reset_reason"] = esp_reset_reason();
  object["sys"]["reset_reason_msg"] = verbose_print_reset_reason(esp_reset_reason());
  object["sys"]["reset_crash_counter"] = crashCounter;
  // object["sys"]["core_dump"] = esp_core_dump_image_check();
  // object["system"]["heap_free"] = esp_get_free_internal_heap_size();    // in bytes
  object["sys"]["heap_min_free"] = esp_get_minimum_free_heap_size(); // in bytes
  object["ntp"]["time_set"] = timeClient.isTimeSet();
  object["mqtt"]["disconnect_reason"] = mqttDisconnectReason;
  object["mqtt"]["disconnect_time"] = mqttDisconnectTime;
  object["mqtt"]["disconnect_timestamp"] = mqttDisconnectTimestamp;

  serializeJson(object, jsonString);
  return jsonString;
}
/* #endregion */

/* #region Sensors */
void printAddress(DeviceAddress deviceAddress)
{
  char addr[24] = {0};
  char *ptr = addr;
  for (uint8_t i = 0; i < 8; i++)
  {
    ptr += sprintf(ptr, "%02X", deviceAddress[i]);
  }
  ESP_LOGI(TAG, "%s", addr);
}

// function to format a device address
String formatAdress(DeviceAddress deviceAddress)
{
  String adr;
  for (uint8_t i = 0; i < 8; i++)
  {
    // zero pad the address if necessary
    if (deviceAddress[i] < 16)
      adr = adr + "0";
    adr = adr + String(deviceAddress[i], HEX);
  }
  return adr;
}

/// in: valid chars are 0-9 + A-F + a-f
/// out_len_max==0: convert until the end of input string, out_len_max>0 only convert this many numbers
/// returns actual out size
int hexStr2Arr(uint8_t *out, const char *in, size_t out_len_max = 0)
{
  if (!in || !out)
    return -1;

  const size_t in_len = strlen(in);

  // Länge muss gerade sein
  if (in_len % 2 != 0)
    return -1;

  // maximale Ausgabegröße bestimmen
  const size_t max_out = in_len / 2;
  const size_t out_len = (out_len_max == 0 || out_len_max > max_out)
                             ? max_out
                             : out_len_max;

  auto hexVal = [](char c) -> int
  {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1; // invalid
  };

  for (size_t i = 0; i < out_len; i++)
  {
    int hi = hexVal(in[2 * i]);
    int lo = hexVal(in[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return -1; // invalid char

    out[i] = (hi << 4) | lo;
  }

  return (int)out_len;
}

void detectSensors()
{
  int i;
  // locate devices on the bus
  ESP_LOGI(TAG, "Searching devices...");
  if (xSemaphoreTake(tempSemaphore, 15000))
  { // Warte auf Zugriff auf Sensoren
    ESP_LOGI(TAG, "Found %u devices.", sensors.getDeviceCount());

    // fill connected devices for configuration
    for (i = 0; i < sensors.getDeviceCount(); i++)
    {
      DeviceAddress sensor_id;
      char str[5];
      sensors.getAddress(sensor_id, i);
      printAddress(sensor_id);
      snprintf(chooserNames[i], sizeof(chooserNames[i]), "%s - %.2f C°", formatAdress(sensor_id).c_str(), sensors.getTempC(sensor_id));
      snprintf(chooserValues[i], sizeof(chooserValues[i]), "%s", formatAdress(sensor_id).c_str());
    }
    xSemaphoreGive(tempSemaphore); // Gib Zugriff auf Sensoren frei
  }
  else
  {
    ESP_LOGW(TAG, "Could not take tempSemaphore to detect sensors!");
  }  
  hexStr2Arr(sensorInt_id, tempIntParam.value(), 8);
  hexStr2Arr(sensorOut_id, tempOutParam.value(), 8);
  hexStr2Arr(sensorRet_id, tempRetParam.value(), 8);
}

void checkSensors()
{
  String info;

  if (sensorData.intConnected && sensorData.retConnected && sensorData.outConnected)
  {
    if (sensorError)
    {
      mqttPublish(MQTT_PUB_INFO, "sensorerror solved", true, false);
      info = "sensorerror - internal: " + String(sensorData.intConnected) + " return: " + String(sensorData.retConnected) + " out: " + String(sensorData.outConnected);
      mqttPublish(MQTT_PUB_INFO, info.c_str(), true, false);
      sensorError = false;
      sensors.setResolution(sensorOut_id, 12); // hohe Genauigkeit
      sensors.setResolution(sensorRet_id, 12); // hohe Genauigkeit
    }
  }
  else
  {
    info = "sensorerror - internal: " + String(sensorData.intConnected) + " return: " + String(sensorData.retConnected) + " out: " + String(sensorData.outConnected);
    mqttPublish(MQTT_PUB_INFO, info.c_str(), true, false);
    sensorError = true;
  }
}

void tempTask(void *parameter)
{
  // Parameter ist ein Zeiger auf heap-allokiertes SensorIds_t
  SensorIds_t *pIds = (SensorIds_t *)parameter;

  // Kopiere die IDs lokal (sicherer) und gib den übergebenen Speicher frei
  SensorIds_t ids;
  memcpy(ids.out, pIds->out, sizeof(DeviceAddress_t));
  memcpy(ids.ret, pIds->ret, sizeof(DeviceAddress_t));
  memcpy(ids.intl, pIds->intl, sizeof(DeviceAddress_t));
  free(pIds);

  TempReport_t report;

  for (;;)
  {
    ESP_LOGD(TAG, "Requesting temperatures...");
    if (xSemaphoreTake(tempSemaphore, 1000))
      {
        sensors.requestTemperatures();
      vTaskDelay(pdMS_TO_TICKS(750)); // Warte 1s nach requestTemperatures

      report.tempOut = sensors.getTempC(ids.out);
      report.tempRet = sensors.getTempC(ids.ret);
      report.tempInt = sensors.getTempC(ids.intl);
      xSemaphoreGive(tempSemaphore); // Gib Zugriff auf Sensoren frei
      ESP_LOGD(TAG, "Temperatures read: out=%.2f C°, ret=%.2f C°, int=%.2f C°", report.tempOut, report.tempRet, report.tempInt);
      report.outConnected = !(report.tempOut == DEVICE_DISCONNECTED_C);
      report.retConnected = !(report.tempRet == DEVICE_DISCONNECTED_C);
      report.intConnected = !(report.tempInt == DEVICE_DISCONNECTED_C);

      report.timestamp = xTaskGetTickCount();

      // Sende in Queue, blockiere bis zu 100 ms wenn voll
      if (tempQueue != NULL)
      {
        xQueueOverwrite(tempQueue, &report); // overwrite statt send, damit immer der aktuellste Wert in der Queue ist
      }
    }
    else
    {
      ESP_LOGW(TAG, "Could not take tempSemaphore in tempTask!");
    }
  }
}

void tempRead()
{
  xQueueReceive(tempQueue, &sensorData, 0);
}
/* #endregion */

/* #region Display */
void getLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    ESP_LOGW(TAG, "Failed to obtain time");
    return;
  }
  localTime = timeinfo;
}

void updateTime()
{
  if (iotWebConf.getState() == 4)
  {
    timeClient.update();
    getLocalTime();
  }
}

void updateDisplay()
{
  char tempStr[128];
  char uptimeStr[8];
  float temp;
  unsigned long now = millis();
  unsigned int lineStart = 0;
  unsigned lineEnd = 0;
  unsigned int lineCnt = 1;

  display.clear();
  if (manualMode)
    display.invertDisplay();
  else
    display.normalDisplay();

  switch (displayPage)
  {
  case 0:
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    switch (iotWebConf.getState())
    {
    case 0:
      display.drawString(0, 0, txtBoot[langu]);
      break;
    case 1:
      display.drawString(0, 0, txtSetup[langu]);
      break;
    case 2:
      display.drawString(0, 0, "AP");
      break;
    case 3:
      display.drawString(0, 0, txtConnecting[langu]);
      break;
    case 4:
      display.drawString(0, 0, "Online (" + String(WiFi.RSSI()) + ")");
      break;
    case 5:
      display.drawString(0, 0, "Offline");
      break;
    }

    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    strftime(tempStr, 6, "%H:%M", &localTime);
    display.drawString(128, 0, tempStr);
    display.drawLine(0, 11, 128, 11);
    display.setTextAlignment(TEXT_ALIGN_CENTER);

    if (sensorData.outConnected)
    {
      dtostrf(sensorData.tempOut, 2, 2, tempStr);
      display.drawString(64, 12, String(txtFlow[langu]) + ": " + String(tempStr) + " C°");
    }
    else
      display.drawString(64, 12, String(txtFlow[langu]) + ": ERROR!");
    if (sensorData.retConnected)
    {
      dtostrf(sensorData.tempRet, 2, 2, tempStr);
      display.drawString(64, 24, String(txtReturn[langu]) + ": " + String(tempStr) + " C°");
    }
    else
      display.drawString(64, 24, String(txtReturn[langu]) + ": ERROR!");

    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (pumpRunning)
        display.drawString(64, 36, String(txtPumpOn[langu]));
    else
        display.drawString(64, 36, String(txtPumpOff[langu]));
    if (valveState)
      display.drawString(64, 48, String(txtValveOn[langu]) + ": " + String(valvePressureAvg) + " Bar");
    else
      display.drawString(64, 48, String(txtValveOff[langu]) + ": " + String(valvePressureAvg) + " Bar");
    break;
  case 1:
    // Display Page 2 - last 5 pump starts
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, String(txtLastRuns[langu]));
    display.drawLine(0, 11, 128, 11);
    lineStart = ((displayPageLastRuns)-1) * 5;
    lineEnd = lineStart + 4;
    lineCnt = 1;
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(127, 0, "(" + String(displayPageLastRuns) + "/" + String(nils_length(pump) / 5) + ")");
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    for (int i = lineStart; i <= lineEnd; i++)
    { // display last 5 pumpOn Events in right order
      byte arrIndex = mod((((int)pumpCnt) - i), nils_length(pump));
      display.drawString(0, lineCnt * 10 + 2, String(i + 1) + ": " + pump[arrIndex].substring(1, pump[arrIndex].length() - 4));
      lineCnt++;
    }
    if ((10000 < now - displayPageSubChange))
    {
      if (displayPageLastRuns < nils_length(pump) / 5)
        displayPageLastRuns++;
      else
        displayPageLastRuns = 1;
      displayPageSubChange = now;
    }
    break;
  case 2:
    uptime::calculateUptime();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 0, txtRuntime[langu]);
    display.drawLine(0, 11, 128, 11);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 18, String(uptime::getDays()) + " " + String(txtDays[langu]));
    sprintf(uptimeStr, "%02u:%02u:%02u", uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
    display.drawString(64, 38, String(uptimeStr));
    break;
  case 3:
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, "WiFi Status");
    display.drawLine(0, 11, 128, 11);
    switch (iotWebConf.getState())
    {
    case 0:
      display.drawString(64, 12, txtBoot[langu]);
      break;
    case 1:
      display.drawString(64, 12, txtNotSetup[langu]);
      break;
    case 2:
      display.drawString(64, 12, "AP");
      break;
    case 3:
      display.drawString(64, 12, String(txtConnecting[langu]) + "...");
      break;
    case 4:
      display.drawString(64, 12, "Online");
      break;
    case 5:
      display.drawString(64, 12, "Offline");
      break;
    }
    display.drawString(64, 22, "SSID: " + WiFi.SSID());
    display.drawString(64, 32, "RSSI: " + String(WiFi.RSSI()));
    display.drawString(64, 42, String(txtTxPower[langu]) + ": " + String(WiFi.getTxPower()));
    if (WiFi.isConnected())
    {
      display.drawString(64, 52, WiFi.localIP().toString());
    }
    else
    {
      display.drawString(64, 52, txtNoIp[langu]);
    }
    break;
  case 4:
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, txtWifiNetwork[langu]);
    display.drawLine(0, 11, 128, 11);
    if (iotWebConf.getState() == 4)
    {
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 18, txtWifiConnected1[langu]);
      display.drawString(64, 38, txtWifiConnected2[langu]);
    }
    else
    {
      // WiFi.scanNetworks will return the number of networks found
      if (WiFi.getMode() != WIFI_STA)
      {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
      }
      if ((60000 < now - lastScan) && WiFi.getMode() == WIFI_STA) // blocked for 60s
      {
        ESP_LOGI(TAG, "scan started");
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        networksPage = 1;
        networksPageFirstCall = true;
      }
      networksFound = WiFi.scanComplete();

      ESP_LOGI(TAG, "Scan status: %d", networksFound);
      if (networksFound == -1)
      {
        display.setFont(ArialMT_Plain_24);
        display.drawString(64, 24, txtSearching[langu]);
        lastScan = now;
      }
      else if (networksFound == -2)
      {
        display.setFont(ArialMT_Plain_24);
        display.drawString(64, 24, txtWaiting[langu]);
      }
      else if (networksFound == 0)
      {
        ESP_LOGI(TAG, "Networks found: %d", networksFound);
        display.setFont(ArialMT_Plain_16);
        display.drawString(64, 24, txtNoAp[langu]);
      }
      else
      {
        ESP_LOGI(TAG, "Networks found: %d", networksFound);
        if (networksPageFirstCall)
        {
          networksPageFirstCall = false;
          displayPageSubChange = now;
          networksPageTotal = (int)ceil(networksFound / 5.0);
          lastScan = now;
        }
        lineStart = ((networksPage)-1) * 5;
        lineEnd = min((lineStart + 4), (unsigned int)networksFound);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(127, 0, "(" + String(networksPage) + "/" + String(networksPageTotal) + ")");

        for (int i = lineStart; i < lineEnd; i++)
        {
          // Print SSID and RSSI for each network found
          ESP_LOGI(TAG, "%d: %s (%d)%s", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : "*");
          display.setFont(ArialMT_Plain_10);
          display.setTextAlignment(TEXT_ALIGN_LEFT);
          display.drawString(0, 1 + (10 * lineCnt), String(i + 1) + ": " + String(WiFi.SSID(i)) + " (" + String(WiFi.RSSI(i)) + ")");
          lineCnt++;
        }
        if ((10000 < now - displayPageSubChange) && networksPageTotal > 1)
        {
          if (networksPage < networksPageTotal)
            networksPage++;
          else
            networksPage = 1;
          displayPageSubChange = now;
        }
      }
    }
  }
  display.display();
}
/* #endregion */

/* #region control logic */
void pumpOn()
{
  char tempStr[128];
  ESP_LOGI(TAG, "Turn on circulation");
  pumpRunning = true;
  pumpStartedAt = millis();
  digitalWrite(PUMPPIN, LOW);
  mqttSendTopics();
  strftime(tempStr, 40, "%d.%m.%Y %T", &localTime);
  ESP_LOGI(TAG, "Pump on: %s", tempStr);
  if (pumpCntInit)
    pumpCntInit = false;
  else if (++pumpCnt > nils_length(pump) - 1)
    pumpCnt = 0; // Reset counter
  pump[pumpCnt] = tempStr;
}

void pumpOff()
{
  ESP_LOGI(TAG, "Turn off circulation");
  pumpRunning = false;
  pump[pumpCnt] += " (" + String((int)round((millis() - pumpStartedAt) / 1000 / 60)) + " min.)";
  digitalWrite(PUMPPIN, HIGH);
  mqttSendTopics();
}

void checkPump()
{
  if (sensorError)
  {
    // Emergency Mode if missing sensors
    if ((localTime.tm_hour >= 6 && localTime.tm_hour < 23) && (localTime.tm_min >= 00 && localTime.tm_min < 10))
    {
      if (!pumpRunning)
        pumpOn();
    }
    else
    {
      if (pumpRunning)
        pumpOff();
    }
  }
  else
  {
    if (mqttThermalDesinfection)
    {
      if (!pumpRunning)
        pumpOn();
    }
    else if (!manualMode)
    {
      if (++checkCnt >= 5)
        checkCnt = 0; // Reset counter
      t[checkCnt] = sensorData.tempOut;
      int cnt_alt = (checkCnt + 6) % 5;
      tempDiff = t[checkCnt] - t[cnt_alt]; // Difference to 5 sec before
      if (!pumpRunning)
      {
        if ((((tempDiff >= tempDiffTrigger && (mqttHeaterStatus || !mqttClient.connected())) || mqttPump) && (300000 < millis() - pumpBlock || pumpFirstCall)) || 86400000 < millis() - pumpStartedAt)
        { // smallest temp change is 0,0625°C,
          ESP_LOGI(TAG, "Temperature Delta: %.2f", tempDiff);
          if (mqttPump)
          {
            mqttPump = false;
            ESP_LOGI(TAG, "MQTT pump action done");
          }
          pumpBlock = millis();
          pumpFirstCall = false;
          pumpOn();
        }
      }
      else if (sensorData.tempRet > (sensorData.tempOut - tempRetDiffParam.value()) && !(tempDiff >= tempDiffTrigger) && 120000 < (millis() - pumpStartedAt))
      { // if return flow temp near temp out stop pump with a delay of 2 minutes and other rules
        pumpOff();
      }
    }
  }
}

void valveOpen()
{
  char tempStr[128];

  ESP_LOGI(TAG, "Open valve - %u", valveState);
  if (!valveState)
  {
    valveState = true;
    valveOpenAt = millis();
    saveValveTimestampNvs("valveOpenAt", valveOpenAt);    
    valveOpenAtTs = timeClient.getEpochTime();
    saveValveTimestampNvs("valveOpenAtTs", valveOpenAtTs);
    if (valveCloseAtTs > 0)
      mqttPublish(MQTT_PUB_VALVE_SEC_TO_REFILL, String(valveOpenAtTs - valveCloseAtTs).c_str(), true, false);
    valveCloseAt = 0;
    digitalWrite(VALVEPIN, LOW);

    strftime(tempStr, 40, "%d.%m.%Y %T", &localTime);
    ESP_LOGI(TAG, "%s", tempStr);
    if (valveHistCntInit)
      valveHistCntInit = false;
    else if (++valveHistCnt > nils_length(valveHist) - 1)
      valveHistCnt = 0; // Reset counter
    valveHist[valveHistCnt] = tempStr;
  }
}

void valveClose()
{
  ESP_LOGI(TAG, "Close valve - %u", valveState);
  if (valveState)
  {
    valveState = false;
    valveCloseAt = millis();
    saveValveTimestampNvs("valveCloseAt", valveCloseAt);
    valveCloseAtTs = timeClient.getEpochTime();
    mqttPublish(MQTT_PUB_VALVE_SEC_OPENED, String((valveCloseAt - valveOpenAt) / 1000).c_str(), true, false);
    saveValveTimestampNvs("valveCloseAtTs", valveCloseAtTs);
    digitalWrite(VALVEPIN, HIGH);
    valveHist[valveHistCnt] += " (" + String((valveCloseAt - valveOpenAt) / 1000 / 60) + " min.)";
  }
}

void checkValve()
{
  if (!manualMode)
  {
    if (!valveError && valveMaxOpen && mqttClient.connected() && valvePressureAvg > 0.0f)
    {
      if (valveState && (((millis() - valveOpenAt) / 60000.0 > valveMaxOpen) || (valvePressureAvg <= valvePressureLowParam.value() - 0.2f) && valveInitFill))
      // error if valve is opened too long or if the pressure is 0.2 below low pressure setting for longer than quarter of the valveMaxOpen time.
      {
        valveError = true;
        valveClose();
        return;
      }
      if (!valveState && roundTo(valvePressureAvg, 2) <= roundTo(valvePressureLowParam.value(), 2))
      {
        valveOpen();
        return;
      }
      if (valveState && roundTo(valvePressureAvg, 2) >= roundTo(valvePressureHighParam.value(), 2))
      {
        valveClose();
        valveInitFill = false;
        return;
      }
    }
    else if (valveState)
      valveClose();
  }
}
/* #endregion */

/* #region timer */
bool onSec1Timer(void *)
{
  updateTime();
  tempRead();
  checkPump();
  checkValve();
  mqttSendTopics();
  updateDisplay();

  return true;
}

bool onSec10Timer(void *)
{
  checkSensors();

  return true;
}

bool onMin1Timer(void *)
{
  mqttPublishUptime();
  mqttPublish(MQTT_PUB_WIFI, getWifiJson().c_str(), false, true);
  ESP_LOGI(TAG, "Add pressure to calc: %.2f", valvePressure);
  valvePressureAvg.addValue(valvePressure);
  mqttPublish(MQTT_PUB_VALVE_PRESSURE_AVG, String(valvePressureAvg.getAverage()).c_str(), false, false);

  return true;
}

void handleResetValveError()
{
  valveOpenAt = 0;
  valveOpenAtTs = 0;
  saveValveTimestampNvs("valveOpenAt", valveOpenAt);
  saveValveTimestampNvs("valveOpenAtTs", valveOpenAtTs);
  valveCloseAt = 0;
  valveCloseAtTs = 0;
  saveValveTimestampNvs("valveCloseAt", valveCloseAt);
  saveValveTimestampNvs("valveCloseAtTs", valveCloseAtTs);
  valveError = false;
  valveInitFill = true;

  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Warmwater Recirculation Pump</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  s += "Valve status reset successful.";
  s += "<p><button type=\"button\" onclick=\"javascript:history.back()\">Back</button>";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
}
/* #endregion */

/* #region button */
void handleUserBtnClick(void *oneButton)
{
  ESP_LOGD(TAG, "Button pressed ms: %u", ((OneButton *)oneButton)->getPressedMs());
  if (manualMode)
  {
    if (pumpRunning)
      pumpOff();
    else
      pumpOn();
  }
  else
  {
    if (!displayOn)
    { // display was off, do not switch page
      display.displayOn();
      displayTimer.attach_ms(500, updateDisplay);
      displayOn = true;
    }
    else
    {
      displayPageSubChange = now; // init the subpage timer
      if (displayPage == 4)
        displayPage = 0;
      else
        displayPage++;

      if (displayPage == 4)
      {
        if (iotWebConf.getState() != 4)
          iotWebConf.goOffLine();
      }
      else
      {
        if (iotWebConf.getState() == 5)
          iotWebConf.goOnLine();
      }
    }
  }
}

void handleUserBtnLongPress(void *oneButton)
{
  ESP_LOGD(TAG, "Long press detected: %u", ((OneButton *)oneButton)->getPressedMs());
  manualMode = !manualMode;
}

void handleUserBtnDoublePress(void *oneButton)
{
  ESP_LOGD(TAG, "Double press detected: %u", ((OneButton *)oneButton)->getPressedMs());
  static byte doublePressCnt = 0;
  doublePressCnt++;
  if (doublePressCnt == 1)
    pumpOn();
  else if (doublePressCnt == 2)
    pumpOff();
    else if (doublePressCnt == 3)
    valveOpen();
  else if (doublePressCnt == 4)
    valveClose();
  else
    doublePressCnt = 0;
}

void handleResetBtnLongPress(void *oneButton)
{
  ESP_LOGD(TAG, "Reset button long press detected: %u", ((OneButton *)oneButton)->getPressedMs());
  iotWebConf.getRootParameterGroup()->applyDefaultValue();
  iotWebConf.saveConfig();
  needReset = true;
  ESP_LOGI(TAG, "Factory reset performed, restarting...");
}
/* #endregion */

/* #region logging */
void sendSyslogMessage(int level, const char *message) {
  if (strlen(syslogServer) == 0 || syslogPort == 0) return;
  
  // Syslog priority: facility * 8 + level
  // Facility 1 = user-level messages
  int priority = 1 * 8 + level;
  
  // Simple syslog format: <priority>timestamp hostname tag: message
  char syslogMsg[1024];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(syslogMsg, sizeof(syslogMsg), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  } else {
    strcpy(syslogMsg, "unknown-time");
  }
  
  char fullMsg[1100];
  snprintf(fullMsg, sizeof(fullMsg), "<%d>%s %s %s: %s", priority, syslogMsg, hostname, TAG, message);
  
  syslogUDP.beginPacket(syslogServer, syslogPort);
  syslogUDP.write((uint8_t*)fullMsg, strlen(fullMsg));
  syslogUDP.endPacket();
}

void applySyslogLogLevel()
{
  const char *level = syslogLogLevelParam.value();
  if (strcmp(level, "NONE") == 0)
    esp_log_level_set(TAG, ESP_LOG_NONE);
  else if (strcmp(level, "ERROR") == 0)
    esp_log_level_set(TAG, ESP_LOG_ERROR);
  else if (strcmp(level, "WARN") == 0)
    esp_log_level_set(TAG, ESP_LOG_WARN);
  else if (strcmp(level, "INFO") == 0)
    esp_log_level_set(TAG, ESP_LOG_INFO);
  else if (strcmp(level, "DEBUG") == 0)
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
  else if (strcmp(level, "VERBOSE") == 0)
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
  else
    esp_log_level_set(TAG, ESP_LOG_INFO);

  ESP_LOGI(TAG, "ESP_LOG level set to %s", level);
}

int custom_vprintf(const char *fmt, va_list args) {
  char buf[1024];
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  if (len > 0)
  {
    // Print to serial
    uart_write_bytes(UART_NUM_0, buf, len);
    // Send to syslog if configured
    if (strlen(syslogServer) > 0) {
      int level = LOG_INFO;
      if (strstr(buf, "[E]")) level = LOG_ERR;
      else if (strstr(buf, "[W]")) level = LOG_WARNING;
      else if (strstr(buf, "[I]")) level = LOG_INFO;
      else if (strstr(buf, "[D]")) level = LOG_DEBUG;
      else if (strstr(buf, "[V]")) level = LOG_DEBUG;
      sendSyslogMessage(level, buf);
    }
  }
  return len;
}
/* #endregion */

void setup()
{
  // basic setup
  Serial.begin(115200);
  esp_log_set_vprintf(custom_vprintf);
  // initCoreDumpFlash();
  // esp_core_dump_init();
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PUMPPIN, OUTPUT);
  pinMode(VALVEPIN, OUTPUT);
  digitalWrite(PUMPPIN, HIGH);
  digitalWrite(VALVEPIN, HIGH);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(DISPLAYPIN, INPUT_PULLUP);
  pinMode(WIFICONFIGPIN, INPUT_PULLUP);

  // Watchdog für diesen Task aktivieren (10 Sekunden Timeout)
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 10000,
      .idle_core_mask = (1 << 1), // Core 1 = loopTask
      .trigger_panic = true};
  // esp_task_wdt_init(&wdt_config);
  // esp_task_wdt_add(NULL); // current task (loopTask)
  handleCrashCounter();
  checkForFailover();
  ESP_LOGI(TAG, "Watchdog initialized and crash counter checked.");

  display.init();
  display.setFont(ArialMT_Plain_10);
  displayOnAt = millis();
  ESP_LOGI(TAG, "Display ready");

  // WiFi.onEvent(onWifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // Start NVS configuration
  nvs_stats_t nvs_stats;
  nvs_get_stats(NULL, &nvs_stats);
  ESP_LOGI(TAG, "NVS-Statistics:");
  ESP_LOGI(TAG, "Used entries: %u", nvs_stats.used_entries);
  ESP_LOGI(TAG, "Free entries: %u", nvs_stats.free_entries);
  ESP_LOGI(TAG, "Total entries: %u", nvs_stats.total_entries);

  changeNvsMode(false);
  valveOpenAt = preferences.getULong("valveOpenAt", 0);
  valveOpenAt = preferences.getULong("valveOpenAtTs", 0);
  valveCloseAt = preferences.getULong("valveCloseAt", 0);
  valveCloseAt = preferences.getULong("valveCloseAtTs", 0);
  ESP_LOGI(TAG, "NVS loaded");

  applySyslogLogLevel();

  iotWebConf.setupUpdateServer(
      [](const char *updatePath)
      { httpUpdater.setup(&server, updatePath); },
      [](const char *userName, char *password)
      { httpUpdater.updateCredentials(userName, password); });

  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);
  mqttGroup.addItem(&mqttTopicPathParam);
  mqttGroup.addItem(&mqttHeaterStatusTopicParam);
  mqttGroup.addItem(&mqttHeaterStatusValueParam);
  mqttGroup.addItem(&mqttPumpTopicParam);
  mqttGroup.addItem(&mqttPumpValueParam);
  mqttGroup.addItem(&mqttThermalDesinfectionTopicParam);
  mqttGroup.addItem(&mqttThermalDesinfectionValueParam);
  mqttGroup.addItem(&mqttValvePressureTopicParam);
  iotWebConf.addParameterGroup(&mqttGroup);
  ntpGroup.addItem(&ntpServerParam);
  ntpGroup.addItem(&ntpTimezoneParam);
  iotWebConf.addParameterGroup(&ntpGroup);
  valveGroup.addItem(&valvePressureHighParam);
  valveGroup.addItem(&valvePressureLowParam);
  valveGroup.addItem(&valveMaxOpenParam);
  iotWebConf.addParameterGroup(&valveGroup);
  tempGroup.addItem(&tempOutParam);
  tempGroup.addItem(&tempRetParam);
  tempGroup.addItem(&tempIntParam);
  tempGroup.addItem(&tempRetDiffParam);
  tempGroup.addItem(&tempDiffTriggerParam);
  iotWebConf.addParameterGroup(&tempGroup);
  miscGroup.addItem(&languParam);
  iotWebConf.addParameterGroup(&miscGroup);
  syslogGroup.addItem(&syslogServerParam);
  syslogGroup.addItem(&syslogPortParam);
  syslogGroup.addItem(&syslogLogLevelParam);
  iotWebConf.addParameterGroup(&syslogGroup);

  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&onWifiConnected);
  iotWebConf.setConfigPin(WIFICONFIGPIN);

  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    ESP_LOGW(TAG, "Invalid config detected - restoring WiFi settings...");
    // much better handling than iotWebConf library to avoid lost wifi on configuration change
    if (preferences.isKey("apPassword"))
      strncpy(iotWebConf.getApPasswordParameter()->valueBuffer, preferences.getString("apPassword").c_str(), iotWebConf.getApPasswordParameter()->getLength());
    else
      ESP_LOGW(TAG, "AP Password not found for restauration.");
    if (preferences.isKey("wifiSsid"))
      strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer, preferences.getString("wifiSsid").c_str(), iotWebConf.getWifiSsidParameter()->getLength());
    else
      ESP_LOGW(TAG, "WiFi SSID not found for restauration.");
    if (preferences.isKey("wifiPassword"))
      strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer, preferences.getString("wifiPassword").c_str(), iotWebConf.getWifiPasswordParameter()->getLength());
    else
      ESP_LOGW(TAG, "WiFi Password not found for restauration.");
    iotWebConf.saveConfig();
    iotWebConf.resetWifiAuthInfo();
  }

  langu = atoi(languParam.value());
  tempDiffTrigger = atof(tempDiffTriggerParam.valueBuffer);
  ESP_LOGI(TAG, "tempDiffTrigger set to: %.2f", tempDiffTrigger);
  valveMaxOpen = atoi(valveMaxOpenParam.valueBuffer);
  ESP_LOGI(TAG, "valveMaxOpen set to: %d", valveMaxOpen);

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []() {
    detectSensors();
    iotWebConf.handleConfig();
  });
  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });
  server.on("/coredump", handleCoreDump);
  server.on("/deletecoredump", handleDeleteCoreDump);
  server.on("/crash", startCrash); // Adress to create a coredump for testing
  server.on("/resetValveError", handleResetValveError);
  // TODO: detectSensors per Link aufrufen
  ESP_LOGI(TAG, "Wifi manager ready.");

  strcpy(mqttWillTopic, mqttTopicPath);
  strcat(mqttWillTopic, MQTT_PUB_STATUS);
  mqttClient.setWill(mqttWillTopic, 0, true, "Offline", 7);
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onSubscribe(onMqttSubscribe);

  if (mqttUser != "")
    mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, MQTT_PORT);
  ESP_LOGI(TAG, "MQTT ready");

  // start OneWire sensor reading
  tempSemaphore = xSemaphoreCreateBinary();
  xSemaphoreGive(tempSemaphore); // Initialisiere Semaphore als frei
  tempQueue = xQueueCreate(TEMP_QUEUE_LENGTH, sizeof(TempReport_t));
  sensors.setWaitForConversion(false); // wichtig für Task‑Betrieb
  sensors.begin();
  detectSensors();
  // Allokiere SensorIds auf dem Heap und fülle sie
  SensorIds_t *pIds = (SensorIds_t *)malloc(sizeof(SensorIds_t));
  memcpy(pIds->out, sensorOut_id, sizeof(DeviceAddress_t));
  memcpy(pIds->ret, sensorRet_id, sizeof(DeviceAddress_t));
  memcpy(pIds->intl, sensorInt_id, sizeof(DeviceAddress_t));
  // Erstelle Tasks
  xTaskCreatePinnedToCore(tempTask, "TempTask", 4096, pIds, 1, &tempTaskHandle, 1);
  ESP_LOGI(TAG, "Sensors ready");

  // configure the timezone
  configTime(0, 0, ntpServer);
  setTimezone(ntpTimezone);
  ESP_LOGI(TAG, "NTP ready");

  // Init OTA function
  ArduinoOTA.onStart([]()
                     {
    ESP_LOGI(TAG, "Start OTA");
    display.displayOn();
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2 - 10, "OTA Update -> " + String(next->label));
    display.display(); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        {
    // esp_task_wdt_reset();
    ESP_LOGI(TAG, "OTA Progress: %u%%", (progress / (total / 100)));
    display.drawProgressBar(4, 32, 120, 8, progress / (total / 100) );
    display.display(); });
  ArduinoOTA.onEnd([]()
                   {
    ESP_LOGI(TAG, "\nEnd OTA");
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "Restart");
    display.display(); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    ESP_LOGE(TAG, "Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) ESP_LOGE(TAG, "Auth Failed");
    else if (error == OTA_BEGIN_ERROR) ESP_LOGE(TAG, "Begin Failed");
    else if (error == OTA_CONNECT_ERROR) ESP_LOGE(TAG, "Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) ESP_LOGE(TAG, "Receive Failed");
    else if (error == OTA_END_ERROR) ESP_LOGE(TAG, "End Failed");
    display.clear();
    display.setFont(ArialMT_Plain_24);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "OTA Failed"); });
  ESP_LOGI(TAG, "OTA ready");

  // Timers
  timer.every(1000, onSec1Timer);
  timer.every(10000, onSec10Timer);
  timer.every(60000, onMin1Timer);
  ESP_LOGI(TAG, "Timer ready");
  
  // Button
  userBtn.attachClick(handleUserBtnClick, &userBtn);
  userBtn.attachLongPressStop(handleUserBtnLongPress, &userBtn);
  userBtn.attachDoubleClick(handleUserBtnDoublePress, &userBtn);
  userBtn.setLongPressIntervalMs(1000);
  resetBtn.attachLongPressStop(handleResetBtnLongPress, &resetBtn);
  resetBtn.setLongPressIntervalMs(1000);
  ESP_LOGI(TAG, "Buttons ready");

  // Firmware als gültig markieren
  esp_ota_mark_app_valid_cancel_rollback();
}

void loop()
{
  // esp_task_wdt_reset();
  ArduinoOTA.handle();
  iotWebConf.doLoop();
  timer.tick();
  userBtn.tick();
  resetBtn.tick();

  if (needReset)
  {
    ESP_LOGI(TAG, "Rebooting in 1 second.");
    iotWebConf.delay(1000);
    ESP.restart();
  }

  if (!manualMode && displayOn && 60000 < now - displayOnAt)
  { // switch display off after 10mins
    display.displayOff();
    displayTimer.detach();
    displayOn = false;
  }
}