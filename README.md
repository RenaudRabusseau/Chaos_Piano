# Chaos Piano

## Project Overview

The Chaos Piano is an interactive art installation developed for the Fondation du Doute. It transforms visitor proximity into a coordinated response of light, vibration, and sound, using a decommissioned upright piano as its physical host.

The system is designed around a control-command architecture split across two microcontrollers. As a visitor approaches the piano, the system responds with increasing intensity: vibrating motors excite the piano strings, LED strips strobe in rhythmic patterns, and piezoelectric sensors capture the resulting string vibrations, which are processed in real time through a chain of digital audio effects and output through a speaker array.

The project serves as a practical demonstration of embedded systems design, real-time signal processing, and human-machine interaction, developed as part of a BTS CIEL curriculum.

---

## System Architecture

### Dual-Brain Strategy

The final system splits its logic across two RP2040-Zero microcontrollers to achieve electrical isolation between the high-power actuation domain and the noise-sensitive audio domain.

**Brain 1 — Physical Controller**
- Manages visitor proximity detection via 4x HC-SR04 ultrasonic sensors
- Controls 3x 12V vibrating motors and 3x 12V LED strips via IRLZ44N MOSFETs
- Sends proximity data to Brain 2 over I2C

**Brain 2 — Audio Engine**
- Captures piano string vibrations via raw piezo discs and MAX9814 preamp modules
- Applies real-time DSP effects (distortion, bitcrush, flanger) via RP2040 dual-core processing
- Outputs processed audio through a PCM5102 I2S DAC to a PAM8610 amplifier
- Manages ambient MP3 playback via DFPlayer Mini, modulated by proximity data from Brain 1

### Three-Board Hardware Architecture

**Board 1 — Power and Actuation**
Handles all 12V switching. Contains the buck converter (12V to 5V), 6x MOSFETs, and output terminals for motors and LED strips. Physically isolated from signal boards to contain EMI.

**Board 2 — Brain Board**
Houses both RP2040-Zero modules, I2C pull-up resistors, and input terminals for sensors and piezo preamps.

**Board 3 — Audio Processing**
Contains the DFPlayer Mini, PCM5102 I2S DAC, PAM8610 amplifier, and output terminals for the speaker array.

### Power Distribution

| Component | Voltage | Source |
|---|---|---|
| Motors and LED strips | 12V | Main supply (direct) |
| PAM8610 amplifier | 12V | Main supply (direct) |
| RP2040 brains | 5V | Buck converter |
| DFPlayer Mini | 5V | Buck converter |
| MAX9814 preamps | 5V | Buck converter |
| PCM5102 DAC | 3.3V | RP2040 onboard regulator |

### Critical Wiring Constraints

- All grounds share a single common point (WAGO connector) to prevent ground loops
- 1kΩ resistor on DFPlayer RX line to suppress serial noise
- 12V motor and LED wiring kept physically separated from audio and I2C signal wiring
- Audio wiring uses twisted pairs throughout

---

## Sensor Strategy: Piezo Pickup

Early testing revealed that the DollaTek piezo modules are unsuitable for continuous audio pickup. Their onboard comparator circuitry functions as a tap detector, gating out sustained vibrations like plucked strings. The final design uses raw piezo discs connected directly to MAX9814 autogain preamp modules (mic capsule removed, piezo soldered to input pads). This provides a continuous, amplified signal suitable for real-time DSP processing.

Guitar pickups (electromagnetic) are maintained as a backup input option and may prove superior for electromagnetic string excitation from the vibrating motors.

---

## Software Logic

1. Brain 1 sweeps the 4 ultrasonic sensors and maps the closest detected distance to a single byte (0-255)
2. Brain 1 sends that byte to Brain 2 over I2C
3. Brain 2 adjusts DFPlayer volume and motor burst patterns based on proximity
4. Brain 2 continuously samples the piezo preamp output, applies the DSP chain, and streams processed audio to the DAC

### DSP Effects Chain (Brain 2)

1. Sample raw piezo signal via 12-bit ADC
2. Distortion: multiply and hard-clip
3. Bitcrush: reduce effective bit depth via bitmask
4. Flanger: circular delay buffer with triangle LFO
5. Output via I2S to PCM5102 DAC

---

## Repository Structure

```
Chaos_Piano/
├── README.md
├── platformio.ini
├── brain1_physical/
│   └── src/
│       └── main.cpp        (Brain 1: sensors, motors, LEDs, I2C)
├── brain2_audio/
│   └── src/
│       └── main.cpp        (Brain 2: piezo, effects, DFPlayer, DAC)
└── docs/
    ├── v0.1_validation.md
    ├── v0.2_validation.md
    └── schematics/
        ├── Chaos_Piano_V0.1.kicad_pro
        ├── Chaos_Piano_V0.1.kicad_sch
        └── Chaos_Piano_V0.1.pdf
```

---

## Validation History

| Version | Focus | Status |
|---|---|---|
| v0.1 | Proximity sensing, MOSFET switching, motor and LED control | Validated |
| v0.2 | Audio chain: piezo pickup, DFPlayer serial control, DSP effects | Validated |
| v1.0 | Full integrated system on RP2040 with I2S DAC | In progress |

---

## Academic Context

This project demonstrates practical competency across the Purdue Model automation hierarchy:

- **Level 0 — Field:** Ultrasonic sensing, piezoelectric transduction, motor actuation
- **Level 1 — Control:** Real-time DSP, PWM modulation, I2C inter-processor communication
- **Level 2 — Supervision:** Proximity-driven state machine, ambient audio modulation

Additional competencies demonstrated: MOSFET high-side switching, flyback protection, common-ground power architecture, embedded C++ firmware development with PlatformIO.
