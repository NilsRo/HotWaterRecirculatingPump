# Recirculation pump automated
How to optimize your water heating with an ESP32.

The project follows the ideas of https://github.com/MakeMagazinDE/Zirkulationspumpensteuerung and http://www.kabza.de/MyHome/CircPump/CirculationPump.php to control the hot water recirculation pump (Warmwasserzirkulationspumpe) via two temperature sensors comparing the warm water flow and return temperature. This saves electricity and thermal energy as the circulation will by done on request and not countinuously.

![diagram](docs/diagram.drawio.png)

In the HT3 warm water diagram you see the period without warm water taking is flattend so the heater does heatup fewer.

![warm water log](img/HT3_Warmwasser.png)

## Featurelist
* Managing the pump via flow and return temperature
* MQTT publishing to get logging information
* MQTT subscription for your smart home and to use heater information like provided by https://github.com/norberts1/hometop_HT3
* Realtime clock for desinfection and logging
* SSD1306 OLED to view status information
* Guided setup to avoid storing WiFi password, etc. in the code
* 2 channel relay to add e.g. a magnetic vent later

## [Hardware](docs/schema.pdf)
* Wemos D1 Mini ESP32
* Standard Arduino/Raspberry 2-channel relay with optokoppler
* [BOM](docs/HotWaterRecirculatingPump.csv)
* STL for [case](docs/Warmwasserpumpe.stl) and [cover](docs/Warmwasserpumpe(2).stl)

![Case](img/SpaceClaim_2022-10-28%20163143.png)
![Case opened](img/SpaceClaim_2022-10-28%20163208.png)
![Case with components mounted](img/Case%20with%20components.JPG)

## Mounting
The flow sensor have to be mounted 50cm away from the hot water reserviour. The return sensor near the end of the return pipe but not side by side to the reserviour. 

# Software
On first start the thing will open an Access Point named "Zirkulationspumpe" to provide the setup interface. The interface is still available after the device is connected so you can change everything later. But at first the firmware has to be uploaded. You can compile your own version or use the firmware provided in the releases.

1. Do the system configuration and set things name (hostname), AP password (if WiFi connection is lost) and WiFi credentials for your network.
2. MQTT configuration (optional)
   1. publish the following topics (folder structure can be changed):
      * "ww/ht/dhw_Tflow_measured": out temperature of the warm water
      * "ww/ht/dhw_Treturn": return temperature of the warm water circulation
      * "ww/ht/Tint": system internal temperature
      * "ww/ht/dhw_pump_circulation": pump is running or not
      * "ww/ht/status": availability of the system is connected to mqtt broker (Online) or not (Offline)
      * "ww/ht/sysinfo": status of the system showing the actual mode as Json
        * mode 
          * auto - automatic mode
          * manual - control the pump via the button. The 24h timer to clean the pipes is still running.
        * state
          * heater on - temperature is watched and pump is starting automatically or by MQTT triggers
          * heater off - pump can be started by MQTT triggers
          * desinfection - Desinfection is running, all triggers are blocked until the desinfection is finished by MQTT trigger
          * emergency - some sort of problem detected like with DS18B20. The pump will start by a timer regularly.
      * "ww/ht/info": system information as text
   2. subscribe to heater topics
      * heater status topic (e.g. ht3/hometop/ht/hc1_Tniveau): if the value is matched the system switches to automatic mode. If it is not matched the pump is off and reacts on desinfection and manual starts only.
      * external pump start topic (e.g. ht3/hometop/ht/dhw_pump_circulation):  The event is triggered if the message contains the value set in the configuration. The pump will swtched off if the desired return temperature is reached. Could be used to have a dump switch with HT3 and avoids buying a second IPM module for water pump (automatic mode should be disabled) or much better you can trigger the pump from your smart home or something more advanced.
      * thermal desinfection topic (e.g. ht3/hometop/ht/dhw_thermal_desinfection): if the value is matched the pump will switch on infinitly until another value is received on this topic. This can be used for desinfection topics. If desinfection is switched off the pump will still run until the necessary temperature is reached on the return pipe to guarantee the desinfection of the whole pipe.
3. NTP configuration to get RTC infos for logging (default is fine for german timezone)
4. Temperature configuration to map DS18B20 sensors detected
5. Define your return temperature to switch off when the water is gone through the whole circulation pipe.
6. button short press to swtich screen, button long press for testing the relay/pump.

![config page](img/opera_2022-10-31%20213941.png)

## OLED infos
<img src="img/Displaypage1.JPG"  width="30%" height="30%">
<img src="img/Displaypage2.JPG"  width="30%" height="30%">
<img src="img/Displaypage3.JPG"  width="30%" height="30%">
<img src="img/Displaypage4.JPG"  width="30%" height="30%">
<img src="img/Displaypage5.JPG"  width="30%" height="30%">
<img src="img/Displaypage6.JPG"  width="30%" height="30%">
<img src="img/Displaypage7.JPG"  width="30%" height="30%">

# Tips
* Increase the pumps flow to the maximum that the water goes through the pipes as fast as possible.
* Setup special schedules via a smart home (MQTT) if you like to have warm water on request (e.g. by phone) or at a specific time.
* second relay channel is actually unused - suggest how to use it if you need