// ===========================================================================
// Chaos Piano — Brain 1: Physical Controller
// ===========================================================================
//
// This is the "physical world" brain. It reads ultrasonic distance sensors,
// converts the closest detected distance into a 0-255 proximity byte, and
// sends it to Brain 2 (audio engine) over I2C. It also directly controls
// the vibrating motors and LED strip strobes via MOSFET-switched 12V PWM.
//
// DUAL-CORE ARCHITECTURE:
//   Core 0 — Sensor sweep (blocking pulseIn calls) + I2C transmission
//   Core 1 — LED strobe + motor PWM (independent timing, never blocked)
//
// The RP2040 has two independent ARM Cortex-M0+ cores. The arduino-pico
// framework lets you use both by defining setup1()/loop1() alongside the
// normal setup()/loop(). Both cores share RAM (including currentProximity),
// but execute independently.
//
// Core 1 runs the strobe because Core 0's sensor sweep blocks for up to
// ~300ms per cycle (pulseIn waits for each ultrasonic echo). If the strobe
// ran on Core 0, it would freeze during sensor reads.
// ===========================================================================

#include <Arduino.h>             // Core Arduino: pinMode, analogWrite, millis, etc.
#include <Wire.h>                // I2C library for communication with Brain 2
#include <Adafruit_NeoPixel.h>   // Onboard WS2812 RGB LED on GPIO16
#include "chaos_protocol.h"      // Shared constants: pins, I2C address, message format

// ===========================================================================
// TUNING PARAMETERS — adjust these during exhibit setup
// ===========================================================================

// --- Detection Range ---
constexpr uint16_t DETECT_MIN_CM = 1;       // Minimum detection distance (cm)
constexpr uint16_t DETECT_MAX_CM = 75;      // Maximum detection distance (cm)

// --- Motors ---
// Baseline: soft constant vibration that scales with proximity.
// Outbursts: random bursts to higher intensity, one motor at a time.
constexpr uint8_t MOTOR_BASELINE_MIN = 0;       // Baseline PWM at prox=0 (off when nobody near)
constexpr uint8_t MOTOR_BASELINE_MAX = 40;      // Baseline PWM at max proximity (gentle hum)
constexpr uint8_t MOTOR_BURST_MIN = 80;         // Minimum burst intensity
constexpr uint8_t MOTOR_BURST_MAX = 180;        // Maximum burst intensity (255 = full 12V)
constexpr uint16_t MOTOR_BURST_DUR_MIN = 50;    // Shortest burst duration (ms)
constexpr uint16_t MOTOR_BURST_DUR_MAX = 400;   // Longest burst duration (ms)
constexpr uint16_t MOTOR_PAUSE_MIN = 200;       // Shortest gap between bursts (ms)
constexpr uint16_t MOTOR_PAUSE_MAX = 2000;      // Longest gap between bursts (ms)
constexpr uint8_t MOTOR_FADE_SPEED = 8;         // PWM change per loop iteration during fade
                                                 // Higher = faster fade. At ~10kHz loop rate,
                                                 // 8 means full 0-255 ramp in ~3ms (sharp).
                                                 // 2 would be ~12ms (softer).

// --- LED Strobe ---
// IDLE mode: slow dim pulse when nobody is nearby (installation looks alive).
// ACTIVE mode: speed and brightness increase with proximity until full strobe.
// DROPOUTS: each LED independently goes dark for a random duration, then returns.
constexpr uint8_t LED_IDLE_BRIGHT = 15;          // Idle ON brightness (0-255)
constexpr uint8_t LED_IDLE_DIM = 5;              // Idle OFF brightness (0 = fully off)
constexpr uint16_t LED_IDLE_PERIOD_MS = 1000;    // Idle half-period in ms (1000 = 0.5Hz)
constexpr uint8_t LED_STROBE_BRIGHT_MIN = 30;    // Active brightness at low proximity
constexpr uint8_t LED_STROBE_BRIGHT_MAX = 255;   // Active brightness at max proximity
constexpr uint32_t LED_STROBE_SLOW_US = 500000;  // Slowest strobe half-period (µs) = 1Hz
constexpr uint32_t LED_STROBE_FAST_US = 2000;    // Fastest strobe half-period (µs) = 250Hz
constexpr uint8_t LED_PROX_THRESHOLD = 5;        // Below this proximity, LEDs go idle
constexpr uint16_t LED_DROPOUT_DUR_MIN = 30;     // Shortest dropout (ms)
constexpr uint16_t LED_DROPOUT_DUR_MAX = 100;    // Longest dropout (ms)
constexpr uint8_t LED_DROPOUT_CHANCE_MIN = 0;    // Dropout probability at low prox (0=never)
constexpr uint8_t LED_DROPOUT_CHANCE_MAX = 20;   // Dropout probability at max prox (out of 255)

// --- Sensor Filtering ---
constexpr uint8_t SENSOR_ZERO_MAX = 10;

// ===========================================================================
// END TUNING PARAMETERS
// ===========================================================================

// Onboard NeoPixel — blue heartbeat indicates Brain 1 is running.
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// --- State variables ---
uint16_t sensorDistances[NUM_SENSORS] = {0};  // Current reading per sensor (cm)
volatile uint8_t currentProximity = 0;         // Shared between cores. 0=nobody, 255=close.
uint8_t validSensorCount = 0;                  // How many sensors got valid readings this sweep
uint32_t lastSweepTime = 0;                    // Timestamp of last sweep start

// ---------------------------------------------------------------------------
// Fast pseudo-random number generator (LCG)
// ---------------------------------------------------------------------------
// We need random numbers on Core 1 for the chaos effects. The standard
// random() function is not guaranteed to be thread-safe on the RP2040,
// so we use our own simple linear congruential generator. It's not
// cryptographically secure, but it's perfectly fine for visual/tactile
// randomness. The constants are the same ones used in glibc.
uint32_t chaosRNG = 42;  // Seed (any nonzero value works)

uint32_t chaosRandom() {
    chaosRNG = chaosRNG * 1664525 + 1013904223;
    return chaosRNG;
}

// Returns a random value in the range [min, max] inclusive.
uint16_t chaosRandomRange(uint16_t minVal, uint16_t maxVal) {
    if (minVal >= maxVal) return minVal;
    uint32_t range = maxVal - minVal + 1;
    return minVal + (uint16_t)(chaosRandom() % range);
}

// ---------------------------------------------------------------------------
// Ultrasonic Sensor Reading
// ---------------------------------------------------------------------------
uint16_t readSinglePing(uint8_t echoPin) {
    digitalWrite(PIN_US_TRIGGER, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_US_TRIGGER, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_US_TRIGGER, LOW);

    uint32_t duration = pulseIn(echoPin, HIGH, US_TIMEOUT_US);
    if (duration == 0) return 0;

    uint16_t distanceCm = duration / 58;
    if (distanceCm > DETECT_MAX_CM) return 0;
    return distanceCm;
}

uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { uint16_t t = b; b = c; c = t; }
    if (a > b) { uint16_t t = a; a = b; b = t; }
    return b;
}

uint16_t readSensorCm(uint8_t echoPin) {
    uint16_t r1 = readSinglePing(echoPin);
    delayMicroseconds(200);
    uint16_t r2 = readSinglePing(echoPin);
    delayMicroseconds(200);
    uint16_t r3 = readSinglePing(echoPin);
    return median3(r1, r2, r3);
}

// ---------------------------------------------------------------------------
// Sensor Sweep with timeout-based release
// ---------------------------------------------------------------------------
uint16_t lastValidDistance[NUM_SENSORS] = {0};
uint8_t zeroCount[NUM_SENSORS] = {0};

void sweepSensors() {
    uint16_t minDistance = DETECT_MAX_CM;
    validSensorCount = 0;

    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        uint16_t reading = readSensorCm(ECHO_PINS[i]);

        if (reading > 0) {
            sensorDistances[i] = reading;
            lastValidDistance[i] = reading;
            zeroCount[i] = 0;
        } else {
            zeroCount[i]++;
            if (zeroCount[i] >= SENSOR_ZERO_MAX) {
                lastValidDistance[i] = 0;
                sensorDistances[i] = 0;
            } else {
                sensorDistances[i] = lastValidDistance[i];
            }
        }

        if (sensorDistances[i] > 0) {
            validSensorCount++;
            if (sensorDistances[i] < minDistance) {
                minDistance = sensorDistances[i];
            }
        }
    }

    if (validSensorCount == 0) {
        currentProximity = 0;
    } else {
        currentProximity = (uint8_t)map(
            constrain(minDistance, DETECT_MIN_CM, DETECT_MAX_CM),
            DETECT_MIN_CM, DETECT_MAX_CM,
            255, 1
        );
    }
}

// ---------------------------------------------------------------------------
// I2C Transmission to Brain 2
// ---------------------------------------------------------------------------
void sendProximityToBrain2() {
    ProximityMessage msg;
    msg.header = MSG_HEADER_MAGIC;
    msg.proximity = currentProximity;
    msg.sensor_count = validSensorCount;
    msg.checksum = computeChecksum(msg.header, msg.proximity, msg.sensor_count);

    Wire.beginTransmission(I2C_TARGET_ADDRESS);
    Wire.write((uint8_t*)&msg, sizeof(msg));
    uint8_t error = Wire.endTransmission();
    if (error != 0) {
        Serial.print("I2C error: ");
        Serial.println(error);
    }
}

// ---------------------------------------------------------------------------
// Core 1: Chaotic LED Strobe + Motor Outbursts
// ---------------------------------------------------------------------------

// --- Motor outburst state (one per motor) ---
struct MotorState {
    uint8_t currentPWM;      // What we're actually outputting right now
    uint8_t targetPWM;       // What we're fading toward
    uint32_t burstEndTime;   // When the current burst expires (millis)
    uint32_t nextBurstTime;  // Earliest time the next burst can start (millis)
    bool bursting;           // Is this motor currently in a burst?
    bool useFade;            // Does this burst fade in/out or snap?
};

MotorState motors[NUM_MOTORS];

// --- LED dropout state (one per LED strip) ---
struct LEDDropoutState {
    uint32_t dropoutEndTime;   // When this LED comes back from dropout (millis)
    bool droppedOut;           // Is this LED currently blacked out?
};

LEDDropoutState ledDropouts[NUM_LEDS];

// Core 1 setup
void setup1() {
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        pinMode(LED_PINS[i], OUTPUT);
        digitalWrite(LED_PINS[i], LOW);
        ledDropouts[i].droppedOut = false;
        ledDropouts[i].dropoutEndTime = 0;
    }
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        pinMode(MOTOR_PINS[i], OUTPUT);
        analogWrite(MOTOR_PINS[i], 0);
        motors[i].currentPWM = 0;
        motors[i].targetPWM = 0;
        motors[i].burstEndTime = 0;
        motors[i].nextBurstTime = 0;
        motors[i].bursting = false;
        motors[i].useFade = false;
    }

    // Seed the RNG with something that varies between boots.
    // micros() at the point Core 1 starts will differ slightly each time
    // due to USB enumeration timing, giving us a different sequence.
    chaosRNG = micros() ^ 0xDEADBEEF;
}

// Core 1 main loop
void loop1() {
    uint8_t prox = currentProximity;
    uint32_t now = millis();

    // =====================================================================
    // MOTORS: baseline hum + independent random outbursts
    // =====================================================================
    // Baseline PWM scales linearly with proximity.
    uint8_t baseline = MOTOR_BASELINE_MIN +
        ((uint16_t)prox * (MOTOR_BASELINE_MAX - MOTOR_BASELINE_MIN) / 255);

    // Check if any motor is currently bursting (we enforce one-at-a-time).
    bool anyBursting = false;
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        if (motors[i].bursting) {
            anyBursting = true;
            break;
        }
    }

    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        MotorState& m = motors[i];

        if (m.bursting) {
            // --- Currently in a burst ---
            if (now >= m.burstEndTime) {
                // Burst is over: set target back to baseline
                m.bursting = false;
                m.targetPWM = baseline;
                // Schedule the next possible burst for this motor.
                // The pause duration shrinks with proximity: close = more frequent.
                uint16_t pauseRange = MOTOR_PAUSE_MAX - MOTOR_PAUSE_MIN;
                uint16_t pause = MOTOR_PAUSE_MAX -
                    (uint16_t)((uint32_t)prox * pauseRange / 255);
                m.nextBurstTime = now + chaosRandomRange(pause / 2, pause);
            }
        } else if (prox > 0 && !anyBursting && now >= m.nextBurstTime) {
            // --- Eligible for a new burst ---
            // Not all motors get picked every cycle. We roll the dice
            // to choose one randomly. This creates organic timing.
            // Higher proximity = higher chance of triggering.
            uint8_t roll = (uint8_t)(chaosRandom() >> 24);
            uint8_t chance = (uint8_t)((uint32_t)prox * 30 / 255);  // 0-30 out of 255
            if (roll < chance) {
                m.bursting = true;
                anyBursting = true;

                // Random burst intensity, biased higher at close proximity
                uint8_t burstIntensity = MOTOR_BURST_MIN +
                    (uint8_t)((uint32_t)prox * (MOTOR_BURST_MAX - MOTOR_BURST_MIN) / 255);
                // Add some randomness to the intensity (jitter +-20%)
                int16_t jitter = (int16_t)chaosRandomRange(0, burstIntensity / 5) -
                                 (int16_t)(burstIntensity / 10);
                burstIntensity = (uint8_t)constrain(
                    (int16_t)burstIntensity + jitter, MOTOR_BURST_MIN, 255);
                m.targetPWM = burstIntensity;

                // Random burst duration: shorter at low prox, longer at high prox
                uint16_t durRange = MOTOR_BURST_DUR_MAX - MOTOR_BURST_DUR_MIN;
                uint16_t dur = MOTOR_BURST_DUR_MIN +
                    (uint16_t)((uint32_t)prox * durRange / 255);
                // Add randomness to duration too
                m.burstEndTime = now + chaosRandomRange(dur / 2, dur);

                // Randomly choose fade or sharp attack (50/50)
                m.useFade = (chaosRandom() & 1) == 0;
            }
        }

        // If not bursting, target is always the baseline
        if (!m.bursting) {
            m.targetPWM = baseline;
        }

        // --- Apply fade or snap toward target ---
        if (m.useFade || !m.bursting) {
            // Fade: move currentPWM toward targetPWM by MOTOR_FADE_SPEED per iteration.
            // We always fade back DOWN to baseline (smooth return).
            // We randomly fade or snap UP to burst target (decided above).
            if (m.currentPWM < m.targetPWM) {
                uint16_t next = (uint16_t)m.currentPWM + MOTOR_FADE_SPEED;
                m.currentPWM = (next > m.targetPWM) ? m.targetPWM : (uint8_t)next;
            } else if (m.currentPWM > m.targetPWM) {
                int16_t next = (int16_t)m.currentPWM - MOTOR_FADE_SPEED;
                m.currentPWM = (next < m.targetPWM) ? m.targetPWM : (uint8_t)next;
            }
        } else {
            // Sharp: jump immediately to target
            m.currentPWM = m.targetPWM;
        }

        analogWrite(MOTOR_PINS[i], m.currentPWM);
    }

    // =====================================================================
    // LEDs: unified strobe with independent random dropouts
    // =====================================================================
    // The base strobe logic is the same as before (all LEDs share the same
    // toggle timer and rate). On top of that, each LED independently
    // decides to "drop out" (go dark) for a random duration.

    static uint32_t lastToggle = 0;
    static bool ledState = false;

    // Compute what brightness the strobe wants this cycle
    uint8_t strobeBrightness = 0;

    if (prox < LED_PROX_THRESHOLD) {
        // IDLE: slow dim pulse
        uint32_t nowMs = millis();
        if (nowMs - lastToggle >= LED_IDLE_PERIOD_MS) {
            lastToggle = nowMs;
            ledState = !ledState;
        }
        strobeBrightness = ledState ? LED_IDLE_BRIGHT : LED_IDLE_DIM;
    } else {
        // ACTIVE STROBE: speed and brightness scale with proximity
        uint32_t halfPeriodUs = map(prox, LED_PROX_THRESHOLD, 255,
                                    LED_STROBE_SLOW_US, LED_STROBE_FAST_US);
        uint8_t brightness = map(prox, LED_PROX_THRESHOLD, 255,
                                 LED_STROBE_BRIGHT_MIN, LED_STROBE_BRIGHT_MAX);
        uint32_t nowUs = micros();
        if (nowUs - lastToggle >= halfPeriodUs) {
            lastToggle = nowUs;
            ledState = !ledState;
        }
        strobeBrightness = ledState ? brightness : 0;
    }

    // Apply per-LED dropout logic on top of the shared strobe
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        LEDDropoutState& d = ledDropouts[i];

        if (d.droppedOut) {
            // Currently in a dropout: stay dark until time expires
            if (now >= d.dropoutEndTime) {
                d.droppedOut = false;
            }
        } else if (prox >= LED_PROX_THRESHOLD) {
            // Not dropped out: roll the dice each strobe ON edge to maybe start one.
            // We only check on the ON phase to avoid triggering during dark phases,
            // which would be invisible and waste the dropout.
            if (ledState) {
                uint8_t roll = (uint8_t)(chaosRandom() >> 24);
                uint8_t chance = LED_DROPOUT_CHANCE_MIN +
                    ((uint16_t)prox * (LED_DROPOUT_CHANCE_MAX - LED_DROPOUT_CHANCE_MIN) / 255);
                if (roll < chance) {
                    d.droppedOut = true;
                    d.dropoutEndTime = now +
                        chaosRandomRange(LED_DROPOUT_DUR_MIN, LED_DROPOUT_DUR_MAX);
                }
            }
        }

        // Output: if dropped out, force dark. Otherwise, use strobe brightness.
        analogWrite(LED_PINS[i], d.droppedOut ? 0 : strobeBrightness);
    }

    // Yield bus time to Core 0
    delayMicroseconds(100);
}

// ---------------------------------------------------------------------------
// Setup (Core 0)
// ---------------------------------------------------------------------------
void setup() {
    pixel.begin();
    pixel.setBrightness(30);
    pixel.setPixelColor(0, pixel.Color(0, 0, 255));
    pixel.show();

    Serial.begin(115200);
    delay(2000);

    Serial.println("=== Chaos Piano: Brain 1 (Physical Controller) ===");
    Serial.println("Core 0: Sensors + I2C | Core 1: LED strobe + Motors");
    Serial.print("Detection: ");
    Serial.print(DETECT_MIN_CM);
    Serial.print("-");
    Serial.print(DETECT_MAX_CM);
    Serial.println("cm");
    Serial.print("Motor baseline: ");
    Serial.print(MOTOR_BASELINE_MIN);
    Serial.print("-");
    Serial.print(MOTOR_BASELINE_MAX);
    Serial.print(" | Burst: ");
    Serial.print(MOTOR_BURST_MIN);
    Serial.print("-");
    Serial.println(MOTOR_BURST_MAX);
    Serial.print("Strobe: ");
    Serial.print(LED_STROBE_SLOW_US);
    Serial.print("-");
    Serial.print(LED_STROBE_FAST_US);
    Serial.println("us");
    Serial.print("LED dropout chance: ");
    Serial.print(LED_DROPOUT_CHANCE_MIN);
    Serial.print("-");
    Serial.println(LED_DROPOUT_CHANCE_MAX);

    pinMode(PIN_US_TRIGGER, OUTPUT);
    digitalWrite(PIN_US_TRIGGER, LOW);
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        pinMode(ECHO_PINS[i], INPUT);
    }

    Wire.setSDA(PIN_I2C_SDA);
    Wire.setSCL(PIN_I2C_SCL);
    Wire.setClock(I2C_CLOCK_HZ);
    Wire.begin();

    Serial.println("Brain 1 ready. Proximity: 0=nobody, 255=close.");
}

// ---------------------------------------------------------------------------
// Main Loop (Core 0) — sensor sweep at 20Hz
// ---------------------------------------------------------------------------
void loop() {
    uint32_t now = millis();

    bool neoOn = ((now / 1000) % 2) == 0;
    pixel.setPixelColor(0, neoOn ? pixel.Color(0, 0, 255) : pixel.Color(0, 0, 0));
    pixel.show();

    if (now - lastSweepTime >= SENSOR_SWEEP_MS) {
        lastSweepTime = now;
        sweepSensors();
        sendProximityToBrain2();

        Serial.print("Prox: ");
        Serial.print(currentProximity);
        Serial.print(" | Sensors: ");
        Serial.print(validSensorCount);
        Serial.print(" | Distances: ");
        for (uint8_t i = 0; i < NUM_SENSORS; i++) {
            Serial.print(sensorDistances[i]);
            Serial.print("cm ");
        }
        Serial.println();
    }
}