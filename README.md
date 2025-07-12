# Multithreaded Alarm System in C

This repository contains two implementations of a multithreaded alarm system using POSIX threads in the C programming language. These systems were developed as coursework for EECS3221 (Operating Systems), with one version utilizing **mutex locks** for thread synchronization and another using **semaphores**.

---

## Features

Both implementations support the following functionality:

- Adding, changing, canceling, and viewing alarms.
- Alarms include fields such as `Alarm_ID`, `Type`, `Time`, and `Message`.
- Thread-safe access to the alarm list using synchronization primitives.
- Three distinct thread types:
  - Main thread: handles user input and command parsing.
  - Alarm thread: manages thread assignments and alarm state.
  - Display threads: periodically print assigned alarms until they expire, are canceled, or changed.

---

## Directory Structure

```bash
Multithreaded-Alarm-System/
│
├── Mutex-Based/                     # Assignment 2 implementation (uses mutexes)
│   ├── new_alarm_mutex.c
│   ├── errors.h
│   ├── Makefile
│   ├── Test_output.txt
│   ├── Report.pdf
│   └── README.md                    # Detailed notes and build instructions
│
├── Semaphore-Based/                 # Assignment 3 implementation (uses semaphores)
│   ├── new_alarm_cond.c
│   ├── errors.h
│   ├── Makefile
│   ├── Test_output.txt
│   ├── Report.pdf
│   └── README.md                    # Detailed notes and build instructions
│
├── 3221E_F24_Mutex-Based.pdf        # Assignment 2 PDF specification
└── 3221E_F24_Semaphore-Based.pdf    # Assignment 3 PDF specification
