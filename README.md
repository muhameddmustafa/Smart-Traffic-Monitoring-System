# Smart Traffic Monitoring System🚦

An IoT-based Smart Traffic Monitoring System designed to monitor vehicle movement, capture vehicle images, and provide real-time traffic information through a web-based dashboard.

The system integrates ESP32 microcontrollers, ESP32-CAM modules, and IR sensors to create a connected traffic monitoring solution.



## → Project Overview

This project combines embedded systems, wireless communication, camera-based monitoring, and web technologies to develop a smart traffic monitoring platform.

The system consists of a main ESP32 controller and ESP32-CAM modules that work together to detect vehicles, capture images, and communicate monitoring data to a web-based dashboard.

## → Key Features

- Vehicle detection using IR sensors.
- Vehicle image capturing using ESP32-CAM.
- Wireless communication between system modules.
- Real-time traffic monitoring dashboard.
- Traffic light status monitoring.
- Vehicle violation counters and records.
- Captured image viewing and management.
- Camera connection status monitoring.

## → Hardware Components

| Component | Purpose |
|---|---|
| ESP32 / ESP32-S3 | Main system controller |
| ESP32-CAM | Vehicle image capturing |
| IR Sensors | Vehicle detection |
| Traffic Light LEDs | Traffic signal indication |
| Wi-Fi | Wireless connectivity |
| Power Supply | System operation |

## → System Architecture

The system is divided into two main hardware modules:

### 1. Main Controller (ESP32)

- Handles the main traffic monitoring logic.
- Processes sensor readings.
- Manages traffic light control.
- Handles communication with other modules.
- Hosts the web-based monitoring dashboard.

### 2. Camera Module (ESP32-CAM)

- Captures images of detected vehicles.
- Connects to the wireless network.
- Provides camera access through its web interface.
- Communicates camera information with the main controller.

## → Software & Technologies

- Arduino IDE
- Embedded C/C++
- ESP32 Development Framework
- Wi-Fi Communication
- HTML
- CSS
- JavaScript

## → Repository Structure

```text
Smart-Traffic-Monitoring-System/
│
├── Dashboard/
│   ├── Dashboard.html
│   └── README.md
│
├── Datasheets/
│   ├── 4_Digit_Display_Datasheet.pdf
│   ├── Esp32S3_datasheet_en.pdf
│   ├── Esp32_Camera_Datasheet.pdf
│   ├── IR_Sensor_Datasheet.pdf
│   ├── Traffic_Module_Datasheet.pdf
│   └── Readme.md
│
├── Documents/
│   ├── Bill_Of_Materials.xlsx
│   ├── Manual_Report.pdf
│   └── Readme.md
│
├── Firmware/
│   ├── ESP32/
│   │   ├── README.md
│   │   └── [ESP32 Firmware Code]
│   │
│   └── ESP32-CAM/
│       ├── README.md
│       └── Smart_Traffic_Monitoring_System_Camera_Module_Code.ino
│
├── Images/
│   ├── Breadboard_Prototyping.jpg
│   ├── Dashboard.jpg
│   ├── Final_Project.jpg
│   ├── Final_Project_With_Camera.jpg
│   ├── Hardware_Implementation.jpg
│   ├── Lane_A_Violation_Dashboard.jpg
│   ├── Lane_B_Violation_Dashboard.jpg
│   ├── QR_Detections_Camera.jpg
│   ├── Terminal_Violation_Alerts.PNG
│   ├── Violation_Records_Dashboard.jpg
│   └── README.md
│
├── Software Diagram/
│   ├── Schematic_Diagram.png
│   └── README.md
│
├── LICENSE
├── .gitignore
└── README.md
```

## → Installation & Setup

1. Install Arduino IDE.
2. Configure ESP32 board support.
3. Install the required libraries.
4. Open the corresponding firmware file.
5. Configure Wi-Fi credentials for the modules.
6. Upload the firmware to the ESP32 and ESP32-CAM modules.
7. Connect all hardware components.
8. Connect to the same Wi-Fi network.
9. Access the monitoring dashboard using the main ESP32's assigned IP address.

## → Project Documentation

- **Firmware:** Contains the source code for the main ESP32 controller and ESP32-CAM.
- **Dashboard:** Contains the HTML dashboard interface.
- **Hardware:** Contains circuit diagrams and hardware component documentation.
- **Images:** Contains project photos and visual documentation.
- 
## → Project Supervision

This project was developed under the supervision of:

- **Project Supervisor:** Dr. Khaled Fouda
- **Teaching Assistant:** George Tawadrous

## → Developers

- Muhamed Mustafa
- Muhamed Taha
- Muhamed Saeed
- Tasneem Sabry

Electrical, Electronics & Communications Engineering Students @ AAST

---

*Developed as an Embedded Systems and IoT Engineering Project.*
