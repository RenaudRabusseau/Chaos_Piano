/**
 * @author Rabusseau Renaud
 * @project Chaos Piano - Interactive Art Exhibit
 * @date 2026-02-13
 * * DESCRIPTION:
 * Uses an HC-SR04 ultrasonic sensor to trigger a 12V LED strip via an IRLZ44N MOSFET.
 * As a visitor approaches, the strobe frequency and light intensity increase.
 */

#include <Arduino.h>
#include <LiquidCrystal.h>

// --- Pin Definitions ---
// LCD Display (Parallel Interface)
const int PIN_LCD_RS = 12;
const int PIN_LCD_E  = 11;
const int PIN_LCD_D4 = 5;
const int PIN_LCD_D5 = 4;
const int PIN_LCD_D6 = 3;
const int PIN_LCD_D7 = 2;

// Ultrasonic Sensor (HC-SR04)
const int PIN_TRIG   = 9;
const int PIN_ECHO   = 8;

// Actuators
const int PIN_LED_STRIP = 10; // PWM capable for MOSFET gate control
const int PIN_BUZZER    = 7;  // Active buzzer for haptic/audio feedback

// --- Configuration Constants ---
const float DISTANCE_MIN  = 10.0;   // Distance for maximum chaos (cm)
const float DISTANCE_MAX  = 150.0;  // Distance where system activates (cm)
const int STROBE_FAST     = 30;     // Fast blink interval in ms (close)
const int STROBE_SLOW     = 500;    // Slow blink interval in ms (far)
const int BRIGHT_MAX      = 255;    // Full 12V intensity
const int BRIGHT_MIN      = 5;      // Dim state for distant detection

// --- Object Initialization ---
LiquidCrystal lcd(PIN_LCD_RS, PIN_LCD_E, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);

// --- Global Variables ---
float currentDistance     = 200.0;  // Filtered distance value
unsigned long prevBlink   = 0;      // Timing for LED strobe
unsigned long prevLCD     = 0;      // Timing for LCD refresh
unsigned long prevSensor  = 0;      // Timing for Ultrasonic ping
bool isLedActive          = false;  // Toggle state for strobing

void setup() {
    Serial.begin(9600);
    
    // LCD Stabilization & Initialization
    delay(1000); 
    lcd.begin(16, 2);
    lcd.clear();
    lcd.print("CHAOS PIANO");
    lcd.setCursor(0, 1);
    lcd.print("BOOTING...");
    
    // Pin Modes
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    pinMode(PIN_LED_STRIP, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    
    delay(1500);
    lcd.clear();
}

void loop() {
    unsigned long currentMillis = millis();

    // 1. SENSOR DATA ACQUISITION (Ping every 35ms)
    if (currentMillis - prevSensor >= 35) {
        prevSensor = currentMillis;
        
        digitalWrite(PIN_TRIG, LOW);
        delayMicroseconds(2);
        digitalWrite(PIN_TRIG, HIGH);
        delayMicroseconds(10);
        digitalWrite(PIN_TRIG, LOW);
        
        long duration = pulseIn(PIN_ECHO, HIGH, 25000); // 25ms timeout
        float newDistance = (duration * 0.034) / 2.0;

        // Apply Low-Pass Filter to smooth out sensor noise
        if (newDistance >= 2.0 && newDistance <= DISTANCE_MAX) {
            currentDistance = (currentDistance * 0.7) + (newDistance * 0.3);
        } else {
            // Drift back to "idle" state if no object detected
            currentDistance = (currentDistance * 0.95) + (200.0 * 0.05);
        }
    }

    // 2. LOGIC & ACTUATION
    if (currentDistance <= DISTANCE_MAX) {
        // Calculate strobe speed: closer = smaller interval (faster)
        int blinkInterval = map((int)currentDistance, DISTANCE_MIN, DISTANCE_MAX, STROBE_FAST, STROBE_SLOW);
        blinkInterval = constrain(blinkInterval, STROBE_FAST, STROBE_SLOW);

        // Calculate intensity: closer = higher PWM value
        int brightness = map((int)currentDistance, DISTANCE_MIN, DISTANCE_MAX, BRIGHT_MAX, BRIGHT_MIN);
        brightness = constrain(brightness, BRIGHT_MIN, BRIGHT_MAX);

        // Strobe Timing Block
        if (currentMillis - prevBlink >= (unsigned long)blinkInterval) {
            prevBlink = currentMillis;
            isLedActive = !isLedActive;
            
            // Apply outputs
            analogWrite(PIN_LED_STRIP, isLedActive ? brightness : 0);
            
            // Sync buzzer with light strobe if extremely close
            if (currentDistance < 40.0) {
                digitalWrite(PIN_BUZZER, isLedActive ? HIGH : LOW);
            } else {
                digitalWrite(PIN_BUZZER, LOW);
            }
        }
    } else {
        // Standby State
        analogWrite(PIN_LED_STRIP, 0);
        digitalWrite(PIN_BUZZER, LOW);
    }

    // 3. HUMAN INTERFACE (LCD Update every 200ms)
    if (currentMillis - prevLCD >= 200) {
        prevLCD = currentMillis;
        
        lcd.setCursor(0, 0);
        lcd.print("Dist: ");
        lcd.print((int)currentDistance);
        lcd.print(" cm    "); // Clear trailing characters

        lcd.setCursor(0, 1);
        if (currentDistance < DISTANCE_MIN + 5) {
            lcd.print("STATUS: CHAOS!! ");
        } else if (currentDistance <= DISTANCE_MAX) {
            lcd.print("STATUS: ACTIVE  ");
        } else {
            lcd.print("STATUS: IDLE    ");
        }
    }
}