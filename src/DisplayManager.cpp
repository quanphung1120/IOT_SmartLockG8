#include "DisplayManager.h"

DisplayManager::DisplayManager(uint8_t lcd_addr, uint8_t cols, uint8_t rows)
    : lcd(lcd_addr, cols, rows)
{
}

void DisplayManager::init()
{
    Wire.begin();
    lcd.init();
    lcd.clear();
    lcd.backlight();
}

void DisplayManager::clear()
{
    std::lock_guard<std::mutex> lock(displayMutex);
    lcd.clear();
}

void DisplayManager::print(uint8_t col, uint8_t row, const String &text)
{
    std::lock_guard<std::mutex> lock(displayMutex);
    lcd.setCursor(col, row);
    lcd.print(text);
}

void DisplayManager::showMessage(const String &line1, const String &line2, uint16_t displayTime)
{
    std::lock_guard<std::mutex> lock(displayMutex);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);

    if (line2.length() > 0)
    {
        lcd.setCursor(0, 1);
        lcd.print(line2);
    }

    if (displayTime > 0)
    {
        delay(displayTime);
    }
}

void DisplayManager::turnOffBacklight()
{
    std::lock_guard<std::mutex> lock(displayMutex);
    lcd.noBacklight();
}

void DisplayManager::turnOnBacklight()
{
    std::lock_guard<std::mutex> lock(displayMutex);
    lcd.backlight();
}
