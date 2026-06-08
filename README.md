# 🎵 Rhythm Tiles Game

A microcontroller‑based rhythm game built for the NUC1xx platform. Players hit falling tiles in sync with the beat, earning points for timing accuracy while managing limited lives. Features multiple difficulty levels, EEPROM‑based leaderboard, and visual feedback via LCD, LEDs, and 7‑segment display.

## ✨ Features
- Three difficulty modes: Easy, Medium, Hard (adjustable via ADC input).
- Random tile spawning across three lanes.
- Scoring system with perfect/good timing zones.
- Persistent leaderboard stored in EEPROM (24LC64 via I2C).
- LCD menus and HUD, LED indicators, and 7‑segment score display.
- Safe EEPROM read/write functions with timeout handling.

## 🎮 Controls
- **Keypad Inputs**  
  - `1`: Up in menus / Hit left lane  
  - `2`: Select option / Hit middle lane  
  - `3`: Down in menus / Hit right lane  

## 🛠 Technical Details
- Written in **C** for NUC1xx microcontrollers.
- Uses **I2C** for EEPROM communication.
- Implements **ADC** for difficulty selection.
- Timer interrupts for score display updates.
- Custom graphics rendering with 2D drawing functions.

## 📦 Hardware Requirements
- NUC1xx microcontroller board  
- LCD display  
- 7‑segment display  
- LEDs (GPC12–GPC15)  
- EEPROM (24LC64 via I2C)  
- Keypad input  
