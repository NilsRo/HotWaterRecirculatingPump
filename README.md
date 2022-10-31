# HotWaterRecirculatingPump
Automation of hot water recirculating pump via ESP32

The project (actually with german LCD GUI) follows the ideas of https://github.com/MakeMagazinDE/Zirkulationspumpensteuerung and http://www.kabza.de/MyHome/CircPump/CirculationPump.php to control the hot water recirculation pump (Warmwasserzirkulationspumpe) via two temperature sensors comparing the warm water out and return temperature.

Featurelist
* Control the pump via out and return temperature sensor
* MQTT publishing to get logging information
* MQTT subscription to get heater information like provided by https://github.com/norberts1/hometop_HT3
* Realtime Clock support to maintain sleep ciycles and also cleaning everything every 24h
* 2 relay support to add e.g. a magnetic vent
* SSD1306 support to view status information
* AP Setup support to avoid storing password, etc. hardcoded

[Hardware](docs/schema.pdf)
* Wemos D1 Mini ESP32
* Standard Arduino/Raspberry 2-Channel Relay with Optokoppler
* see [BOM](docs/HotWaterRecirculatingPump.csv)
* STL for the [case](docs/Warmwasserpumpe.stl) and [cover](docs/Warmwasserpumpe(2).stl)

![Case](img/SpaceClaim_2022-10-28%20163143.png)
![Open Case](img/SpaceClaim_2022-10-28%20163208.png)
![Case with components mounted](img/Case%20with%20components.JPG)

# Software
To setup the device it will open an Access Point named "Zirkulationspumpe" to provide an configuration interface. It is still available after the device is connected to an WiFi Network so you can do the 1-Wire configuration later. You have to mount the sensor 50cm from the hot water reserviour.

1. system configuration and hostname to get an WiFi connection (everything later can be configured later via the normal network)
2. MQTT configuration to publish the following topics: 
    * "ww/ht/dhw_Tflow_measured": out temperature of the warm water
    * "ww/ht/dhw_Treturn": return temperture of the warm water circulation
    * "ww/ht/Tint": system internal temperture
    * "ww/ht/dhw_pump_circulation": pump is running or not
    * "ww/ht/info": system information as text
3. MQTT configuration for heater status subscription to block pump if heater is off
4. NTP configuration to get the RTC infos for logging
5. Temperature configuration to map the DS18B20 sensors detected
6. Return Temperature that the pump can switch off when the water is gone through the whole circulation pipe.

![config page](img/opera_2022-10-31%20213941.png)

## OLED infos
![Displaypage1](img/Displaypage1.JPG)
<img src="img/Displaypage2.JPG"  width="30%" height="30%">
<img src="img/Displaypage3.JPG"  width="30%" height="30%">
<img src="img/Displaypage4.JPG"  width="30%" height="30%">
<img src="img/Displaypage5.JPG"  width="30%" height="30%">
<img src="img/Displaypage6.JPG"  width="30%" height="30%">
<img src="img/Displaypage7.JPG"  width="30%" height="30%">

# ToDos
* made a better documentation
* Test the detection of the pump events
* Make OLED language configurable

# Note
This project is still under early development but it is running without issues. But it is actually not clear of the warm water detection will work as expected as there should be some more tests. So feal free to help...