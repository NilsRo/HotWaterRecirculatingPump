# HotWaterRecirculatingPump
Automation of hot water recirculating pump via ESP32

The project (actually with german LCD GUI) follows the ideas of https://github.com/MakeMagazinDE/Zirkulationspumpensteuerung and http://www.kabza.de/MyHome/CircPump/CirculationPump.php to control the hot water recirculation pump (Warmwasserzirkulationspumpe) via two temperature sensors comparing the warm water out and return temperature.

Featurelist
* Control the pump via out and return temperautre sensor
* MQTT messaging to get logging information
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

# Software
To setup the device it will open an Access Point named "Zirkulationspumpe" to provide an configuration interface. It is still available after the device is connected to an WiFi Network so you can do the 1-Wire configuration later. You have to mount the sensor 50cm from the hot water reserviour.

# ToDos
* made a better documentation
* Test the detection of the pump events
* Make OLED language configurable

# Note
This project is still under early development but it is running without issues. But it is actually not clear of the warm water detection will work as expected as there should be some more tests. So feal free to help...