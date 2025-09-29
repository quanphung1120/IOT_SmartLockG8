# IOT_SmartLockG8

A smart lock project leveraging IoT (Internet of Things) principles. This repository demonstrates how embedded systems and connected devices can be used to build a secure, remote-controllable locking mechanism which was submitted as an final assignment for my IoT102 subject at FPT University.

## Features

- **Remote Lock/Unlock:** Control the lock from a connected device or remotely via network protocols.
- **Availability while offline:** System ensures smooth users' experience even if the microcontroller is disconnected from WiFi by leveraging multi-core architecture of ESP-32
- **Embedded System:** Runs on microcontroller hardware with efficient C++ code.
- **Security:** Implements basic security features for safe operation.

## Technologies & Hardware Used

This project combines several modern technologies and concepts:

- **C++ Programming Language:**  
  The core logic is written in C++, ideal for embedded systems due to its efficiency, low-level hardware access, and real-time performance.
- **ESP32 (Multi-core Architecture):**  
  The ESP32 microcontroller with 2 dedicated cores provides smooth experience to users.
- **Blynk Edgent:**  
  It enables cloud connectivity and remote device management, allowing users to control the lock from anywhere. In addition, the Edgent package provides Wi-Fi provisioning functionality, making it possible to set up the demonstration project just like a real product.
- **AS608 Fingerprint Sensor:**  
  Provides secure biometric authentication for unlocking.
- **HC-SR501 PIR Motion Sensor:**  
  Detects motion to improve user experience.
- **Keypad 4x4:**  
  Offers an alternative method for user input and authentication.
- **IoT Networking:**  
  System can communicate with Blynk's servers and end-users over Wi-Fi.
- **Sensor & Actuator Integration:**  
  The hardware works with sensors and actuators to physically manage the locking mechanism.
