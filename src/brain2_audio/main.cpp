// ===========================================================================
// Chaos Piano — Brain 2: Full Audio Engine
// ===========================================================================
//
// This is the "audio world" brain. It receives proximity data from Brain 1
// over I2C and uses it to control a real-time audio processing pipeline.
// Two parallel signal paths are mixed at the output:
//
//   DRONE PATH (synthesized):
//     Sine oscillator → Distortion → Bitcrusher → Flanger → Volume → Autopan
//     All effects scale with proximity: 0=clean sine, 255=full chaos.
//
//   MIC PATH (live input from MAX9814 condenser mic):
//     ADC (DMA) → Noise Gate → Volume → Reverb → Delay
//     Reverb/delay add space to live sounds; gate prevents feedback.
//
//   FINAL MIX:
//     Drone (stereo, panned) + Mic (mono, centered) → I2S → PCM5102 DAC → speakers
//
//   DFPLAYER (separate analog path):
//     Brain 2 sends UART volume commands → DFPlayer → its own DAC → PAM8610 #2
//     This is an ambient track that plays continuously alongside the live audio.
//
// ===========================================================================

#include <Arduino.h>             // Core Arduino functions
#include <ADCInput.h>            // DMA-based ADC for jitter-free mic sampling
#include <Wire.h>                // I2C library (target/slave mode)
#include <I2S.h>                 // I2S output to PCM5102 DAC
#include <Adafruit_NeoPixel.h>   // Onboard WS2812 LED on GPIO16
#include "chaos_protocol.h"      // Shared constants: pins, I2C address, message format

// ===========================================================================
// TUNING PARAMETERS — adjust these during exhibit setup
// ===========================================================================

// --- Mic Input ---
constexpr int16_t MIC_BIAS = 1540;          // DC offset of MAX9814 output (measured, not 2048)
constexpr uint8_t MIC_VOLUME = 75;       // Mic loudness: 0=silent, 128=unity, 255=2x boost

// --- Noise Gate (mic) ---
// The gate prevents the mic from amplifying room noise and speaker feedback
// when nobody is making sounds. It uses an asymmetric envelope follower:
// attack is fast (gate opens quickly when you clap) and release is slow
// (gate stays open briefly after sound stops, preserving natural decay).
constexpr float GATE_THRESHOLD = 1500.0f;   // Below this envelope level → mute mic
constexpr float GATE_ATTACK = 0.5f;         // Gate open speed (0.0-1.0, higher=faster)
constexpr float GATE_RELEASE = 0.1f;        // Gate close speed (lower=slower tail)

// --- Reverb (mic only) ---
// Schroeder reverb: 4 parallel comb filters with different delay lengths.
// Each comb filter recirculates the signal with decay, creating dense echoes.
constexpr uint8_t REVERB_DECAY = 215;       // Tail: 190≈2s, 210≈4s, 220≈6s. Stay below 225!
constexpr uint8_t REVERB_WET = 200;         // Wet/dry: 0=dry, 128=50/50, 255=all reverb

// --- Delay (mic only) ---
// Simple delay line: input is written to a circular buffer, output is read
// from DELAY_TIME_MS earlier. Feedback recirculates the delayed signal.
constexpr uint16_t DELAY_TIME_MS = 600;     // Echo time in ms (300-600 for rhythmic feel)
constexpr uint8_t DELAY_FEEDBACK = 100;     // Repeats: 0=one echo, 128=many, 200+=danger
constexpr uint8_t DELAY_MIX = 140;          // Echo volume: 0=dry, 128=equal to dry, 200=loud

// --- Drone Oscillator ---
constexpr uint16_t DRONE_FREQ_HZ = 440;     // Base frequency in Hz (440 = concert A)
constexpr uint8_t DRONE_PRE_SHIFT = 2;      // Right-shift before effects chain for headroom
                                             // 2=quarter amplitude, 3=eighth. Prevents clipping.
constexpr uint8_t DRONE_VOLUME_MIN = 0;     // Volume at prox 0 (0-255). 1 = barely audible.
constexpr uint8_t DRONE_VOLUME_MAX = 75;    // Volume at prox 255. Keep low so mic is audible.

// --- Distortion (drone only) ---
// Hard clipping with gain. The gain multiplier increases with proximity,
// pushing the waveform into clipping more aggressively.
constexpr uint8_t DIST_GAIN_MULTIPLIER = 7; // Max gain = 1 + (255 × this / 256). 7 = up to 8x.

// --- Bitcrusher (drone only) ---
// Reduces bit depth by right-shifting then left-shifting. At max proximity,
// up to BITCRUSH_MAX_BITS bits are lost, creating a gritty lo-fi sound.
constexpr uint8_t BITCRUSH_MAX_BITS = 12;   // Maximum bits crushed at prox 255 (12 = very harsh)

// --- Flanger (drone only) ---
// Reads from a delay buffer at a position modulated by a triangle-wave LFO.
// The moving delay creates a sweeping comb filter (the "jet engine" sound).
constexpr uint32_t FLANGER_LFO_SPEED = 97391;  // LFO phase increment per sample (~0.5Hz)
constexpr uint16_t FLANGER_BUF_SIZE = 1024;     // Delay buffer size in samples (power of 2!)

// --- Autopan (drone only) ---
// Triangle-wave LFO that moves the drone between left and right speakers.
// Speed and depth both increase with proximity (Leslie cabinet effect).
constexpr uint32_t PAN_SPEED_MIN = 15000;   // LFO speed when far (very slow drift)
constexpr uint32_t PAN_SPEED_MAX = 300000;  // LFO speed when close (moderate sweep)
constexpr uint8_t PAN_DEPTH_MIN = 0;        // Pan depth when far (0=dead center)
constexpr uint8_t PAN_DEPTH_MAX = 200;      // Pan depth when close (255=full hard LR)

// --- DFPlayer (ambient track) ---
constexpr uint8_t DFPLAYER_VOL_MIN = 10;     // Volume when nobody near (DFPlayer 0-30 range)
constexpr uint8_t DFPLAYER_VOL_MAX = 30;    // Volume at max proximity
constexpr uint16_t DFPLAYER_TRACK = 1;      // Track to loop (001.mp3 on SD card)

// --- Glitch / Broken Transmission (DFPlayer only) ---
// Random muting of the DFPlayer to simulate an unstable signal.
// Set GLITCH_CHANCE_MAX to 0 to disable entirely (no CPU cost when disabled).
constexpr uint8_t GLITCH_CHANCE_MIN = 0;    // Probability when far (0=never)
constexpr uint8_t GLITCH_CHANCE_MAX = 0;    // Probability when close (0=disabled, 120=frequent)
constexpr uint16_t GLITCH_MIN_LENGTH = 2000;  // Shortest cut in samples (2000 ≈ 45ms)
constexpr uint16_t GLITCH_MAX_LENGTH = 10000; // Longest cut in samples (10000 ≈ 227ms)
constexpr int16_t GLITCH_FADE_SPEED = 4;    // Gain change per sample during fade (higher=faster)

// ===========================================================================
// END TUNING PARAMETERS
// ===========================================================================

// Onboard NeoPixel — green triple-flash pattern indicates Brain 2.
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------------
// I2C Reception (interrupt-driven)
// ---------------------------------------------------------------------------
// Brain 2 operates as an I2C target (slave). When Brain 1 sends a
// ProximityMessage, the Wire library triggers onI2CReceive() as an interrupt.
// We read into a buffer, validate the checksum, and update the proximity value.
// All variables touched by the ISR must be volatile.

volatile uint8_t proximity = 0;                        // Current proximity from Brain 1
volatile bool proximityUpdated = false;                // Flag: new data available
volatile uint32_t lastI2CReceiveTime = 0;              // Timestamp of last valid message
volatile uint8_t i2cBuffer[sizeof(ProximityMessage)];  // Raw receive buffer

// Called by Wire library when Brain 1 sends data. Runs in interrupt context.
void onI2CReceive(int numBytes) {
    if (numBytes == sizeof(ProximityMessage)) {
        // Read all bytes into our buffer
        for (int i = 0; i < numBytes; i++) {
            i2cBuffer[i] = Wire.read();
        }
        // Cast buffer to message struct and validate checksum
        const ProximityMessage* msg = (const ProximityMessage*)i2cBuffer;
        if (validateMessage(*msg)) {
            proximity = msg->proximity;        // Update shared proximity value
            proximityUpdated = true;
            lastI2CReceiveTime = millis();     // Record when we last heard from Brain 1
        }
    } else {
        // Wrong message size — drain the buffer to avoid corruption
        while (Wire.available()) Wire.read();
    }
}

// ---------------------------------------------------------------------------
// DFPlayer Mini (UART control)
// ---------------------------------------------------------------------------
// The DFPlayer is an autonomous MP3 decoder module with its own DAC.
// We control it via 10-byte serial frames at 9600 baud.
// It plays audio from an SD card through its own analog outputs,
// which go to PAM8610 amplifier #2 (separate from the I2S audio path).

#define DFPLAYER_SERIAL Serial1  // UART0 on GPIO0 (TX) and GPIO1 (RX)

// Send a raw command frame to the DFPlayer.
// Frame format: 0x7E FF 06 CMD 00 PARAM_H PARAM_L CHECKSUM_H CHECKSUM_L 0xEF
void dfplayerSendCommand(uint8_t cmd, uint16_t param) {
    uint8_t frame[10];
    frame[0] = 0x7E;                          // Start byte
    frame[1] = 0xFF;                          // Version
    frame[2] = 0x06;                          // Data length (always 6)
    frame[3] = cmd;                           // Command byte
    frame[4] = 0x00;                          // Feedback flag (0 = no feedback)
    frame[5] = (param >> 8);                  // Parameter high byte
    frame[6] = (param & 0xFF);               // Parameter low byte
    int16_t checksum = 0;                     // Checksum = negative sum of bytes 1-6
    for (uint8_t i = 1; i <= 6; i++) checksum -= frame[i];
    frame[7] = (checksum >> 8) & 0xFF;       // Checksum high byte
    frame[8] = checksum & 0xFF;              // Checksum low byte
    frame[9] = 0xEF;                          // End byte
    DFPLAYER_SERIAL.write(frame, 10);         // Send all 10 bytes
}

// Convenience wrappers
void dfplayerSetVolume(uint8_t vol) { dfplayerSendCommand(0x06, vol); }   // cmd 0x06 = set volume
void dfplayerSetRepeat(uint16_t track) { dfplayerSendCommand(0x08, track); } // cmd 0x08 = loop track

// DFPlayer state tracking
uint8_t lastDFPlayerVolume = 255;  // Initialize to impossible value to force first send
uint32_t lastDFPlayerUpdate = 0;   // Rate limit timestamp
bool dfplayerStarted = false;      // Has the DFPlayer been initialized?

// Update DFPlayer volume based on proximity. Rate-limited to every 200ms
// to avoid overloading the DFPlayer's slow serial interface.
void updateDFPlayer(uint8_t prox) {
    uint32_t now = millis();
    if (now - lastDFPlayerUpdate < 200) return;  // Rate limit: max 5 commands/second
    lastDFPlayerUpdate = now;

    // First call: shouldn't happen since we init in setup(), but safety fallback
    if (!dfplayerStarted) {
        dfplayerSetVolume(DFPLAYER_VOL_MIN);
        delay(100);
        dfplayerSetRepeat(DFPLAYER_TRACK);
        dfplayerStarted = true;
        lastDFPlayerVolume = DFPLAYER_VOL_MIN;
        return;
    }

    // Map proximity to DFPlayer volume range. Only send if changed.
    uint8_t targetVolume = map(prox, 0, 255, DFPLAYER_VOL_MIN, DFPLAYER_VOL_MAX);
    if (targetVolume != lastDFPlayerVolume) {
        dfplayerSetVolume(targetVolume);
        lastDFPlayerVolume = targetVolume;
    }
}

// ---------------------------------------------------------------------------
// I2S + ADCInput (audio hardware)
// ---------------------------------------------------------------------------
// I2S: serial audio protocol to the PCM5102 DAC. 16-bit, 44100Hz, stereo.
// ADCInput: DMA-driven ADC that samples the MAX9814 mic at 44100Hz with
// zero jitter (unlike analogRead which has timing variations that sound
// like bitcrushing artifacts).

I2S i2sOutput(OUTPUT, PIN_I2S_BCK, PIN_I2S_DOUT);  // BCK=GPIO6, DOUT=GPIO8
ADCInput micInput(PIN_PIEZO_0, PIN_PIEZO_1);  // two channels, interleaved

// ---------------------------------------------------------------------------
// DSP Effects: Distortion (drone only)
// ---------------------------------------------------------------------------
// Hard clipping distortion. Multiplies the sample by a gain factor that
// scales with proximity, then clips to ±32767. At low proximity, gain ≈ 1x
// (clean). At max proximity, gain ≈ 8x (heavily distorted square wave).
int16_t applyDistortion(int16_t sample, uint8_t intensity) {
    if (intensity == 0) return sample;  // No proximity = bypass

    // Fixed-point gain: 256 = 1.0x. At intensity 255: 256 + 255×7 = 2041 ≈ 8x.
    uint16_t gain_fp = 256 + ((uint16_t)intensity * DIST_GAIN_MULTIPLIER);
    int32_t amplified = ((int32_t)sample * gain_fp) >> 8;  // Apply gain, shift back

    // Hard clip to 16-bit range (this IS the distortion — the clipping)
    if (amplified > 32767) amplified = 32767;
    if (amplified < -32768) amplified = -32768;
    return (int16_t)amplified;
}

// ---------------------------------------------------------------------------
// DSP Effects: Bitcrusher (drone only)
// ---------------------------------------------------------------------------
// Reduces effective bit depth. Right-shifting discards low bits, then
// left-shifting restores the scale but with staircase-like quantization.
// This creates a digital, aliased, "retro" sound.
int16_t applyBitcrush(int16_t sample, uint8_t intensity) {
    if (intensity == 0) return sample;

    // Scale intensity (0-255) to bits to crush (0 to BITCRUSH_MAX_BITS)
    uint8_t crushBits = (uint8_t)((uint16_t)intensity * BITCRUSH_MAX_BITS / 255);
    if (crushBits == 0) return sample;

    // Right-shift destroys low bits, left-shift fills with zeros = quantization
    return (sample >> crushBits) << crushBits;
}

// ---------------------------------------------------------------------------
// DSP Effects: Flanger (drone only)
// ---------------------------------------------------------------------------
// A flanger works by mixing the original signal with a delayed copy of itself,
// where the delay time oscillates via an LFO. The changing delay creates
// constructive/destructive interference at different frequencies, producing
// the characteristic sweeping "jet engine" or "whoosh" sound.

int16_t flangerBuffer[FLANGER_BUF_SIZE] = {0};  // Circular delay buffer
uint16_t flangerWriteIndex = 0;                   // Current write position
uint32_t flangerLFOPhase = 0;                     // LFO phase accumulator (32-bit wrapping)

int16_t applyFlanger(int16_t sample, uint8_t intensity) {
    // Always write to the buffer (maintains history even when bypassed)
    flangerBuffer[flangerWriteIndex] = sample;
    flangerWriteIndex = (flangerWriteIndex + 1) & (FLANGER_BUF_SIZE - 1);  // Wrap with bitmask

    if (intensity == 0) return sample;  // Bypass when nobody nearby

    // Advance the LFO. Phase wraps at 32 bits = one full cycle.
    flangerLFOPhase += FLANGER_LFO_SPEED;

    // Max delay depth scales with proximity (more proximity = deeper sweep)
    uint16_t maxDelay = (uint16_t)((uint32_t)intensity * (FLANGER_BUF_SIZE / 4) / 255);
    if (maxDelay < 1) maxDelay = 1;

    // Triangle wave LFO: phase 0→0x7FFFFFFF ramps up, 0x80000000→0xFFFFFFFF ramps down
    uint16_t lfoValue;
    if (flangerLFOPhase < 0x80000000UL) {
        lfoValue = (uint16_t)((uint32_t)(flangerLFOPhase >> 16) * maxDelay / 32768);
    } else {
        uint32_t invPhase = 0xFFFFFFFFUL - flangerLFOPhase;
        lfoValue = (uint16_t)((uint32_t)(invPhase >> 16) * maxDelay / 32768);
    }
    if (lfoValue < 1) lfoValue = 1;

    // Read from the delayed position in the circular buffer
    uint16_t readIndex = (flangerWriteIndex - lfoValue) & (FLANGER_BUF_SIZE - 1);
    int16_t delayedSample = flangerBuffer[readIndex];

    // Mix original + delayed at 50/50 (the interference creates the flanger effect)
    return (int16_t)(((int32_t)sample + (int32_t)delayedSample) / 2);
}

// ---------------------------------------------------------------------------
// Autopanner (drone only)
// ---------------------------------------------------------------------------
// Triangle-wave LFO that distributes a mono signal between left and right.
// At prox=0: centered (both speakers equal).
// As proximity increases: wider swing and faster oscillation (Leslie effect).

uint32_t panLFOPhase = 0;  // Phase accumulator for pan LFO

void applyPanning(int16_t monoSample, uint8_t prox, int16_t* leftOut, int16_t* rightOut) {
    // Bypass: no proximity and no minimum depth = center output
    if (prox == 0 && PAN_DEPTH_MIN == 0) {
        *leftOut = monoSample;
        *rightOut = monoSample;
        return;
    }

    // LFO speed scales with proximity
    uint32_t panSpeed = PAN_SPEED_MIN + ((uint32_t)prox * (PAN_SPEED_MAX - PAN_SPEED_MIN) / 255);
    panLFOPhase += panSpeed;

    // Convert phase to triangle wave (0-255)
    uint8_t rawPos = (uint8_t)(panLFOPhase >> 24);
    uint8_t trianglePos;
    if (rawPos < 128) {
        trianglePos = rawPos * 2;          // Rising half: 0→255
    } else {
        trianglePos = (255 - rawPos) * 2;  // Falling half: 255→0
    }

    // Depth scales with proximity (how far from center the pan swings)
    uint8_t depth = PAN_DEPTH_MIN + ((uint16_t)prox * (PAN_DEPTH_MAX - PAN_DEPTH_MIN) / 255);

    // Apply depth to get pan position (128 = center, 0 = full left, 255 = full right)
    int16_t deviation = ((int16_t)trianglePos - 128);
    deviation = (int16_t)((int32_t)deviation * depth / 255);
    uint8_t panPos = (uint8_t)(128 + deviation);

    // Convert pan position to left/right gain (complementary: sum ≈ 255)
    uint8_t rightGain = panPos;
    uint8_t leftGain = 255 - panPos;

    // Apply gains. Divide by 128 (not 255) for slight boost to compensate
    // for the perceived volume drop when signal moves to one side.
    int32_t leftSample = ((int32_t)monoSample * leftGain) / 128;
    int32_t rightSample = ((int32_t)monoSample * rightGain) / 128;

    // Clip to 16-bit range
    if (leftSample > 32767) leftSample = 32767;
    if (leftSample < -32768) leftSample = -32768;
    if (rightSample > 32767) rightSample = 32767;
    if (rightSample < -32768) rightSample = -32768;

    *leftOut = (int16_t)leftSample;
    *rightOut = (int16_t)rightSample;
}

// ---------------------------------------------------------------------------
// Proximity Volume (drone only)
// ---------------------------------------------------------------------------
// Scales the drone amplitude between DRONE_VOLUME_MIN and DRONE_VOLUME_MAX
// based on proximity. This is the master volume for the drone, applied
// AFTER the effects chain (so effects don't change the volume envelope).
int16_t applyProximityVolume(int16_t sample, uint8_t prox) {
    uint8_t volume = DRONE_VOLUME_MIN + ((uint16_t)prox * (DRONE_VOLUME_MAX - DRONE_VOLUME_MIN) / 255);
    return (int16_t)(((int32_t)sample * volume) >> 8);  // >>8 because volume is 0-255
}

// ---------------------------------------------------------------------------
// Drone Oscillator
// ---------------------------------------------------------------------------
// Phase-accumulator sine approximation. The 32-bit phase wraps naturally,
// creating a continuous waveform. Phase increment determines frequency.

uint32_t sinePhase = 0;

// Phase increment computed at compile time from DRONE_FREQ_HZ.
// Formula: increment = (2^32 × frequency) / sample_rate
// At 440Hz, 44100Hz: 4294967296 × 440 / 44100 ≈ 42,849,735
constexpr uint32_t SINE_PHASE_INCREMENT = (uint32_t)((4294967296.0 * DRONE_FREQ_HZ) / AUDIO_SAMPLE_RATE);

// Fast sine approximation using parabolic curve fitting.
// Takes upper 16 bits of phase as position, outputs ±~32767.
// Accuracy is ~99.8% — good enough for audio, much faster than sin().
int16_t fastSine(uint32_t phase) {
    int16_t x = (int16_t)(phase >> 16);        // Map phase to -32768..32767
    int16_t ax = (x < 0) ? -x : x;            // Absolute value
    int32_t y = (int32_t)x * (32767 - (int32_t)ax);  // Parabola
    return (int16_t)(y >> 14);                 // Scale to 16-bit range
}

// ---------------------------------------------------------------------------
// Reverb (mic only) — Schroeder 4-comb-filter design
// ---------------------------------------------------------------------------
// A comb filter recirculates audio through a delay buffer with decay.
// Four parallel comb filters with different prime-number lengths create
// a dense, natural-sounding reverb tail. The lengths are chosen to be
// mutually prime (no common factors) to avoid resonant peaks.

constexpr uint16_t COMB1_LEN = 1187;  // ~27ms at 44100Hz
constexpr uint16_t COMB2_LEN = 1499;  // ~34ms
constexpr uint16_t COMB3_LEN = 1747;  // ~40ms
constexpr uint16_t COMB4_LEN = 1907;  // ~43ms

int16_t comb1[COMB1_LEN] = {0};  // Delay buffers (zero-initialized)
int16_t comb2[COMB2_LEN] = {0};
int16_t comb3[COMB3_LEN] = {0};
int16_t comb4[COMB4_LEN] = {0};

uint16_t comb1Idx = 0;  // Write/read index per buffer
uint16_t comb2Idx = 0;
uint16_t comb3Idx = 0;
uint16_t comb4Idx = 0;

// Process one sample through a single comb filter.
// Returns the delayed output. Writes input + decayed feedback into the buffer.
int16_t processComb(int16_t input, int16_t* buffer, uint16_t bufLen, uint16_t& idx) {
    int16_t delayed = buffer[idx];             // Read the oldest sample
    // New value = input + (delayed × decay/256). This is the feedback loop.
    int32_t newVal = (int32_t)input + (((int32_t)delayed * REVERB_DECAY) >> 8);
    if (newVal > 32767) newVal = 32767;        // Clip to prevent runaway
    if (newVal < -32768) newVal = -32768;
    buffer[idx] = (int16_t)newVal;             // Write back to buffer
    idx++;                                      // Advance index
    if (idx >= bufLen) idx = 0;                // Wrap around (circular buffer)
    return delayed;                             // Return the delayed sample
}

// Apply reverb: run input through all 4 comb filters, average their outputs,
// then mix wet/dry according to REVERB_WET.
int16_t applyReverb(int16_t sample) {
    int32_t c1 = processComb(sample, comb1, COMB1_LEN, comb1Idx);
    int32_t c2 = processComb(sample, comb2, COMB2_LEN, comb2Idx);
    int32_t c3 = processComb(sample, comb3, COMB3_LEN, comb3Idx);
    int32_t c4 = processComb(sample, comb4, COMB4_LEN, comb4Idx);

    int16_t wet = (int16_t)((c1 + c2 + c3 + c4) / 4);  // Average the 4 outputs

    // Wet/dry mix: (dry × (256-wet_amount) + wet × wet_amount) / 256
    int32_t mixed = ((int32_t)sample * (256 - REVERB_WET) + (int32_t)wet * REVERB_WET) >> 8;
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    return (int16_t)mixed;
}

// ---------------------------------------------------------------------------
// Delay (mic only) — circular buffer echo
// ---------------------------------------------------------------------------
// At 44100Hz, DELAY_TIME_MS=600 → buffer of 26460 samples (52920 bytes).
constexpr uint16_t DELAY_LEN = (uint16_t)((uint32_t)AUDIO_SAMPLE_RATE * DELAY_TIME_MS / 1000);
int16_t delayBuffer[DELAY_LEN] = {0};
uint16_t delayWriteIdx = 0;

// Process one sample: read the delayed output, write input + feedback, return mix.
int16_t applyDelay(int16_t sample) {
    int16_t delayed = delayBuffer[delayWriteIdx];  // Read from current position (oldest)

    // Write: input + attenuated feedback from previous echo
    int32_t newVal = (int32_t)sample + (((int32_t)delayed * DELAY_FEEDBACK) >> 8);
    if (newVal > 32767) newVal = 32767;
    if (newVal < -32768) newVal = -32768;
    delayBuffer[delayWriteIdx] = (int16_t)newVal;

    delayWriteIdx++;                               // Advance write position
    if (delayWriteIdx >= DELAY_LEN) delayWriteIdx = 0;  // Wrap

    // Mix dry + delayed: (dry × (256-mix) + wet × mix) / 256
    int32_t mixed = ((int32_t)sample * (256 - DELAY_MIX) + (int32_t)delayed * DELAY_MIX) >> 8;
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    return (int16_t)mixed;
}

// ---------------------------------------------------------------------------
// Noise Gate state
// ---------------------------------------------------------------------------
float micEnvelope = 0.0f;  // Smoothed absolute mic level (envelope follower)

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    // NeoPixel: green = Brain 2
    pixel.begin();
    pixel.setBrightness(30);
    pixel.setPixelColor(0, pixel.Color(0, 255, 0));
    pixel.show();

    // USB serial for debug
    Serial.begin(115200);
    delay(2000);  // 2s for USB enumeration (exhibit-safe, replaces while(!Serial))

    Serial.println("=== Chaos Piano: Brain 2 (Full Audio Engine) ===");

    // --- I2S output to PCM5102 DAC ---
    // 16-bit samples, 44100Hz, stereo (left then right per frame).
    // The PCM5102's SCK pin must be tied to GND (internal clock mode).
    i2sOutput.setBitsPerSample(16);
    if (!i2sOutput.begin(AUDIO_SAMPLE_RATE)) {
        Serial.println("I2S FAILED");
    } else {
        Serial.print("I2S OK at ");
        Serial.print(AUDIO_SAMPLE_RATE);
        Serial.println("Hz");
    }

    // --- ADC mic input via DMA ---
    // ADCInput uses DMA to read the ADC at a precise sample rate,
    // filling buffers in the background. This eliminates the timing jitter
    // of analogRead() which caused audible bitcrushing artifacts.
    micInput.setFrequency(AUDIO_SAMPLE_RATE);
    micInput.setBuffers(4, 64);  // 4 buffers × 64 samples = 256 sample pipeline
    micInput.begin();

    // --- I2C target (slave) ---
    Wire.setSDA(PIN_I2C_SDA);
    Wire.setSCL(PIN_I2C_SCL);
    Wire.begin(I2C_TARGET_ADDRESS);     // Join bus as target at address 0x42
    Wire.onReceive(onI2CReceive);       // Register interrupt callback

    // --- DFPlayer init ---
    // Initialize UART and send play commands during setup (before the
    // audio loop starts consuming CPU). The 1-second delay gives the
    // DFPlayer's own microcontroller time to boot after power-on.
    DFPLAYER_SERIAL.setTX(PIN_DFPLAYER_TX);
    DFPLAYER_SERIAL.setRX(PIN_DFPLAYER_RX);
    DFPLAYER_SERIAL.begin(9600);
    delay(1000);                         // DFPlayer boot time
    dfplayerSetVolume(DFPLAYER_VOL_MIN);
    delay(100);                          // Wait for volume command to process
    dfplayerSetRepeat(DFPLAYER_TRACK);   // Start looping the ambient track
    dfplayerStarted = true;
    lastDFPlayerVolume = DFPLAYER_VOL_MIN;
    Serial.print("DFPlayer: track ");
    Serial.print(DFPLAYER_TRACK);
    Serial.print(", vol ");
    Serial.print(DFPLAYER_VOL_MIN);
    Serial.print("-");
    Serial.println(DFPLAYER_VOL_MAX);

    // --- Report configuration ---
    uint32_t bufferRAM = (COMB1_LEN + COMB2_LEN + COMB3_LEN + COMB4_LEN
                          + DELAY_LEN + FLANGER_BUF_SIZE) * 2;
    Serial.print("Buffer RAM: ");
    Serial.print(bufferRAM);
    Serial.print(" bytes (");
    Serial.print(bufferRAM * 100 / (264 * 1024));
    Serial.println("% of 264KB)");
    Serial.print("Drone: ");
    Serial.print(DRONE_FREQ_HZ);
    Serial.print("Hz, shift>>");
    Serial.print(DRONE_PRE_SHIFT);
    Serial.print(", vol ");
    Serial.print(DRONE_VOLUME_MIN);
    Serial.print("-");
    Serial.println(DRONE_VOLUME_MAX);
    Serial.print("Mic vol: ");
    Serial.println(MIC_VOLUME);

    Serial.println("DRONE: Sine → Distort → Bitcrush → Flange → Vol → Pan");
    Serial.println("MIC:   MAX9814 → Gate → Vol → Reverb → Delay");
    Serial.println("MIX:   Drone(stereo) + Mic(mono) → I2S → DAC");
}

// ---------------------------------------------------------------------------
// Main Loop
// ---------------------------------------------------------------------------
void loop() {
    uint32_t now = millis();

    // --- NeoPixel: green triple-flash pattern (distinguishes from Brain 1's blue) ---
    static uint32_t lastPixelUpdate = 0;
    if (now - lastPixelUpdate >= 50) {
        lastPixelUpdate = now;
        uint16_t phase = now % 1500;
        bool on = (phase < 100) || (phase >= 200 && phase < 300) || (phase >= 400 && phase < 500);
        pixel.setPixelColor(0, on ? pixel.Color(0, 255, 0) : pixel.Color(0, 0, 0));
        pixel.show();
    }

    // --- Read proximity from I2C ---
    // If no I2C message received for 2 seconds, assume Brain 1 is disconnected
    // and fall back to prox=0 (nobody detected, everything quiet).
    uint8_t prox = proximity;
    if (millis() - lastI2CReceiveTime > 2000) {
        prox = 0;
    }

    // --- Debug output every 3 seconds ---
    static uint32_t lastDebugPrint = 0;
    if (now - lastDebugPrint >= 3000) {
        lastDebugPrint = now;
        Serial.print("Prox:");
        Serial.print(prox);
        Serial.print(" Env:");
        Serial.print((int)micEnvelope);
        Serial.print(" Gate:");
        Serial.print(micEnvelope >= GATE_THRESHOLD ? "OPEN" : "SHUT");
        Serial.print(" I2C:");
        Serial.print(proximityUpdated ? "ok" : "no");
        Serial.print(" DFP:");
        Serial.println(lastDFPlayerVolume);
        Serial.print(" | DFPtgt:");
        Serial.println(map(prox, 0, 255, DFPLAYER_VOL_MIN, DFPLAYER_VOL_MAX));
    }

    // --- Audio processing: runs whenever the ADC has a new sample ---
    // The ADCInput DMA fills buffers at 44100Hz. We process one sample
    // per available() call, which keeps us synchronized to the sample rate.
    if (micInput.available()) {

        // === DRONE PATH ===
        // 1. Generate sine wave and pre-attenuate for effects headroom
        sinePhase += SINE_PHASE_INCREMENT;
        int16_t drone = fastSine(sinePhase) >> DRONE_PRE_SHIFT;

        // 2. Apply proximity-driven effects chain
        drone = applyDistortion(drone, prox);      // Gain + hard clip
        drone = applyBitcrush(drone, prox);         // Reduce bit depth
        drone = applyFlanger(drone, prox);           // Sweeping comb filter
        drone = applyProximityVolume(drone, prox);   // Master drone volume

        // 3. Split to stereo with autopanning
        int16_t droneLeft, droneRight;
        applyPanning(drone, prox, &droneLeft, &droneRight);

        // === MIC PATH ===
        // 1. Read both DMA samples (interleaved: mic0 then mic1) and remove DC bias
        int16_t raw0 = micInput.read();             // Mic 0 (GPIO26) - 12-bit ADC value
        int16_t raw1 = micInput.read();             // Mic 1 (GPIO27) - 12-bit ADC value
        int16_t mic0 = (raw0 - MIC_BIAS) * 2;      // Center around zero
        int16_t mic1 = (raw1 - MIC_BIAS) * 2;      // Center around zero
        // Sum both mics and halve to prevent clipping (average = mono downmix)
        int16_t mic = (int16_t)(((int32_t)mic0 + (int32_t)mic1) / 2);

        // 2. Noise gate: track envelope, mute if below threshold
        float absMic = (mic < 0) ? (float)(-mic) : (float)mic;
        if (absMic > micEnvelope) {
            // Attack: envelope rises quickly toward loud sounds
            micEnvelope = GATE_ATTACK * absMic + (1.0f - GATE_ATTACK) * micEnvelope;
        } else {
            // Release: envelope falls slowly, preserving natural sound decay
            micEnvelope = GATE_RELEASE * absMic + (1.0f - GATE_RELEASE) * micEnvelope;
        }
        if (micEnvelope < GATE_THRESHOLD) {
            mic = 0;  // Below threshold: silence (prevents feedback amplification)
        }

        // 3. Apply mic volume (>>7 so that MIC_VOLUME=128 = unity gain)
        mic = (int16_t)(((int32_t)mic * MIC_VOLUME) >> 6);

        // 4. Spatial effects
        mic = applyReverb(mic);                     // Room simulation
        mic = applyDelay(mic);                       // Echo

        // === GLITCH SYSTEM (DFPlayer only, zero cost when disabled) ===
        static uint16_t glitchCounter = 0;
        static uint32_t glitchRNG = 12345;
        static bool glitchActive = false;

        if (GLITCH_CHANCE_MAX > 0) {
            if (glitchCounter > 0) {
                glitchCounter--;
                glitchActive = true;
                if (glitchCounter == 0) glitchActive = false;
            } else if (prox > 0 && !glitchActive) {
                // LCG pseudo-random: fast, no library needed
                glitchRNG = glitchRNG * 1664525 + 1013904223;
                uint8_t roll = (uint8_t)(glitchRNG >> 24);
                uint8_t chance = GLITCH_CHANCE_MIN +
                    ((uint16_t)prox * (GLITCH_CHANCE_MAX - GLITCH_CHANCE_MIN) / 255);
                if (roll < chance) {
                    uint32_t range = GLITCH_MAX_LENGTH - GLITCH_MIN_LENGTH;
                    glitchCounter = GLITCH_MIN_LENGTH + (uint16_t)((glitchRNG >> 8) % range);
                    glitchActive = true;
                }
            }

            // Mute/unmute DFPlayer during glitch (rate-limited serial commands)
            static bool dfplayerMuted = false;
            static uint32_t lastGlitchMuteUpdate = 0;
            if (now - lastGlitchMuteUpdate >= 50) {
                if (glitchActive && !dfplayerMuted) {
                    dfplayerSetVolume(0);
                    dfplayerMuted = true;
                    lastGlitchMuteUpdate = now;
                } else if (!glitchActive && dfplayerMuted) {
                    lastDFPlayerVolume = 255;      // Force resend of correct volume
                    dfplayerMuted = false;
                    lastGlitchMuteUpdate = now;
                }
            }
        }

        // === FINAL MIX ===
        // Drone (stereo) + mic (mono, same in both channels) → clip → I2S output.
        int32_t finalLeft  = (int32_t)droneLeft  + (int32_t)mic;
        int32_t finalRight = (int32_t)droneRight + (int32_t)mic;

        // Clip to 16-bit range (prevents wrap-around distortion)
        if (finalLeft > 32767) finalLeft = 32767;
        if (finalLeft < -32768) finalLeft = -32768;
        if (finalRight > 32767) finalRight = 32767;
        if (finalRight < -32768) finalRight = -32768;

        // Write stereo pair to I2S: left first, then right (PCM5102 frame format)
        i2sOutput.write((int16_t)finalLeft);
        i2sOutput.write((int16_t)finalRight);
    }

    // --- DFPlayer volume update (outside audio block, rate-limited internally) ---
    updateDFPlayer(prox);

    // --- USB keepalive ---
    // yield() lets the USB CDC stack process outgoing serial data.
    // Without periodic yields, the USB buffer fills and Serial.print() blocks,
    // which would stall the audio loop and cause glitches.
    static uint16_t keepalive = 0;
    if (++keepalive >= 1000) {
        keepalive = 0;
        yield();
    }
}
