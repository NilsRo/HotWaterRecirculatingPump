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

// ports
const int ONEWIREPIN = D8;
const int PUMPPIN = D3;
const int VALVEPIN = D4;
const int DISPLAYPIN = D5;
const int WIFICONFIGPIN = D7;

String pump[5] = {"", "", "", "", ""};
unsigned int pumpCnt = 0;
static float t[] = {0.0, 0.0, 0.0, 0.0, 0.0}; // letzten 5 Temepraturwerte speichern
bool pumpRunning = false;
bool pumpManual = false;
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
bool networksPageFirstCall = true;
int displayPinState = HIGH;
unsigned long displayLastChanged = 0;
bool displayOn = true;

char mqttServer[STRING_LEN];
char mqttUser[STRING_LEN];
char mqttPassword[STRING_LEN];
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

Ticker mqttReconnectTimer;
Ticker disinfection24hTimer;
Ticker secTimer;
Ticker displayTimer;
Ticker publishUptimeTimer;


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
int networksLineStart = 0;
int networksLineEnd = 0;
unsigned int networksPageTotal = 0;
unsigned long networksPageChange = 0;

#define CONFIG_VERSION "1"
int iotWebConfPinState = HIGH;
unsigned long iotWebConfLastChanged = 0;
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
IotWebConfParameterGroup ntpGroup = IotWebConfParameterGroup("ntp", "NTP configuration");
IotWebConfTextParameter ntpServerParam = IotWebConfTextParameter("NTP Server", "ntpServer", ntpServer, STRING_LEN, "at.pool.ntp.org");
IotWebConfTextParameter ntpTimezoneParam = IotWebConfTextParameter("NTP timezone", "ntpTimezone", ntpTimezone, STRING_LEN, "CET-1CEST,M3.5.0/02,M10.5.0/03");
IotWebConfParameterGroup tempGroup = IotWebConfParameterGroup("temp", "Temperature configuration");
iotwebconf::SelectTParameter<STRING_LEN> tempOutParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("tempOutParam").label("Sensor Out").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("1").build();
iotwebconf::SelectTParameter<STRING_LEN> tempRetParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("tempRetParam").label("Sensor Return").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("2").build();
iotwebconf::SelectTParameter<STRING_LEN> tempIntParam =
    iotwebconf::Builder<iotwebconf::SelectTParameter<STRING_LEN>>("tempIntParam").label("Sensor Internal").optionValues((const char *)chooserValues).optionNames((const char *)chooserNames).optionCount(sizeof(chooserValues) / STRING_LEN).nameLength(STRING_LEN).defaultValue("3").build();
iotwebconf::FloatTParameter tempRetDiffParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("tempRetDiffParam").label("Return Off Diff").defaultValue(20.0).step(0.5).placeholder("e.g. 23.4").build();

// -- SECTION: Wifi Manager
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>Warmwater Recirculation Pump</title></head><body>";
  s += "Network<ul>";
  s += "<li>Hostname : ";
  s += hostname;
  s += "</li>";
  s += "</ul>MQTT<ul>";
  s += "<li>MQTT Server: ";
  s += mqttServer;
  s += "</li>";
  s += "</ul>NTP<ul>";
  s += "<li>NTP Server: ";
  s += ntpServer;
  s += "</li>";
  s += "<li>NTP Timezone: ";
  s += ntpTimezone;
  s += "</li>";
  s += "</ul>Sensors<ul>";
  s += "<li>Sensor Out: ";
  s += tempOutParam.value();
  s += "<li>Sensor Return: ";
  s += tempRetParam.value();
  s += "<li>Sensor Internal: ";
  s += tempIntParam.value();
  s += "<li>Return Temperature Difference (Off Trigger): ";
  s += tempRetDiffParam.value();
  s += "</li>";
  s += "Go to <a href='config'>Configuration</a>";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void configSaved()
{
  Serial.println("Configuration saved.");
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
  digitalWrite(LED_BUILTIN, HIGH);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  Serial.println("Disconnected from MQTT.");
  digitalWrite(LED_BUILTIN, LOW);

  if (WiFi.isConnected())
  {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void onMqttPublish(uint16_t packetId)
{
  // Serial.print("Publish acknowledged.");
  // Serial.print("  packetId: ");
  // Serial.println(packetId);
}
//-- END SECTION: connection handling
void checkSensors()
{
  if (sensorDetectionError)
  {
    if (mqttClient.connected()) mqttClient.publish(MQTT_PUB_INFO, 0, true, "Sensorfehler");
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
      if (mqttClient.connected()) mqttClient.publish(MQTT_PUB_INFO, 0, true, "Sensorfehler");
      sensorError = true;
    }
  }
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

void publishUptime()
{
  char msg_out[20];
  uptime::calculateUptime();
  sprintf(msg_out, "%03u Tage %02u:%02u:%02u", uptime::getDays(), uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  Serial.println(msg_out);
  if (mqttClient.connected()) mqttClient.publish(MQTT_PUB_INFO, 0, true, msg_out);
}

void updateDisplay()
{
  char tempStr[128];
  unsigned int lineCnt = 1;
  char uptimeStr[8];
  float temp;
  DeviceAddress sensor1_id;
  DeviceAddress sensor2_id;
  DeviceAddress sensor3_id;
  unsigned long now = millis();

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
    for (unsigned int cnt = 0; cnt++; cnt <= 4)
    { // display last 5 pumpOn Events in right order
      display.drawString(64, lineCnt * 10 + 2, pump[(pumpCnt + cnt) % 5]);
    }
    break;
  case 2:
    uptime::calculateUptime();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 0, "Laufzeit");
    display.drawLine(0, 11, 128, 11);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 24, String(uptime::getDays()) + " Tage ");
    sprintf(uptimeStr, "%02u:%02u:%02u", uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
    display.drawString(64, 44, String(uptimeStr));
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
      display.drawString(64, 12, "1: " + String(tempStr) + "C°");
    }
    else
      display.drawString(64, 12, "1: kein Sensor");
    if (sensors.getAddress(sensor2_id, 1))
    {
      temp = sensors.getTempC(sensor2_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 30, "2: " + String(tempStr) + "C°");
    }
    else
      display.drawString(64, 30, "2: kein Sensor");
    if (sensors.getAddress(sensor3_id, 2))
    {
      temp = sensors.getTempC(sensor3_id);
      dtostrf(temp, 2, 2, tempStr);
      display.drawString(64, 48, "3: " + String(tempStr) + "C°");
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
      display.drawString(64, 12, "WiFi bereits");
      display.drawString(64, 30, "verbunden");
    }
    else
    {      
      // WiFi.scanNetworks will return the number of networks found
      if (WiFi.getMode() != WIFI_STA)
      {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
      }      
      if ((60000 < now - lastScan) && WiFi.getMode() == WIFI_STA)  //blocked for 60s
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
          networksPageChange = now;
          networksPageTotal = ceil(networksFound / 5.0);
          lastScan = now;
        }
        networksLineStart = ((networksPage) - 1) * 5;
        networksLineEnd = min(networksPage * 5, networksFound);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(127, 0, "(" + String(networksPage) + "/" + String(networksPageTotal) + ")");

        for (int i = networksLineStart; i < networksLineEnd; i++)
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
          display.drawString(0, 1 + (10 * lineCnt), String(i + 1) + ": " +String(WiFi.SSID(i)) + " (" + String(WiFi.RSSI(i)) + ")");
          lineCnt++;
        }
        if ((10000 < now - networksPageChange) && networksPageTotal > 1)
        {
          if (networksPage < networksPageTotal)
            networksPage++;
          else
            networksPage = 1;
          networksPageChange = now;
        }
      }
    }
  }
  display.display();
}

void pumpOn()
{
  Serial.println("Turn on circulation");
  if (mqttClient.connected()) mqttClient.publish(MQTT_PUB_PUMP, 0, true, "1");
  pumpRunning = true;
  digitalWrite(PUMPPIN, LOW);
  // digitalWrite(VALVEPIN, HIGH);

  timeClient.update();
  Serial.println(timeClient.getFormattedTime());
  pump[pumpCnt] = timeClient.getFormattedTime();
  if (++pumpCnt >= 5)
    pumpCnt = 0; // Reset counter
}

void pumpOff()
{
  Serial.println("Turn off circulation");
  if (mqttClient.connected()) mqttClient.publish(MQTT_PUB_PUMP, 0, true, "0");
  pumpRunning = false;
  digitalWrite(PUMPPIN, HIGH);
  // digitalWrite(VALVEPIN, LOW);
}

void disinfection()
{
  pumpOn();
}

void check()
{
  char msg_out[20];
  if (iotWebConf.getState() == 4)
  {
    timeClient.update();
    getLocalTime();
  }

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
    sensors.requestTemperatures(); // Send the command to get temperatures
    tempOut = sensors.getTempC(sensorOut_id);
    tempRet = sensors.getTempC(sensorRet_id);
    tempInt = sensors.getTempC(sensorInt_id);
    Serial.print(" Temp Out: ");
    Serial.println(tempOut);
    Serial.print(" Temp In: ");
    Serial.println(tempRet);
    Serial.print(" Temp Int: ");
    Serial.println(tempInt);    
    if (mqttClient.connected())
    {
      dtostrf(tempOut, 2, 2, msg_out);    
      mqttClient.publish(MQTT_PUB_TEMP_OUT, 0, true, msg_out);
      dtostrf(tempRet, 2, 2, msg_out);
      mqttClient.publish(MQTT_PUB_TEMP_RET, 0, true, msg_out);
      dtostrf(tempInt, 2, 2, msg_out);
      mqttClient.publish(MQTT_PUB_TEMP_INT, 0, true, msg_out);
    }

    if (!pumpManual)
    {
      float temperatur_delta = 0.0;
      bool tempRetBlock = tempRet > (tempOut - tempRetDiffParam.value());
      if (++checkCnt >= 5)
        checkCnt = 0; // Reset counter
      t[checkCnt] = tempOut;
      int cnt_alt = (checkCnt + 6) % 5;
      if (!pumpRunning && tempRet < tempOut - 10.0)
      {                                              // start only if retern flow temp is 10 degree below temp out (hystersis)
        temperatur_delta = t[checkCnt] - t[cnt_alt]; // Difference to 5 sec before
        if (temperatur_delta >= 0.12)
        { // smallest temp change is 0,12°C,
          Serial.print("Temperature Delta: ");
          Serial.println(temperatur_delta);
          pumpOn();
          disinfection24hTimer.attach(86400, disinfection);
        }
      }
      else
      {
        if (pumpRunning && tempRetBlock)
        { // if return flow temp near temp out stop pump with a delay
          pumpOff();
        }
      }
    }
  }
}

void onSecTimer()
{
  check();
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
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(hostname);
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
  iotWebConf.init();
  // -- Set up required URL handlers on the web server.
  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServer[0] = '\0';
    mqttUser[0] = '\0';
    mqttPassword[0] = '\0';
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
  publishUptimeTimer.attach(10, publishUptime);
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
  if ((500 < now - iotWebConfLastChanged) && (iotWebConfPinState != digitalRead(WIFICONFIGPIN)))
  {
    iotWebConfPinState = 1 - iotWebConfPinState; // invert pin state as it is changed
    iotWebConfLastChanged = now;
    if (iotWebConfPinState)
    { // reset settings and reboot
      iotWebConf.getRootParameterGroup()->applyDefaultValue();
      iotWebConf.saveConfig();
      needReset = true;
    }
  }
  if ((500 < now - displayLastChanged) && (displayPinState != digitalRead(DISPLAYPIN)))
  {
    displayPinState = 1 - displayPinState; // invert pin state as it is changed
    displayLastChanged = now;    
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
            displayOn = true;
          }
          else
          {
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
      timePressed = millis();
      Serial.println("Button pressed"); 
    }
  }
  if (displayLastChanged > 600000)
  { // switch display off after 10mins
    display.displayOff();
    displayOn = false;
  }
}