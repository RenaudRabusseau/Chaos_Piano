// ============================================================
// CHAOS PIANO — Brain 2: Audio Engine
// Validation v0.2 — Arduino UNO R3
// ------------------------------------------------------------
// Signal Chain:
//   Raw Piezo Disc → MAX9814 Preamp → A0
//   → Distortion → PWM Pin 9 → RC Filter → PAM8610 Left → Speaker L
//
//   DFPlayer Mini (MP3 playback) → PAM8610 Right → Speaker R
// ------------------------------------------------------------
// EFFECT TOGGLES
// Comment out any line to disable that effect.
// Comment ALL out to hear the clean dry signal.
#define ENABLE_DISTORTION
// #define ENABLE_BITCRUSH
// #define ENABLE_FLANGER
// ============================================================

#include <Arduino.h>
#include <SoftwareSerial.h>

// --- PIN DEFINITIONS ---
#define PIEZO_PIN       A0  // MAX9814 OUT → A0
#define PWM_OUT_PIN     9   // PWM → 470Ω → 0.1µF+10µF → PAM8610 Left IN+
#define DFPLAYER_RX     10  // Arduino RX ← DFPlayer TX
#define DFPLAYER_TX     11  // Arduino TX → 1kΩ → DFPlayer RX

// --- SIGNAL CALIBRATION ---
// Baseline ADC value when piezo is untouched.
// Measured at 290 for this specific MAX9814 module.
// Recalibrate if swapping hardware: enable Serial.println(raw)
// in loop() and read the Serial Monitor at rest.
#define SIGNAL_BASELINE 290

// --- NOISE FLOOR ---
// Minimum signal amplitude to process (0-512).
// Raise if background noise bleeds through when untouched.
// Lower if soft strikes are not registering.
#define NOISE_FLOOR     50

// --- DISTORTION ---
// Gain multiplier before hard clipping.
// 2 = mild overdrive, 4 = medium fuzz, 8 = heavy saturation
#define DISTORTION_GAIN 4

// --- BITCRUSH ---
// Target bit depth (1-8). Lower = more crushed.
// 7 = subtle, 5 = crunchy, 3 = extreme lo-fi
#define BITCRUSH_BITS   5

// --- FLANGER ---
// DEPTH = max delay sweep in samples (1-50)
// SPEED = LFO increment per sample (1 = slow, 3 = fast)
#define FLANGER_DEPTH   30
#define FLANGER_SPEED   1
#define FLANGER_BUFFER  64  // Must be power of 2, >= FLANGER_DEPTH * 2

// --- DFPLAYER ---
// 0-30. Raise if ambient audio is too quiet in the mix.
#define DFPLAYER_VOLUME 30

// ============================================================

SoftwareSerial dfPlayerSerial(DFPLAYER_RX, DFPLAYER_TX);

int flangerBuffer[FLANGER_BUFFER];
int flangerIndex  = 0;
int lfoValue      = 0;
int lfoDirection  = 1;

// ============================================================
// DFPLAYER SERIAL PROTOCOL
// MP3-TF-16P V3.0 with MH2024K chip.
// Requires proper checksum and 500ms inter-command delay.
// ============================================================

void dfPlayerSendCommand(byte command, byte param1, byte param2) {
  uint16_t checksum = 0xFFFF
    - (0xFF + 0x06 + command + 0x00 + param1 + param2)
    + 1;

  byte packet[10] = {
    0x7E,
    0xFF,
    0x06,
    command,
    0x00,
    param1,
    param2,
    (byte)(checksum >> 8),
    (byte)(checksum & 0xFF),
    0xEF
  };

  for (int i = 0; i < 10; i++) {
    dfPlayerSerial.write(packet[i]);
  }
  delay(500); // MH2024K requires 500ms between commands
}

void dfPlayerSetVolume(byte volume) {
  dfPlayerSendCommand(0x06, 0x00, volume);
}

void dfPlayerPlayTrack(byte track) {
  dfPlayerSendCommand(0x03, 0x00, track);
}

// ============================================================

void setup() {
  pinMode(PWM_OUT_PIN, OUTPUT);
  analogWrite(PWM_OUT_PIN, 128); // Midpoint = silence, prevents boot thump

  dfPlayerSerial.begin(9600);
  delay(2000);           // MH2024K boot time
  dfPlayerSetVolume(DFPLAYER_VOLUME);
  dfPlayerPlayTrack(1);  // Begin ambient playback on boot
}

void loop() {

  // --- 1. SAMPLE ---
  int raw    = analogRead(PIEZO_PIN);
  int signal = raw - SIGNAL_BASELINE; // Centre around measured baseline
  signal     = -signal;               // Invert: piezo swings negative on hit

  // Noise floor gate
  if (abs(signal) < NOISE_FLOOR) {
    signal = 0;
  }

  // --- 2. DISTORTION ---
  #ifdef ENABLE_DISTORTION
    signal = signal * DISTORTION_GAIN;
    signal = constrain(signal, -512, 511);
  #endif

  // --- 3. BITCRUSH ---
  #ifdef ENABLE_BITCRUSH
    int bitsToRemove = 8 - BITCRUSH_BITS;
    int mask         = ~((1 << bitsToRemove) - 1);
    signal           = signal & mask;
  #endif

  // --- 4. FLANGER ---
  #ifdef ENABLE_FLANGER
    flangerBuffer[flangerIndex] = signal;

    lfoValue += lfoDirection * FLANGER_SPEED;
    if (lfoValue >= FLANGER_DEPTH || lfoValue <= 0) {
      lfoDirection = -lfoDirection;
    }

    int delayedIndex  = (flangerIndex - lfoValue + FLANGER_BUFFER) % FLANGER_BUFFER;
    int delayedSample = flangerBuffer[delayedIndex];
    signal            = (signal + delayedSample) / 2;
    flangerIndex      = (flangerIndex + 1) % FLANGER_BUFFER;
  #endif

  // --- 5. OUTPUT ---
  int output = map(signal, -512, 511, 0, 255);
  analogWrite(PWM_OUT_PIN, output);

  // --- DEBUG ---
  // Uncomment to visualise signal in Serial Plotter (USB only):
  // Serial.println(raw);
}