#include "KeypadHandler.h"

KeypadHandler::KeypadHandler(DisplayManager &display_mgr,
                             ServoLock &servo,
                             const uint8_t row_pins[ROWS],
                             const uint8_t col_pins[COLS])
    : display(display_mgr),
      lockServo(servo),
      keypad(makeKeymap(keyMap), (byte *)row_pins, (byte *)col_pins, ROWS, COLS),
      passcodeIndex(0),
      lastKeyPressTime(millis()) // Initialize the last key press time
{
    // Copy pin arrays
    for (int i = 0; i < ROWS; i++)
    {
        this->rowPins[i] = row_pins[i];
    }

    for (int i = 0; i < COLS; i++)
    {
        this->colPins[i] = col_pins[i];
    }

    memset(currentPasscode, 0, sizeof(currentPasscode));
}

void KeypadHandler::init(String pin)
{
    storedPin = pin;
    Serial.println("Stored PIN: " + storedPin);
    display.showMessage("Smart Lock", "System Ready", 1000);
    clearPasscodeDisplay();
}

void KeypadHandler::clearPasscodeDisplay()
{
    passcodeIndex = 0;
    memset(currentPasscode, 0, sizeof(currentPasscode));
    display.clear();
    display.print(0, 0, "Enter Passcode:");
    display.print(5, 1, "______");
}

void KeypadHandler::processKeyInput()
{
    unsigned long currentTime = millis();
    // Check if more than 30 seconds have passed since last key press
    // if (currentTime - lastKeyPressTime >= KEYPAD_TIMEOUT)
    // {
    //     // Timeout occurred, reset the input
    //     clearPasscodeDisplay();
    //     display.turnOffBacklight();
    //     return;
    // }

    char pressedKey = keypad.getKey();
    if (pressedKey)
    {
        display.turnOnBacklight();
        lastKeyPressTime = millis(); // Update the last key press time

        // Check if the lock is engaged before accepting input
        if (!lockServo.getStatus())
        {
            return; // Ignore input if lock is not engaged
        }
        if (isdigit(pressedKey))
        {
            if (passcodeIndex < PASSCODE_LENGTH)
            {
                currentPasscode[passcodeIndex] = pressedKey;
                display.print(5 + passcodeIndex, 1, String(pressedKey)); // Display the actual digit/character
                passcodeIndex++;
            }
        }
        // Handle delete key (*)
        else if (pressedKey == '*' && passcodeIndex > 0)
        {
            passcodeIndex--;
            currentPasscode[passcodeIndex] = 0;
            display.print(5 + passcodeIndex, 1, "_");
        }
        else if (pressedKey == '#')
        {
            // Verify the entered passcode against stored pin
            if (String(currentPasscode) == storedPin)
            {
                display.showMessage("Access Granted!", "Door Unlocked", 2000);
                lockServo.unlock();

                display.print(0, 0, "Auto-locking in:");

                for (int i = 0; i <= 5; i++)
                {
                    delay(1000);
                    display.print(0, 1, String(4 - i) + " seconds");
                }

                lockServo.lock();
                display.showMessage("Door Locked", "", 1000);
            }
            else
            {
                display.showMessage("Access Denied!", "", 2000);
            }

            clearPasscodeDisplay();
        }

        // If passcode is complete (6 digits entered)
        if (passcodeIndex == PASSCODE_LENGTH)
        {
            display.print(0, 0, "Press # to verify");
        }
    }
}