#ifndef KEYPAD_HANDLER_H
#define KEYPAD_HANDLER_H

#include <Arduino.h>
#include <Keypad.h>
#include "ServoLock.h"
#include "DisplayManager.h"

class KeypadHandler
{
private:
    static const uint8_t ROWS = 4;
    static const uint8_t COLS = 4;
    static const uint8_t PASSCODE_LENGTH = 6;
    static const unsigned long KEYPAD_TIMEOUT = 30000; // 30 seconds in milliseconds

    DisplayManager &display;
    Keypad keypad;
    ServoLock &lockServo;
    String storedPin;

    char currentPasscode[PASSCODE_LENGTH + 1]; // +1 for null terminator
    unsigned long lastKeyPressTime;
    int passcodeIndex;

    char keyMap[ROWS][COLS] = {
        {'1', '2', '3', 'A'},
        {'4', '5', '6', 'B'},
        {'7', '8', '9', 'C'},
        {'*', '0', '#', 'D'}};

    uint8_t rowPins[ROWS];
    uint8_t colPins[COLS];

public:
    KeypadHandler(DisplayManager &display_mgr,
                  ServoLock &servo,
                  const uint8_t row_pins[ROWS],
                  const uint8_t col_pins[COLS]);

    void init(String pin);
    void processKeyInput();
    void clearPasscodeDisplay();
};

#endif // KEYPAD_HANDLER_H