# ESPBoOm8266
Raw IQ Capture with ESP8266 SDR
<img width="1883" height="894" alt="image" src="https://github.com/user-attachments/assets/3ead72e1-44fa-4e9b-857b-5430e31fba72" />

# ESPBoOm8266 — ESP8266 WebSDR

**Original project credit: [ESPARGOS / ESP-WebSDR](https://github.com/ESPARGOS/esp-web-sdr).**

The purpose of ESPBoOm8266 is simply to port this beautiful project to the
**ESP8266**, keeping the ESPARGOS browser interface and adapting it to this chip.
Credit for the original project, web interface, and the foundation on which this
port is built belongs to **ESPARGOS and its contributors**.

Credit for figuring out how to make raw I/Q capture work on the **ESP8266 in
this project** belongs to **Guillaume**, the creator of this port. The rest of
the project builds on ESPARGOS's work, with the adaptations needed to connect
the ESP8266 firmware to the browser. This is an independent port.

## What it does

ESPBoOm8266 captures short bursts of raw I/Q samples from an ESP8266 and displays
their spectrum and waterfall in a web browser. It consists of two runtime files:

- **One Arduino sketch:** `ESP8266_WebSDR/ESP8266_WebSDR.ino`.
- **One self-contained HTML page:** `ESP8266_WebSDR.html`.

The HTML runs on your computer and connects directly to the board through
**USB serial using WebSerial**. The ESP8266 supplies the samples; the browser
decodes them, calculates the FFT, and draws the display. The page includes its
scripts, styles, and fonts. It does not need a remote server during use.

The firmware runs from normal flash using the Arduino ESP8266 core. No RFTest
firmware, RFTest library, or custom linker script is required. The HTML is opened
on your computer; it is not uploaded to the ESP8266 or served over its Wi-Fi.

![ESPBoOm8266 browser interface](validation/interface_ESPBoOm8266.png)

## Current capabilities

| Feature | This release |
| --- | --- |
| Firmware | v4, with a default serial rate of **1,000,000 baud, 8N1** |
| Center frequency | **2412–2462 MHz**, in **1 MHz steps** |
| Gain | Manual index **0–29**; these indices are not dB values |
| Internal sample rate | **40 million complex samples/s, nominal**, fixed |
| Capture size | **2–2048 complex samples** per firmware capture |
| Browser FFT sizes | **512, 1024, or 2048** |
| Sample representation | Two signed **10-bit** components per sample |
| Display | Spectrum, waterfall snapshots, averaging, and max hold |
| Transport validation | CRC32 checking and resynchronization |

Captures are **bursts separated by gaps**, not a continuous 40 MS/s stream over
USB. A full 2048-sample burst contains **51.2 microseconds** of RF signal. Its
transfer takes much longer. The serial rate and the internal sample rate are
different quantities.

The supported tuning range belongs to this release. Reception at 433 MHz,
5 GHz, or experimental frequencies obtained by changing crystal/PLL settings
is not an implemented or validated feature.

## What you need

- An ESP8266 development board with a USB-to-serial bridge. The reference
  configuration is **NodeMCU 1.0 / ESP-12E with 4 MB flash**.
- A USB **data** cable and the appropriate USB-to-serial driver for your board.
- Arduino IDE with **esp8266 by ESP8266 Community, version 3.1.2**.
- Desktop **Chrome or Edge** with WebSerial available.

The tested build uses **CPU 80 MHz** and **NONOSDK22x_190703**, the default SDK
of the specified core. Other core versions, SDKs, CPU speeds, and memory layouts
have not been qualified for this capture method. No extra Arduino library is
needed: `ESP8266WiFi` is included with the core.

## Compile and flash with Arduino IDE

1. Open Arduino IDE preferences and add the following Boards Manager URL:

   ```text
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```

2. Open **Boards Manager**, search for **esp8266**, and install **esp8266 by
   ESP8266 Community**, selecting version **3.1.2**. See the
   [official core installation instructions](https://arduino-esp8266.readthedocs.io/en/3.1.2/installing.html).

3. Open `ESP8266_WebSDR/ESP8266_WebSDR.ino`. Keep the sketch inside its
   `ESP8266_WebSDR` folder so that the folder and sketch names match.

4. Select the following build settings:

   | Arduino setting | Reference value |
   | --- | --- |
   | Board | **NodeMCU 1.0 (ESP-12E Module)** |
   | CPU Frequency | **80 MHz** |
   | Flash Size | **4MB (FS:1MB OTA:~1019KB)** |
   | SDK | Default for core 3.1.2: **NONOSDK22x_190703** |
   | Port | The serial port belonging to your ESP8266 |

   Keep the remaining board options at their defaults. Do not select 160 MHz:
   the capture timing and clock checks require 80 MHz. The reference Arduino
   CLI board identifier is `esp8266:esp8266:nodemcuv2:xtal=80,eesz=4M1M`.
   Here, `xtal=80` is the core's CPU-speed menu identifier; it is not an
   instruction to install or declare an 80 MHz physical crystal.

5. Connect the board, close any application using its serial port, then click
   **Verify** and **Upload**. Wait for the upload to finish. If automatic
   bootloader entry fails on a board with FLASH and RESET buttons, hold
   **FLASH**, press and release **RESET**, then release **FLASH** and retry.
   Reset the board after uploading if it does not restart automatically.

6. Close the Arduino Serial Monitor before connecting the browser.

The IDE's **Upload Speed** controls flashing only. It does not change the
firmware's runtime serial rate of **1,000,000 baud**. These are separate
settings; see the [core's board options](https://arduino-esp8266.readthedocs.io/en/3.1.2/ideoptions.html).
The HTML does not flash the board. Use the sketch and the procedure above for
this ESP8266 port.

## Connect and use the browser interface

1. Connect the board by USB to the computer running the browser. If a VM owns
   the USB device through passthrough or redirection, release it to the host
   first. The browser must be able to open the board's local serial port.
2. Open **`ESP8266_WebSDR.html`** in desktop Chrome or Edge. If browser policy
   prevents WebSerial from a local file, serve the same file from **localhost**
   or **HTTPS**. See the [WebSerial documentation](https://developer.chrome.com/docs/capabilities/serial).
3. Click **Connect ESP8266** and select the board's USB-to-serial port. Use
   **Choose WebSerial Device…** when you need to select a different board.
4. Wait for the firmware identification and the first captures. The delivered
   HTML and firmware both start their normal connection at **1,000,000 baud**.
5. Choose a **Center frequency**, a **Manual gain index**, and an **FFT bins**
   setting. After typing a frequency, press **Tab** or click outside the field
   to apply it. Start with a low gain index and increase it as needed; high
   gain can clip strong nearby signals.
6. Use **Pause** to suspend acquisition, **Clear** to reset displayed traces,
   and **Disconnect** to release the serial port when finished.

Only one application or browser tab can own the port at a time. Close serial
monitors, flashing tools, and collection scripts before connecting. This port
uses **WebSerial**, not the original project's Android/WebUSB workflow.

The **Floor**, **Range**, **Trace averaging**, **Max hold**, and **Auto scale**
controls adjust how the spectrum is presented. They do not change the physical
sample rate or turn the burst captures into a continuous recording.

## Understand the display

| FFT size | Nominal duration per FFT window | FFT-bin spacing |
| ---: | ---: | ---: |
| 512 | 12.8 microseconds | 78.125 kHz |
| 1024 | 25.6 microseconds | 39.0625 kHz |
| 2048 | 51.2 microseconds | 19.53125 kHz |

The browser normally captures 2048 samples. With a 512- or 1024-point FFT, it
averages the power spectra of four or two windows within that capture. Selecting
a smaller FFT therefore does not normally reduce the serial payload.

Bin spacing is `40 MHz / FFT size`. It is not a measurement of the analog
receiver's resolution or flat bandwidth. Zooming the plot does not create more
frequency resolution, and the capture buffer does not contain 4096 contiguous
samples.

- **Waterfall · snapshots** shows successive captures with time gaps.
- **frames/s** is the display update rate.
- **kS/s delivered** is the average sample delivery rate over the connection.
- **ms / capture + transfer** includes acquisition and transfer time.
- **dBFS10** is a relative digital amplitude scale, normalized by 512. It is
  not calibrated in dBm, volts, or antenna power.

Each hardware word contains two signed 10-bit components:

```text
A = sign10(word & 0x3FF)
B = sign10((word >> 10) & 0x3FF)
complex sample used by the browser = A - jB
```

This is the convention used for the frequency-axis direction in this project.
The numeric range is -512 to +511 per stored component; it does not establish
10 effective bits of analog resolution.

## Serial throughput and troubleshooting

**1 Mbaud is the configured default, not a guarantee of lossless operation on
every USB bridge or host.** Byte losses were observed during high-rate tests,
including a prolonged serial-only test through the VM's native USB connection.
The browser rejects truncated or CRC-invalid captures before drawing them.

At startup, the firmware configures **`PACE 3000`**, adding 3000 microseconds
between output blocks at rates above 115200 baud. Binary blocks contain up to
64 bytes. A 2048-sample capture packs into **5120 bytes**: approximately
51.2 ms of UART wire time at 1 Mbaud, plus about 240 ms of configured pacing,
before other overhead. Use the application's reported rate for actual throughput.

| Symptom | What to check |
| --- | --- |
| No serial port appears | Use a data cable, check the USB-to-serial driver, and confirm the OS sees the board. |
| Port appears but cannot be opened | Close other programs/tabs and release the device from any VM. On Linux, check serial-device permissions. |
| WebSerial is unavailable | Use desktop Chrome/Edge with the API enabled; try localhost or HTTPS if local-file access is blocked. |
| Upload cannot connect | Check board and port selection; try the FLASH/RESET bootloader sequence above. |
| CRC errors or truncated captures | Check the cable and direct USB connection; use **Switch to 115200 baud** when offered. |
| Lower-baud switch fails | Reopening some bridges resets the ESP8266, restoring 1 Mbaud. The page detects and rejects a mismatched rate. |
| Capture initialization fails | Restore the exact core, CPU, and board settings; source changes may have moved the capture buffer. |
| Strong signals look clipped | Reduce the manual gain index. |

At 115200 baud, the extra pacing is disabled, but 5120 payload bytes alone take
about 444.4 ms. That rate is also not a universal guarantee against transport
errors. A hardware reset restores **1,000,000 baud** and **`PACE 3000`**.

## Developer notes

The capture mechanism reserves **8192 bytes at `0x3FFE8000`**, followed by
**64 guard bytes at `0x3FFEA000`**. The `iqStorage` symbol must remain at
**`0x3FFE8000`, size `0x2040`**, and the `saved` array must remain outside that
region. Check the ELF/linker map after changing the source or build settings.
An added initialized global or library can change this placement.

PHY, PBUS, and PLL calls use private SDK interfaces. Capture calls are
serialized in `setup()`/`loop()`; they are not supported in an ISR, callback,
or concurrent context. Startup and capture checks verify memory placement,
clock, radio profile, and guards. Serial transmission happens after the
critical capture section.

The browser handles the command protocol automatically. For a separate client,
close the browser connection first and send commands terminated by LF:

| Command | Purpose |
| --- | --- |
| `INFO` | Protocol identity: `ESP8266SDR 6 burst 2048`. |
| `CAPS` | Supported commands/features; `TUNEEXT` advertises fine tuning. |
| `RANGE?` | Current firmware: `RANGE 2412 2462 1`. |
| `LIMITS?` | Gain, sample rate, component width, and filter limits. |
| `TRANSPORT?` | Current UART baud rate. |
| `GAIN?` / `GAIN MANUAL 0` | Read or set the manual gain index. |
| `FREQ 2412` | Tune the receiver and invalidate the previous capture. |
| `STAT?` | Capture, guard, profile, and transport diagnostics as JSON. |
| `PACE 3000` | Set output pacing; accepted range is 0–10000 microseconds. |
| `BAUD 115200` | Acknowledge at the old baud rate, then switch; the client must switch too. |
| `CAP20 2048 1` | New capture: `DATA N CRC32 captureUs` header, then `ceil(20*N/8)` binary bytes. |
| `RAW?` | Read the same saved capture: `BIN N CRC32` header, then `4*N` binary bytes. |

`CAP20` packs the low 20 bits of each sample, least-significant bit first; two
samples occupy five bytes. `RAW?` returns little-endian 32-bit hardware words.
CRC32 IEEE covers the transmitted payload bytes. There is **no extra newline
or marker after the binary payload**. Read exactly the announced length.
`captureUs` measures the capture engine's wait to completion, rounded to
microseconds, rather than transfer time. Rejected commands or invalid captures
return an error instead of a binary block.

## Validation and included files

The detailed [validation report](VALIDATION.md) distinguishes firmware versions
and test conditions. RF measurements were made on v2, with further command,
frequency/gain, and browser checks on v3. The v4 checks covered the build,
flashing, startup at 1 Mbaud, command communication, and frontend regression
checks. They are not a new RF qualification or proof of lossless long transfers
at 1 Mbaud. A complete comparison of both boards was not finished.

| File | Purpose |
| --- | --- |
| [ESP8266_WebSDR/ESP8266_WebSDR.ino](ESP8266_WebSDR/ESP8266_WebSDR.ino) | Arduino firmware source. |
| [ESP8266_WebSDR.html](ESP8266_WebSDR.html) | Standalone browser application. |
| [VALIDATION.md](VALIDATION.md) | Detailed test results and limitations; currently in French. |
| [validation/firmware_v4_1M.bin](validation/firmware_v4_1M.bin) | Reference compiled v4 firmware for the documented configuration. |
| [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) | Upstream attribution and bundled license notices. |
| [SHA256SUMS.txt](SHA256SUMS.txt) | Checksums for the packaged files. |

## Upstream and licenses

The web interface is derived from
[ESPARGOS/esp-web-sdr](https://github.com/ESPARGOS/esp-web-sdr), using upstream
commit `6995ed78915a034be54be68abf986d608d7624c4` as the reference for this port.
Its layout, styles, and embedded Carlito fonts are retained, with ESP8266
controls/transport and the **ESPBoOm8266** header logo. Original attribution
links remain in the page footer.

ESPARGOS licenses ESP-WebSDR under **GNU GPL version 3 or later**. Bundled
third-party assets retain their own notices, including the font's SIL Open
Font License. See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for the
included license text and attribution.


