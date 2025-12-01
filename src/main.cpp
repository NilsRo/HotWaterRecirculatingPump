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
#include "average.h"

#define STRING_LEN 128
#define DALLASADRESS_LEN 17
#define nils_length(x) ((sizeof(x) / sizeof(0 [x])) / ((size_t)(!(sizeof(x) % sizeof(0 [x])))))
// #define nils_length( x ) ( sizeof(x) )

// ports
const int ONEWIREPIN = D8;
const int PUMPPIN = D3;
const int VALVEPIN = D4;
const int DISPLAYPIN = D5;
const int WIFICONFIGPIN = D7;

String pump[20] = {"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};
unsigned int pumpCnt;
bool pumpCntInit = true;
static float t[] = {255.0, 255.0, 255.0, 255.0, 255.0}; // letzten 5 Temepraturwerte speichern
bool pumpRunning = false;
bool pumpManual = false;
unsigned long pumpBlock;
unsigned long pumpStartedAt;
unsigned long timePressed;
unsigned long timeReleased;
float tempOut;
float tempRet;
float tempInt;
float tempDiff;
float tempDiffTrigger;
char tempDiffTriggerStr[32];
float mqttTempOut;
float mqttTempRet;
float mqttTempInt;
float mqttTempDiff;
unsigned int checkCnt;

float valvePressure;
bool valveOpened = false;
unsigned long valveOpenedAt;
unsigned long valveOpenedAt_ts;
unsigned long valveClosedAt;
unsigned long valveClosedAt_ts;
unsigned long valveOpenedForSec;
int valveMaxOpen;
char valveMaxOpenStr[4];
bool valveError = false;
SimpleAverage valvePressureAvg(10);

OneWire oneWire(ONEWIREPIN);

DallasTemperature sensors(&oneWire);
DeviceAddress sensorOut_id;
DeviceAddress sensorRet_id;
DeviceAddress sensorInt_id;
bool sensorError = false;

// OLED Display
SSD1306Wire display(0x3C, SDA, SCL); // ADDRESS, SDA, SCL  -  SDA and SCL usually populate automatically based on your board's pins_arduino.h e.g. https://github.com/esp8266/Arduino/blob/master/variants/nodemcu/pins_arduino.h
unsigned int displayPage;
unsigned int displayPageLastRuns = 1;
bool networksPageFirstCall = true;
bool pumpFirstCall = true;
int displayPinState = HIGH;
unsigned int displayPinChanged;
bool displayOn = true;
bool needReset = false;

#define MQTT_PORT 1883
#define MQTT_PUB_TEMP_OUT "dhw_Tflow_measured"
#define MQTT_PUB_TEMP_RET "dhw_Treturn"
#define MQTT_PUB_TEMP_DIFF "dhw_Tdelta"
#define MQTT_PUB_TEMP_INT "Tint"
#define MQTT_PUB_PUMP "dhw_pump_circulation"
#define MQTT_PUB_VALVE_OPENED "dhw_valve_opened"
#define MQTT_PUB_VALVE_OPENED_FOR_SEC "dhw_valve_opened_for_sec"
#define MQTT_PUB_VALVE_ERROR "dhw_valve_error"
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

#define CONFIG_VERSION "6"
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

/* #region common */
int mod(int x, int y)
{
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
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

void initCoreDumpFlash()
{
  if (esp_core_dump_image_check() != ESP_OK)
  {
    esp_core_dump_image_erase();
    Serial.println("Invalid core dump deleted!");
  }
}

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
    Serial.println("Error opening NVS-Namespace");
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
  dtostrf(tempOut, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(tempRetParam.label) + ": </td>";
  s += "<td>";
  s += tempRetParam.value();
  dtostrf(tempRet, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(tempIntParam.label) + ": </td>";
  s += "<td>";
  s += tempIntParam.value();
  dtostrf(tempInt, 2, 2, tempStr);
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
  s += "</tr><tr>";
  s += "<td>detected devices: </td>";
  s += "<td>" + String(sensors.getDeviceCount()) + "</td>";
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
    s += "<p>status valve : error";
  else
    s += "<p>status valve : ok";
  if (valveOpened)
  {
    s += "<p>valve: open (";
    dtostrf((millis() - valveOpenedAt) / 60000.0, 2, 2, tempStr);
    s += tempStr;
    s += "min)";
  }
  else
    s += "<p>valve: closed";

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
  s += "<p>";
  switch (esp_core_dump_image_check())
  {
  case ESP_OK:
    s += "<a href=/coredump>core dump found</a> - <a href=/deletecoredump>delete core dump</a>";
    break;
  case ESP_ERR_NOT_FOUND:
    s += "no core dump found";
    break;
  case ESP_ERR_INVALID_SIZE:
    s += "core dump with invalid size - <a href=/deletecoredump>delete core dump</a>";
    break;
  case ESP_ERR_INVALID_CRC:
    s += "core dump with invalid CRC - <a href=/deletecoredump>delete core dump</a>";
  }
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
  Serial.println(iotWebConf.getApPasswordParameter()->getLength() > 0);
  if (iotWebConf.getApPasswordParameter()->getLength() > 0)
    preferences.putString("apPassword", String(iotWebConf.getApPasswordParameter()->valueBuffer));
  preferences.putString("wifiSsid", String(iotWebConf.getWifiAuthInfo().ssid));
  preferences.putString("wifiPassword", String(iotWebConf.getWifiAuthInfo().password));

  Serial.println("Configuration saved.");
  // TODO: Neustart bei normalen Parametern vermeiden
  needReset = true;
}

bool formValidator(iotwebconf::WebRequestWrapper *webRequestWrapper)
{
  Serial.println("Validating form.");
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
  Serial.printf("  Setting Timezone to %s\n", ntpTimezone);
  setenv("TZ", ntpTimezone, 1); //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}
/* #endregion */

/* #region connection handling */
void connectToMqtt()
{
  if (strlen(mqttServer) > 0)
  {
    Serial.println("Connecting to MQTT...");
    mqttClient.connect();
  }
}

void onWifiConnected()
{
  Serial.println("Connected to Wi-Fi.");
  Serial.println(WiFi.localIP());
  connectToMqtt();
  timeClient.begin();
  ArduinoOTA.begin();
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.println("Disconnected from Wi-Fi.");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  timeClient.end();
  // ArduinoOTA.end();  TODO: Fehler untersuchen, dumped.
  Serial.println("TCP services stopped.");
}
/* #endregion */

/* #region MQTT */
void onMqttConnect(bool sessionPresent)
{
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
  mqttPublish(MQTT_PUB_STATUS, "Online", true, false);
  mqttPublish(MQTT_PUB_WIFI, getWifiJson().c_str(), true, true);
  uint16_t packetIdSub;
  if (strlen(mqttHeaterStatusTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttHeaterStatusTopic, 2);
    Serial.print("Subscribed to topic: ");
    Serial.println(String(mqttHeaterStatusTopic) + " - " + String(packetIdSub));
  }
  if (strlen(mqttPumpTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttPumpTopic, 2);
    Serial.print("Subscribed to topic: ");
    Serial.println(String(mqttPumpTopic) + " - " + String(packetIdSub));
  }
  if (strlen(mqttThermalDesinfectionTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttThermalDesinfectionTopic, 2);
    Serial.print("Subscribed to topic: ");
    Serial.println(String(mqttThermalDesinfectionTopic) + " - " + String(packetIdSub));
  }
  if (strlen(mqttValvePressureTopic) > 0)
  {
    packetIdSub = mqttClient.subscribe(mqttValvePressureTopic, 2);
    Serial.print("Subscribed to topic: ");
    Serial.println(String(mqttValvePressureTopic) + " - " + String(packetIdSub));
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

  Serial.printf(" [%8u] Disconnected from the broker reason = %s\n", millis(), mqttDisconnectReason.c_str());
  digitalWrite(LED_BUILTIN, LOW);

  if (WiFi.isConnected())
  {
    Serial.printf(" [%8u] Reconnecting to MQTT..\n", millis());
    mqttReconnectTimer.once(5, connectToMqtt);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos)
{
  Serial.printf(" [%8u] Subscribe acknowledged id: %u, qos: %u\n", millis(), packetId, qos);
}

void onMqttPublish(uint16_t packetId)
{
  // Serial.print("Publish acknowledged.");
  // Serial.print("  packetId: ");
  // Serial.println(packetId);
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
  // Serial.println("Publish received.");
  // Serial.print("  topic: ");
  // Serial.println(topic);
  // Serial.print("  qos: ");
  // Serial.println(properties.qos);
  // Serial.print("  payload: ");
  // Serial.println(payload);
  // Serial.print("  dup: ");
  // Serial.println(properties.dup);
  // Serial.print("  retain: ");
  // Serial.println(properties.retain);
  // Serial.print("  len: ");
  // Serial.println(len);
  // Serial.print("  index: ");
  // Serial.println(index);
  // Serial.print("  total: ");
  // Serial.println(total);
  char new_payload[len + 1];
  strncpy(new_payload, payload, len);
  new_payload[len] = '\0';
  if (strcmp(topic, mqttPumpTopic) == 0)
  {
    Serial.print("mqtt pump: ");
    mqttPump = (strcmp(mqttPumpValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttThermalDesinfectionTopic) == 0)
  {
    Serial.print("mqtt thermal desinfection: ");
    Serial.println(new_payload);
    mqttThermalDesinfection = (strcmp(mqttThermalDesinfectionValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttHeaterStatusTopic) == 0)
  {
    Serial.print("mqtt heater status: ");
    mqttHeaterStatus = (strcmp(mqttHeaterStatusValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttValvePressureTopic) == 0)
  {
    Serial.print("mqtt pressure: ");
    valvePressure = atof(new_payload);
  }
  Serial.println(new_payload);
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
        Serial.println("MQTT send: " + tempTopicStr + " = " + newPayloadStr);
        if (mqttClient.publish(tempTopicStr.c_str(), 0, true, newPayloadStr.c_str()) > 0)
          // TODO: Statt dem String ggf. einen Hash wegspeichern zur Optimierung der Speichernutzung
          mqttLastMessage[topicStr] = payloadStr;
        else
          Serial.println("MQTT (error) not send: " + tempTopicStr + " = " + newPayloadStr);
      }
    }
    else
    {
      Serial.println("MQTT not send: " + tempTopicStr + " = " + payloadStr);
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
  dtostrf(tempOut, 2, 2, msg_out);
  mqttPublish(MQTT_PUB_TEMP_OUT, msg_out, mqttInit, false);
  dtostrf(tempRet, 2, 2, msg_out);
  mqttPublish(MQTT_PUB_TEMP_RET, msg_out, mqttInit, false);
  dtostrf(tempInt, 2, 2, msg_out);
  mqttPublish(MQTT_PUB_TEMP_INT, msg_out, mqttInit, false);
  dtostrf(tempDiff, 2, 4, msg_out);
  mqttPublish(MQTT_PUB_TEMP_DIFF, msg_out, mqttInit, false);
  if (valveOpened)
    mqttPublish(MQTT_PUB_VALVE_OPENED, "1", mqttInit, false);
  else
    mqttPublish(MQTT_PUB_VALVE_OPENED, "0", mqttInit, false);
  if (valveError)
    mqttPublish(MQTT_PUB_VALVE_ERROR, "1", mqttInit, false);
  else
    mqttPublish(MQTT_PUB_VALVE_ERROR, "0", mqttInit, false);
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
  else if (pumpManual)
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
  if (pumpManual)
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
  object["dhw"]["sensor_out_conn"] = sensors.isConnected(sensorOut_id);
  object["dhw"]["sensor_ret_conn"] = sensors.isConnected(sensorRet_id);
  object["dhw"]["sensor_int_present"] = sensors.isConnected(sensorInt_id);

  object["valve"]["valve_error"] = valveError;

  object["sys"]["reset_reason"] = esp_reset_reason();
  object["sys"]["reset_reason_msg"] = verbose_print_reset_reason(esp_reset_reason());
  object["sys"]["core_dump"] = esp_core_dump_image_check();
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
  for (uint8_t i = 0; i < 8; i++)
  {
    if (deviceAddress[i] < 16)
      Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
  Serial.println("");
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
int hexStr2Arr(unsigned char *out, const char *in, size_t out_len_max = 0)
{
  if (!out_len_max)
    out_len_max = 2147483647; // INT_MAX

  const int in_len = strnlen(in, out_len_max * 2);
  if (in_len % 2 != 0)
    return -1; // error, in str len should be even

  // calc actual out len
  const int out_len = out_len_max < (in_len / 2) ? out_len_max : (in_len / 2);

  for (int i = 0; i < out_len; i++)
  {
    char ch0 = in[2 * i];
    char ch1 = in[2 * i + 1];
    uint8_t nib0 = (ch0 & 0xF) + (ch0 >> 6) | ((ch0 >> 3) & 0x8);
    uint8_t nib1 = (ch1 & 0xF) + (ch1 >> 6) | ((ch1 >> 3) & 0x8);
    out[i] = (nib0 << 4) | nib1;
  }
  return out_len;
}

void detectSensors()
{
  int i;
  // locate devices on the bus
  Serial.print("Searching devices...");
  Serial.print("Found ");
  Serial.print(sensors.getDeviceCount(), DEC);
  Serial.println(" devices.");

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
  hexStr2Arr(sensorInt_id, tempIntParam.value());
  hexStr2Arr(sensorOut_id, tempOutParam.value());
  hexStr2Arr(sensorRet_id, tempRetParam.value());
}

void checkSensors()
{
  String info;
  if (sensors.isConnected(sensorInt_id) && sensors.isConnected(sensorRet_id) && sensors.isConnected(sensorOut_id) && sensors.getDeviceCount() == 3)
  {
    if (sensorError)
      mqttPublish(MQTT_PUB_INFO, "sensorerror solved", true, false);
    sensorError = false;
    sensors.setResolution(sensorOut_id, 12); // hohe Genauigkeit
    sensors.setResolution(sensorRet_id, 12); // hohe Genauigkeit
  }
  else
  {
    info = "sensorerror - internal: " + String(sensors.isConnected(sensorInt_id)) + " return: " + String(sensors.isConnected(sensorRet_id)) + " out: " + String(sensors.isConnected(sensorOut_id));
    mqttPublish(MQTT_PUB_INFO, info.c_str(), true, false);
    sensorError = true;
  }
}

void getTemp()
{
  sensors.requestTemperatures(); // Send the command to get temperatures
  if (sensors.isConnected(sensorOut_id))
    tempOut = sensors.getTempC(sensorOut_id);
  else
    tempOut = 0;
  if (sensors.isConnected(sensorRet_id))
    tempRet = sensors.getTempC(sensorRet_id);
  else
    tempRet = 0;
  if (sensors.isConnected(sensorInt_id))
    tempInt = sensors.getTempC(sensorInt_id);
  else
    tempInt = 0;
  // Serial.print(" Temp Out: ");
  // Serial.println(tempOut);
  // Serial.print(" Temp In: ");
  // Serial.println(tempRet);
  // Serial.print(" Temp Int: ");
  // Serial.println(tempInt);
}
/* #endregion */

/* #region Display */
void getLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time 1");
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
  DeviceAddress sensor1_id;
  DeviceAddress sensor2_id;
  DeviceAddress sensor3_id;
  unsigned long now = millis();
  unsigned int lineStart = 0;
  unsigned lineEnd = 0;
  unsigned int lineCnt = 1;

  display.clear();

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
    // display.drawString(128, 0, timeClient.getFormattedTime() );
    strftime(tempStr, 6, "%H:%M", &localTime);
    display.drawString(128, 0, tempStr);
    display.drawLine(0, 11, 128, 11);
    display.setTextAlignment(TEXT_ALIGN_CENTER);

    if (sensors.isConnected(sensorOut_id))
    {
      dtostrf(tempOut, 2, 2, tempStr);
      display.drawString(64, 12, String(txtFlow[langu]) + ": " + String(tempStr) + " C°");
    }
    else
      display.drawString(64, 12, String(txtFlow[langu]) + ": ERROR!");
    if (sensors.isConnected(sensorRet_id))
    {
      dtostrf(tempRet, 2, 2, tempStr);
      display.drawString(64, 24, String(txtReturn[langu]) + ": " + String(tempStr) + " C°");
    }
    else
      display.drawString(64, 24, String(txtReturn[langu]) + ": ERROR!");

    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (pumpRunning)
    {
      display.invertDisplay();
      if (pumpManual)
        display.drawString(64, 36, String(txtManualOn[langu]));
      else
        display.drawString(64, 36, String(txtPumpOn[langu]));
    }
    else
    {
      display.normalDisplay();
      if (pumpManual)
        display.drawString(64, 36, String(txtManualOff[langu]));
      else
        display.drawString(64, 36, String(txtPumpOff[langu]));
    }
    if (valveOpened)
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
    display.drawString(64, 0, txtSensors[langu]);
    display.drawLine(0, 11, 128, 11);
    display.setFont(ArialMT_Plain_16);
    if (sensors.getAddress(sensor1_id, 0))
    {
      temp = sensors.getTempC(sensor1_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 12, "1: " + String(tempStr) + "°C");
    }
    else
      display.drawString(64, 12, "1: " + String(txtNoSensor[langu]));
    if (sensors.getAddress(sensor2_id, 1))
    {
      temp = sensors.getTempC(sensor2_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 30, "2: " + String(tempStr) + "°C");
    }
    else
      display.drawString(64, 30, "2: " + String(txtNoSensor[langu]));
    if (sensors.getAddress(sensor3_id, 2))
    {
      temp = sensors.getTempC(sensor3_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 48, "3: " + String(tempStr) + "°C");
    }
    else
      display.drawString(64, 48, "3: " + String(txtNoSensor[langu]));
    break;
  case 4:
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, String(sensors.getDeviceCount()) + " DS18B20 Device(s)");
    display.drawLine(0, 11, 128, 11);
    if (sensors.getAddress(sensor1_id, 0))
    {
      printAddress(sensor1_id);
      display.drawString(64, 14, "1: " + formatAdress(sensor1_id));
    }
    else
    {
      Serial.println("Unable to find address for Sensor 1");
      display.drawString(64, 14, "1: " + String(txtNoSensor[langu]));
    }
    if (sensors.getAddress(sensor2_id, 1))
    {
      printAddress(sensor2_id);
      display.drawString(64, 32, "2: " + formatAdress(sensor2_id));
    }
    else
    {
      Serial.println("Unable to find address for Sensor 2");
      display.drawString(64, 32, "2: " + String(txtNoSensor[langu]));
    }
    if (sensors.getAddress(sensor3_id, 2))
    {
      printAddress(sensor3_id);
      display.drawString(64, 50, "3: " + formatAdress(sensor3_id));
    }
    else
    {
      Serial.println("Unable to find address for Sensor 3");
      display.drawString(64, 50, "3: " + String(txtNoSensor[langu]));
    }
    break;
  case 5:
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
  case 6:
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
        Serial.println("scan started");
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        networksPage = 1;
        networksPageFirstCall = true;
      }
      networksFound = WiFi.scanComplete();

      Serial.print("Scan status: ");
      Serial.println(networksFound);
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
        Serial.print("Networks found: ");
        Serial.println(networksFound);
        display.setFont(ArialMT_Plain_16);
        display.drawString(64, 24, txtNoAp[langu]);
      }
      else
      {
        Serial.print("Networks found: ");
        Serial.println(networksFound);
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
          Serial.print(i + 1);
          Serial.print(": ");
          Serial.print(WiFi.SSID(i));
          Serial.print(" (");
          Serial.print(WiFi.RSSI(i));
          Serial.print(")");
          Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : "*");
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
  Serial.println("Turn on circulation");
  pumpRunning = true;
  pumpStartedAt = millis();
  digitalWrite(PUMPPIN, LOW);
  mqttSendTopics();
  Serial.print("Pump on: ");
  strftime(tempStr, 40, "%d.%m.%Y %T", &localTime);
  Serial.println(tempStr);
  if (pumpCntInit)
    pumpCntInit = false;
  else if (++pumpCnt > nils_length(pump) - 1)
    pumpCnt = 0; // Reset counter
  pump[pumpCnt] = tempStr;
}

void pumpOff()
{
  Serial.println("Turn off circulation");
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
    else if (!pumpManual)
    {
      if (++checkCnt >= 5)
        checkCnt = 0; // Reset counter
      t[checkCnt] = tempOut;
      int cnt_alt = (checkCnt + 6) % 5;
      tempDiff = t[checkCnt] - t[cnt_alt]; // Difference to 5 sec before
      if (!pumpRunning)
      {
        if ((((tempDiff >= tempDiffTrigger && (mqttHeaterStatus || !mqttClient.connected())) || mqttPump) && (300000 < millis() - pumpBlock || pumpFirstCall)) || 86400000 < millis() - pumpStartedAt)
        { // smallest temp change is 0,0625°C,
          Serial.print("Temperature Delta: ");
          Serial.println(tempDiff);
          if (mqttPump)
          {
            mqttPump = false;
            Serial.println("MQTT pump action done");
          }
          pumpBlock = millis();
          pumpFirstCall = false;
          pumpOn();
        }
      }
      else if (tempRet > (tempOut - tempRetDiffParam.value()) && !(tempDiff >= tempDiffTrigger) && 120000 < (millis() - pumpStartedAt))
      { // if return flow temp near temp out stop pump with a delay of 2 minutes and other rules
        pumpOff();
      }
    }
  }
}

void valveOpen()
{
  Serial.print("Open valve - ");
  Serial.println(valveOpened);
  if (!valveOpened)
  {
    valveOpened = true;
    valveOpenedForSec = valveOpenedAt_ts - valveClosedAt_ts;
    mqttPublish(MQTT_PUB_VALVE_OPENED_FOR_SEC, String(valveOpenedForSec).c_str(), true, false);
    valveOpenedAt = millis();
    saveValveTimestampNvs("valveOpenedAt", valveOpenedAt);
    valveOpenedAt_ts = timeClient.getEpochTime();
    saveValveTimestampNvs("valveOpenedAt_ts", valveOpenedAt_ts);
    valveClosedAt = 0;
    digitalWrite(VALVEPIN, LOW);
  }
}

void valveClose()
{
  Serial.print("Close valve - ");
  Serial.println(valveOpened);
  if (valveOpened)
  {
    valveOpened = false;
    valveClosedAt = millis();
    saveValveTimestampNvs("valveClosedAt", valveClosedAt);
    valveClosedAt_ts = timeClient.getEpochTime();

    saveValveTimestampNvs("valveClosedAt_ts", valveClosedAt_ts);
    digitalWrite(VALVEPIN, HIGH);
  }
}

void checkValve()
{
  if (!valveError && valveMaxOpen && mqttClient.connected() && valvePressureAvg > 0.0f)
  {
    if (valveOpened && (((millis() - valveOpenedAt) / 60000.0 > valveMaxOpen) || valvePressureAvg < valvePressureLowParam.value() - 0.2f))
    // error if valve is opened too long or if the pressure is 0.2 below low pressure setting.
    {
      valveError = true;
      valveClose();
      return;
    }
    if (!valveOpened && valvePressureAvg <= valvePressureLowParam.value())
      valveOpen();
    if (valveOpened && valvePressureAvg >= valvePressureHighParam.value())
      valveClose();
  }
  else if (valveOpened)
    valveClose();
}
/* #endregion */

/* #region Timer */
bool onSec1Timer(void *)
{
  updateTime();
  getTemp();
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
  Serial.print("Add pressure to calc: ");
  Serial.println(valvePressure);
  valvePressureAvg.addValue(valvePressure);
  mqttPublish(MQTT_PUB_VALVE_PRESSURE_AVG, String(valvePressureAvg.getAverage()).c_str(), false, false);

  return true;
}
/* #endregion */

void setup()
{
  // basic setup
  Serial.begin(115200);
  // initCoreDumpFlash();
  esp_core_dump_init();
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PUMPPIN, OUTPUT);
  pinMode(VALVEPIN, OUTPUT);
  digitalWrite(PUMPPIN, HIGH);
  digitalWrite(VALVEPIN, HIGH);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(DISPLAYPIN, INPUT_PULLUP);
  pinMode(WIFICONFIGPIN, INPUT_PULLUP);

  display.init();
  display.setFont(ArialMT_Plain_10);

  // WiFi.onEvent(onWifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // Start NVS configuration
  nvs_stats_t nvs_stats;
  nvs_get_stats(NULL, &nvs_stats);
  Serial.println("NVS-Statistics:");
  Serial.print("Used entries: ");
  Serial.println(nvs_stats.used_entries);
  Serial.print("Free entries: ");
  Serial.println(nvs_stats.free_entries);
  Serial.print("Total entries: ");
  Serial.println(nvs_stats.total_entries);

  changeNvsMode(false);
  valveOpenedAt = preferences.getULong("valveOpenedAt", 0);
  valveOpenedAt = preferences.getULong("valveOpenedAt_ts", 0);
  valveClosedAt = preferences.getULong("valveClosedAt", 0);
  valveClosedAt = preferences.getULong("valveClosedAt_ts", 0);
  Serial.println("NVS loaded");

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

  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&onWifiConnected);
  iotWebConf.setConfigPin(WIFICONFIGPIN);

  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    Serial.println("Invalid config detected - restoring WiFi settings...");
    // much better handling than iotWebConf library to avoid lost wifi on configuration change
    if (preferences.isKey("apPassword"))
      strncpy(iotWebConf.getApPasswordParameter()->valueBuffer, preferences.getString("apPassword").c_str(), iotWebConf.getApPasswordParameter()->getLength());
    else
      Serial.println("AP Password not found for restauration.");
    if (preferences.isKey("wifiSsid"))
      strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer, preferences.getString("wifiSsid").c_str(), iotWebConf.getWifiSsidParameter()->getLength());
    else
      Serial.println("WiFi SSID not found for restauration.");
    if (preferences.isKey("wifiPassword"))
      strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer, preferences.getString("wifiPassword").c_str(), iotWebConf.getWifiPasswordParameter()->getLength());
    else
      Serial.println("WiFi Password not found for restauration.");
    iotWebConf.saveConfig();
    iotWebConf.resetWifiAuthInfo();
  }

  langu = atoi(languParam.value());
  tempDiffTrigger = atof(tempDiffTriggerParam.valueBuffer);
  Serial.print("tempDiffTrigger set to: ");
  Serial.println(tempDiffTrigger);
  valveMaxOpen = atoi(valveMaxOpenParam.valueBuffer);
  Serial.print("valveMaxOpen set to: ");
  Serial.println(valveMaxOpen);

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", [] { // detectSensors();
    iotWebConf.handleConfig();
  });
  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });
  server.on("/coredump", handleCoreDump);
  server.on("/deletecoredump", handleDeleteCoreDump);
  server.on("/crash", startCrash); // Adress to create a coredump for testing
  // TODO: detectSensors per Link aufrufen
  Serial.println("Wifi manager ready.");

  strcpy(mqttWillTopic, mqttTopicPath);
  strcat(mqttWillTopic, MQTT_PUB_STATUS);
  mqttClient.setWill(mqttWillTopic, 0, true, "Offline", 7);
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onSubscribe(onMqttSubscribe);

  if (mqttUser != "")
    mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, MQTT_PORT);
  Serial.println("MQTT ready");

  sensors.begin();
  detectSensors();
  checkSensors();
  Serial.println("Sensors ready");

  // configure the timezone
  configTime(0, 0, ntpServer);
  setTimezone(ntpTimezone);
  Serial.println("NTP ready");

  // Init OTA function
  ArduinoOTA.onStart([]()
                     {
    Serial.println("Start OTA");
    display.displayOn();
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2 - 10, "OTA Update");
    display.display(); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
    display.drawProgressBar(4, 32, 120, 8, progress / (total / 100) );
    display.display(); });
  ArduinoOTA.onEnd([]()
                   {
    Serial.println("\nEnd OTA");
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "Restart");
    display.display(); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    display.clear();
    display.setFont(ArialMT_Plain_24);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "OTA Failed"); });
  Serial.println("OTA Ready");

  // Timers
  timer.every(1000, onSec1Timer);
  timer.every(10000, onSec10Timer);
  timer.every(60000, onMin1Timer);
  Serial.println("Timer ready");
}

void loop()
{
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  timer.tick();

  if (needReset)
  {
    Serial.println("Rebooting in 1 second.");
    iotWebConf.delay(1000);
    ESP.restart();
  }

  // Check buttons
  unsigned long now = millis();
  if ((500 < now - iotWebConfPinChanged) && (iotWebConfPinState != digitalRead(WIFICONFIGPIN)))
  {
    iotWebConfPinState = 1 - iotWebConfPinState; // invert pin state as it is changed
    iotWebConfPinChanged = now;
    if (iotWebConfPinState)
    { // reset settings and reboot
      iotWebConf.getRootParameterGroup()->applyDefaultValue();
      iotWebConf.saveConfig();
      needReset = true;
    }
  }
  if ((500 < now - displayPinChanged) && (displayPinState != digitalRead(DISPLAYPIN)))
  {
    displayPinState = 1 - displayPinState; // invert pin state as it is changed
    displayPinChanged = now;
    if (displayPinState) // button pressed action - set pressed time
    {
      // button released
      timeReleased = millis();
      Serial.println("Button released");
      Serial.print("Display Button State: ");
      Serial.print(displayPinState);
      Serial.print(", Time: ");
      Serial.println(timeReleased - timePressed);
      if (2000 < (timeReleased - timePressed))
      {
        Serial.println("Long press detected");
        pumpManual = !pumpManual;
      }
      else
      {
        if (pumpManual)
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
            if (displayPage == 6)
              displayPage = 0;
            else
              displayPage++;

            if (displayPage == 6)
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
    }
    else
    {
      timePressed = now;
      Serial.println("Button pressed");
    }
  }
  if (600000 < now - timePressed)
  { // switch display off after 10mins
    display.displayOff();
    displayTimer.detach();
    displayOn = false;
  }
}