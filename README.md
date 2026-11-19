# Smart Traffic Monitoring System🚦

An IoT-based Smart Traffic Monitoring System designed to monitor vehicle movement, capture vehicle images, and provide real-time traffic information through a web-based dashboard.

The system uses ESP32 microcontrollers, ESP32-CAM, and IR sensors to create an integrated traffic monitoring solution.

## →  Project Overview

This project combines embedded systems, wireless communication, camera-based monitoring, and web technologies to develop a smart traffic monitoring platform.

The system consists of a main ESP32 controller and ESP32-CAM modules that work together to detect vehicles, capture images, and communicate monitoring data to the dashboard.

## →  Key Features

- Vehicle detection using IR sensors.
- Vehicle image capturing using ESP32-CAM.
- Wireless communication between system modules.
- Real-time traffic monitoring dashboard.
- Web-based monitoring interface.
- Centralized traffic monitoring and data visualization.

## →  Hardware Components

| Component | Purpose |
|---|---|
| ESP32 / ESP32-S3 | Main system controller |
| ESP32-CAM | Vehicle image capturing |
| IR Sensors | Vehicle detection |
| Wi-Fi | Wireless connectivity |

## →  System Architecture

The system is divided into two main hardware modules:

### 1. Main Controller (ESP32)

- Handles the main traffic monitoring logic.
- Processes sensor readings.
- Manages communication with other modules.
- Provides data to the monitoring dashboard.

### 2. Camera Module (ESP32-CAM)

- Captures images of detected vehicles.
- Connects to the wireless network.
- Provides camera access through its web interface.

## →  Software & Technologies

- Arduino IDE
- Embedded C/C++
- ESP32 Development Framework
- Wi-Fi Communication
- HTML / CSS / JavaScript (Dashboard, if applicable)

## → Repository Structure

```text
Smart-Traffic-Monitoring-System/
│
├── Firmware/
│   ├── ESP32/
│   │   └── Main_Controller.ino
│   │
│   └── ESP32-CAM/
│       └── Camera_Module.ino
│
├── Dashboard/
│
├── Hardware/
│   ├── Circuit_Diagrams/
│   └── Components/
│
├── Images/
│
├── Videos/
│
├── README.md
├── LICENSE
└── .gitignore
```

## → Installation & Setup

1. Install Arduino IDE.
2. Configure ESP32 board support.
3. Install the required libraries.
4. Open the corresponding firmware file.
5. Configure Wi-Fi credentials.
6. Upload the firmware to the ESP32 and ESP32-CAM modules.
7. Connect all hardware components.
8. Access the monitoring dashboard through the assigned IP address.

## Developers
Muhamed Mustafa, Muhamed Taha, Muhamed Saeed, Tasneem Sabry

Electrical, Electronics & Communications Engineering Students @AAST 
