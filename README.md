# MQTT Kitchen Scale

ESP32-based kitchen scale using an HX711 load cell amplifier. Publishes weight readings over MQTT and is designed to be monitored and controlled via **Ignition** (or any MQTT-capable SCADA/HMI). It's intended to be used in conjunction with a nutrition database to create a homebrew calorie/macro tracking app in Ignition, where a food item is selected, and nutritional content is calculated based on the weight and the values from the selected food item.

<img width="1919" height="864" alt="image" src="https://github.com/user-attachments/assets/9e09bf83-022a-4b57-9c0c-2df76a899243" />


## Hardware

| Component | Pin |
|-----------|-----|
| HX711 DOUT | GPIO 4 |
| HX711 SCK | GPIO 5 |

## Dependencies

Install via Arduino Library Manager:

- [HX711](https://github.com/bogde/HX711) by bogde
- [PubSubClient](https://github.com/knolleary/pubsubclient) by Nick O'Leary
- [ArduinoJson](https://arduinojson.org/) by Benoit Blanchon

## Configuration

Edit the top of `KitchenScalev2.ino` before flashing:

```cpp
const char* WIFI_SSID     = "your_ssid";
const char* WIFI_PASSWORD = "your_password";

const char* MQTT_BROKER   = "192.168.x.x";   // your broker IP
const uint16_t MQTT_PORT  = 1883;
const char* MQTT_CLIENT_ID = "kitchen-scale-01";
```

## MQTT Topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `home/scale01/telemetry` | Publish | Weight data at 500 ms interval |
| `home/scale01/status` | Publish | LWT / status strings |
| `home/scale01/command/#` | Subscribe | Control commands |

### Telemetry Payload

Published every 500 ms as JSON:

```json
{
  "raw_avg":     123456.0,
  "grams":       125.34,
  "stable":      true,
  "offset":      98765,
  "scale_factor": 432.18,
  "uptime_s":    3600,
  "wifi_rssi":   -62,
  "heap_free":   214320
}
```

`stable` is `true` when the last 10 readings span less than 0.5 g.

### Commands

#### Tare

Topic: `home/scale01/command/tare`  
Payload: *(any / empty)*

Zeroes the scale using 20 averaged readings. Saves the new offset to flash.

#### Calibrate

Topic: `home/scale01/command/calibrate`  
Payload:
```json
{ "known_grams": 500 }
```

Place a known weight on the scale **after** taring, then publish this command. The computed scale factor is saved to flash.

#### Clear Calibration

Topic: `home/scale01/command/clear_calibration`  
Payload: *(any / empty)*

Erases offset and scale factor from flash and resets to defaults.

#### Reboot

Topic: `home/scale01/command/reboot`  
Payload: *(any / empty)*

Restarts the ESP32.

### Status Values

| Value | Meaning |
|-------|---------|
| `online` | Connected and running |
| `offline` | LWT — broker did not receive a clean disconnect |
| `tare_ok` / `tare_failed` | Result of tare command |
| `calibration_ok` / `calibration_failed` | Result of calibrate command |
| `calibration_cleared` | Calibration erased |
| `rebooting` | About to restart |

## Calibration Procedure

1. Power on the scale with nothing on the platform.
2. Publish to `home/scale01/command/tare` to zero it.
3. Place a known weight (e.g. 500 g) on the platform.
4. Publish `{"known_grams": 500}` to `home/scale01/command/calibrate`.
5. Calibration is saved to flash — survives power cycles.

## Ignition Integration

Use the **MQTT Engine / Transmission** modules (Cirrus Link) pointed at your broker. The telemetry topic maps cleanly to Ignition tags:

- `grams` → primary process value
- `stable` → boolean tag for triggering logic on settled readings
- `wifi_rssi` / `heap_free` / `uptime_s` → diagnostics

Commands can be sent from Ignition scripts using `system.mqtt.publish()` or a writable tag mapped to the command topics.
