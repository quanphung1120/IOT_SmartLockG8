#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <mutex>

class DisplayManager
{
private:
    LiquidCrystal_I2C lcd;
    std::mutex displayMutex;

public:
    // Constructor
    DisplayManager(uint8_t lcd_addr = 0x27, uint8_t cols = 16, uint8_t rows = 2);

    // Initialization
    void init();

    // Thread-safe operations
    void clear();
    void print(uint8_t col, uint8_t row, const String &text);
    void turnOffBacklight();
    void turnOnBacklight();
    void showMessage(const String &line1, const String &line2 = "", uint16_t displayTime = 0);
};

#endif // DISPLAY_MANAGER_H