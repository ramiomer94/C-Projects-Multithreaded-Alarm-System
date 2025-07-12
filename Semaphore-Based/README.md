# Semaphore-Based Multithreaded Alarm System in C

This project contains a multithreaded alarm system implemented in C using POSIX threads and **semaphores** for synchronization. It was developed as Assignment 3 for the EECS3221 (Operating Systems) course at York University.

## Features

- Add, change, cancel, suspend, reactivate, and view alarms
- Synchronization with POSIX **semaphores**
- Thread-safe shared alarm list
- Three types of threads:
  - **Main Thread** – handles user input and command parsing
  - **Alarm Thread** – dispatches alarms to their appropriate display threads
  - **Display Threads** – one per group, displays active alarms in real-time
  
