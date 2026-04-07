// ===========================================================================
// Chaos Piano — Shared Protocol Definitions
// ===========================================================================
// This file is the SINGLE SOURCE OF TRUTH for all constants shared between
// Brain 1 (Physical Controller) and Brain 2 (Audio Engine).
//
// Both environments include this via the lib/ folder. If you change a pin,
// an I2C address, or a message format here, both brains see it on next build.
// ===========================================================================

#ifndef CHAOS_PROTOCOL_H
#define CHAOS_PROTOCOL_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// I2C Configuration
// ---------------------------------------------------------------------------
// Why 0x42? It's outside the reserved range (0x00-0x07, 0x78-0x7F) and
// doesn't collide with common I2C peripherals (OLED=0x3C, BMP280=0x76, etc.)
// The RP2040 Wire library defaults to I2C0 on GPIO4 (SDA) and GPIO5 (SCL).
constexpr uint8_t I2C_TARGET_ADDRESS = 0x42;

// I2C bus speed. 100kHz is standard and very reliable over short wires.
// Don't go to 400kHz unless you've tested signal integrity on your actual
// wiring — longer wires + breadboards = reflections at high speed.
constexpr uint32_t I2C_CLOCK_HZ = 100000;

// I2C pins — using the default I2C0 peripheral on both brains.
// These MUST be the same on both boards since they share the bus.
constexpr uint8_t PIN_I2C_SDA = 4;  // GPIO4
constexpr uint8_t PIN_I2C_SCL = 5;  // GPIO5

// ---------------------------------------------------------------------------
// I2C Message Format
// ---------------------------------------------------------------------------
// Brain 1 sends a fixed-size packet to Brain 2 on every sensor sweep.
// We use a simple struct rather than raw bytes so both sides agree on
// the layout. The __attribute__((packed)) prevents the compiler from
// inserting padding bytes between fields (which would differ between
// compiler versions and break the protocol silently).

struct __attribute__((packed)) ProximityMessage {
    uint8_t header;          // Always 0xCP (0xCP = "Chaos Piano" magic byte)
    uint8_t proximity;       // 0 = touching, 255 = nobody nearby
    uint8_t sensor_count;    // How many sensors got valid readings (0-4)
    uint8_t checksum;        // XOR of header ^ proximity ^ sensor_count
};

// Magic byte for message validation. If Brain 2 receives a message
// that doesn't start with this, it knows the transmission was corrupted
// and should discard it rather than acting on garbage data.
constexpr uint8_t MSG_HEADER_MAGIC = 0xC0;  // "C0" for Chaos, close enough

// Helper to compute checksum
inline uint8_t computeChecksum(uint8_t header, uint8_t proximity, uint8_t sensor_count) {
    return header ^ proximity ^ sensor_count;
}

// Helper to validate a received message
inline bool validateMessage(const ProximityMessage& msg) {
    return (msg.header == MSG_HEADER_MAGIC) &&
           (msg.checksum == computeChecksum(msg.header, msg.proximity, msg.sensor_count));
}

// ---------------------------------------------------------------------------
// Brain 1 Pin Assignments — Physical Controller
// ---------------------------------------------------------------------------
// HC-SR04 Ultrasonics: each needs a TRIGGER and ECHO pin.
// TRIGGER is output (we send a 10µs pulse), ECHO is input (we time the return).
// We share a single TRIGGER pin for all 4 sensors and read ECHOs sequentially.
// Why? Saves 3 GPIO pins, and we sweep sensors one at a time anyway (to avoid
// crosstalk — ultrasonic pulses from one sensor triggering another's echo).
constexpr uint8_t PIN_US_TRIGGER     = 6;   // Shared trigger for all 4 sensors
constexpr uint8_t PIN_US_ECHO_0      = 7;   // Sensor 0 echo
constexpr uint8_t PIN_US_ECHO_1      = 8;   // Sensor 1 echo
constexpr uint8_t PIN_US_ECHO_2      = 9;   // Sensor 2 echo
constexpr uint8_t PIN_US_ECHO_3      = 10;  // Sensor 3 echo

constexpr uint8_t NUM_SENSORS = 4;
constexpr uint8_t ECHO_PINS[NUM_SENSORS] = {
    PIN_US_ECHO_0, PIN_US_ECHO_1, PIN_US_ECHO_2, PIN_US_ECHO_3
};

// MOSFETs for motors (12V switching via logic-level N-channel MOSFETs)
constexpr uint8_t PIN_MOTOR_0        = 11;
constexpr uint8_t PIN_MOTOR_1        = 12;
constexpr uint8_t PIN_MOTOR_2        = 13;
constexpr uint8_t NUM_MOTORS = 3;
constexpr uint8_t MOTOR_PINS[NUM_MOTORS] = {
    PIN_MOTOR_0, PIN_MOTOR_1, PIN_MOTOR_2
};

// MOSFETs for LED strips (same switching approach as motors)
// NOTE: GPIO16 is reserved — it's hardwired to the onboard WS2812 NeoPixel
// on the RP2040-Zero. Using it for a MOSFET would conflict. So we skip to 17.
constexpr uint8_t PIN_LED_0          = 0;
constexpr uint8_t PIN_LED_1          = 1;
constexpr uint8_t PIN_LED_2          = 2;
constexpr uint8_t NUM_LEDS = 3;
constexpr uint8_t LED_PINS[NUM_LEDS] = {
    PIN_LED_0, PIN_LED_1, PIN_LED_2
};

// Onboard WS2812 NeoPixel LED — present on all RP2040-Zero boards.
// We use the framework's built-in PIN_NEOPIXEL macro (defined in pins_arduino.h
// as GPIO16) rather than defining our own, to avoid a name collision.
// In code, just use PIN_NEOPIXEL directly — it's already available.

// ---------------------------------------------------------------------------
// Brain 2 Pin Assignments — Audio Engine
// ---------------------------------------------------------------------------
// Piezo ADC inputs — using the RP2040's built-in 12-bit ADC.
// IMPORTANT: The RP2040 only has ADC on GPIO26-29 (4 channels total).
// GPIO29 is used internally for VSYS voltage measurement on most boards,
// so we have GPIO26, 27, 28 available = exactly our 3 piezos.
constexpr uint8_t PIN_PIEZO_0        = 26;  // ADC0
constexpr uint8_t PIN_PIEZO_1        = 27;  // ADC1
constexpr uint8_t PIN_PIEZO_2        = 28;  // ADC2
constexpr uint8_t NUM_PIEZOS = 3;
constexpr uint8_t PIEZO_PINS[NUM_PIEZOS] = {
    PIN_PIEZO_0, PIN_PIEZO_1, PIN_PIEZO_2
};

// PCM5102 I2S DAC — Brain 2 outputs processed audio here.
// IMPORTANT: GPIO0 and GPIO1 are the default UART0 pins on the RP2040-Zero.
// The framework may claim them for Serial1, conflicting with I2S.
// We use GPIO6/7/8 instead — all free on Brain 2.
// Remember: LRCK is always BCK+1 in the arduino-pico I2S library.
constexpr uint8_t PIN_I2S_BCK        = 6;   // Bit clock
constexpr uint8_t PIN_I2S_LRCK       = 7;   // Word select (auto-assigned as BCK+1)
constexpr uint8_t PIN_I2S_DOUT       = 8;   // Serial data out

// DFPlayer Mini — controlled via Serial1 (UART0).
// GPIO0 = TX, GPIO1 = RX are the default UART0 pins on the RP2040-Zero
// and are available on the edge headers.
// The 1kΩ resistor on RX is a HARDWARE requirement — put it on the wire.
constexpr uint8_t PIN_DFPLAYER_TX    = 0;   // RP2040 TX → DFPlayer RX (via 1kΩ)
constexpr uint8_t PIN_DFPLAYER_RX    = 1;   // DFPlayer TX → RP2040 RX

// ---------------------------------------------------------------------------
// Audio / DSP Constants
// ---------------------------------------------------------------------------
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;  // Hz — good balance of quality vs CPU
constexpr uint16_t ADC_MAX_VALUE     = 4095;   // 12-bit ADC: 0 to 4095
constexpr uint16_t ADC_MID_VALUE     = 2048;   // Midpoint (silence with bias)

// EMA (Exponential Moving Average) filter coefficient for noise gating.
// Higher = more smoothing = slower response. 0.1 is a good starting point
// for filtering piezo noise while preserving attack transients.
constexpr float EMA_ALPHA = 0.1f;

// Noise gate threshold — below this ADC deviation from midpoint, signal
// is treated as silence. Prevents amplifying electrical noise when nobody
// is playing. This needs tuning on real hardware.
constexpr uint16_t NOISE_GATE_THRESHOLD = 50;

// ---------------------------------------------------------------------------
// Timing Constants
// ---------------------------------------------------------------------------
// Sensor sweep interval. 50ms = 20Hz scan rate, which is plenty fast for
// human proximity detection (humans don't move faster than ~2m/s, so at
// 20Hz you detect motion with <10cm resolution).
constexpr uint32_t SENSOR_SWEEP_MS   = 50;

// I2C transmission interval — send proximity data to Brain 2 at this rate.
// Same as sensor sweep since we send after each sweep.
constexpr uint32_t I2C_SEND_MS       = 50;

// HC-SR04 timeout in microseconds. At 343 m/s (speed of sound), a 3-meter
// round trip takes ~17,500µs. We set 25,000µs to give margin. Any echo
// longer than this means "no object detected."
constexpr uint32_t US_TIMEOUT_US     = 25000;

// Maximum detection range in centimeters. The HC-SR04 datasheet claims 4m,
// but in practice 3m is more reliable, and for an exhibit you probably
// only care about the last 2 meters.
constexpr uint16_t US_MAX_RANGE_CM   = 200;

#endif // CHAOS_PROTOCOL_H
