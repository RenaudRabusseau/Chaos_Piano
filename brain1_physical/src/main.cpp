// ============================================================
// CHAOS PIANO — Brain 1: Physical Controller
// Validation v0.2 — Arduino UNO R3
// ------------------------------------------------------------
// Signal Chain:
//   4x HC-SR04 Ultrasonic Sensors → Proximity Detection
//   → 3x Vibrating Motors (via MOSFET) — Chaos Actuation
//   → 3x LED Strips (via MOSFET)      — Proximity Strobe
//   → I2C → Brain 2 (distance byte 0-255)
// ------------------------------------------------------------
// Current validation hardware: 1x sensor, 1x motor, 1x LED strip
// Final exhibit arrays are defined but commented out below.
// ============================================================

#include <Arduino.h>
#include <Wire.h>

// --- PIN DEFINITIONS ---
#define PIN_TRIG      11  // HC-SR04 trigger
#define PIN_ECHO      8   // HC-SR04 echo
#define PIN_LED_STRIP 10  // LED strip (via MOSFET)
#define PIN_MOTOR     6   // Vibrating motor (via MOSFET)

// --- FINAL EXHIBIT PIN ARRAYS ---
// Uncomment when scaling to full hardware
// const int PINS_TRIG[]  = {11, 7, 4, 2};
// const int PINS_ECHO[]  = {8,  5, 3, 12};
// const int PINS_MOTOR[] = {6,  A1, A2};
// const int PINS_LED[]   = {10, A3, 13};
// #define NUM_SENSORS 4
// #define NUM_MOTORS  3
// #define NUM_LEDS    3

// --- PROXIMITY SETTINGS ---
#define DISTANCE_MIN  10.0   // cm — closest expected range
#define DISTANCE_MAX  50.0   // cm — activation threshold
#define DISTANCE_IDLE 200.0  // cm — reported when no echo

// --- LED STROBE ---
#define STROBE_FAST 35   // ms between blinks at closest range
#define STROBE_SLOW 450  // ms between blinks at furthest range

// --- MOTOR CHAOS ---
// Motor alternates between BURSTING and IDLE states.
// Burst and gap durations are randomised and shrink as audience gets closer.
// Result: glitchy/intermittent when far, almost continuous when close.
#define BURST_MIN_CLOSE 30    // ms
#define BURST_MAX_CLOSE 120   // ms
#define BURST_MIN_FAR   50    // ms
#define BURST_MAX_FAR   300   // ms
#define GAP_MIN_CLOSE   20    // ms
#define GAP_MAX_CLOSE   150   // ms
#define GAP_MIN_FAR     200   // ms
#define GAP_MAX_FAR     1200  // ms

// --- I2C ---
#define I2C_BRAIN2_ADDRESS 0x08  // Brain 2 target address

// ============================================================

float         currentDistance = DISTANCE_IDLE;
unsigned long prevSensor      = 0;
unsigned long prevBlink       = 0;
bool          isLedActive     = false;

enum MotorState { MOTOR_BURSTING, MOTOR_IDLE };
MotorState    motorState    = MOTOR_IDLE;
unsigned long motorStateEnd = 0;
int           motorPower    = 0;

// ============================================================
// HELPERS
// ============================================================

// Returns distance in cm from HC-SR04. Returns 0 if no echo.
float readDistance(int pinTrig, int pinEcho) {
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);
  long duration = pulseIn(pinEcho, HIGH, 20000);
  return (duration * 0.034) / 2.0;
}

// Maps distance to 0-255 byte for I2C transmission to Brain 2.
// 255 = audience at DISTANCE_MIN (closest)
// 0   = audience at DISTANCE_MAX or beyond (absent)
byte distanceToByte(float distance) {
  if (distance >= DISTANCE_MAX) return 0;
  return (byte)constrain(
    map((int)distance, (int)DISTANCE_MIN, (int)DISTANCE_MAX, 255, 0),
    0, 255
  );
}

// ============================================================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG,      OUTPUT);
  pinMode(PIN_ECHO,      INPUT);
  pinMode(PIN_LED_STRIP, OUTPUT);
  pinMode(PIN_MOTOR,     OUTPUT);

  // High-speed Timer 1 for pins 9 & 10 (31.4 kHz PWM)
  // Reduces audible whine on PWM-controlled motors
  TCCR1B = (TCCR1B & 0b11111000) | 0x01;

  Wire.begin(); // I2C controller

  Serial.println(F("Brain 1 ready — Physical Controller"));
}

void loop() {
  unsigned long now = millis();

  // --- 1. PROXIMITY SENSING (every 40ms) ---
  if (now - prevSensor >= 40) {
    prevSensor = now;

    float newDistance = readDistance(PIN_TRIG, PIN_ECHO);

    // EMA filter: smooth out sensor noise
    if (newDistance >= 2.0 && newDistance <= DISTANCE_MAX) {
      currentDistance = (currentDistance * 0.7) + (newDistance * 0.3);
    } else if (newDistance == 0) {
      currentDistance = (currentDistance * 0.7) + (DISTANCE_IDLE * 0.3);
    } else {
      currentDistance++;
    }

    // Send distance byte to Brain 2 over I2C
    byte distByte = distanceToByte(currentDistance);
    Wire.beginTransmission(I2C_BRAIN2_ADDRESS);
    Wire.write(distByte);
    Wire.endTransmission();

    // --- DEBUG ---
    // Uncomment to monitor distance in Serial Monitor (USB only):
    // Serial.print(F("Distance: ")); Serial.print(currentDistance);
    // Serial.print(F(" cm | I2C byte: ")); Serial.println(distByte);
  }

  // --- 2. ACTUATION ---
  if (currentDistance <= DISTANCE_MAX) {

    byte proximity = distanceToByte(currentDistance); // 0-255, 255 = closest

    // LED strobe — faster and brighter as audience approaches
    int blinkInt = constrain(
      map(proximity, 0, 255, STROBE_SLOW, STROBE_FAST),
      STROBE_FAST, STROBE_SLOW
    );
    int bright = constrain(map(proximity, 0, 255, 5, 255), 5, 255);

    if (now - prevBlink >= (unsigned long)blinkInt) {
      prevBlink   = now;
      isLedActive = !isLedActive;
      analogWrite(PIN_LED_STRIP, isLedActive ? bright : 0);
    }

    // Motor chaos — random bursts with gaps that shrink as audience approaches
    motorPower = constrain(map(proximity, 0, 255, 70, 255), 70, 255);

    if (now >= motorStateEnd) {
      if (motorState == MOTOR_BURSTING) {
        motorState = MOTOR_IDLE;
        analogWrite(PIN_MOTOR, 0);
        int gapMin    = map(proximity, 0, 255, GAP_MIN_FAR,   GAP_MIN_CLOSE);
        int gapMax    = map(proximity, 0, 255, GAP_MAX_FAR,   GAP_MAX_CLOSE);
        motorStateEnd = now + random(gapMin, gapMax);
      } else {
        motorState = MOTOR_BURSTING;
        analogWrite(PIN_MOTOR, motorPower);
        int burstMin  = map(proximity, 0, 255, BURST_MIN_FAR, BURST_MIN_CLOSE);
        int burstMax  = map(proximity, 0, 255, BURST_MAX_FAR, BURST_MAX_CLOSE);
        motorStateEnd = now + random(burstMin, burstMax);
      }
    }

  } else {
    // Standby — audience out of range
    analogWrite(PIN_LED_STRIP, 0);
    analogWrite(PIN_MOTOR, 0);
    isLedActive = false;
    motorState  = MOTOR_IDLE;
  }
}