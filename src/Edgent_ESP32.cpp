#define BLYNK_TEMPLATE_ID "TMPL608a18IeD"
#define BLYNK_TEMPLATE_NAME "ESP32G8 SmartLock"
#define BLYNK_FIRMWARE_VERSION "0.1.0"
#define BLYNK_PRINT Serial
#define APP_DEBUG
#define DEFAULT_PIN "123456"

#include <Arduino.h>
#include <BlynkEdgent.h>
#include <ESP32Servo.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <Wire.h>

#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// HARDWARE CONFIG
LiquidCrystal_I2C lcd(0x27, 16, 2);
std::mutex displayMutex;
Servo lockServo;
Preferences prefs;

// DISPLAY CONSTANTS
const unsigned long BACKLIGHT_TIMEOUT = 30000;  // 30 seconds timeout
unsigned long lastDisplayActivity = 0;
bool backlightEnabled = true;

// PIN CONFIG
const int SERVO_PIN = 13;
const uint8_t rowPins[4] = {14, 27, 26, 25};
const uint8_t colPins[4] = {33, 32, 18, 19};
char keyMap[4][4] = {{'1', '2', '3', 'A'},
                     {'4', '5', '6', 'B'},
                     {'7', '8', '9', 'C'},
                     {'*', '0', '#', 'D'}};

// CONSTANTS
const int LOCK_POSITION = 0;
const int UNLOCK_POSITION = 90;
const unsigned long UNLOCK_DURATION = 5000;
const uint8_t PASSCODE_LENGTH = 6;
const unsigned long KEYPAD_TIMEOUT = 30000;

// STATE VARIABLES
Keypad keypad =
    Keypad(makeKeymap(keyMap), (byte *)rowPins, (byte *)colPins, 4, 4);
char currentPasscode[PASSCODE_LENGTH + 1] = {0};
int passcodeIndex = 0;
unsigned long lastKeyPressTime = 0;
String storedPin;
bool isLocked = true;
TaskHandle_t keypadTaskHandle = NULL;

// DISPLAY FUNCTIONS
void displayUpdate(uint8_t col, uint8_t row, const String &text,
                   bool clearFirst = false) {
    std::lock_guard<std::mutex> lock(displayMutex);
    if (clearFirst)
        lcd.clear();
    lcd.setCursor(col, row);
    lcd.print(text);

    // Update activity timestamp and ensure backlight is on
    lastDisplayActivity = millis();
    if (!backlightEnabled) {
        lcd.backlight();
        backlightEnabled = true;
    }
}

void displayMessage(const String &line1, const String &line2 = "",
                    uint16_t displayTime = 0) {
    std::lock_guard<std::mutex> lock(displayMutex);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);

    if (line2.length() > 0) {
        lcd.setCursor(0, 1);
        lcd.print(line2);
    }

    // Update activity timestamp and ensure backlight is on
    lastDisplayActivity = millis();
    if (!backlightEnabled) {
        lcd.backlight();
        backlightEnabled = true;
    }

    if (displayTime > 0)
        delay(displayTime);
}

void setBacklight(bool on) {
    std::lock_guard<std::mutex> lock(displayMutex);
    on ? lcd.backlight() : lcd.noBacklight();
    backlightEnabled = on;
}

// SERVO FUNCTIONS
void setLockPosition(bool lock) {
    lockServo.write(lock ? LOCK_POSITION : UNLOCK_POSITION);
    isLocked = lock;
    Serial.println(lock ? "Door locked" : "Door unlocked");
}

void unlockTemporarily() {
    setLockPosition(false);  // unlock
    delay(UNLOCK_DURATION);
    setLockPosition(true);  // lock
}

// PIN MANAGEMENT
bool isValidPin(const String &pin) {
    if (pin.length() != 6)
        return false;
    for (char c : pin)
        if (!isdigit(c))
            return false;
    return true;
}

String loadPin() {
    prefs.begin("smartlock", true);
    String pin = prefs.getString("pin", DEFAULT_PIN);
    prefs.end();

    return isValidPin(pin) ? pin : DEFAULT_PIN;
}

bool savePin(const String &newPin) {
    if (!isValidPin(newPin))
        return false;

    prefs.begin("smartlock", false);
    prefs.putString("pin", newPin);
    prefs.end();

    storedPin = newPin;
    return true;
}

// KEYPAD FUNCTIONS
void resetPasscodeEntry() {
    passcodeIndex = 0;
    memset(currentPasscode, 0, sizeof(currentPasscode));
    displayUpdate(0, 0, "Enter Passcode:", true);
    displayUpdate(5, 1, "______");
}

void processKeypadInput() {
    // Check for timeout
    if (passcodeIndex > 0 && millis() - lastKeyPressTime >= KEYPAD_TIMEOUT) {
        displayMessage("Timeout", "Input cleared", 1500);
        resetPasscodeEntry();
        setBacklight(false);
        return;
    }

    char key = keypad.getKey();
    if (!key)
        return;

    setBacklight(true);
    lastKeyPressTime = millis();

    // Ignore input when door is unlocked
    if (!isLocked)
        return;

    // Handle digit keys (0-9)
    if (isdigit(key) && passcodeIndex < PASSCODE_LENGTH) {
        currentPasscode[passcodeIndex] = key;
        displayUpdate(5 + passcodeIndex, 1, String(key));
        passcodeIndex++;

        if (passcodeIndex == PASSCODE_LENGTH) {
            displayUpdate(0, 0, "Press # to verify", true);
        }
        return;
    }

    // Handle backspace (*)
    if (key == '*' && passcodeIndex > 0) {
        passcodeIndex--;
        currentPasscode[passcodeIndex] = 0;
        displayUpdate(5 + passcodeIndex, 1, "_");
        return;
    }

    // Handle enter key (#)
    if (key == '#') {
        if (String(currentPasscode) == storedPin) {
            // Success - unlock door
            setLockPosition(false);
            displayMessage("Access Granted!", "Door Unlocked", 2000);

            // Auto-lock countdown
            displayUpdate(0, 0, "Auto-locking in:", true);
            for (int i = 5; i >= 0; i--) {
                displayUpdate(0, 1, String(i) + " seconds");
                delay(1000);
            }

            setLockPosition(true);
            displayMessage("Door Locked", "", 1000);
        } else {
            // Failed attempt
            Blynk.logEvent("access_denied");
            displayMessage("Access Denied!", "", 2000);
        }
        resetPasscodeEntry();
    }
}

// KEYPAD TASK
void keypadTask(void *parameter) {
    for (;;) {
        // Check if backlight should be turned off
        if (backlightEnabled &&
            (millis() - lastDisplayActivity >= BACKLIGHT_TIMEOUT)) {
            setBacklight(false);
        }

        processKeypadInput();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// BLYNK HANDLERS
BLYNK_WRITE(V0) {
    if (param.asInt()) {
        displayMessage("Door Unlocked", "Blynk Command");
        unlockTemporarily();
        resetPasscodeEntry();
    }
}

BLYNK_WRITE(V1) {
    String newPin = param.asStr();
    if (savePin(newPin)) {
        displayMessage("PIN Changed", "Successfully", 4000);
    } else {
        displayMessage("PIN Change", "Failed", 3000);
    }
    resetPasscodeEntry();
}

// SETUP AND LOOP
void setup() {
    Serial.begin(115200);

    // Initialize hardware
    Wire.begin();
    lcd.init();
    lcd.clear();
    lcd.backlight();

    ESP32PWM::allocateTimer(0);
    lockServo.attach(SERVO_PIN);
    setLockPosition(true);

    // Load PIN from preferences
    storedPin = loadPin();
    Serial.println("Current PIN: " + storedPin);

    // Initialize system
    displayMessage("Initializing...", "Please wait");
    BlynkEdgent.begin();

    // Prepare keypad
    resetPasscodeEntry();
    lastKeyPressTime = millis();

    // Start keypad task
    xTaskCreatePinnedToCore(keypadTask, "KeypadTask", 4096, NULL, 1,
                            &keypadTaskHandle, 1);
}

void loop() {
    BlynkEdgent.run();
    delay(10);  // Small delay to prevent CPU hogging
}