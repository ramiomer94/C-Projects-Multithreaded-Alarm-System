# Makefile for Mutex-Based Alarm System

CC = gcc
CFLAGS = -Wall -pthread
TARGET = alarm_system
SOURCES = new_alarm_mutex.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)