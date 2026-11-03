# ESP8266 WebSDR — Wi-Fi Wide Sweep User Guide

This guide describes the **three-color Wi-Fi snapshot version** of [ESP8266_WebSDR.ino](ESP8266_WebSDR/ESP8266_WebSDR.ino).

One ESP8266 captures raw radio I/Q samples and sends them over Wi-Fi to a browser. The ESP also serves the complete web interface. Its USB/serial connection is used to upload firmware; application I/Q data does not travel through the serial port.

The ESP joins an existing 2.4 GHz Wi-Fi access point as a station. You do not need a second ESP acting as a receiver, a serial-to-network bridge, or a separate HTML file.

## 1. Quick start

After uploading the configured sketch:

1. Power the ESP8266. USB may remain connected for power.
2. Connect your computer to the same local network. Ethernet is suitable if it can reach the wireless clients.
3. Open **http://192.168.1.37/** in your browser.
4. Check the **Receiver WebSocket** field. With the supplied IP settings, it must contain **`ws://192.168.1.37:81/ws`**.
5. Click **Connect Wi-Fi**. This connects the browser to the ESP; it does not configure the ESP's Wi-Fi credentials.
6. Leave **Wide sweep · 3 windows** checked for the three-window display. Uncheck it to select one capture frequency manually.

The status changes to receiving, the spectrum and waterfall update, and the footer reports the measured sweep or frame rate. There is no browser serial-port selection or Web Serial permission prompt.

If you change the ESP's IP, use the new address in **both** the browser URL and the Receiver WebSocket field. See section 3: changing the firmware IP does not update the HTML's prefilled WebSocket address automatically.

## 2. Hardware, Arduino configuration and upload

### Reference configuration

| Item | Configuration for this sketch |
|---|---|
| Board | ESP8266 D1 mini, 4 MB flash |
| Arduino board package | **esp8266 by ESP8266 Community, version 3.1.2** |
| CPU frequency | **160 MHz** |
| Flash layout | **4 MB, FS: 1 MB** |
| SDK | Default **NONOSDK22x_190703** |
| MMU | Default **32 KB instruction cache + 32 KB IRAM** |
| Additional libraries | **None** beyond the ESP8266 board package |
| Access point | Existing 2.4 GHz AP; this implementation accepts home channels **1–11** |
| Browser | JavaScript, Canvas and WebSocket support |

The reference FQBN is:

```text
esp8266:esp8266:d1_mini:xtal=160,eesz=4M1M
```

The sketch checks several of these requirements at compile time. This version uses SDK-specific radio functions and a specific RAM layout. Keep the reference settings when reproducing the qualified build. An ESP32, a different SDK, or a different MMU layout is not a drop-in target.

### Prepare Arduino IDE

1. Install Arduino IDE if necessary.
2. In Preferences, add this ESP8266 Boards Manager URL:

   ```text
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```

3. In Boards Manager, install **esp8266 by ESP8266 Community**, selecting **3.1.2**.
4. Select **LOLIN(WEMOS) D1 R2 & mini** in the ESP8266 board list, then set the options in the table above.

These are the board-package installation steps documented by the [ESP8266 Arduino project](https://arduino-esp8266.readthedocs.io/en/latest/installing.html). The pinned version and radio settings above come from this sketch's reference build.

### Upload this sketch

Keep the following folder structure:

```text
ESP8266_WebSDR/
└── ESP8266_WebSDR.ino
```

Open the `.ino`, edit the network settings described below, select the board's actual upload port, and use **Verify**, then **Upload**. Arduino's [upload instructions](https://support.arduino.cc/hc/en-us/articles/4733418441116-Upload-a-sketch-in-Arduino-IDE) explain board and port selection.

The supplied upload procedure used 115200 baud. Upload speed is a programming setting, not the I/Q transport speed. Do not select a COM port in the web interface: this version has no runtime serial I/Q connection.

Do not install an additional WebSockets library or copy headers from an older multi-file version. This single `.ino` contains the application, WebSocket transport and compressed web page. The standard ESP8266 board package provides the SDK and core networking libraries.

The device does not need a VM or VirtualHere during normal operation. If serial passthrough is used for uploading, it can be released afterward while the board remains powered.

## 3. Where to edit SSID, password, IP address and subnet mask

Open **`ESP8266_WebSDR.ino`** in a text editor or Arduino IDE. Search for the names below instead of relying on line numbers, which move after edits.

| Setting | Search for | Location in this delivered file |
|---|---|---|
| Wi-Fi network name | `SDR_WIFI_SSID` | `WifiCredentials.h` section, approximately line 2366 |
| Wi-Fi password | `SDR_WIFI_PASSWORD` | Same section, approximately line 2367 |
| Static IP | `WiFi.config(` | `DirectSession.h` section, approximately line 2413 |
| Subnet mask | Third argument of `WiFi.config(` | Same line |
| Default gateway | Second argument of `WiFi.config(` | Same line |

These section names are comments inside the single `.ino`; you do not need separate files with those names.

### SSID and password

Replace the values of these two definitions. The following example uses placeholders, not the credentials stored in your configured copy:

```cpp
#define SDR_WIFI_SSID "YOUR_2_4_GHZ_SSID"
#define SDR_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

Use the exact SSID, including capitalization and spaces. A hidden SSID is supported by supplying its exact name; the ESP does not need to broadcast its own access point.

The values are C++ strings. An embedded double quote must be written as `\"`, and a literal backslash as `\\`. Ordinary characters such as `@` can remain inside the quotes.

The firmware connects using:

```cpp
WiFi.begin(SDR_WIFI_SSID, SDR_WIFI_PASSWORD, 0, nullptr, true);
```

Changing the Receiver WebSocket field in the browser does **not** change these Wi-Fi credentials. After changing a credential in the sketch, compile and upload the sketch again.

### IP address, gateway and mask

The delivered configuration is:

```cpp
WiFi.config(
    IPAddress(192, 168, 1, 37),  // ESP's local IP address
    IPAddress(0, 0, 0, 0),      // Default gateway
    IPAddress(255, 255, 255, 0) // Subnet mask
);
```

**The argument order is local IP, gateway, subnet mask.** The mask is the third argument, not the second.

`255.255.255.0` corresponds to `/24`. In this example, a computer on the same `192.168.1.x` subnet can reach the ESP directly. The supplied `0.0.0.0` gateway is intended for that local-subnet setup; it provides no default route for reaching clients outside the subnet.

Choose an unused address appropriate for your LAN, and prevent your DHCP server from assigning that address to another device. If routed access is required, enter the correct gateway for your own network in the second argument.

For example, to move the ESP to `192.168.1.50` while retaining a local-only `/24` setup:

```cpp
WiFi.config(
    IPAddress(192, 168, 1, 50),
    IPAddress(0, 0, 0, 0),
    IPAddress(255, 255, 255, 0)
);
```

After uploading this change:

1. Open **`http://192.168.1.50/`**.
2. Replace the Receiver WebSocket field with **`ws://192.168.1.50:81/ws`**.
3. Click **Connect Wi-Fi**.

### Important: the embedded page's address is separate

The HTML currently has a prefilled WebSocket address of `ws://192.168.1.37:81/ws`. It is stored inside the compressed `sdrPageGzip` byte array. It does not automatically follow the address in `WiFi.config`. The application does not persist a new default; after reloading the page, check the field again and correct it if the original address reappears.

You can use a different device IP simply by entering the correct WebSocket address in the browser each time. **Do not manually edit the large hexadecimal byte array.** Making a new permanent HTML default requires editing the HTML source and regenerating its compressed array.

URLs in introductory source comments are explanatory text; changing those comments does not change the network configuration.

The current sketch uses compiled settings, not a browser-based Wi-Fi setup wizard, a captive portal or an OTA configuration service. Network-setting changes require another firmware upload.

## 4. Using the display

### Wide sweep: three successive acquisitions

Wide sweep starts enabled after connecting to compatible firmware. It captures three windows in this order:

| Acquisition | Color | Capture center | Nominal 40 MHz FFT window |
|---|---|---:|---:|
| 1 | **Purple** — `#c084fc` | 2412 MHz | 2392–2432 MHz |
| 2 | **Yellow** — `#facc15` | 2437 MHz | 2417–2457 MHz |
| 3 | **Green** — `#4ade80` | 2462 MHz | 2442–2482 MHz |

Those frequency windows are **nominal digital spans**, not a measurement of the analog receiver's usable passband. The useful RF bandwidth, edge response, sensitivity and image response have not been calibrated.

The browser assembles the three power spectra on a common nominal 90 MHz axis. The spectrum line, filled area and waterfall use the three acquisition colors. Waterfall brightness still indicates power.

Colors identify the **dominant acquisition** in each part of the composite. Adjacent windows overlap, and their powers are blended in those overlaps. Consequently, the color boundaries are at **2424.5 MHz** and **2449.5 MHz**, rather than at equal screen thirds. These boundaries follow frequency when you zoom or pan.

The numbered legend states that the captures occur at different times. A single waterfall row in this mode represents an assembled sweep, not one simultaneous 90 MHz observation. Trace averaging and max hold can combine information from additional sweeps as well.

Manual capture-center selection and offset-LO control are disabled while Wide sweep is checked. The three sweep centers are fixed in this version.

### Single-frequency mode

Uncheck **Wide sweep · 3 windows**. Then use **Capture center frequency** to select an integer frequency from **2412 to 2462 MHz**, in **1 MHz steps**.

With **0 Hz (zero-IF) → show as is**, the default, the next acquisition uses that frequency. The ESP then returns its physical radio to the access point's frequency before sending the data. The AP channel shown in the connection status can therefore remain channel 11 even when you select, for example, a 2413 MHz capture center. The AP channel and capture center describe different things.

If you choose **offset LO**, the viewer intentionally moves the physical capture LO relative to the entered tuned frequency to keep the signal away from the DC spike. At 40 MS/s, the usual offset is 5 MHz, constrained by the available tuning range. This option is disabled in Wide sweep.

Normal mode retains the original green spectrum trace and normal power-color waterfall. The three-color acquisition legend appears only in Wide sweep.

### Supported controls

| Control | Meaning in this ESP8266 build |
|---|---|
| FFT bins | 512, 1024 or 2048 per acquisition window |
| Sample rate | Nominal **40 MS/s** |
| I/Q format | Signed **10-bit I + 10-bit Q**, packed for transfer |
| Manual gain | Index **0–29**; these indices are not calibrated dB values |
| Floor / Range | Display scaling in relative dBFS10, not calibrated dBm |
| Trace averaging | Smooths successive displayed spectra; also mixes information over time |
| Max hold | Retains peaks across observations; Clear removes the accumulated result |
| Auto scale | Adjusts the visible power scale |
| Pause / Resume | Stops or resumes acquisition requests while retaining the browser connection |
| Clear | Clears displayed trace/history state |
| Mouse wheel | Zooms the displayed frequency axis |
| Drag | Pans the view; Wide sweep remains within its fixed assembled RF coverage |
| Double-click | Returns to the full displayed span |

Start with a low manual gain index. If the page reports clipping, reduce gain. Filling DC bins is a display treatment of the zero-IF/DC artifact; it does not calibrate or remove the physical receiver impairment.

The shared HTML contains controls and presets for other receiver implementations. After connection, firmware capabilities determine which options are usable. An initial generic 5.500 GHz preset, 80 MS/s option, larger FFT option or AGC button is **not** evidence that this ESP8266 build supports it.

This firmware does not advertise a continuous On-Chip Spectrum stream. Its optional one-shot `ZCAP` fast-zoom path in single mode is distinct from a continuous spectrum service. GPIO control and adjustable analog bandwidth are not supported by this build.

## 5. How radio capture and Wi-Fi coexist

The ESP has one shared radio. It alternates capture and network transfer; it does not receive arbitrary-frequency I/Q while simultaneously transmitting the samples over Wi-Fi.

For a normal wide-sweep request, the firmware:

1. Starts with an established Wi-Fi station association and a browser WebSocket connection.
2. Sends a Wi-Fi power-management notification requesting a short absence, then checks radio/MAC readiness.
3. Temporarily inhibits MAC reception and borrows the radio for I/Q capture. The SDK's **logical channel stays on the AP's home channel**.
4. Programs the native RF synthesizer for each requested physical capture center, verifies its programmed divider and calibration-complete state, and captures one window at each center.
5. Preserves the three packed payloads in memory, with integrity checks. No application sample payload is sent during this RF excursion.
6. Restores the capture-related radio state, calls the native home-channel restoration path, and verifies the physical PLL settings against the saved home state before resuming network output.
7. Sends the Wi-Fi resume notification, then transfers the complete checked sweep to the browser. The browser checks each payload's CRC before displaying the assembled result.

This sequence does not deliberately disconnect and reassociate for every capture. It is also **not deep sleep or CPU light sleep**: the CPU continues to execute capture and control code, with normal Wi-Fi sleep disabled for this implementation.

A bounded busy refusal can discard an attempt and retry after verified home restoration. Failed or partial sweeps are not presented as complete data. PM-frame retries remain limited to three attempts; completed transmission failures are spaced by 10 ms before another permitted attempt. A persistent failure can pause acquisition and show an error instead of retrying indefinitely.

With 2048-point windows, each payload contains 2048 complex I/Q pairs packed into 5120 bytes. A three-window sweep carries **15,360 bytes of I/Q payload**, plus protocol overhead. The three independent FFTs are combined as power spectra with overlap weighting, not by concatenating raw I/Q into one phase-coherent recording. At this setting, the composite display grid contains 4608 bins; it is not a 4608-point simultaneous capture or a 90 MS/s ADC stream.

## 6. What changed from the serial versions

| Topic | Earlier serial workflow | This Wi-Fi version |
|---|---|---|
| I/Q data path | ESP UART → USB/serial bridge → computer → browser | ESP Wi-Fi → access point/LAN → browser WebSocket |
| Web interface | Separate HTML file opened on the computer | Complete HTML/CSS/JavaScript served by the ESP on HTTP port 80 |
| Browser connection | Serial-device selection and serial access permission | Receiver WebSocket URL and Connect Wi-Fi button |
| Cable requirement | Serial cable/passthrough needed for operation | Serial needed for programming; power can remain over USB |
| Runtime transport settings | Baud rate and pacing affect the data path | TCP/WebSocket transport; no runtime UART baud or pacing setting |
| Radio/Wi-Fi handling | The supplied serial v6 explicitly disconnected the Wi-Fi station for radio initialization | Associated station context is preserved across verified RF excursions |
| Frequency selection | Capture tuning controlled through serial commands | Browser selection reaches the ESP over WebSocket; the physical capture frequency is separated from the logical AP channel |
| Wide view | A single tuned observation is not simultaneous full-band data | Three ordered captures are assembled, with colors showing their different acquisition times |
| Data integrity | Serial framing/CRC and transport pacing | Per-window CRC, complete-sweep validation, radio restoration checks and bounded retries |
| Main transport constraints | UART rate, USB bridge behavior, pacing and browser serial handling | Wi-Fi airtime, TCP behavior, RF switching, validation work and browser processing |
| Deployment | Computer must reach the serial device | A browser on the reachable LAN can operate the ESP |

The supplied original serial v6 defaulted to **1,000,000 baud**, offered 115200/1,000,000/2,000,000 baud settings, and used transmission pacing. This Wi-Fi build removes that UART transport dependency. It does not prove that every Wi-Fi operating condition will outperform every possible serial configuration.

For scale, a 5120-byte payload alone takes about 444 ms at 115200 baud using 8N1, or 51.2 ms at 1 Mbaud, before protocol and pacing overhead. These are wire-time calculations, not measured frame rates of the older application.

The physical I/Q sampling principle, finite capture buffer, signed sample interpretation and FFT-based display remain related to the serial implementation. Switching to Wi-Fi does not make the RF receiver calibrated or turn finite snapshots into continuous capture.

Some internal function calls still say `Serial.print` or `Serial.write` because the protocol code was reused. In the application section, **`#define Serial IQSocket`** redirects those calls to the WebSocket transport. Those names do not indicate that samples are still being sent to the UART.

The new tradeoff is that network transfer and off-channel radio capture share the same RF hardware. The serial path did not need to use that radio to transport its samples. The Wi-Fi version therefore needs explicit radio borrowing, return-to-home verification and recovery behavior.

## 7. Capture rate, display rate and validation scope

### Sampling is fast; observations are separated by gaps

| FFT/window size | Nominal acquisition duration at 40 MS/s |
|---:|---:|
| 512 I/Q pairs | 12.8 µs |
| 1024 I/Q pairs | 25.6 µs |
| 2048 I/Q pairs | 51.2 µs |

At the default 2048 setting, a sweep contains **three successive 51.2 µs snapshots**. Their sample times total 153.6 µs, but RF preparation, checking, switching, restoration and transfer make the complete cycle much longer.

The reference Wi-Fi firmware completed **2000 full sweeps in approximately 180 seconds**, or **11.1 complete sweeps per second**. That corresponds to roughly 33.3 individual acquisition windows per second and about 68,300 delivered I/Q pairs per second across the three centers. It does not mean 40 million I/Q pairs per second are continuously delivered to the browser.

The measured reference cycle averaged approximately **90 ms per sweep**. Capture itself was reported at about 52 µs, while the surrounding per-window capture/profile/check/restore work was approximately 2.9 ms. The remainder includes switching, protocol work, network transfer and waiting. This result is a stable measured operating point, **not a demonstrated maximum acquisition rate of the ESP8266**.

The nominal internal RF I/Q sampling rate does not describe the external A0 `analogRead()` input. Sample count format also does not establish calibrated effective ADC resolution, noise figure or absolute RF power accuracy.

### What was verified

The underlying Wi-Fi firmware was checked on the reference D1 mini configuration with:

- 153 single-frequency captures covering every integer MHz from 2412 through 2462, over three rounds.
- 2000 complete three-window sweeps, with each output payload CRC checked.
- Physical home-return checks and unchanged association, disconnect, boot and WebSocket-session counters during the long run.
- 16 safely refused sweep attempts recovered by bounded retries and 7 failed PM transmissions recovered without a terminal PM failure.
- The HTML client protocol/DSP path exercised over the real Wi-Fi connection, including after releasing VirtualHere.

The subsequent **purple/yellow/green revision changes only the embedded web page**. Its renderer, zoom mapping, normal-mode behavior and embedded-page integrity were checked offline. That color revision was not flashed or tested on the ESP, as requested. The earlier radio benchmark therefore describes the unchanged firmware backend, not a new hardware qualification of the colored page.

No calibrated RF source or frequency-response measurement established a flat usable 90 MHz analog passband. The full display is a nominal stitched view, and transient events can appear in one acquisition but be absent by the time the next acquisition occurs.

## 8. Diagnostics and troubleshooting

### Read-only HTTP diagnostics

Use your configured IP in place of the example address:

| URL | Purpose |
|---|---|
| `http://192.168.1.37/` | Embedded browser interface |
| `http://192.168.1.37/health` | JSON with boot/association/disconnect counters, AP channel, fault status, capture readiness and SDK version |
| `http://192.168.1.37/reset` | Last reset reason and reset information — **this does not reboot the ESP** |

In `/health`, a ready, non-faulted device normally reports `captureReady: 1` and `fault: 0`. The `channel` value is the AP's logical channel, not the selected off-channel capture center. Compare counters before and after a test when assessing continuity; their values belong to the current boot/session history.

The WebSocket service also has developer diagnostics such as `NET?`, `PERF?` and `SWEEP?`. These are WebSocket protocol commands, not browser HTTP URLs or Serial Monitor commands. Do not attach a second diagnostic client while the page owns the capture connection.

### Common symptoms

| Symptom | Check or action |
|---|---|
| Page does not open | Confirm power, exact SSID/password, the compiled static IP/mask, address conflicts, and LAN reachability. Guest Wi-Fi/client isolation can prevent access. |
| Page opens but Connect Wi-Fi fails | Verify `ws://IP:81/ws`, especially after an IP edit. HTTP port 80 working does not prove port 81 is reachable. |
| A second browser tab cannot connect | This firmware accepts **one WebSocket data client**. Disconnect or close the other capture client. |
| No COM-port chooser appears | Expected. This version uses WebSocket, not browser serial access. |
| Changing the browser WebSocket address does not move the ESP to another network | Edit `SDR_WIFI_SSID`, `SDR_WIFI_PASSWORD` and/or `WiFi.config` in the sketch, then upload again. |
| Frequency control is disabled | Uncheck Wide sweep to select a single capture center. |
| A generic rate, band or control is unavailable | The connected firmware supports a restricted ESP8266 profile; see section 4. |
| Signals look clipped or distorted | Reduce manual gain. Gain indices and dBFS10 are not calibrated RF power units. |
| Short-lived signals differ between colored areas | Expected possibility: the three areas come from successive captures. Overlap blending and averaging can mix observations. |
| Capture pauses with a persistent error | Read the message. Resume can retry a transient failure; it does not clear every latched radio fault. Check `/health`. Reboot the ESP if a latched fault prevents recovery. |
| Colors or controls look like an older page after uploading | Reload the page, or use a hard refresh. Load the page served by the updated ESP rather than an older separately saved HTML file. |
| Compilation reports CPU, SDK, MMU or memory-layout errors | Restore the exact reference Arduino settings. Do not remove the checks to force an unsupported build. |

The firmware may attempt to reconnect after a Wi-Fi loss outside an active capture transaction. A radio verification failure or a disconnection during a transaction can stop capture instead. The successful no-reassociation test does not guarantee that an unreliable AP or lost RF link can never require reconnection.

## 9. Operating boundaries and file handling

- Supported capture centers are **2412–2462 MHz**, not arbitrary RF or 5 GHz reception.
- This is a snapshot spectrum viewer, not a continuous, lossless or phase-coherent wideband I/Q recorder.
- The three colors identify successive acquisition contributions; they do not provide simultaneous timing across the whole axis.
- Keep to one active capture client. Multi-client broadcasting is not implemented.
- HTTP and WebSocket are unencrypted and have no application login. Use a trusted LAN; this build is not designed as an Internet-exposed service.
- The configured `.ino` contains Wi-Fi credentials. This guide intentionally uses placeholders and does not repeat them. Replace credentials before sharing source publicly.
- Keep the original notices and embedded licenses when redistributing the code.

### File identity

This guide targets the delivered colored sketch with SHA-256:

```text
9b61974c1123f001229ea4ac26a405c6f47bb88b188d141b22342ecdea600819
```

Its underlying hardware-qualified, pre-color firmware source had SHA-256:

```text
4c1913bc1a3d4c0304dad010f3d8068dde50afc540a2f19e24a8353e499fae0b
```

Editing network settings will naturally change the file hash. The approximate source line numbers above refer to the unedited colored sketch.
