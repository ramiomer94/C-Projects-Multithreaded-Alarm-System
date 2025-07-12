# Makefile for Semaphore-Based Alarm System

CC = gcc
CFLAGS = -Wall -pthread
TARGET = alarm_system
SOURCES = new_alarm_semaphore.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)