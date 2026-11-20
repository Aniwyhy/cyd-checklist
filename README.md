# CYD Checklist

Touch checklist firmware for the 4" "Cheap Yellow Display" (ESP32-3248S040: ESP32, ST7796 320×480, XPT2046 resistive touch, CH340 USB), in portrait.

- **Tap** a task to tick it off. **Hold** it to edit or delete. **Drag** to scroll.
- **+ Add** opens an on-screen keyboard.
- **Clear N done** at the end of the list removes ticked tasks.
- Tasks are saved to flash (LittleFS) and survive power cycles.
- The screen dims after 2 minutes idle; the first touch only wakes it.

## Keyboard

- **EN / あ** switches between English and Japanese (remembered).
- English: auto-capitalises the first letter; double-tap shift for caps lock.
- Japanese: type romaji and it becomes kana as you go (`konnichiha` → こんにちは, `kitte` → きって, `ra-men` → らーめん). The shift key becomes **かな/カナ** to switch hiragana/katakana. While romaji is pending, space shows **確定** and commits it.
- **123** for numbers and symbols. Hold ⌫ to delete repeatedly.
- No kanji conversion (the dictionary would not fit on the board); the font can display kanji.

## Build and flash

```bash
pio run -t upload --upload-port /dev/cu.usbserial-110
```

Upload speed is 460800 because the CH340 is unreliable at 921600.

## Touch calibration

On first boot the screen asks you to tap four corner arrows; the result is saved in NVS. Hold **BOOT** for 2 seconds at any time to recalibrate.
