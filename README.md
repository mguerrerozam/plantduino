# 🌱 PlantDuino – Automated Irrigation with ESP8266 and Telegram

**PlantDuino** is an automated plant irrigation system powered by an **ESP8266** microcontroller, enabling real-time monitoring and remote control of watering through **Telegram**. This project was developed by first-year Computer Engineering students at the **University of Concepción, Chile**.

---

## 🚀 Project Overview

The system continuously monitors soil moisture using an **HW-080 sensor** placed at root level. If moisture falls below a defined threshold, the system activates a **water pump** to restore adequate hydration. It also sends notifications via Telegram and allows users to control irrigation remotely.

Two operational modes are included:

### 🧭 Standard Mode
- Manual or remote irrigation via Telegram commands.
- Users can specify the volume of water to be dispensed.
- The system calculates pump run time based on prior flow calibration to deliver the precise amount.

### ✈️ Travel Mode
- Fully automated irrigation.
- Users define target **minimum and maximum soil moisture values**.
- When moisture drops below the minimum, the system irrigates until the target value is reached — without requiring user input.

In both modes, the system allows remote monitoring of:
- Real-time soil moisture percentage.
- Last irrigation details.
- Current system status.

---

## 🛠️ Hardware Components

| Component              | Description                                                |
|------------------------|------------------------------------------------------------|
| ESP8266                | Wi-Fi-enabled microcontroller, core of the system          |
| Arduino UNO            | Used for power supply (can be optional)                    |
| HW-080 Moisture Sensor | Soil moisture sensor with electrodes and signal converter  |
| LCD1602A I2C Display   | Shows real-time system status and sensor readings          |
| Water Pump             | Activated via relay (flow rate must be manually calibrated)|

---

## 💻 Technologies Used

- Developed using **Arduino IDE**
- Remote communication via **Telegram Bot**
- Code written with support from Claude (AI assistant)

---

## 🔧 Setup Instructions

1. **Install ESP8266 board drivers** in Arduino IDE.
2. Create a Telegram bot via [@BotFather](https://t.me/BotFather) and obtain your **bot token**.
3. In the source code:
   - Enter your **Wi-Fi SSID** and **password**.
   - Paste your **Telegram bot token**.
   - Add **your personal Telegram user ID** (not the bot’s ID).

---

## ✅ Requirements

- Arduino IDE
- ESP8266 board drivers
- Telegram account
- Created bot via [@BotFather](https://t.me/BotFather)

---

## ⚠️ Notes

- **No software license** is currently attached to this repository.
- This project was created for academic purposes.
- Water pump flow rate must be manually calibrated for accurate irrigation.

---

## 📩 Contact

Developed by first-year Computer Engineering students  
**Universidad de Concepción – Chile**

---
