# Pi-tmaster-Improved

Based on JayTongue/Pi-Tmaster fine work.

I wanted to create a BBQ thermometer with a couple requirements:
1) Used the Weber iGrill temperature probes (thermistors) I already owned
2) Be able to handle 1 - 4 probes and able to handle them being plugged in / unplugged while running
3) Connect to my home WiFi network so I could monitor it from my phone anywhere in my house
4) Be able to show/keep temperature readings through a restart of the system or disconnecting from the network



------------------------------------------------
## 1. Hardware

![Wiring Diagram](exhibits/schematic.png)

### Microcontroller

For this project I used a Raspberry Pi Pico 2w that I already had, but you should be able to run this on a Pico W just as well. This project requires a file
system on the Pico to store temperature history in. In the Arduino IDE choose a flash size with a file system (Tools > Flash Size) for example: "2MB (Sketch: 
1MB, FS: 1MB)"

### Thermistors and Converter

The original project used thermocouples, but I wanted to use my probes from my iGrill which are thermistors (100k NTC), not thermocouples. 
Because these are analog probes and the Pico 2W only has 3 usable ADC pins, we require a separate ADC board. I went with the ADS1115 which is a 4-channel
16 bit ADC. And because it is an I2C device, it can share the same bus as the OLED which simplifies wiring. If you use the same probes you can probably
get away with using the Steinhart-Hart coefficients in the code already, but if not you will need to record the resistance in the probes at three different
temperatures and enter them into [this Thermistor Calculator](https://www.thinksrs.com/downloads/programs/Therm%20Calc/NTCCalibrator/NTCcalculator.htm) and enter
the values for A, B and C in the code. The library to use this module is Adafruit ADS1X15 library. Each probe sits inside a voltage divider (3.3V, a fixed 
10k resistor, the probe, ground). The ADS1115 measures the midpoint voltage, the sketch converts it to a resistance, and the Steinhart-Hart equation converts 
that to degrees F. Readings are averaged to reduce noise. A voltage near 0V or near 3.3V is treated as "no probe connected". 

### Screen
I used the same SH1106 OLED display as the original project. it uses the same I2C connection as the ADS1115 and uses the GyverOLED library. The screen will
display the Pico's IP Address once it's connected to the WiFi and the temperatures from the 4 screens (or N/A if the probe is disconnected) 

### Power Management
Again, I went with the same LX-LCBST charging board as the original project. I paired this with a 3000mAh battery I had laying around. 

When powered on I measured a current draw of 80 milliamps, which is just slightly more than the original project's 70 mA. With the 3000 mAh battery that should give
a theoretical run time of just under 40 hours, but I expect in actual use it will be less than that. 

### Case
TBD 

------------------------------------
## 2. Software

#### Raspberry Pi Pico 2W Code

In keeping with the original project, the ``Pi-tmaster-improved.ino`` sketch is written in Arduino C++. Required external libraries are ``Adafruit ADS1X15`` and
``GyverOLED`` the following libraries come with the core ``WiFi``, ``WebServer`` and ``LittleFS``.

### Start up

On startup the software connects to the WiFi network you've supplied in the code. Once connected it then sets it's clock using NTP from the internet. Finally
it starts up a web server that handles requests for the page showing the probe temperatures and the history graph, as well as API requests for the probe data itself.

### Web page

The web page code is stored in ``page.h`` and the file must be kept in the same folder as the sketch for it to compile. The whole page is loaded into flash memory
and does not rely on any CDNs so it will continue to work even if the home network internet is down. The web page shows the current temperatures of any probes connected, as
well as a live graph that can be selected to show 30 minutes, 2 hours, 6 hours or 24 hours of historical data. The Y axis adjusts based on the highest and lowest reading
in the specified time range. If the pico can't be reached or restarts a warning will be shown on the page, and any missing data will be back-filled in the graph when it
becomes available again. The page will poll the Pico for live temperature data every 3 seconds and refreshes the history every 30 seconds.

### Temperature history

Temperature readings are made every 15 seconds and stored in RAM on the Pico, up to 24 hours. Readings are initially timestamped with seconds since the Pico booted, but once 
the clock is set from the internet it will convert those elapsed times to actual times. Once the time is set, the system will write the temperature history to the file
system (Flash memory) every 2 minutes so it will survive a restart of the Pico if, for example, the battery dies. 

---------------------------------------
## 3. Notes and Limitations

In the end, the resulting graph might look something like this: 

* History from before a restart only comes back if it was saved, which needs the flash filesystem and a set clock (the Pico needs internet access for NTP).
* New history is saved every 2 minutes, so turning the power off can lose the last couple of minutes.
* History is capped at 24 hours.
* The web page has no password. Keep it on your home network; for remote access, use a VPN to your router rather than exposing the Pico to the internet.


---------------------------------------
## 4. Price Breakdown
I had a lot of this stuff lying around but here's what it would cost if you were to buy it all from Aliexpress. I did not include the case since I 
printed that myself.


| Item             | # Required |Unit Price (USD)  |Total  |
|------------------|------------|------------------|-------|
| RPi Pico 2W      | 1          | 20.09            | 20.09 |      
| Switch           | 1          | 0.06             | 0.06  |
| SH1106 display   | 1          | 1.09             | 1.09  |
| LX-LCBST         | 1          | 3.33             | 3.33  |
| Perf Board       | 1          | 0.00 *           | 0.00  |
| Diode            | 1          | 0.01             | 0.01  |
| ADS1115          | 1          | 1.09             | 1.09  |
| 10K Resistor     | 4          | 1.09             | 1.09  |
| LiPo battery     | 1          | 5.96             | 5.96  |
* free with any purchase

Total Price: $32.72


--------------------------------------
# 4. Possible Further Development

A. Dynamic Wifi settings
Have the Wifi SSID and Password setable dynamically via the phone so it doesn't have to be hard coded.

B. Battery Indicator
As with the original, this has no way to judge the remaining battery. It would be nice to have something indicating at least what the battery voltage currently is, even
though it's not a good representation of state of charge for LiPo.

C. Temperature targets
It might be nice to have user selectable target temperatures for each probe along with an alarm or notification that it's been reached.
