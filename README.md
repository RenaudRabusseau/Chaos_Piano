# Chaos Piano

An interactive sound and light installation that transforms visitor proximity into coordinated chaos. Built around a decommissioned upright piano for the Fondation du Doute (Blois, France).

As a visitor approaches, the system responds with increasing intensity: vibrating motors excite the piano frame, LED strips strobe with random independent dropouts, a synthesized drone grows louder and more distorted, live microphones capture ambient sound and feed it through reverb and delay, and an ambient track swells through a separate speaker pair.

---

## How It Works

The system is split across two RP2040-Zero microcontrollers connected via I2C. Brain 1 handles the physical world (sensing and actuation), Brain 2 handles audio (synthesis, live processing, and playback). Two separate 12V power supplies isolate the noisy actuation domain from the audio amplifiers.

Four ultrasonic sensors detect visitor proximity and map the closest distance to a single byte (0 = nobody, 255 = very close). This byte drives everything: drone volume and distortion, LED strobe speed, motor outburst frequency, mic processing intensity, and ambient track volume.

### Signal Flow

```
Visitor proximity
    │
    ├──► Brain 1 (RP2040-Zero, dual-core)
    │       Core 0: 4x HC-SR04 sensors → proximity byte → I2C to Brain 2
    │       Core 1: 3x LED strobes with random dropouts
    │                3x motor outbursts (one at a time, spatialized)
    │
    └──► Brain 2 (RP2040-Zero, single-core)
            Drone: sine → distortion → bitcrush → flanger → autopan → I2S DAC
            Mics:  2x MAX9814 → noise gate → reverb → delay → I2S DAC
            Ambient: DFPlayer Mini → separate amp → speakers 3+4
```

### Schematic

[View KiCad schematic (PDF)](KICAD/chaos_piano/chaos_piano.pdf)

---

## Hardware

| Component | Qty | Role |
|-----------|-----|------|
| RP2040-Zero (Waveshare) | 2 | Brain 1 (controller), Brain 2 (audio) |
| HC-SR04 ultrasonic sensor | 4 | Proximity detection |
| MAX9814 preamp module | 2 | Condenser mic input (40dB, fast AGC) |
| PCM5102 I2S DAC | 1 | Digital audio output |
| PAM8610 amplifier | 2 | Speaker drivers (separate PSU from actuation) |
| Speakers (4 ohm 10W) | 4 | 2 for live audio, 2 for ambient track |
| DFPlayer Mini | 1 | Ambient MP3 playback |
| IRLZ44N N-MOSFET | 6 | 12V switching (3 motors, 3 LED strips) |
| Vibrating motor | 3 | Tactile/acoustic excitation |
| 12V RGB LED strip | 3 | Visual feedback (white strobe) |
| 12V PSU | 2 | #1 brains + actuation, #2 audio amplifiers |
| 5V step-down converter | 1 | Powers sensors and DFPlayer from PSU #1 |

### Custom Boards

Three perfboards built by hand with screw terminals for field serviceability:

**MOSFET board:** 6x IRLZ44N with 10k pull-downs and flyback diodes, 12V/GND rails.
**Voltage divider board:** 4x dividers (1k/2k) converting 5V HC-SR04 echo signals to 3.3V.
**I2C board:** 2x 4.7k pull-ups, 4 screw terminals (SDA, SCL, 3.3V, GND).

### Power Architecture

Two separate 12V supplies eliminate ground-coupled PWM noise from the audio path:

- **PSU #1:** MOSFET board, 5V step-down (sensors, DFPlayer), brains (via USB for desk prototype)
- **PSU #2:** Both PAM8610 amplifiers only

---

## Audio DSP Chain

Brain 2 runs a real-time audio engine at 44100Hz, synchronized to DMA-based ADC sampling.

**Drone path (synthesized):** Phase-accumulator sine oscillator (440Hz) through proximity-scaled distortion (up to 8x gain, hard clip), bitcrusher (up to 12 bits removed), flanger (1024-sample buffer, triangle LFO), proximity volume, and stereo autopanner.

**Mic path (live input):** Two MAX9814 condenser mics sampled via round-robin ADC at 22050Hz each, mono downmixed, through asymmetric noise gate, Schroeder reverb (4 comb filters), and 600ms delay with feedback.

**Ambient path (separate analog):** DFPlayer Mini decodes MP3 from SD card, analog output to PAM8610 #2 through its own DAC. Volume controlled by Brain 2 via 9600 baud UART.

---

## Actuation Behavior

**LEDs:** Unified strobe rate scales with proximity. Each strip independently drops out (goes dark) for random durations. Dropout probability increases with proximity, creating visual chaos.

**Motors:** Gentle baseline hum scales with proximity. Individual motors fire random outbursts (one at a time, never simultaneous) with randomly chosen sharp or faded attacks. Outburst frequency, intensity, and duration increase with proximity, spatializing tactile chaos across three positions.

---

## Build and Upload

Requires [PlatformIO](https://platformio.org/).

```bash
# Build both brains
pio run

# Upload Brain 1 (blue heartbeat LED)
pio run -e brain1_controller -t upload

# Upload Brain 2 (green triple-flash LED)
pio run -e brain2_audio -t upload

# Monitor Brain 1
pio device monitor -p /dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F4BB630-if00 -b 115200

# Monitor Brain 2
pio device monitor -p /dev/serial/by-id/usb-Waveshare_RP2040_Zero_E66428C51F63B530-if00 -b 115200
```

---

## Repository Structure

```
Chaos_Piano/
├── README.md                        # This file
├── ARCHITECTURE.md                  # Complete technical reference
├── platformio.ini                   # Build config: two environments
├── KICAD/
│   └── chaos_piano/
│       ├── chaos_piano.kicad_sch    # Schematic
│       ├── chaos_piano.kicad_pcb    # PCB layout
│       └── chaos_piano.pdf          # Schematic export
├── lib/
│   └── shared_protocol/
│       ├── chaos_protocol.h         # Pin assignments, I2C protocol, message format
│       └── library.json             # PlatformIO library metadata
└── src/
    ├── brain1_controller/
    │   └── main.cpp                 # Core 0: sensors + I2C. Core 1: chaotic LEDs + motors.
    └── brain2_audio/
        └── main.cpp                 # Audio engine: drone DSP + dual mic + DFPlayer
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for the complete technical reference including pin maps, wiring diagrams, communication protocol, and all tuning parameters.

---

## Academic Context

Developed as a portfolio project for a BTS CIEL (Cybersecurity, IT and Electronics) program. Demonstrates practical competency across embedded systems, real-time signal processing, and human-machine interaction:

- Dual-microcontroller architecture with I2C inter-processor communication
- Real-time DSP on a 133MHz ARM Cortex-M0+ (no FPU, no RTOS)
- DMA-driven ADC for jitter-free audio sampling
- Power domain isolation for EMI management
- Custom perfboard design and hand assembly
- Proximity-driven state machine with randomized actuation behavior

---

## Author

**Renaud Rabusseau** - Electronics engineering student, former technical translator and conference interpreter. This project bridges 15 years of professional audio experience with embedded systems engineering.

---

## License

This project is open source. See individual files for details.
