#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <math.h>
#include <stdlib.h>

extern "C" {
#include "user_interface.h"
extern char _data_start, _bss_start, _bss_end;
void ram_pbus_debugmode(void);
void rom_pbus_force_test(uint32_t, uint32_t, uint32_t);
uint32_t rom_pbus_rd(uint32_t, uint32_t);
void pbus_set_rxbbgain(int);
extern uint8_t chip6_phy_init_ctrl[];
void set_rf_freq_offset(int xtal, int mhz, int offset);
}

// ESP8266 Arduino 3.1.2, NONOSDK22x_190703, CPU 80 MHz.
// Aucun RFTest ni linker personnalisé. Vérifier le placement à chaque build.
// API réservée à setup()/loop() : ni ISR, ni callback, ni appel concurrent.
struct CaptureStorage {
  volatile uint32_t words[2048];
  volatile uint32_t guard[16];
};
CaptureStorage iqStorage __attribute__((section(".data"), used, aligned(4))) =
    {{0x49514350}, {0x47554152}};
static_assert(sizeof(CaptureStorage) == 0x2040, "Taille SRAM inattendue");

enum class IqStatus : uint8_t {
  NotReady = 0, Ok, BadSize, Busy, BadContext, WrongClock, MemoryLayout,
  RadioInit, ProfileMismatch, Timeout, StaleDone, CountMismatch,
  NotWritten, Overrun, DisarmFailed
};

struct IqResult {
  IqStatus code;
  uint32_t sequence, requested;
  uint32_t armed, done, status, after, cycles;
  uint32_t changed, beyond, guards;
  bool sawLow, valid;
  int64_t sumA, sumB, energy;
};

static constexpr uint32_t BASE = 0x3ffe8000, CTRL = 0x3ff20e20;
static constexpr uint32_t IQ_STATUS = 0x3ff20e24, LAYOUT = 0x3ff20e4c;
static constexpr uint32_t CPU_CLOCK = 0x3ff00014;
static constexpr uint32_t ARM = 1UL << 31, DONE = 1UL << 18;
static constexpr uint32_t PULSE = 1UL << 19, TIMEOUT_CYCLES = 80000;
static constexpr unsigned MAX_WORDS = 2048, GUARD_WORDS = 16;
static uint32_t saved[MAX_WORDS];
static bool ready = false, busy = false, lastValid = false;
static uint16_t lastWords = 0;
static uint32_t sequence = 0;
static IqResult result = {};
// Keep additional writable globals zero-initialized (.bss). iqStorage must
// remain the first initialized object at 0x3FFE8000 in the final linker map.
static uint32_t serialBaud = 0;
static uint32_t configuredPaceUs = 0;
static uint8_t activeChannel = 0, manualGain = 0;
static uint32_t lastPbus21 = 0, lastPbus31 = 0, lastPbus32 = 0;
static bool pbusKnown = false;
static uint32_t transmitErrors = 0;
static uint8_t receiverMac[6] = {};
static bool macKnown = false;
// Private PLL path qualified by captured RF shifts on the reference board.
// Frequency/gain units and the limits of that qualification are in VALIDATION.md.
static constexpr bool ENABLE_FINE_TUNE = true;
static uint16_t activeFrequencyMHz = 0, requestedFrequencyMHz = 0;
static int32_t activePllOffset = 0;
// Set BEFORE a private PLL call. A failed attempt must not let a later
// same-channel command skip the explicit zero-offset restoration.
static bool pllOffsetNeedsReset = false;

struct PayloadCursor {
  uint16_t word;
  uint8_t held;
  uint32_t accumulator;
};

// Prototypes explicites : évitent les ambiguïtés du préprocesseur Arduino.
bool iqBegin(uint8_t channel);
bool iqTuneFrequency(uint16_t mhz);
bool iqSetGain(uint8_t index);
bool IRAM_ATTR iqCapture(uint16_t n);
const uint32_t *iqSamples();
uint16_t iqCount();
const IqResult &iqLastResult();
int16_t iqSigned10(uint32_t value);
void iqDecode(uint32_t word, int16_t &a, int16_t &b);
static bool takeOperation();
static void invalidate(IqStatus code);
static bool memoryLayoutValid();
static uint32_t expectedGainBank1(uint8_t index);
static bool profileMatches(uint8_t index);
static bool profileIsValid();
static bool applyProfile();
static uint16_t currentFrequency();
static uint32_t effectivePaceUs();
static void finishTextChunk();
static void printStatus();
static uint32_t crc32Byte(uint32_t crc, uint8_t value);
static size_t fillPayload(PayloadCursor &cursor, uint16_t n, bool raw,
                          uint8_t *bytes, size_t capacity);
static uint32_t payloadCrc(uint16_t n, bool raw);
static bool sendPayload(uint16_t n, bool raw);
static void capturePacket(uint16_t n);
static void rawPacket();
static int32_t zoomRoundQ30(int64_t value);
static void zoomFft(int32_t *re, int32_t *im, uint16_t n);
static void zoomSpectrum(uint16_t nSamples, uint16_t nFft,
                         uint16_t firstBin, uint16_t binCount,
                         int32_t *re, int32_t *im, float *power,
                         uint8_t *payload, uint16_t &clipped);
static void zoomPacket(uint16_t nSamples, uint16_t nFft,
                       uint16_t firstBin, uint16_t binCount);
static bool parseUnsigned(const char *text, uint32_t &value);
static void handleCommand(char *line);
void setup();
void loop();

static inline __attribute__((always_inline)) uint32_t rd(uint32_t address) {
  __asm__ volatile("memw" ::: "memory");
  return *reinterpret_cast<volatile uint32_t *>(address);
}
static inline __attribute__((always_inline)) void wr(uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(address) = value;
  __asm__ volatile("memw" ::: "memory");
}
static inline __attribute__((always_inline)) uint32_t cycleCount() {
  uint32_t value;
  __asm__ volatile("rsr.ccount %0" : "=a"(value));
  return value;
}
static inline __attribute__((always_inline)) uint32_t processorStatus() {
  uint32_t value;
  __asm__ volatile("rsr.ps %0" : "=a"(value));
  return value;
}

static void invalidate(IqStatus code) {
  lastValid = false;
  lastWords = 0;
  result = {};
  result.code = code;
}

static bool takeOperation() {
  // Ne pas modifier les diagnostics d'une opération déjà en cours.
  if (busy) return false;
  if (processorStatus() & 0x1f) {
    invalidate(IqStatus::BadContext);
    return false;
  }
  busy = true; // Tous les appelants autorisés partagent le contexte principal.
  return true;
}

static bool memoryLayoutValid() {
  return uintptr_t(&_data_start) == BASE
      && uintptr_t(&iqStorage.words[0]) == BASE
      && uintptr_t(&iqStorage.guard[0]) == BASE + 0x2000
      && uintptr_t(&_bss_start) >= BASE + sizeof(iqStorage)
      && uintptr_t(saved) >= BASE + sizeof(iqStorage)
      && uintptr_t(saved) + sizeof(saved) <= uintptr_t(&_bss_end);
}

int16_t iqSigned10(uint32_t value) {
  value &= 0x3ffu;
  return int16_t(value < 512 ? int32_t(value) : int32_t(value) - 1024);
}

void iqDecode(uint32_t word, int16_t &a, int16_t &b) {
  a = iqSigned10(word);
  b = iqSigned10(word >> 10);
}

const uint32_t *iqSamples() { return lastValid ? saved : nullptr; }
uint16_t iqCount() { return lastValid ? lastWords : 0; }
const IqResult &iqLastResult() { return result; }

static uint32_t expectedGainBank1(uint8_t index) {
  // RXBB coarse-gain fields, in hexadecimal; six fine steps per group.
  switch (index / 6) {
    case 0: return 0x00;
    case 1: return 0x40;
    case 2: return 0x60;
    case 3: return 0x70;
    default: return 0x78;
  }
}

static bool profileMatches(uint8_t index) {
  // Called with normal IRQ state and an initialized, awake radio.
  lastPbus21 = rom_pbus_rd(2, 1);
  lastPbus31 = rom_pbus_rd(3, 1);
  lastPbus32 = rom_pbus_rd(3, 2);
  pbusKnown = true;
  return index <= 29
      && lastPbus21 == 0x1c6
      && lastPbus31 == expectedGainBank1(index)
      && lastPbus32 == (6u | (uint32_t(index % 6) << 3))
      && rd(LAYOUT) == 0x03020100;
}

static bool profileIsValid() {
  return profileMatches(manualGain);
}

static bool applyProfile() {
  // Primitives SDK/ROM : certaines attentes internes ne sont pas bornées.
  // Les appeler seulement après réveil et initialisation normale du SDK.
  ram_pbus_debugmode();
  rom_pbus_force_test(2, 1, 0x1c6);
  rom_pbus_force_test(3, 1, 0);
  rom_pbus_force_test(3, 2, 6);
  // Gain zero preserves the exact PBUS writes of the validated v4 boot.
  // Gain indices are not calibrated dB; high indices can clip the receiver.
  if (manualGain != 0) pbus_set_rxbbgain(int(manualGain));
  wr(LAYOUT, 0x03020100);
  return profileIsValid();
}

bool iqBegin(uint8_t channel) {
  if (!takeOperation()) return false;
  invalidate(IqStatus::NotReady);
  ready = false;
  if (!memoryLayoutValid()) result.code = IqStatus::MemoryLayout;
  else if (channel < 1 || channel > 11) result.code = IqStatus::RadioInit;
  else {
    system_update_cpu_freq(80);
    if (rd(CPU_CLOCK) & 1) result.code = IqStatus::WrongClock;
    else {
      WiFi.persistent(false);
      wifi_station_set_auto_connect(0);
      if (!WiFi.mode(WIFI_STA)) result.code = IqStatus::RadioInit;
      else {
        wifi_station_disconnect();
        wifi_set_sleep_type(NONE_SLEEP_T);
        if (!wifi_set_channel(channel) || wifi_get_channel() != channel)
          result.code = IqStatus::RadioInit;
        else {
          delay(100);
          if (!applyProfile()) result.code = IqStatus::ProfileMismatch;
          else {
            wr(CTRL, rd(CTRL) & ~ARM);
            ready = !(rd(CTRL) & ARM);
            result.code = ready ? IqStatus::Ok : IqStatus::DisarmFailed;
            if (ready) activeChannel = channel;
          }
        }
      }
    }
  }
  busy = false;
  return ready;
}

bool iqSetGain(uint8_t index) {
  if (!takeOperation()) return false;
  invalidate(IqStatus::NotReady);
  if (!ready) {}
  else if (index > 29) result.code = IqStatus::ProfileMismatch;
  else {
    wr(CTRL, rd(CTRL) & ~ARM);
    if (rd(CTRL) & ARM) {
      ready = false;
      result.code = IqStatus::DisarmFailed;
    } else {
      ram_pbus_debugmode();
      pbus_set_rxbbgain(int(index));
      // The requested value is retained for diagnosis/reinitialization if
      // the readback fails; ready=false prevents capture with a bad profile.
      manualGain = index;
      ready = profileIsValid();
      result.code = ready ? IqStatus::Ok : IqStatus::ProfileMismatch;
    }
  }
  const bool ok = result.code == IqStatus::Ok;
  busy = false;
  return ok;
}

bool iqTuneFrequency(uint16_t mhz) {
  // iqBegin takes its own operation guard. All authorized callers share the
  // principal context; no other command runs during this synchronous call.
  if (busy) return false;
  if (processorStatus() & 0x1f) {
    invalidate(IqStatus::BadContext);
    return false;
  }
  if (mhz < 2412 || mhz > 2462
      || (!ENABLE_FINE_TUNE && (mhz - 2412) % 5)) {
    invalidate(IqStatus::RadioInit);
    return false;
  }
  requestedFrequencyMHz = mhz;
  if (ready && mhz == activeFrequencyMHz && profileIsValid()) return true;

  const uint8_t channel = uint8_t(1u + (uint32_t(mhz) - 2412u + 2u) / 5u);
  const int baseMHz = 2407 + 5 * int(channel);
  const int offset = (int(mhz) - baseMHz) * 1024;
  if (channel < 1 || channel > 11 || offset < -2048 || offset > 2048) {
    invalidate(IqStatus::RadioInit);
    return false;
  }

  // Always restore the normal SDK channel path when changing frequency,
  // including fine-to-fine changes that share the same nearest channel.
  // iqBegin reapplies the chosen gain but does not publish a new MHz value.
  const bool resetPreviousOffset = pllOffsetNeedsReset;
  if (!iqBegin(channel)) return false;
  if (offset == 0 && !resetPreviousOffset) {
    // Standard center -> standard center uses the original v4 path only.
    activeFrequencyMHz = mhz;
    activePllOffset = 0;
    return true;
  }

  if (!takeOperation()) return false;
  invalidate(IqStatus::NotReady);
  ready = false;
  const uint8_t xtal = chip6_phy_init_ctrl[1];
  if (!memoryLayoutValid()) result.code = IqStatus::MemoryLayout;
  else if (rd(CPU_CLOCK) & 1) result.code = IqStatus::WrongClock;
  else if (xtal > 2) result.code = IqStatus::RadioInit;
  else {
    wr(CTRL, rd(CTRL) & ~ARM);
    if (rd(CTRL) & ARM) result.code = IqStatus::DisarmFailed;
    else {
      pllOffsetNeedsReset = true;
      // SDK ABI: selector 0=40MHz, 1=26MHz, 2=24MHz; signed offset units
      // are 1/1024 MHz. CPU 80MHz is unrelated to the crystal selector.
      // This void function has internal waits. Return does not prove RF lock.
      set_rf_freq_offset(int(xtal), baseMHz, offset);
      delay(5);
      if (!applyProfile()) result.code = IqStatus::ProfileMismatch;
      else {
        wr(CTRL, rd(CTRL) & ~ARM);
        if (rd(CTRL) & ARM) result.code = IqStatus::DisarmFailed;
        else if (!memoryLayoutValid()) result.code = IqStatus::MemoryLayout;
        else if (rd(CPU_CLOCK) & 1) result.code = IqStatus::WrongClock;
        else {
          ready = true;
          result.code = IqStatus::Ok;
          // Commit only after all software guards. These are programmed
          // settings, not a calibrated measurement of the actual LO.
          activeFrequencyMHz = mhz;
          activePllOffset = offset;
          pllOffsetNeedsReset = offset != 0;
        }
      }
    }
  }
  busy = false;
  return ready;
}

bool IRAM_ATTR iqCapture(uint16_t n) {
  if (!takeOperation()) return false;
  invalidate(IqStatus::NotReady);
  result.requested = n;
  if (!ready) {}
  else if (n < 2 || n > MAX_WORDS) result.code = IqStatus::BadSize;
  else if (!memoryLayoutValid()) result.code = IqStatus::MemoryLayout;
  else if (rd(CPU_CLOCK) & 1) result.code = IqStatus::WrongClock;
  else if (!profileIsValid()) result.code = IqStatus::ProfileMismatch;
  else {
    // Désarmer AVANT de préparer la région réservée. Aucun autre propriétaire.
    wr(CTRL, rd(CTRL) & ~ARM);
    if (rd(CTRL) & ARM) result.code = IqStatus::DisarmFailed;
    else {
      result.sequence = ++sequence;
      const uint32_t sentinel = 0x5aa50000 ^ ((sequence & 0xfffu) << 4);
      for (unsigned i = 0; i < MAX_WORDS; ++i)
        wr(BASE + i * 4, sentinel ^ i);
      for (unsigned i = 0; i < GUARD_WORDS; ++i)
        iqStorage.guard[i] = 0xcafe0000 ^ sequence ^ i;

      uint32_t ps;
      __asm__ volatile("rsil %0, 15\n\trsync" : "=a"(ps) :: "memory");
      wr(CTRL, (uint32_t(n) + 1) | 0x8000);
      wr(CTRL, rd(CTRL) | ARM);
      const uint32_t armed = rd(CTRL), start = cycleCount();
      wr(CTRL, rd(CTRL) | PULSE);
      wr(CTRL, rd(CTRL) & ~PULSE);
      uint32_t done = rd(CTRL);
      bool sawLow = !(armed & DONE) || !(done & DONE);
      while (!(done & DONE) && uint32_t(cycleCount() - start) < TIMEOUT_CYCLES) {
        sawLow = true;
        done = rd(CTRL);
      }
      const uint32_t elapsed = cycleCount() - start;
      const uint32_t status = rd(IQ_STATUS);
      wr(CTRL, rd(CTRL) & ~ARM);
      const uint32_t after = rd(CTRL);
      for (unsigned i = 0; i < n; ++i) saved[i] = rd(BASE + i * 4);
      __asm__ volatile("wsr.ps %0\n\trsync" :: "a"(ps) : "memory");

      result.armed = armed;
      result.done = done;
      result.status = status;
      result.after = after;
      result.cycles = elapsed;
      result.sawLow = sawLow;
      // Le moteur est arrêté ; la copie privée est stable jusqu'au prochain appel.
      // Un masque IRQ n'arrête pas un DMA : bornes et gardes restent contrôlées.
      for (unsigned i = 0; i < n; ++i) {
        result.changed += saved[i] != (sentinel ^ i);
        int16_t a, b;
        iqDecode(saved[i], a, b);
        result.sumA += a;
        result.sumB += b;
        result.energy += int32_t(a) * a + int32_t(b) * b;
      }
      for (unsigned i = n; i < MAX_WORDS; ++i)
        result.beyond += rd(BASE + i * 4) != (sentinel ^ i);
      for (unsigned i = 0; i < GUARD_WORDS; ++i)
        result.guards += iqStorage.guard[i] != (0xcafe0000 ^ sequence ^ i);

      if (after & ARM) result.code = IqStatus::DisarmFailed;
      else if (!(done & DONE)) result.code = IqStatus::Timeout;
      else if (!sawLow) result.code = IqStatus::StaleDone;
      else if (((status >> 16) & 0x3fff) != uint32_t(n) + 1
               || (status & 0x3fff) != n) result.code = IqStatus::CountMismatch;
      else if (result.beyond || result.guards) result.code = IqStatus::Overrun;
      else if (result.changed != n) result.code = IqStatus::NotWritten;
      else {
        result.code = IqStatus::Ok;
        result.valid = lastValid = true;
        lastWords = n;
      }
    }
  }
  busy = false;
  return lastValid;
}

static uint16_t currentFrequency() {
  return activeFrequencyMHz;
}

static uint32_t effectivePaceUs() {
  return serialBaud > 115200 ? configuredPaceUs : 0;
}

static void finishTextChunk() {
  Serial.flush();
  const uint32_t pause = effectivePaceUs();
  if (pause >= 1000) delay(pause / 1000);
  if (pause % 1000) delayMicroseconds(pause % 1000);
  yield();
}

static void printStatus() {
  const bool profile = ready && profileIsValid();
  // Keep every formatted write below 100 bytes, including worst-case numbers.
  // The tested USB/UART route lost bytes at high baud without pacing.
  // Flush/pace between fragments; exactly one LF terminates the JSON object.
  Serial.printf("STAT {\"seq\":%u,\"n\":%u,\"valid\":%s,\"code\":%u,",
    result.sequence, result.requested, result.valid ? "true" : "false",
    unsigned(result.code));
  finishTextChunk();
  Serial.printf("\"ready\":%s,\"profile\":%s,\"guards\":%u,\"beyond\":%u,",
    ready ? "true" : "false", profile ? "true" : "false", result.guards, result.beyond);
  finishTextChunk();
  Serial.printf("\"changed\":%u,\"cycles\":%u,\"low\":%u,",
    result.changed, result.cycles, unsigned(result.sawLow));
  finishTextChunk();
  Serial.printf("\"armed\":%u,\"done\":%u,\"status\":%u,\"after\":%u,",
    result.armed, result.done, result.status, result.after);
  finishTextChunk();
  Serial.printf("\"A\":%lld,\"B\":%lld,\"E\":%lld,", result.sumA, result.sumB, result.energy);
  finishTextChunk();
  Serial.printf("\"heap\":%u,\"freq\":%u,\"gain\":%u,",
    ESP.getFreeHeap(), unsigned(currentFrequency()), unsigned(manualGain));
  finishTextChunk();
  Serial.printf("\"requestedFreq\":%u,\"baseChannel\":%u,\"xtal\":%u,\"offset1024\":%d,",
    unsigned(requestedFrequencyMHz), unsigned(activeChannel),
    unsigned(chip6_phy_init_ctrl[1]), int(activePllOffset));
  finishTextChunk();
  Serial.printf("\"pllOffsetNeedsReset\":%s,\"extfreq\":%s,",
    pllOffsetNeedsReset ? "true" : "false", ENABLE_FINE_TUNE ? "true" : "false");
  finishTextChunk();
  Serial.printf("\"pbus\":[%u,%u,%u],\"pbusKnown\":%s,",
    lastPbus21, lastPbus31, lastPbus32, pbusKnown ? "true" : "false");
  finishTextChunk();
  Serial.printf("\"data\":\"0x%08x\",\"saved\":\"0x%08x\",",
    unsigned(uintptr_t(&iqStorage.words[0])), unsigned(uintptr_t(saved)));
  finishTextChunk();
  Serial.printf("\"layout\":%u,\"cpuReg\":%u,\"baud\":%u,\"txErrors\":%u,",
    rd(LAYOUT), rd(CPU_CLOCK), serialBaud, transmitErrors);
  finishTextChunk();
  Serial.printf("\"paceUs\":%u,\"paceConfiguredUs\":%u,",
    effectivePaceUs(), configuredPaceUs);
  finishTextChunk();
  Serial.printf("\"macKnown\":%s,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",
    macKnown ? "true" : "false", unsigned(receiverMac[0]), unsigned(receiverMac[1]),
    unsigned(receiverMac[2]), unsigned(receiverMac[3]), unsigned(receiverMac[4]),
    unsigned(receiverMac[5]));
  finishTextChunk();
}

static uint32_t crc32Byte(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (unsigned bit = 0; bit < 8; ++bit)
    crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  return crc;
}

static size_t fillPayload(PayloadCursor &cursor, uint16_t n, bool raw,
                          uint8_t *bytes, size_t capacity) {
  size_t used = 0;
  while (used < capacity) {
    if (cursor.held < 8) {
      if (cursor.word < n) {
        // CAP20: append one 20-bit A/B word to a little-endian bit stream.
        // RAW: append an unmodified 32-bit word. held is always zero then.
        const uint32_t word = saved[cursor.word++];
        cursor.accumulator |= (raw ? word : word & 0xfffffu) << cursor.held;
        cursor.held += raw ? 32 : 20;
      } else {
        if (cursor.held) {
          // An odd CAP20 sample count leaves four low bits; high bits are zero.
          bytes[used++] = uint8_t(cursor.accumulator);
          cursor.accumulator = 0;
          cursor.held = 0;
        }
        break;
      }
    }
    bytes[used++] = uint8_t(cursor.accumulator);
    cursor.accumulator >>= 8;
    cursor.held -= 8;
  }
  return used;
}

static uint32_t payloadCrc(uint16_t n, bool raw) {
  PayloadCursor cursor = {};
  uint8_t bytes[128];
  uint32_t crc = 0xffffffffu;
  size_t count;
  while ((count = fillPayload(cursor, n, raw, bytes, sizeof(bytes))) != 0) {
    for (size_t i = 0; i < count; ++i) crc = crc32Byte(crc, bytes[i]);
    yield();
  }
  return crc ^ 0xffffffffu;
}

static bool sendPayload(uint16_t n, bool raw) {
  PayloadCursor cursor = {};
  uint8_t bytes[64];
  size_t count;
  while ((count = fillPayload(cursor, n, raw, bytes, sizeof(bytes))) != 0) {
    if (Serial.write(bytes, count) != count) {
      ++transmitErrors;
      // Do not insert an error line inside an already-started binary packet.
      // The receiver detects truncation, times out and sends a fresh SYNC.
      return false;
    }
    // No printing here: only wait for the binary chunk to drain, then pace.
    finishTextChunk();
  }
  // Protocol contract: no newline, END marker or other byte after the payload.
  return true;
}

static void capturePacket(uint16_t n) {
  if (!iqCapture(n)) {
    Serial.printf("ERR capture %u\n", unsigned(result.code));
    return;
  }
  const uint32_t crc = payloadCrc(lastWords, false);
  // cycles measures trigger-to-DONE at the checked CPU clock of 80 MHz.
  // It does not include validation, encoding or UART transfer.
  const uint32_t captureUs = (result.cycles + 79u) / 80u;
  Serial.printf("DATA %u %08x %u\n", unsigned(lastWords), crc, captureUs);
  sendPayload(lastWords, false);
}

static void rawPacket() {
  if (!lastValid || !lastWords) {
    Serial.print("ERR no valid capture\n");
    return;
  }
  const uint32_t crc = payloadCrc(lastWords, true);
  Serial.printf("BIN %u %08x\n", unsigned(lastWords), crc);
  sendPayload(lastWords, true);
}

// ---- Optional post-capture spectrum: ZOOMFFT / ZCAP -------------------------
// The legacy INFO identity, CAP20/RAW? packets and acquisition routines remain
// unchanged. ZCAP is one request/one reply; it never enables a persistent mode.
//
// ZCAP <samples> <fft> <first> <count>
//   fft = 512/1024/2048; samples <= 2048, a positive multiple of fft.
//   first/count select bins in the browser's FFTshift order (Q is negated).
// ZDATA1 <samples> <fft> <first> <count> <seq> <crc> <captureUs> <processUs> <clips>\n
//   followed by exactly count bytes, with no trailing delimiter.
//   dBFS10 = -140 + byte * 5/8. Values are rounded to nearest, then clamped;
//   code 0 also represents everything below the encoding floor.
//   CRC32 covers the payload only, as in CAP20. processUs covers post-capture
//   allocation, DSP and CRC, excluding the UART reply; clips counts pairs.
//
// No allocation or DSP occurs until iqCapture() has returned successfully.
// Its stable saved[] copy is read-only here. The motor is disarmed and the
// normal interrupt state restored. All pacing stays at its configured value.
// Keep constants in flash: no initialized writable global may precede iqStorage.
static const int32_t ZOOM_FFT_ROOTS[11][2] PROGMEM = {
  {-1073741824, 0},          // exp(-j*2*pi/2), Q30
  {0, -1073741824},          // 4
  {759250125, -759250125},    // 8
  {992008094, -410903207},    // 16
  {1053110176, -209476638},   // 32
  {1068571464, -105245103},   // 64
  {1072448455, -52686014},    // 128
  {1073418433, -26350943},    // 256
  {1073660973, -13176464},    // 512
  {1073721611, -6588356},     // 1024
  {1073736771, -3294193},     // 2048
};
static const int32_t ZOOM_HANN_ROOTS[3][2] PROGMEM = {
  {1073660656, 13202248},     // exp(j*2*pi/(512-1)), Q30
  {1073721572, 6594796},      // 1024
  {1073736766, 3295803},      // 2048
};

static int32_t zoomRoundQ30(int64_t value) {
  // Division (rather than a signed right shift) defines negative rounding too.
  const int64_t half = int64_t(1) << 29;
  return int32_t((value + (value < 0 ? -half : half)) / (int64_t(1) << 30));
}

static void zoomFft(int32_t *re, int32_t *im, uint16_t n) {
  for (unsigned i = 1, j = 0; i < n; ++i) {
    unsigned bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      const int32_t r = re[i], q = im[i];
      re[i] = re[j]; im[i] = im[j];
      re[j] = r; im[j] = q;
    }
  }
  unsigned stage = 0;
  for (unsigned len = 2; len <= n; len <<= 1, ++stage) {
    const int32_t cr = int32_t(pgm_read_dword(&ZOOM_FFT_ROOTS[stage][0]));
    const int32_t ci = int32_t(pgm_read_dword(&ZOOM_FFT_ROOTS[stage][1]));
    int32_t wr = int32_t(1) << 30, wi = 0;
    const unsigned half = len >> 1;
    // Generate each twiddle only once per stage, then reuse across its groups.
    for (unsigned j = 0; j < half; ++j) {
      for (unsigned k = j; k < n; k += len) {
        const unsigned l = k + half;
        const int32_t tr = zoomRoundQ30(int64_t(re[l]) * wr - int64_t(im[l]) * wi);
        const int32_t ti = zoomRoundQ30(int64_t(re[l]) * wi + int64_t(im[l]) * wr);
        const int32_t ar = re[k], ai = im[k];
        // Scale every stage by 1/2. Wide intermediates avoid signed overflow.
        re[k] = int32_t((int64_t(ar) + tr) / 2);
        im[k] = int32_t((int64_t(ai) + ti) / 2);
        re[l] = int32_t((int64_t(ar) - tr) / 2);
        im[l] = int32_t((int64_t(ai) - ti) / 2);
      }
      const int32_t next = zoomRoundQ30(int64_t(wr) * cr - int64_t(wi) * ci);
      wi = zoomRoundQ30(int64_t(wr) * ci + int64_t(wi) * cr);
      wr = next;
    }
    yield(); // Post-capture work only; never inside the acquisition IRQ mask.
  }
}

static void zoomSpectrum(uint16_t nSamples, uint16_t nFft,
                         uint16_t firstBin, uint16_t binCount,
                         int32_t *re, int32_t *im, float *power,
                         uint8_t *payload, uint16_t &clipped) {
  const int32_t scale = int32_t(1) << 20;
  const int32_t one = int32_t(1) << 30;
  const unsigned windowIndex = nFft == 512 ? 0 : nFft == 1024 ? 1 : 2;
  const int32_t cr = int32_t(pgm_read_dword(&ZOOM_HANN_ROOTS[windowIndex][0]));
  const int32_t ci = int32_t(pgm_read_dword(&ZOOM_HANN_ROOTS[windowIndex][1]));
  const unsigned blocks = nSamples / nFft;
  memset(power, 0, size_t(binCount) * sizeof(*power));
  clipped = 0;
  for (unsigned block = 0; block < blocks; ++block) {
    const unsigned base = block * nFft;
    int32_t sumA = 0, sumB = 0;
    for (unsigned j = 0; j < nFft; ++j) {
      int16_t a, b;
      iqDecode(saved[base + j], a, b);
      sumA += a; sumB += b;
      if (a <= -256 || a >= 255 || b <= -256 || b >= 255) ++clipped;
    }
    const int32_t meanA = int32_t(int64_t(sumA) * scale / nFft);
    const int32_t meanB = int32_t(int64_t(sumB) * scale / nFft);
    int32_t wr = one, wi = 0;
    for (unsigned j = 0; j < nFft / 2; ++j) {
      int32_t window = int32_t((int64_t(one) - wr) / 2);
      if (window < 0) window = 0;
      if (window > one) window = one;
      // Symmetric Hann, denominator n-1, and unwindowed mean removal match JS.
      // Fill mirrored samples together: fewer rotations and exactly symmetric
      // coefficients, without allocating a separate window table.
      for (unsigned side = 0; side < 2; ++side) {
        const unsigned k = side ? nFft - 1 - j : j;
        int16_t a, b;
        iqDecode(saved[base + k], a, b);
        re[k] = zoomRoundQ30((int64_t(a) * scale - meanA) * window);
        im[k] = zoomRoundQ30((int64_t(meanB) - int64_t(b) * scale) * window);
      }
      const int32_t next = zoomRoundQ30(int64_t(wr) * cr - int64_t(wi) * ci);
      wi = zoomRoundQ30(int64_t(wr) * ci + int64_t(wi) * cr);
      wr = next;
      if ((j & 127u) == 127u) yield();
    }
    zoomFft(re, im, nFft);
    for (unsigned j = 0; j < binCount; ++j) {
      const unsigned bin = (firstBin + j + nFft / 2) % nFft;
      const int64_t r = re[bin], q = im[bin];
      power[j] += float(uint64_t(r * r) + uint64_t(q * q));
    }
    yield();
  }
  // The FFT has been divided by n. Restore its normalization and divide the
  // original signed-10 counts by 512, then by sum(Hann) = (n-1)/2.
  const float normalized = float(nFft) / (512.0f * float(scale) * (float(nFft - 1) * 0.5f));
  const float powerScale = normalized * normalized / float(blocks);
  for (unsigned j = 0; j < binCount; ++j) {
    const float p = power[j] * powerScale;
    const float db = p > 1e-14f ? 10.0f * log10f(p) : -140.0f;
    int32_t encoded = int32_t((db + 140.0f) * 1.6f + 0.5f);
    if (encoded < 0) encoded = 0;
    if (encoded > 255) encoded = 255;
    payload[j] = uint8_t(encoded);
    if ((j & 63u) == 63u) yield();
  }
}

static void zoomPacket(uint16_t nSamples, uint16_t nFft,
                       uint16_t firstBin, uint16_t binCount) {
  if (!iqCapture(nSamples)) {
    Serial.printf("ERR capture %u\n", unsigned(result.code));
    return;
  }
  // All new work starts here, AFTER the existing capture has fully completed.
  const uint32_t start = micros();
  const size_t bytes = size_t(nFft) * 2 * sizeof(int32_t)
                     + size_t(binCount) * (sizeof(float) + sizeof(uint8_t));
  // Leave room for SDK/serial work and the normal stack. The buffer is released
  // after this one reply, so legacy commands retain their original heap budget.
  if (ESP.getFreeHeap() < bytes + 8192u) {
    Serial.print("ERR ZCAP memory\n");
    return;
  }
  void *storage = malloc(bytes);
  if (!storage) {
    Serial.print("ERR ZCAP memory\n");
    return;
  }
  int32_t *re = static_cast<int32_t *>(storage), *im = re + nFft;
  float *power = reinterpret_cast<float *>(im + nFft);
  uint8_t *payload = reinterpret_cast<uint8_t *>(power + binCount);
  uint16_t clipped = 0;
  zoomSpectrum(nSamples, nFft, firstBin, binCount, re, im, power, payload, clipped);
  uint32_t crc = 0xffffffffu;
  for (unsigned j = 0; j < binCount; ++j) {
    crc = crc32Byte(crc, payload[j]);
    if ((j & 127u) == 127u) yield();
  }
  crc ^= 0xffffffffu;
  const uint32_t processUs = micros() - start;
  const uint32_t captureUs = (result.cycles + 79u) / 80u;
  // Below 100 bytes even for the largest decimal uint32_t diagnostics.
  Serial.printf("ZDATA1 %u %u %u %u %u %08x %u %u %u\n",
    unsigned(nSamples), unsigned(nFft), unsigned(firstBin), unsigned(binCount),
    result.sequence, crc, captureUs, processUs, unsigned(clipped));
  for (unsigned offset = 0; offset < binCount;) {
    const size_t count = binCount - offset < 64 ? binCount - offset : 64;
    if (Serial.write(payload + offset, count) != count) {
      ++transmitErrors;
      break; // No diagnostic inserted into a binary reply; host uses SYNC.
    }
    offset += count;
    finishTextChunk(); // Same flush, chunk size and configured pause as CAP20.
  }
  free(storage);
  // No suffix, and no persistent streaming/zoom state.
}

static bool parseUnsigned(const char *text, uint32_t &value) {
  if (!text || !*text) return false;
  uint32_t parsed = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint32_t digit = uint32_t(*p - '0');
    if (parsed > (0xffffffffu - digit) / 10u) return false;
    parsed = parsed * 10u + digit;
  }
  value = parsed;
  return true;
}

static void handleCommand(char *line) {
  // SYNC echo is byte-exact for one printable token, followed by LF only.
  if (!strncmp(line, "SYNC ", 5)) {
    const char *token = line + 5;
    if (!*token || strchr(token, ' ') || strchr(token, '\t')) {
      Serial.print("ERR SYNC token\n");
      return;
    }
    Serial.write(reinterpret_cast<const uint8_t *>(line), strlen(line));
    Serial.write(uint8_t('\n'));
    return;
  }

  char *tokens[5];
  unsigned count = 0;
  char *p = line;
  while (*p) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) break;
    if (count == 5) {
      Serial.print("ERR too many arguments\n");
      return;
    }
    tokens[count++] = p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    if (*p) *p++ = 0;
  }
  if (!count) return;

  uint32_t value = 0, rate = 0;
  if (!strcmp(tokens[0], "INFO") && count == 1) {
    // Identity contract: spaces around "burst", and the actual sample limit.
    // INFO identifies the protocol even after init failure; STAT? exposes it.
    Serial.print("ESP8266SDR 6 burst 2048\n");
  } else if (!strcmp(tokens[0], "CAPS") && count == 1) {
    Serial.print(ENABLE_FINE_TUNE ? "CAPS RXLIMITS UARTBAUD GAIN TUNEEXT ZOOMFFT\n"
                                 : "CAPS RXLIMITS UARTBAUD GAIN ZOOMFFT\n");
  } else if (!strcmp(tokens[0], "RANGE?") && count == 1 && ENABLE_FINE_TUNE) {
    Serial.print("RANGE 2412 2462 1\n");
  } else if (!strcmp(tokens[0], "LIMITS?") && count == 1) {
    Serial.print("LIMITS {\"gain\":[0,29,1],\"bandwidth\":null,"
                 "\"rates\":[40000000],\"bits\":[10]}\n");
  } else if (!strcmp(tokens[0], "TRANSPORT?") && count == 1) {
    Serial.printf("TRANSPORT UART %u\n", serialBaud);
  } else if (!strcmp(tokens[0], "GAIN?") && count == 1) {
    Serial.printf("GAIN MANUAL %u 0 29\n", unsigned(manualGain));
  } else if (!strcmp(tokens[0], "STAT?") && count == 1) {
    printStatus();
  } else if (!strcmp(tokens[0], "RAW?") && count == 1) {
    rawPacket();
  } else if (!strcmp(tokens[0], "CAP20") && count == 3
             && parseUnsigned(tokens[1], value) && parseUnsigned(tokens[2], rate)) {
    if (value < 2 || value > MAX_WORDS || rate != 1) {
      invalidate(IqStatus::BadSize);
      result.requested = value;
      Serial.print("ERR CAP20 requires 2..2048 samples and rate 1\n");
    } else {
      capturePacket(uint16_t(value));
    }
  } else if (!strcmp(tokens[0], "ZCAP")) {
    uint32_t n = 0, fft = 0, first = 0, bins = 0;
    if (count != 5 || !parseUnsigned(tokens[1], n)
        || !parseUnsigned(tokens[2], fft) || !parseUnsigned(tokens[3], first)
        || !parseUnsigned(tokens[4], bins)
        || (fft != 512 && fft != 1024 && fft != 2048)
        || n > MAX_WORDS || n < fft || n % fft != 0
        || first >= fft || bins == 0 || bins > fft - first) {
      Serial.print("ERR ZCAP requires samples/FFT 512..2048 and valid bin window\n");
    } else {
      zoomPacket(uint16_t(n), uint16_t(fft), uint16_t(first), uint16_t(bins));
    }
  } else if (!strcmp(tokens[0], "FREQ") && count == 2
             && parseUnsigned(tokens[1], value)) {
    if (value < 2412 || value > 2462
        || (!ENABLE_FINE_TUNE && (value - 2412) % 5)) {
      Serial.print(ENABLE_FINE_TUNE ? "ERR FREQ requires integer 2412..2462 MHz\n"
                                   : "ERR FREQ requires 2412..2462 MHz in steps of 5\n");
    } else {
      if (!iqTuneFrequency(uint16_t(value)))
        Serial.printf("ERR frequency %u\n", unsigned(result.code));
      else
        Serial.printf("OK FREQ %u\n", value);
    }
  } else if (!strcmp(tokens[0], "GAIN") && count == 3
             && !strcmp(tokens[1], "MANUAL") && parseUnsigned(tokens[2], value)) {
    if (value > 29) Serial.print("ERR GAIN requires MANUAL 0..29\n");
    else if (!iqSetGain(uint8_t(value)))
      Serial.printf("ERR gain %u\n", unsigned(result.code));
    else
      Serial.printf("OK GAIN MANUAL %u\n", value);
  } else if (!strcmp(tokens[0], "PACE") && count == 2
             && parseUnsigned(tokens[1], value)) {
    if (value > 10000) {
      Serial.print("ERR PACE requires 0..10000 microseconds\n");
    } else {
      configuredPaceUs = value;
      Serial.printf("OK PACE %u\n", value);
      finishTextChunk();
    }
  } else if (!strcmp(tokens[0], "BAUD") && count == 2
             && parseUnsigned(tokens[1], value)) {
    if (value != 115200 && value != 1000000 && value != 2000000) {
      Serial.print("ERR BAUD requires 115200, 1000000 or 2000000\n");
    } else {
      Serial.printf("OK BAUD %u\n", value); // ACK at the old rate.
      Serial.flush();
      if (value != serialBaud) {
        delay(30);
        Serial.updateBaudRate(value);
        serialBaud = value;
      }
    }
  } else {
    Serial.print("ERR command\n");
  }
}

void setup() {
  // Match the browser's 1 Mbaud default without closing/reopening the UART:
  // some USB bridges reset this board when the host changes baud by reopening.
  serialBaud = 1000000;
  configuredPaceUs = 3000;
  Serial.begin(serialBaud);
  Serial.setDebugOutput(false);
  delay(500);
  if (iqTuneFrequency(2462)) iqCapture(2048);
  macKnown = wifi_get_macaddr(STATION_IF, receiverMac);
  // ROM boot noise is unavoidable; the host drains it with SYNC.
  // No unsolicited diagnostics or sample dump during the binary protocol.
  Serial.printf("READY ESP8266SDR code=%u\n", unsigned(result.code));
}

void loop() {
  static char line[128];
  static unsigned used = 0;
  static bool overflow = false, invalid = false, sawCR = false;
  while (Serial.available()) {
    const int input = Serial.read();
    if (input < 0) break;
    const uint8_t c = uint8_t(input);
    if (c == '\n') {
      line[used] = 0;
      if (overflow || invalid) Serial.print("ERR invalid command line\n");
      else handleCommand(line);
      used = 0;
      overflow = invalid = sawCR = false;
      yield();
      continue;
    }
    if (c == '\r') {
      if (sawCR) invalid = true;
      sawCR = true; // CR is accepted only immediately before LF.
      continue;
    }
    if (sawCR || (c < 32 && c != '\t') || c > 126) invalid = true;
    if (used < sizeof(line) - 1) line[used++] = char(c);
    else overflow = true;
  }
  delay(1);
}
