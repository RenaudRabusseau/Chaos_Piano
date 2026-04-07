# Chaos Piano — Architecture Reference

**Version:** April 2026 desk prototype (final working build)
**Target:** La Fondation du Doute, Blois, France — May 2026
**Author:** Renaud

---

## Concept

Interactive sound installation where human proximity controls audio effects,
lighting, and physical actuators. The closer a visitor gets, the more chaotic
the piano becomes: the drone grows louder and more distorted, LED strips strobe
faster with random independent dropouts, motors fire random independent
outbursts, and the ambient track swells.

---

## System Overview

```
  ┌──────────────────────┐          ┌──────────────────────┐
  │    12V PSU #1         │          │    12V PSU #2         │
  │  (brains + actuation) │          │  (audio amplifiers)   │
  └──┬────────┬───────────┘          └──┬────────┬──────────┘
     │        │                         │        │
  ┌──▼──┐  ┌──▼──┐                   ┌──▼──┐  ┌──▼──┐
  │Motor│  │ LED │                   │PAM  │  │PAM  │
  │MOSFET│ │MOSFET│                  │8610 │  │8610 │
  │board │ │board │                  │ #1  │  │ #2  │
  └──▲──┘  └──▲──┘                   └──▲──┘  └──▲──┘
     │        │                         │        │
     │        │     ┌───────────────────┘        │
     │        │     │    ┌──────────────────────-┘
     │        │     │    │
  ┌──┴────────┴─────┴────┴─────────────────────────────────┐
  │                                                        │
  │  ┌──────────────┐    I2C     ┌──────────────────────┐  │
  │  │   BRAIN 1    │◄──────────►│      BRAIN 2         │  │
  │  │  Controller  │  (4 bytes  │    Audio Engine      │  │
  │  │  RP2040-Zero │  @ 20Hz)   │    RP2040-Zero       │  │
  │  └──────┬───────┘            └──┬────┬────┬─────────┘  │
  │         │                       │    │    │            │
  │    ┌────▼────┐             ┌────▼┐ ┌─▼──┐ ┌▼────────┐  │
  │    │ HC-SR04 │             │MAX  │ │I2S │ │DFPlayer │  │
  │    │  ×4     │             │9814 │ │DAC │ │Mini     │  │
  │    └─────────┘             │ ×2  │ │PCM │ │(ambient)│  │
  │                            └─────┘ │5102│ └─────────┘  │
  │                                    └────┘              │
  └────────────────────────────────────────────────────────┘
```

---

## Power Architecture

The system uses two separate 12V power supplies to eliminate ground-coupled
PWM switching noise from the audio path. This was the key fix for audible
LED/motor buzz in the speakers.

| PSU | Voltage | Min Current | Powers |
|-----|---------|-------------|--------|
| PSU #1 | 12V | 2A+ | MOSFET board (motors, LEDs), 5V step-down (sensors, DFPlayer), brains (via USB) |
| PSU #2 | 12V | 2A+ | PAM8610 #1 (live audio), PAM8610 #2 (ambient audio) |

The two PSUs share no ground connection. The brains are powered via USB
from a computer (desk prototype) or from PSU #1's 5V step-down (exhibit mode).
The 5V step-down converts PSU #1's 12V to 5V for the HC-SR04 sensors and
the DFPlayer Mini.

---

## Components

| Component | Qty | Role | Voltage | Notes |
|-----------|-----|------|---------|-------|
| RP2040-Zero (Waveshare) | 2 | Brain 1 (controller), Brain 2 (audio) | 3.3V (USB-C 5V) | Orange tape = Brain 1, new board = Brain 2 |
| HC-SR04 ultrasonic sensor | 4 | Distance detection | 5V | ECHO needs voltage divider (1kΩ + 2kΩ) |
| MAX9814 preamp module | 2 | Condenser mic input | 3.3V | GAIN → 3.3V = 40dB, AR → 3.3V = fast release |
| PCM5102 I2S DAC | 1 | Digital-to-analog audio output | 3.3V | SCK pin → GND (internal clock mode) |
| PAM8610 amplifier | 2 | Speaker driver | 12V (PSU #2) | #1 for live audio, #2 for DFPlayer ambient |
| Speakers (4Ω 10W) | 4 | Sound output | — | 2 per PAM8610 (stereo) |
| DFPlayer Mini | 1 | Ambient track playback | 5V | 4GB MicroSD with 001.mp3 in root |
| IRLZ44N N-MOSFET (TO-220) | 6 | 12V switching for motors/LEDs | — | Logic-level: fully on at 3.3V gate |
| Vibrating motor | 3 | Tactile feedback | 12V | Flyback diode (1N4007) required |
| 12V LED strip (RGB) | 3 | Visual feedback (white strobe) | 12V | All 3 color wires joined → single MOSFET |
| 12V PSU | 2 | Power | 12V | #1 for brains+actuation, #2 for amps |
| 5V step-down converter | 1 | 12V → 5V for sensors + DFPlayer | 5V | Connected to PSU #1 |
| 1kΩ resistor | 5 | 4× voltage divider top + 1× DFPlayer TX | — | |
| 2kΩ resistor | 4 | HC-SR04 voltage divider (bottom) | — | One per echo line |
| 4.7kΩ resistor | 2 | I2C pull-ups (SDA + SCL) | — | To 3.3V, mounted on I2C board |
| 10kΩ resistor | 6 | MOSFET gate pull-downs | — | One per MOSFET: gate → source |
| 1N4007 diode | 3 | Motor flyback protection | — | Cathode (stripe) → motor +, anode → motor − |
| 4GB MicroSD card | 1 | DFPlayer audio storage | — | FAT32, file: /001.mp3 |

---

## Custom Boards

### MOSFET Board
Perfboard with 6× IRLZ44N MOSFETs (3 for LEDs, 3 for motors), 6× 10kΩ
gate pull-down resistors, 3× 1N4007 flyback diodes (motor MOSFETs only),
12V and GND power rails. Screw terminals for load connections.

### Voltage Divider Board
Perfboard with 4× voltage dividers (1kΩ top + 2kΩ bottom) for the HC-SR04
echo lines. Converts 5V echo signals to ~3.3V safe for RP2040 GPIO inputs.
DuPont connectors on the Brain 1 side, open leads to Wago connectors on
the sensor side.

### I2C Board
Small perfboard with 4 screw terminals (SDA, SCL, 3.3V, GND) and 2× 4.7kΩ
pull-up resistors (SDA→3.3V, SCL→3.3V). Both brains connect their I2C lines
here. 3.3V sourced from one brain's 3V3 pin.

---

## Boards

### Brain 1 — Physical Controller

- **Board:** RP2040-Zero (Waveshare), orange tape
- **Serial:** `E66428C51F4BB630`
- **USB port:** `/dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F4BB630-if00`
- **PlatformIO env:** `brain1_controller`
- **NeoPixel:** Blue heartbeat (1s on, 1s off)
- **Role:** Reads 4 ultrasonic sensors, computes proximity, sends I2C to Brain 2, drives chaotic motor outbursts and LED strobes with random dropouts
- **Architecture:** Dual-core. Core 0 = sensors + I2C. Core 1 = LED strobe + motor chaos.

### Brain 2 — Audio Engine

- **Board:** RP2040-Zero (Waveshare), no tape
- **Serial:** `E66428C51F63B530`
- **USB port:** `/dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F63B530-if00`
- **PlatformIO env:** `brain2_audio`
- **NeoPixel:** Green triple-flash (3 blinks, 1.5s cycle)
- **Role:** Receives proximity via I2C, generates drone with DSP effects, processes dual live mics, controls DFPlayer ambient track
- **Architecture:** Single-core. Audio processing synchronized to ADCInput DMA at 44100Hz.

---

## Pin Map

### Brain 1

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| 0 | LED strip 0 MOSFET gate | OUT | 10kΩ pull-down to GND |
| 1 | LED strip 1 MOSFET gate | OUT | 10kΩ pull-down to GND |
| 2 | LED strip 2 MOSFET gate | OUT | 10kΩ pull-down to GND |
| 3 | *free* | — | Available on edge header |
| 4 | I2C SDA | I/O | 4.7kΩ pull-up to 3.3V (on I2C board) |
| 5 | I2C SCL | I/O | 4.7kΩ pull-up to 3.3V (on I2C board) |
| 6 | Ultrasonic TRIGGER (shared) | OUT | All 4 sensors share this pin |
| 7 | Ultrasonic ECHO 0 | IN | 1kΩ + 2kΩ voltage divider from 5V |
| 8 | Ultrasonic ECHO 1 | IN | 1kΩ + 2kΩ voltage divider from 5V |
| 9 | Ultrasonic ECHO 2 | IN | 1kΩ + 2kΩ voltage divider from 5V |
| 10 | Ultrasonic ECHO 3 | IN | 1kΩ + 2kΩ voltage divider from 5V |
| 11 | Motor 0 MOSFET gate | OUT | 10kΩ pull-down, flyback diode on motor |
| 12 | Motor 1 MOSFET gate | OUT | 10kΩ pull-down, flyback diode on motor |
| 13 | Motor 2 MOSFET gate | OUT | 10kΩ pull-down, flyback diode on motor |
| 14 | *free* | — | Available on edge header |
| 15 | *free* | — | Available on edge header |
| 16 | Onboard NeoPixel (WS2812) | OUT | Hardwired — cannot be reassigned |

### Brain 2

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| 0 | DFPlayer TX | OUT | 1kΩ resistor to DFPlayer RX |
| 1 | DFPlayer RX | IN | Direct connection |
| 2 | *free* | — | |
| 3 | *free* | — | |
| 4 | I2C SDA | I/O | 4.7kΩ pull-up to 3.3V (on I2C board) |
| 5 | I2C SCL | I/O | 4.7kΩ pull-up to 3.3V (on I2C board) |
| 6 | I2S BCK (bit clock) | OUT | To PCM5102 BCK |
| 7 | I2S LRCK (word select) | OUT | To PCM5102 LCK (auto = BCK+1) |
| 8 | I2S DOUT (data) | OUT | To PCM5102 DIN |
| 9-15 | *free* | — | |
| 16 | Onboard NeoPixel (WS2812) | OUT | Hardwired |
| 26 | MAX9814 mic 0 (ADC0) | IN | Analog input |
| 27 | MAX9814 mic 1 (ADC1) | IN | Analog input |
| 28 | *free* (ADC2) | IN | |

---

## Wiring Reference

### I2C Bus (via I2C board)

```
Brain 1 GPIO4 (SDA) ──┐
                       ├── I2C Board SDA terminal ── 4.7kΩ ── 3.3V
Brain 2 GPIO4 (SDA) ──┘

Brain 1 GPIO5 (SCL) ──┐
                       ├── I2C Board SCL terminal ── 4.7kΩ ── 3.3V
Brain 2 GPIO5 (SCL) ──┘

Either brain GND ──── I2C Board GND terminal
Either brain 3V3 ──── I2C Board 3.3V terminal
```

### HC-SR04 Ultrasonic Sensor (×4, example for sensor 0)

```
5V (step-down) ──── HC-SR04 VCC
GND            ──── HC-SR04 GND
Brain 1 GPIO6  ──── HC-SR04 TRIG  (shared by all 4 sensors via Wago)

HC-SR04 ECHO ──── 1kΩ ──┬──── Brain 1 GPIO7
                         │
                        2kΩ
                         │
                        GND
```
The 1kΩ + 2kΩ voltage divider converts the HC-SR04's 5V echo signal to
~3.3V for the RP2040's GPIO input. Without this, the 5V would damage the chip.

### Motor Circuit (×3, example for motor 0)

```
12V PSU #1 (+) ──── Motor (+)
                    Motor (−) ──── IRLZ44N Drain (center pin)
                                    IRLZ44N Source (right pin) ──── GND
                                    IRLZ44N Gate (left pin) ──── Brain 1 GPIO11
                                             │
                                            10kΩ
                                             │
                                            GND

Flyback diode across motor:
  Cathode (stripe) → Motor (+)
  Anode → Motor (−)
```

### LED Strip Circuit (×3, example for strip 0)

```
12V PSU #1 (+) ──── LED strip (+12V)
LED strip R (−) ─┐
LED strip G (−) ──┤──── IRLZ44N Drain (center pin)
LED strip B (−) ─┘      IRLZ44N Source (right pin) ──── GND
                          IRLZ44N Gate (left pin) ──── Brain 1 GPIO0
                                   │
                                  10kΩ
                                   │
                                  GND
```
All 3 color wires joined together = white when ON. Single MOSFET controls brightness.

### PCM5102 DAC

```
Brain 2 GPIO6 ──── BCK
Brain 2 GPIO7 ──── LCK
Brain 2 GPIO8 ──── DIN
3.3V          ──── VIN
GND           ──── GND
GND           ──── SCK  (ties internal clock mode)

PCM5102 3.5mm out ──── PAM8610 #1 audio input
```

### DFPlayer Mini

```
Brain 2 GPIO0 ──── 1kΩ ──── DFPlayer RX
Brain 2 GPIO1 ──────────── DFPlayer TX
5V (step-down) ─────────── DFPlayer VCC
GND           ──────────── DFPlayer GND
DFPlayer DAC_L ──── PAM8610 #2 Left input
DFPlayer DAC_R ──── PAM8610 #2 Right input
```

### MAX9814 Microphone (×2)

```
Mic 0:
  3.3V ──── MAX9814 VDD
  GND  ──── MAX9814 GND
  3.3V ──── MAX9814 GAIN  (40dB gain setting)
  3.3V ──── MAX9814 AR    (fastest AGC release = 1:500)
  MAX9814 OUT ──── Brain 2 GPIO26 (ADC0)

Mic 1:
  3.3V ──── MAX9814 VDD
  GND  ──── MAX9814 GND
  3.3V ──── MAX9814 GAIN  (40dB gain setting)
  3.3V ──── MAX9814 AR    (fastest AGC release = 1:500)
  MAX9814 OUT ──── Brain 2 GPIO27 (ADC1)
```

### PAM8610 Amplifiers

```
PAM8610 #1 (live audio):
  12V PSU #2 ──── Power in
  GND PSU #2 ──── GND
  PCM5102 3.5mm ──── Audio input
  Speakers 1+2 ──── Output L/R

PAM8610 #2 (ambient):
  12V PSU #2 ──── Power in
  GND PSU #2 ──── GND
  DFPlayer DAC_L/R ──── Audio input L/R
  Speakers 3+4 ──── Output L/R
```

---

## Communication Protocol

### I2C (Brain 1 → Brain 2)

- **Address:** 0x42
- **Speed:** 100kHz
- **Rate:** 20Hz (every 50ms)
- **Message:** 4 bytes, packed struct:

| Byte | Field | Description |
|------|-------|-------------|
| 0 | header | Magic byte 0xC0 |
| 1 | proximity | 0=nobody, 255=very close |
| 2 | sensor_count | Valid sensors this sweep (0-4) |
| 3 | checksum | XOR of bytes 0-2 |

### DFPlayer Serial (Brain 2 → DFPlayer)

- **UART:** Serial1 at 9600 baud
- **Protocol:** 10-byte frames (0x7E start, 0xEF end, checksum)
- **Commands used:** 0x06 (set volume), 0x08 (loop track)
- **Rate limit:** Max 1 command per 200ms

---

## Audio Signal Chain

```
DRONE PATH:
  fastSine(440Hz)
    → >>DRONE_PRE_SHIFT (headroom)
    → Distortion (gain 1x-8x, hard clip)
    → Bitcrusher (0-12 bits removed)
    → Flanger (1024-sample buffer, triangle LFO)
    → Proximity Volume (DRONE_VOLUME_MIN → DRONE_VOLUME_MAX)
    → Autopan (triangle LFO, speed+depth scale with proximity)
    → stereo L/R

MIC PATH (dual mic, interleaved ADC):
  MAX9814 ×2 (40dB, ADC via DMA at 44100Hz, round-robin → 22050Hz each)
    → DC bias removal (subtract MIC_BIAS)
    → Mono downmix (average of both mics)
    → Noise Gate (asymmetric envelope, threshold-based mute)
    → MIC_VOLUME scaling
    → Reverb (4 comb filters, Schroeder design)
    → Delay (circular buffer, 600ms, feedback)
    → mono (centered in both channels)

FINAL MIX:
  droneLeft + mic → clip → I2S left
  droneRight + mic → clip → I2S right
    → PCM5102 DAC → 3.5mm → PAM8610 #1 (PSU #2) → Speakers 1+2

DFPLAYER PATH (separate analog):
  SD card → DFPlayer decoder → DAC_L/DAC_R → PAM8610 #2 (PSU #2) → Speakers 3+4
  Volume controlled by Brain 2 UART commands (proximity-scaled)
```

---

## Actuation Behavior

### LED Strips (Core 1, Brain 1)

**Idle (prox < LED_PROX_THRESHOLD):** All three strips pulse together in a
slow dim/bright cycle, making the installation look alive even when nobody
is nearby.

**Active (prox >= threshold):** All three strips strobe at the same rate,
which increases with proximity. On top of the shared strobe, each strip
independently and randomly "drops out" (goes dark) for a short random
duration. Dropout probability and duration scale with proximity: at low
proximity the strobe is uniform, at high proximity the three strips blink
in and out chaotically.

### Motors (Core 1, Brain 1)

**Idle (prox = 0):** All motors off.

**Active (prox > 0):** All three motors run at a gentle baseline PWM that
scales with proximity. On top of this baseline, individual motors randomly
fire outbursts: short bursts to higher intensity, one motor at a time (never
simultaneous). Each burst randomly uses either a sharp snap or a smooth fade
for its attack. The return to baseline always fades smoothly. Outburst
frequency, intensity, and duration all increase with proximity, creating
spatialized tactile chaos across the three motor positions.

---

## Build Commands

```bash
# Build both brains
pio run

# Upload Brain 1 only
pio run -e brain1_controller -t upload

# Upload Brain 2 only
pio run -e brain2_audio -t upload

# Monitor Brain 1 (blue heartbeat)
pio device monitor -p /dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F4BB630-if00 -b 115200

# Monitor Brain 2 (green triple-flash)
pio device monitor -p /dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F63B530-if00 -b 115200

# Monitor both simultaneously (interleaved output)
pio device monitor -p /dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F4BB630-if00 -b 115200 &
pio device monitor -p /dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F63B530-if00 -b 115200
```

---

## Tuning Parameters Quick Reference

### Brain 1 (src/brain1_controller/main.cpp)

| Parameter | Default | Range | Effect |
|-----------|---------|-------|--------|
| DETECT_MIN_CM | 1 | 1-50 | Closest detection distance |
| DETECT_MAX_CM | 75 | 50-400 | Farthest detection distance |
| MOTOR_BASELINE_MIN | 0 | 0-50 | Motor baseline PWM at prox=0 |
| MOTOR_BASELINE_MAX | 40 | 10-100 | Motor baseline PWM at max proximity |
| MOTOR_BURST_MIN | 80 | 40-150 | Minimum burst intensity |
| MOTOR_BURST_MAX | 180 | 100-255 | Maximum burst intensity |
| MOTOR_BURST_DUR_MIN | 50 | 20-200 | Shortest burst (ms) |
| MOTOR_BURST_DUR_MAX | 400 | 100-1000 | Longest burst (ms) |
| MOTOR_PAUSE_MIN | 200 | 50-500 | Shortest gap between bursts (ms) |
| MOTOR_PAUSE_MAX | 2000 | 500-5000 | Longest gap between bursts (ms) |
| MOTOR_FADE_SPEED | 8 | 1-32 | PWM change per loop iteration (higher=faster) |
| LED_IDLE_BRIGHT | 15 | 0-255 | Idle blink ON brightness |
| LED_IDLE_DIM | 5 | 0-255 | Idle blink OFF brightness |
| LED_IDLE_PERIOD_MS | 1000 | 200-5000 | Idle blink half-period (ms) |
| LED_STROBE_BRIGHT_MIN | 30 | 0-255 | Active strobe dim brightness |
| LED_STROBE_BRIGHT_MAX | 255 | 0-255 | Active strobe full brightness |
| LED_STROBE_SLOW_US | 500000 | 100000-2000000 | Slowest strobe (us) |
| LED_STROBE_FAST_US | 2000 | 500-50000 | Fastest strobe (us) |
| LED_PROX_THRESHOLD | 5 | 1-50 | Proximity to exit idle mode |
| LED_DROPOUT_DUR_MIN | 30 | 10-200 | Shortest LED dropout (ms) |
| LED_DROPOUT_DUR_MAX | 300 | 50-1000 | Longest LED dropout (ms) |
| LED_DROPOUT_CHANCE_MIN | 0 | 0-20 | Dropout probability at low prox (0-255 scale) |
| LED_DROPOUT_CHANCE_MAX | 40 | 10-100 | Dropout probability at max prox (0-255 scale) |
| SENSOR_ZERO_MAX | 10 | 3-30 | Consecutive 0s before release |

### Brain 2 (src/brain2_audio/main.cpp)

| Parameter | Default | Range | Effect |
|-----------|---------|-------|--------|
| MIC_BIAS | 1540 | 1400-1700 | MAX9814 DC offset (calibrate!) |
| MIC_VOLUME | 255 | 0-255 | Mic loudness (128=unity, 255=2x boost) |
| GATE_THRESHOLD | 200 | 50-5000 | Noise gate sensitivity |
| GATE_ATTACK | 0.5 | 0.01-1.0 | Gate open speed |
| GATE_RELEASE | 0.1 | 0.001-0.5 | Gate close speed |
| REVERB_DECAY | 215 | 150-225 | Reverb tail length |
| REVERB_WET | 200 | 0-255 | Reverb wet/dry mix |
| DELAY_TIME_MS | 600 | 100-800 | Echo time in ms |
| DELAY_FEEDBACK | 100 | 0-200 | Echo repeats (200+=danger) |
| DELAY_MIX | 140 | 0-255 | Echo volume |
| DRONE_FREQ_HZ | 440 | 50-2000 | Drone pitch in Hz |
| DRONE_PRE_SHIFT | 2 | 1-4 | Headroom (higher=quieter input to FX) |
| DRONE_VOLUME_MIN | 120 | 0-255 | Drone volume at idle |
| DRONE_VOLUME_MAX | 255 | 0-255 | Drone volume at closest |
| DIST_GAIN_MULTIPLIER | 7 | 1-15 | Distortion max gain factor |
| BITCRUSH_MAX_BITS | 12 | 1-14 | Max bit reduction |
| FLANGER_LFO_SPEED | 97391 | 10000-500000 | Flanger sweep rate |
| PAN_SPEED_MIN | 15000 | 1000-100000 | Slowest autopan |
| PAN_SPEED_MAX | 300000 | 100000-1000000 | Fastest autopan |
| PAN_DEPTH_MIN | 0 | 0-128 | Pan width when idle |
| PAN_DEPTH_MAX | 200 | 0-255 | Pan width at closest |
| DFPLAYER_VOL_MIN | 15 | 0-30 | Ambient volume idle |
| DFPLAYER_VOL_MAX | 30 | 0-30 | Ambient volume closest |
| DFPLAYER_TRACK | 1 | 1-99 | Track number to loop |
| GLITCH_CHANCE_MIN | 0 | 0-50 | Glitch probability idle |
| GLITCH_CHANCE_MAX | 0 | 0-255 | Glitch probability closest (0=off) |
| GLITCH_MIN_LENGTH | 2000 | 441-22050 | Shortest glitch (samples) |
| GLITCH_MAX_LENGTH | 10000 | 2000-44100 | Longest glitch (samples) |
| GLITCH_FADE_SPEED | 4 | 1-16 | Glitch fade smoothness |

---

## Project Structure

```
chaos-piano/
├── platformio.ini                   # Build config: two environments
├── lib/
│   └── shared_protocol/
│       ├── chaos_protocol.h         # SINGLE SOURCE OF TRUTH: pins, I2C, message format
│       └── library.json             # PlatformIO library metadata
├── src/
│   ├── brain1_controller/
│   │   └── main.cpp                 # Core 0: sensors + I2C. Core 1: chaotic LEDs + motors.
│   └── brain2_audio/
│       └── main.cpp                 # Audio engine: drone DSP + dual mic + DFPlayer
└── ARCHITECTURE.md                  # This file
```

---

## Resolved Issues

- **EMI from MOSFET PWM switching:** 12V PWM switching on the MOSFET board
  coupled ground noise into both audio amplifiers (audible as a buzz/whine
  synchronized with LED blink rate). **Fixed** by using a separate 12V PSU
  for the two PAM8610 amplifiers, completely isolating the audio ground
  from the actuation ground.

- **Floating ADC inputs:** Before the MAX9814 mics were connected, the ADC
  inputs on GPIO26/27 floated at ~2950, causing the noise gate to stay
  permanently open. **Fixed** by connecting both mics (the MAX9814 output
  provides a stable DC bias around 1540).

- **Dead PCM5102 module:** During a rebuild, the PCM5102 DAC's LCK input
  was damaged (pulling the line from 1.6V to 0.5V). **Fixed** by replacing
  the module. Lesson: handle these boards carefully and avoid shorts during
  rewiring.

## Known Limitations

- **Feedback loop:** MAX9814 AGC boosts quiet feedback from speakers. Managed
  by tuning GATE_THRESHOLD, MIC_VOLUME, REVERB_WET, and DELAY_FEEDBACK.
  Physical separation of mic and speakers remains important for the final build.

- **DFPlayer glitch sync:** DFPlayer mute commands have ~30-50ms latency
  (serial processing). Glitch cuts won't be perfectly synchronized with live
  audio. (Glitch system currently disabled: GLITCH_CHANCE_MAX = 0.)

- **Standalone boot:** Uses `delay(2000)` instead of `while(!Serial)`. Works
  for exhibit mode (no computer needed) but serial output may be missed if
  monitor isn't open within 2 seconds of boot.

- **Dual mic sample rate:** With two mics sharing one ADC via round-robin,
  each mic is sampled at 22050Hz (half the 44100Hz ADC clock). Adequate for
  voice and claps but limits high-frequency capture above ~10kHz.
