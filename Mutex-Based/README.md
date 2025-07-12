# Multithreaded Alarm System (Mutex-Based)

This project implements a multithreaded alarm management system using **POSIX threads** and **mutex locks** to synchronize access to shared data structures. Developed as part of **EECS3221: Operating Systems**, the system models real-time alarm scheduling and management using thread-safe techniques.

---

## Features

- **Create Alarms**: User can create alarms with custom messages, trigger time, and alarm type.
- **Change Alarms**: Existing alarms can be modified on the fly.
- **Cancel Alarms**: Alarms can be canceled before triggering.
- **View Alarms**: View all active and expired alarms.
- **Thread Architecture**:
  - **Main Thread**: Parses user input and handles command logic.
  - **Alarm Thread**: Coordinates alarm assignments to display threads.
  - **Display Threads**: Independently show countdowns for assigned alarms.
- **Thread Safety**: All shared data access is synchronized using **mutex locks**.

---

## Authors
    •    Rami Omer
    •    Mohammad Zarif
    •    Shaylin Ziaei
    •    Rayhaan Yaser Mohammed
