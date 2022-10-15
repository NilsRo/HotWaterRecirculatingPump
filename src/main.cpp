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
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>


#define STRING_LEN 128

//ports
const int ONEWIREPIN = D8;
const int PUMPPIN = D3;
const int VALVEPIN = D4;
const int DISPLAYPIN = D5;
const int LEDPIN = D6;
const int WIFICONFIGPIN = D7;

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(ONEWIREPIN);

// Pass our oneWire reference to Dallas Temperature sensor
DallasTemperature sensors(&oneWire);

DeviceAddress sensor0_id;
DeviceAddress sensor1_id;

//OLED Display
SSD1306Wire display(0x3c, SDA, SCL);   // ADDRESS, SDA, SCL  -  SDA and SCL usually populate automatically based on your board's pins_arduino.h e.g. https://github.com/esp8266/Arduino/blob/master/variants/nodemcu/pins_arduino.h
unsigned int displayPage = 0;
int displayPinState = HIGH;
unsigned long displayLastChanged = 0;


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
#define MQTT_PUB_INFO "ww/ht/dhw_info"
AsyncMqttClient mqttClient;

Ticker mqttReconnectTimer;
Ticker disinfection24hTimer;
Ticker checkTimer;
Ticker updateDisplayTimer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
char ntpServer[STRING_LEN] = "at.pool.ntp.org";
char ntpTimezone[STRING_LEN] =  "CET-1CEST,M3.5.0/02,M10.5.0/03";
char hostname[STRING_LEN] =  "ww_pump";
time_t now;

String pump[5] = {"", "", "", "", ""};
unsigned int pumpCnt = 0;

static float t[] = {0.0,  0.0,  0.0,  0.0,  0.0}; //letzten 5 Temepraturwerte speichern
bool pumpRunning = false;
float tempOut;
float tempRet;
float tempInt;
unsigned int checkCnt;
IPAddress localIP;

#define CONFIG_VERSION "1"
int iotWebConfPinState = HIGH;
unsigned long iotWebConfLastChanged = 0;
DNSServer dnsServer;
WebServer server(80);
IotWebConf iotWebConf("Zirkulationspumpe", &dnsServer, &server, "");
IotWebConfParameterGroup networkGroup = IotWebConfParameterGroup("network", "Network configuration");
IotWebConfTextParameter hostnameParam = IotWebConfTextParameter("Hostname", "hostname", hostname, STRING_LEN);
IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("mqtt", "MQTT configuration");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("MQTT server", "mqttServer", mqttServer, STRING_LEN);
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("MQTT user", "mqttUser", mqttUser, STRING_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("MQTT password", "mqttPassword", mqttPassword, STRING_LEN);
IotWebConfParameterGroup ntpGroup = IotWebConfParameterGroup("ntp", "NTP configuration");
IotWebConfTextParameter ntpServerParam = IotWebConfPasswordParameter("NTP Server", "ntpServer", ntpServer, STRING_LEN);
IotWebConfTextParameter ntpTimezoneParam = IotWebConfPasswordParameter("NTP timezone", "ntpTimezone", ntpTimezone, STRING_LEN);


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
  s += "</ul>";
  s += "Go to <a href='config'>Configuration</a>";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void configSaved()
{
  Serial.println("Configuration saved.");
  needReset = true;
}

bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper)
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
void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}

void onWifiConnected() {
  Serial.println("Connected to Wi-Fi.");
  Serial.println(WiFi.localIP());
  connectToMqtt();
  timeClient.begin();
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("Disconnected from Wi-Fi.");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  timeClient.end();
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
  digitalWrite(LED_BUILTIN, HIGH);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
  digitalWrite(LED_BUILTIN, LOW);

  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void onMqttPublish(uint16_t packetId) {
  //Serial.print("Publish acknowledged.");
  //Serial.print("  packetId: ");
  //Serial.println(packetId);
}
//-- END SECTION: connection handling


void updateDisplay() {
  char tempOutStr[5];
  char tempRetStr[5];

  display.clear();
  if (displayPage = 0) {
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (WiFi.isConnected()) {
      display.drawString(0, 0, String(WiFi.localIP()));
    } else {
      display.drawString(0, 0, "nicht verbunden");
    }

    dtostrf(tempOut, 2, 2, tempOutStr);
    dtostrf(tempRet, 2, 2, tempRetStr);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 12, "Vorlauf: ");
    display.drawString(64, 12, String(tempOutStr));
    display.drawString(0, 24, "Rücklauf: ");
    display.drawString(64, 24, String(tempRetStr));

    display.setFont(ArialMT_Plain_24);
    display.drawString(0, 36, "Pumpe: ");
    if (pumpRunning) {
      display.invertDisplay();
      display.drawString(64, 36, "an");
    } else {
      display.normalDisplay();
      display.drawString(64, 36, "aus");
    }
  } else {  // Display Page 2 - last 5 pump starts
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    for(unsigned int cnt = pumpCnt;cnt++;cnt<=4 + pumpCnt) { //display last 5 pumpOn Events in right order
      unsigned int pumpCntTemp;
      if (cnt > 4) pumpCntTemp = cnt - 5; else pumpCntTemp = cnt;
      display.println(pump[pumpCntTemp]);
    }
  }
  display.display();
}

void pumpOn() {
  Serial.println("Turn on circulation");
  mqttClient.publish(MQTT_PUB_PUMP, 0, true, "1");
  pumpRunning = true;
  digitalWrite(PUMPPIN, HIGH);
  digitalWrite(VALVEPIN, HIGH);

  timeClient.update();
  Serial.println(timeClient.getFormattedTime());
  pump[pumpCnt] = timeClient.getFormattedTime();
  if(++pumpCnt >= 5) pumpCnt = 0;   //Reset counter
}

void pumpOff() {
  Serial.println("Turn off circulation");
  mqttClient.publish(MQTT_PUB_PUMP, 0, true, "0");
  pumpRunning = false;
  digitalWrite(PUMPPIN, LOW);
  digitalWrite(VALVEPIN, LOW);
}

void disinfection() {
  pumpOn();
}

void check() {
  char msg_out[20];
  sensors.requestTemperatures(); // Send the command to get temperatures
  tempOut = sensors.getTempCByIndex(0);
  tempRet = sensors.getTempCByIndex(1);
  tempInt = sensors.getTempCByIndex(2);
  Serial.print(" Temp Out: "); Serial.println(tempOut);
  Serial.print(" Temp In: "); Serial.println(tempRet);
  Serial.print(" Temp Int: "); Serial.println(tempInt);
  dtostrf(tempOut, 2, 2, msg_out);
  mqttClient.publish(MQTT_PUB_TEMP_OUT, 0, true, msg_out);
  dtostrf(tempRet, 2, 2, msg_out);
  mqttClient.publish(MQTT_PUB_TEMP_RET, 0, true, msg_out);
  dtostrf(tempInt, 2, 2, msg_out);
  mqttClient.publish(MQTT_PUB_TEMP_INT, 0, true, msg_out);

  float temperatur_delta = 0.0;
  if(++checkCnt >= 5) checkCnt = 0;   //Reset counter
  t[checkCnt] = tempOut;
  int cnt_alt = (checkCnt + 6) % 5;
  if(!pumpRunning && tempRet < tempOut - 10.0) {  //start only if retern flow temp is 10 degree below temp out (hystersis)
    temperatur_delta = t[checkCnt]-t[cnt_alt]; // Difference to 5 sec before
    if(temperatur_delta >= 0.12) {        // smallest temp change is 0,12°C,
      pumpOn();
      disinfection24hTimer.attach(86400, disinfection);
    }
  } else {
    if (tempRet > tempOut - 5.0) { //if return flow temp near temp out stop pump with a delay
      pumpOff();
    }
  }
}

void setup() {
  //basic setup
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PUMPPIN, OUTPUT);
  pinMode(VALVEPIN, OUTPUT);
  pinMode(LEDPIN, OUTPUT);
  digitalWrite(PUMPPIN, LOW);
  digitalWrite(VALVEPIN, LOW);
  digitalWrite(LEDPIN, LOW);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(DISPLAYPIN, INPUT_PULLUP);
  pinMode(WIFICONFIGPIN, INPUT_PULLUP);

  // configure the timezone
  configTime(0, 0, ntpServer);
  setenv("TZ", ntpTimezone, 1); // Set environment variable with your time zone
  tzset();

  // WiFi.onEvent(onWifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(hostname);
  networkGroup.addItem(&hostnameParam);
  iotWebConf.addParameterGroup(&networkGroup);
  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);
  iotWebConf.addParameterGroup(&mqttGroup);
  ntpGroup.addItem(&ntpServerParam);
  ntpGroup.addItem(&ntpTimezoneParam);
  iotWebConf.addParameterGroup(&ntpGroup);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&onWifiConnected);
  iotWebConf.setStatusPin(LEDPIN);
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
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });
  Serial.println("Wifi manager ready.");

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onPublish(onMqttPublish);
  if (mqttUser != "") mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, MQTT_PORT);

  display.init();
  display.setFont(ArialMT_Plain_10);

  sensors.begin();
  Serial.print("Found ");
  Serial.print(sensors.getDeviceCount(), DEC);
  Serial.println(" devices.");
  sensors.getAddress(sensor0_id, 0);
  sensors.getAddress(sensor1_id, 1);
  sensors.setResolution(sensor0_id, 12); //hohe Genauigkeit
  sensors.setResolution(sensor1_id, 12); //hohe Genauigkeit

  // Init OTA function
  ArduinoOTA.onStart([]() {
    Serial.println("Start OTA");
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2 - 10, "OTA Update");
    display.display();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
    display.drawProgressBar(4, 32, 120, 8, progress / (total / 100) );
    display.display();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd OTA");
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "Restart");
    display.display();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    display.clear();
    display.setFont(ArialMT_Plain_24);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "OTA Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.println(WiFi.localIP());

  // Timers
  disinfection24hTimer.attach(86400, disinfection);
  checkTimer.attach(1, check);
  updateDisplayTimer.attach(1, updateDisplay);
}

void loop() {
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
    if (iotWebConfPinState) {  //reset settings and reboot
      iotWebConf.resetWifiAuthInfo();
      needReset = true;
    }
  }
  if ((500 < now - displayLastChanged) && (displayPinState != digitalRead(DISPLAYPIN)))
  {
    displayPinState = 1 - displayPinState; // invert pin state as it is changed
    displayLastChanged = now;
    if (displayPinState) {
      if (displayPage == 0) displayPage = 1; else displayPage = 0;
    }
  }
}