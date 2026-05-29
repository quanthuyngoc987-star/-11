# 教室管家

This standalone ESP-IDF app verifies the phone-to-ESP32-S3 classroom steward GUI path.
It does not use RainMaker and does not touch the existing hardware wiring.

## Default Test: Phone Connects Directly To ESP32-S3

The board starts a Wi-Fi SoftAP:

```text
SSID: ESP32S3-GUI-TEST
Password: 12345678
URL: http://192.168.4.1/
```

Steps:

1. Build and flash this app.
2. Connect the phone to `ESP32S3-GUI-TEST`.
3. Open `http://192.168.4.1/` in the phone browser.
4. If the dashboard shows state cards and buttons, the local GUI path works.

## Competition-Like Test: ESP32-S3 Connects To Phone Hotspot

Use this mode to test: phone hotspot -> ESP32-S3 station -> phone browser opens the ESP32-S3 IP.

```powershell
idf.py menuconfig
```

Open:

```text
GUI Web Test Configuration
```

Set:

- `Wi-Fi mode`: `Station`
- `Station SSID`: phone hotspot SSID
- `Station password`: phone hotspot password
- `HTTPS connectivity test URL`: default `https://www.baidu.com/`

After flashing, read the serial log:

```text
Open GUI: http://192.168.x.x/
```

Open that URL in the phone browser. If it fails, the phone hotspot may block
client-to-client access. For the live demo, use a normal router or use the
default ESP32-S3 SoftAP mode for GUI verification.

## Internet Test

In Station mode, press `AI 分析` on the web page.

If `OpenAI-compatible LLM API URL` and `LLM API key` are empty, the board sends
an HTTPS GET request to `HTTPS connectivity test URL`. The default uses Baidu so
the test is more likely to work on domestic mobile networks. This verifies that
the ESP32-S3 can reach the internet through the phone hotspot.

Expected result:

```text
internet_ok HTTP 200
```

## Optional LLM API Test

To test a real cloud model, open `GUI Web Test Configuration` and set:

- `OpenAI-compatible LLM API URL`, default `https://api.deepseek.com/chat/completions`
- `LLM API key`
- `LLM model`, for example `deepseek-chat`

Then build, flash, open the web page, and press `AI 分析`.

Expected result:

```text
llm_ok HTTP 200
```

The firmware uses an OpenAI-compatible POST body and `Authorization: Bearer <key>`.
Do not commit `sdkconfig` if it contains a real API key.

## 教室管家 Hardware Test

The current test app is a standalone 教室管家 prototype. It keeps
RainMaker out of the loop and drives the wired hardware directly:

```text
Light PWM: GPIO41
Fan PWM: GPIO13
Alarm GPIO: GPIO12
Buzzer GPIO: GPIO37
I2C sensors: SDA GPIO8, SCL GPIO9
PMS5003: RX GPIO17, TX GPIO18
LD2410C: RX GPIO15, TX GPIO16, OUT GPIO4
INMP441: BCLK GPIO1, WS GPIO2, DIN GPIO42
```

### 3-Pin Active Buzzer Wiring

Use a 3-pin active buzzer module. The buzzer follows the `Alarm` state.

```text
3-pin buzzer module      ESP32-S3
------------------      -------
S / IO / Signal   ---->  GPIO37
VCC / +           ---->  3V3
GND / -           ---->  GND
```

Do not connect the buzzer signal pin to 5V. If your buzzer board only works at
5V, keep the signal controlled by GPIO37 and use a suitable driver/transistor
module instead of driving the buzzer directly from an ESP32-S3 GPIO.

The default `真实教室` mode reads real sensors. The `空气闷热`, `异常闯入`,
and `课后无人` buttons switch to simulated demo cases for repeatable tests.

## 教室管家课程表

Open the dashboard and press `课程表`, or open:

```text
http://<esp32-ip>/schedule
```

Students can view the schedule without logging in. Press `管理员编辑` to edit the
schedule. The demo administrator account is:

```text
Username: admin
Password: admin
```

The current implementation stores schedule edits in RAM. A reboot restores the
default schedule.

In `Station` mode, the board also starts SNTP after Wi-Fi connects:

- SNTP server: `ntp.aliyun.com`
- Local timezone: `Asia/Shanghai` / UTC+8
- Schedule check interval: 30 seconds
- 5 minutes before class: local light policy checks ambient light and turns the
  light on when needed, then starts the AI decision task for fan/alarm control.
- 5 minutes after class: if `occupancy_count == 0`, the light is turned off.
- Each class triggers the pre-class action once per day and the post-class
  action once per day. Editing the schedule resets today's trigger records.

If the `北京时间` card shows `等待 SNTP`, the device has not obtained real Beijing
time yet, so schedule automation will wait and no class action will run.

Expected validation:

1. Open the ESP32-S3 URL printed in the monitor.
2. Confirm the page shows `web v10`.
3. Press `真实教室`, then `刷新`; sensor values should come from hardware if detected. Light is shown as fallback because no real light sensor is installed.
4. Press `空气闷热`, then `AI 分析`; the fan should run.
5. Press `异常闯入`, then `AI 分析`; the alarm output should turn on.

## Build And Flash

Run these commands from an ESP-IDF PowerShell or terminal where `idf.py` works:

```powershell
cd E:\aiot\-11\tests\gui_web_test
idf.py set-target esp32s3
idf.py build flash monitor
```

If PowerShell blocks `export.ps1`, start the official ESP-IDF terminal from the
Espressif tools shortcut, or run PowerShell with a process-only execution policy
bypass before loading the ESP-IDF environment.
