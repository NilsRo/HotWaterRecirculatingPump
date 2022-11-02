#include <Arduino.h>
#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
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

#define STRING_LEN 128
#define nils_length(x) ((sizeof(x) / sizeof(0 [x])) / ((size_t)(!(sizeof(x) % sizeof(0 [x])))))
// #define nils_length( x ) ( sizeof(x) )

// ports
const int ONEWIREPIN = D8;
const int PUMPPIN = D3;
const int VALVEPIN = D4;
const int DISPLAYPIN = D5;
const int WIFICONFIGPIN = D7;

String pump[20] = {"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};
unsigned int pumpCnt = 0;
bool pumpCntInit = true;
static float t[] = {255.0, 255.0, 255.0, 255.0, 255.0}; // letzten 5 Temepraturwerte speichern
bool pumpRunning = false;
bool pumpManual = false;
unsigned long pumpBlock = 0;
unsigned long pumpStartedAt = 0;
unsigned long timePressed = 0;
unsigned long timeReleased = 0;
float tempOut;
float tempRet;
float tempInt;
unsigned int checkCnt;

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(ONEWIREPIN);

// Pass our oneWire reference to Dallas Temperature sensor
DallasTemperature sensors(&oneWire);
DeviceAddress sensorOut_id;
DeviceAddress sensorRet_id;
DeviceAddress sensorInt_id;
bool sensorDetectionError = false;
bool sensorError = false;

// OLED Display
SSD1306Wire display(0x3C, SDA, SCL); // ADDRESS, SDA, SCL  -  SDA and SCL usually populate automatically based on your board's pins_arduino.h e.g. https://github.com/esp8266/Arduino/blob/master/variants/nodemcu/pins_arduino.h
unsigned int displayPage = 0;
unsigned int displayPageLastRuns = 1;
bool networksPageFirstCall = true;
bool pumpFirstCall = true;
int displayPinState = HIGH;
unsigned int displayPinChanged = 0;
bool displayOn = true;
bool needReset = false;

// For a cloud MQTT broker, type the domain name
//#define MQTT_HOST "example.com"
#define MQTT_PORT 1883
#define MQTT_PUB_TEMP_OUT "ww/ht/dhw_Tflow_measured"
#define MQTT_PUB_TEMP_RET "ww/ht/dhw_Treturn"
#define MQTT_PUB_TEMP_INT "ww/ht/Tint"
#define MQTT_PUB_PUMP "ww/ht/dhw_pump_circulation"
#define MQTT_PUB_INFO "ww/ht/info"
AsyncMqttClient mqttClient;
char mqttServer[STRING_LEN];
char mqttUser[STRING_LEN];
char mqttPassword[STRING_LEN];
char mqttHeaterStatusTopic[STRING_LEN];
char mqttHeaterStatusValue[STRING_LEN];
bool mqttHeaterStatus = true;
char mqttPumpTopic[STRING_LEN];
char mqttPumpValue[STRING_LEN];
bool mqttPump = false;
char mqttThermalDesinfectionTopic[STRING_LEN];
char mqttThermalDesinfectionValue[STRING_LEN];

Ticker mqttReconnectTimer;
Ticker disinfection24hTimer;
Ticker secTimer;
Ticker displayTimer;
Ticker sec10Timer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
char ntpServer[STRING_LEN];
char ntpTimezone[STRING_LEN];
char hostname[STRING_LEN];
time_t now;
struct tm localTime;

IPAddress localIP;
unsigned long lastScan = 0;
int networksFound;
int networksPage = 0;
unsigned int networksPageTotal = 0;
unsigned long displayPageSubChange = 0;

#define CONFIG_VERSION "4"
int iotWebConfPinState = HIGH;
unsigned long iotWebConfPinChanged = 0;
DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
static const char chooserValues[][STRING_LEN] = {"0", "1", "2"};
static const char chooserNames[][STRING_LEN] = {"Sensor 1", "Sensor 2", "Sensor 3"};
IotWebConf iotWebConf("Zirkulationspumpe", &dnsServer, &server, "", CONFIG_VERSION);
IotWebConfParameterGroup networkGroup = IotWebConfParameterGroup("network", "Network configuration");
IotWebConfTextParameter hostnameParam = IotWebConfTextParameter("Hostname", "hostname", hostname, STRING_LEN, "Zirkulationspumpe");
IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("mqtt", "MQTT configuration");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("MQTT server", "mqttServer", mqttServer, STRING_LEN);
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("MQTT user", "mqttUser", mqttUser, STRING_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("MQTT password", "mqttPassword", mqttPassword, STRING_LEN);
IotWebConfTextParameter mqttHeaterStatusTopicParam = IotWebConfTextParameter("MQTT Heater Status Subscription", "mqttHeaterStatusTopic", mqttHeaterStatusTopic, STRING_LEN, "ht3/hometop/ht/hc1_Tniveau");
IotWebConfTextParameter mqttHeaterStatusValueParam = IotWebConfTextParameter("MQTT Heater Status Value", "mqttHeaterStatusValue", mqttHeaterStatusValue, STRING_LEN, "3");
IotWebConfTextParameter mqttPumpTopicParam = IotWebConfTextParameter("MQTT external pump start Subscription", "mqttPumpTopic", mqttPumpTopic, STRING_LEN, "");
IotWebConfTextParameter mqttPumpValueParam = IotWebConfTextParameter("MQTT external pump start Value", "mqttPumpValue", mqttPumpValue, STRING_LEN, "");
IotWebConfTextParameter mqttThermalDesinfectionTopicParam = IotWebConfTextParameter("MQTT thermal desinfection Subscription", "mqttThermalDesinfectionTopic", mqttThermalDesinfectionTopic, STRING_LEN, "ht3/hometop/ht/dhw_thermal_desinfection");
IotWebConfTextParameter mqttThermalDesinfectionValueParam = IotWebConfTextParameter("MQTT thermal desinfection Value", "mqttThermalDesinfectionValue", mqttThermalDesinfectionValue, STRING_LEN, "1");
IotWebConfParameterGroup ntpGroup = IotWebConfParameterGroup("ntp", "NTP configuration");
IotWebConfTextParameter ntpServerParam = IotWebConfTextParameter("NTP Server", "ntpServer", ntpServer, STRING_LEN, "de.pool.ntp.org");
IotWebConfTextParameter ntpTimezoneParam = IotWebConfTextParameter("NTP timezone", "ntpTimezone", ntpTimezone, STRING_LEN, "CET-1CEST,M3.5.0/02,M10.5.0/03");
IotWebConfParameterGroup tempGroup = IotWebConfParameterGroup("temp", "Temperature configuration");
iotwebconf::SelectTParameter<STRING_LEN> tempOutParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("tempOutParam").label("Sensor Out").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("1").build();
iotwebconf::SelectTParameter<STRING_LEN> tempRetParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("tempRetParam").label("Sensor Return").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("2").build();
iotwebconf::SelectTParameter<STRING_LEN> tempIntParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("tempIntParam").label("Sensor Internal").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("3").build();
iotwebconf::FloatTParameter tempRetDiffParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("tempRetDiffParam").label("Return Off Diff").defaultValue(10.0).step(0.5).placeholder("e.g. 23.4").build();

int mod(int x, int y)
{
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
}

// -- SECTION: Wifi Manager
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
  s += "<link rel=\"stylesheet\" href=\"https://unpkg.com/mvp.css@1.12/mvp.css\">";
  s += "<title>Warmwater Recirculation Pump</title></head><body>";
  s += "<h3>Network</h3><ul>";
  s += "<li>Hostname : ";
  s += hostname;
  s += "</li>";
  s += "</ul><h3>MQTT</h3><ul>";
  s += "<li>MQTT Server: ";
  s += mqttServer;
  s += "</li>";
  s += "</ul><h3>NTP</h3><ul>";
  s += "<li>NTP Server: ";
  s += ntpServer;
  s += "</li>";
  s += "<li>NTP Timezone: ";
  s += ntpTimezone;
  s += "</li>";
  s += "<li>actual local time: ";
  strftime(tempStr, 40, "%d.%m.%Y %T", &localTime);
  s += String(tempStr) + "</li>";
  s += "</ul><h3>Sensors</h3><ul>";
  s += "<li>Sensor Out: ";
  s += tempOutParam.value() + 1;
  dtostrf(tempOut, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "<li>Sensor Return: ";
  s += tempRetParam.value() + 1;
  dtostrf(tempRet, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "<li>Sensor Internal: ";
  s += tempIntParam.value() + 1;
  dtostrf(tempInt, 2, 2, tempStr);
  s += " / " + String(tempStr) + "&#8451;";
  s += "<li>Return Temperature Difference (Off Trigger): ";
  s += tempRetDiffParam.value();
  s += "&#8451;</li>";
  s += "<li>Pump: ";
  if (mqttHeaterStatus)
  {
    if (pumpRunning)
      s += "running";
    else
      s += "stopped";
  }
  else
    s += "heater off";
  s += "</li></ul>";
  s += "<p><h3>" + String(nils_length(pump)) + " Last pump actions</h3>";
  for (int i = 0; i < nils_length(pump); i++)
  { // display last 5 pumpOn Events in right order
    byte arrIndex = mod((((int)pumpCnt) - i), nils_length(pump));
    s += String(i + 1) + ": " + pump[arrIndex] + "<br>";
  }
  uptime::calculateUptime();
  sprintf(tempStr, "%03u Tage %02u:%02u:%02u", uptime::getDays(), uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  s += "<p>Uptime: " + String(tempStr);
  s += "<p>Go to <a href='config'>Configuration</a>";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void configSaved()
{
  Serial.println("Configuration saved.");
  // TODO: Neustart bei normalen Parametern vermeiden
  needReset = true;
}

bool formValidator(iotwebconf::WebRequestWrapper *webRequestWrapper)
{
  Serial.println("Validating form.");
  bool valid = true;

  int l = webRequestWrapper->arg(mqttServerParam.getId()).length();
  if (l < 3)
  {
    mqttServerParam.errorMessage = "Please enter at least 3 chars!";
    valid = false;
  }

  return valid;
}

//-- SECTION: connection handling
void setTimezone(String timezone)
{
  Serial.printf("  Setting Timezone to %s\n", ntpTimezone);
  setenv("TZ", ntpTimezone, 1); //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}

void connectToMqtt()
{
  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}

void onWifiConnected()
{
  Serial.println("Connected to Wi-Fi.");
  Serial.println(WiFi.localIP());
  connectToMqtt();
  timeClient.begin();
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.println("Disconnected from Wi-Fi.");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  timeClient.end();
}

void onMqttConnect(bool sessionPresent)
{
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
  uint16_t packetIdSub;
  if (strlen(mqttHeaterStatusTopic) > 0)
  {

    packetIdSub = mqttClient.subscribe(mqttHeaterStatusTopic, 2);
    Serial.print("Subscribed to topic: ");
    Serial.println(String(mqttHeaterStatusTopic) + " - " + String(packetIdSub));
  }
  if (strlen(mqttPumpTopic) > 0)
  {
    Serial.println(" -" + String(mqttPumpTopic) + "- ");
    // Serial.println(HEX(mqttPumpTopic));
    packetIdSub = mqttClient.subscribe(mqttPumpTopic, 2);
    Serial.print("Subscribed to topic: ");
    Serial.println(String(mqttPumpTopic) + " - " + String(packetIdSub));
  }
  digitalWrite(LED_BUILTIN, HIGH);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  String text;
  switch (reason)
  {
  case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:
    text = "TCP_DISCONNECTED";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
    text = "MQTT_UNACCEPTABLE_PROTOCOL_VERSION";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:
    text = "MQTT_IDENTIFIER_REJECTED";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:
    text = "MQTT_SERVER_UNAVAILABLE";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:
    text = "MQTT_MALFORMED_CREDENTIALS";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:
    text = "MQTT_NOT_AUTHORIZED";
    break;
  }
  Serial.printf(" [%8u] Disconnected from the broker reason = %s\n", millis(), text.c_str());
  Serial.printf(" [%8u] Reconnecting to MQTT..\n", millis());
  digitalWrite(LED_BUILTIN, LOW);

  if (WiFi.isConnected())
  {
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
    Serial.println(new_payload);
    mqttPump = (strcmp(mqttPumpValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttThermalDesinfectionTopic) == 0)
  {
    Serial.print("mqtt thermal desinfection: ");
    Serial.println(new_payload);
    mqttPump = (strcmp(mqttThermalDesinfectionValue, new_payload) == 0);
  }
  else if (strcmp(topic, mqttHeaterStatusTopic) == 0)
  {
    Serial.print("mqtt heater status: ");
    Serial.println(new_payload);
    mqttHeaterStatus = (strcmp(mqttHeaterStatusValue, new_payload) == 0);
  }
}
//-- END SECTION: connection handling

void checkSensors()
{
  if (sensorDetectionError)
  {
    mqttClient.publish(MQTT_PUB_INFO, 0, true, "Sensorfehler");
    sensorError = true;
  }
  else
  {
    if (sensors.isConnected(sensorInt_id) && sensors.isConnected(sensorInt_id) && sensors.isConnected(sensorInt_id))
    {
      if (sensorError)
        mqttClient.publish(MQTT_PUB_INFO, 0, true, "Sensorfehler behoben");
      sensorError = false;
      sensors.setResolution(sensorOut_id, 12); // hohe Genauigkeit
      sensors.setResolution(sensorRet_id, 12); // hohe Genauigkeit
    }
    else
    {
      mqttClient.publish(MQTT_PUB_INFO, 0, true, "Sensorfehler");
      sensorError = true;
    }
  }
}

void getTemp()
{
  sensors.requestTemperatures(); // Send the command to get temperatures
  tempOut = sensors.getTempC(sensorOut_id);
  tempRet = sensors.getTempC(sensorRet_id);
  tempInt = sensors.getTempC(sensorInt_id);
  // Serial.print(" Temp Out: ");
  // Serial.println(tempOut);
  // Serial.print(" Temp In: ");
  // Serial.println(tempRet);
  // Serial.print(" Temp Int: ");
  // Serial.println(tempInt);
}

void mqttSendtemp()
{
  char msg_out[20];
  dtostrf(tempOut, 2, 2, msg_out);
  mqttClient.publish(MQTT_PUB_TEMP_OUT, 0, true, msg_out);
  dtostrf(tempRet, 2, 2, msg_out);
  mqttClient.publish(MQTT_PUB_TEMP_RET, 0, true, msg_out);
  dtostrf(tempInt, 2, 2, msg_out);
  mqttClient.publish(MQTT_PUB_TEMP_INT, 0, true, msg_out);
}

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

void publishUptime()
{
  char msg_out[20];
  uptime::calculateUptime();
  sprintf(msg_out, "%03u Tage %02u:%02u:%02u", uptime::getDays(), uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  // Serial.println(msg_out);
  mqttClient.publish(MQTT_PUB_INFO, 0, true, msg_out);
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
      display.drawString(0, 0, "Booting");
      break;
    case 1:
      display.drawString(0, 0, "Setup");
      break;
    case 2:
      display.drawString(0, 0, "AP");
      break;
    case 3:
      display.drawString(0, 0, "Verbinde");
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
    if (sensorDetectionError)
    {
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 14, "Sensorfehler!");
    }
    else
    {
      if (sensors.isConnected(sensorOut_id))
      {
        dtostrf(tempOut, 2, 2, tempStr);
        display.drawString(64, 12, "Vorlauf: " + String(tempStr) + " C°");
      }
      else
        display.drawString(64, 12, "Vorlauf: ERROR!");
      if (sensors.isConnected(sensorRet_id))
      {
        dtostrf(tempRet, 2, 2, tempStr);
        display.drawString(64, 24, "Rücklauf: " + String(tempStr) + " C°");
      }
      else
        display.drawString(64, 24, "Rücklauf: ERROR!");
    }

    display.setFont(ArialMT_Plain_24);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (pumpRunning)
    {
      display.invertDisplay();
      if (pumpManual)
        display.drawString(64, 36, "Man. an");
      else
        display.drawString(64, 36, "Pumpe an");
    }
    else
    {
      display.normalDisplay();
      if (pumpManual)
        display.drawString(64, 36, "Man. aus");
      else
        display.drawString(64, 36, "Pumpe aus");
    }
    break;
  case 1:
    // Display Page 2 - last 5 pump starts
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, "Letzte Starts");
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
      display.drawString(0, lineCnt * 10 + 2, String(i + 1) + ": " + pump[arrIndex]);
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
    display.drawString(64, 0, "Laufzeit");
    display.drawLine(0, 11, 128, 11);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 18, String(uptime::getDays()) + " Tage ");
    sprintf(uptimeStr, "%02u:%02u:%02u", uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
    display.drawString(64, 38, String(uptimeStr));
    break;
  case 3:
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, "Sensoren");
    display.drawLine(0, 11, 128, 11);
    display.setFont(ArialMT_Plain_16);
    if (sensors.getAddress(sensor1_id, 0))
    {
      temp = sensors.getTempC(sensor1_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 12, "1: " + String(tempStr) + "°C");
    }
    else
      display.drawString(64, 12, "1: kein Sensor");
    if (sensors.getAddress(sensor2_id, 1))
    {
      temp = sensors.getTempC(sensor2_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 30, "2: " + String(tempStr) + "°C");
    }
    else
      display.drawString(64, 30, "2: kein Sensor");
    if (sensors.getAddress(sensor3_id, 2))
    {
      temp = sensors.getTempC(sensor3_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 48, "3: " + String(tempStr) + "°C");
    }
    else
      display.drawString(64, 48, "3: kein Sensor");
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
      display.drawString(64, 14, "1: kein Sensor");
    }
    if (sensors.getAddress(sensor2_id, 1))
    {
      printAddress(sensor2_id);
      display.drawString(64, 32, "2: " + formatAdress(sensor2_id));
    }
    else
    {
      Serial.println("Unable to find address for Sensor 2");
      display.drawString(64, 32, "2: kein Sensor");
    }
    if (sensors.getAddress(sensor3_id, 2))
    {
      printAddress(sensor3_id);
      display.drawString(64, 50, "3: " + formatAdress(sensor3_id));
    }
    else
    {
      Serial.println("Unable to find address for Sensor 3");
      display.drawString(64, 50, "3: kein Sensor");
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
      display.drawString(64, 12, "Booting");
      break;
    case 1:
      display.drawString(64, 12, "Nicht konfiguriert");
      break;
    case 2:
      display.drawString(64, 12, "AP");
      break;
    case 3:
      display.drawString(64, 12, "Verbinde...");
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
    display.drawString(64, 42, "Sendeleistung: " + String(WiFi.getTxPower()));
    if (WiFi.isConnected())
    {
      display.drawString(64, 52, WiFi.localIP().toString());
    }
    else
    {
      display.drawString(64, 52, "keine IP");
    }
    break;
  case 6:
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, "WiFi Netzwerke");
    display.drawLine(0, 11, 128, 11);
    if (iotWebConf.getState() == 4)
    {
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 18, "WiFi bereits");
      display.drawString(64, 38, "verbunden");
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
        display.drawString(64, 24, "Suche...");
        lastScan = now;
      }
      else if (networksFound == -2)
      {
        display.setFont(ArialMT_Plain_24);
        display.drawString(64, 24, "Warte...");
      }
      else if (networksFound == 0)
      {
        Serial.print("Networks found: ");
        Serial.println(networksFound);
        display.setFont(ArialMT_Plain_16);
        display.drawString(64, 24, "Keine APs");
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

void pumpOn()
{
  char tempStr[128];
  Serial.println("Turn on circulation");
  mqttClient.publish(MQTT_PUB_PUMP, 2, true, "1");
  pumpRunning = true;
  pumpStartedAt = millis();
  digitalWrite(PUMPPIN, LOW);
  // digitalWrite(VALVEPIN, HIGH);
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
  mqttClient.publish(MQTT_PUB_PUMP, 2, true, "0");
  pumpRunning = false;
  pump[pumpCnt] += " (" + String((int)round((millis() - pumpStartedAt) / 1000 / 60)) + " min.)";
  digitalWrite(PUMPPIN, HIGH);
  // digitalWrite(VALVEPIN, LOW);
}

void disinfection()
{
  pumpOn();
}

void check()
{

  if (sensorError)
  {
    // Emergeny Mode if missing sensors
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
    getTemp();
    if (!pumpManual)
    {
      float temperatur_delta = 0.0;
      bool tempRetBlock = tempRet > (tempOut - tempRetDiffParam.value());
      if (++checkCnt >= 5)
        checkCnt = 0; // Reset counter
      t[checkCnt] = tempOut;
      int cnt_alt = (checkCnt + 6) % 5;
      if (!pumpRunning)
      {
        temperatur_delta = t[checkCnt] - t[cnt_alt]; // Difference to 5 sec before
        if ((temperatur_delta >= 0.12 || mqttPump) && (300000 < millis() - pumpBlock || pumpFirstCall) && (mqttHeaterStatus || !mqttClient.connected()))
        { // smallest temp change is 0,12°C,
          Serial.print("Temperature Delta: ");
          Serial.println(temperatur_delta);
          if (mqttPump)
          {
            mqttPump = false;
            Serial.println("MQTT pump action done");
          }
          pumpBlock = millis();
          pumpFirstCall = false;
          pumpOn();
          disinfection24hTimer.attach(86400, disinfection);
        }
      }
      else if (tempRetBlock && 120000 < (millis() - pumpStartedAt))
      { // if return flow temp near temp out stop pump with a delay of 2 minutes
        pumpOff();
      }
    }
  }
}

void onSecTimer()
{
  updateTime();
  check();
}

void onSec10Timer()
{
  publishUptime();
  mqttSendtemp();
}

void setup()
{
  // basic setup
  Serial.begin(115200);
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
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  iotWebConf.setupUpdateServer(
      [](const char *updatePath)
      { httpUpdater.setup(&server, updatePath); },
      [](const char *userName, char *password)
      { httpUpdater.updateCredentials(userName, password); });

  networkGroup.addItem(&hostnameParam);
  iotWebConf.addParameterGroup(&networkGroup);
  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);
  mqttGroup.addItem(&mqttHeaterStatusTopicParam);
  mqttGroup.addItem(&mqttHeaterStatusValueParam);
  mqttGroup.addItem(&mqttPumpTopicParam);
  mqttGroup.addItem(&mqttPumpValueParam);
  mqttGroup.addItem(&mqttThermalDesinfectionTopicParam);
  mqttGroup.addItem(&mqttThermalDesinfectionValueParam);
  iotWebConf.addParameterGroup(&mqttGroup);
  ntpGroup.addItem(&ntpServerParam);
  ntpGroup.addItem(&ntpTimezoneParam);
  iotWebConf.addParameterGroup(&ntpGroup);
  tempGroup.addItem(&tempOutParam);
  tempGroup.addItem(&tempRetParam);
  tempGroup.addItem(&tempIntParam);
  tempGroup.addItem(&tempRetDiffParam);
  iotWebConf.addParameterGroup(&tempGroup);

  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&onWifiConnected);
  iotWebConf.setConfigPin(WIFICONFIGPIN);

  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    //TODO: diese 3 Werte in einer separaten Stelle im EEPROM abspeichern.
    // strncpy(iotWebConf.getApPasswordParameter()->valueBuffer, "---", iotWebConf.getApPasswordParameter()->getLength());
    // strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer, "---", iotWebConf.getWifiSsidParameter()->getLength());
    // strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer, "---", iotWebConf.getWifiPasswordParameter()->getLength());
    // iotWebConf.saveConfig();
    // needReset = true;
  }

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []
            { iotWebConf.handleConfig(); });
  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });
  Serial.println("Wifi manager ready.");

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onSubscribe(onMqttSubscribe);

  if (mqttUser != "")
    mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, MQTT_PORT);

  sensors.begin();
  // locate devices on the bus
  Serial.print("Locating devices...");
  Serial.print("Found ");
  Serial.print(sensors.getDeviceCount(), DEC);
  Serial.println(" devices.");
  if (!sensors.getAddress(sensorOut_id, atol(tempOutParam.value())))
    sensorDetectionError = true;
  if (!sensors.getAddress(sensorRet_id, atol(tempRetParam.value())))
    sensorDetectionError = true;
  if (!sensors.getAddress(sensorInt_id, atol(tempIntParam.value())))
    sensorDetectionError = true;
  checkSensors();

  // configure the timezone
  configTime(0, 0, ntpServer);
  setTimezone(ntpTimezone);

  // Init OTA function
  ArduinoOTA.onStart([]()
                     {
    Serial.println("Start OTA");
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
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.println(WiFi.localIP());

  // Timers
  disinfection24hTimer.attach(86400, disinfection);
  secTimer.attach(1, onSecTimer);
  sec10Timer.attach(10, onSec10Timer);
  displayTimer.attach_ms(500, updateDisplay);
}

void loop()
{
  iotWebConf.doLoop();
  ArduinoOTA.handle();

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