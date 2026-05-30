# Makefile
# Builds the update server and client for the distributed software update framework.
# Usage: make all, make server, make client, make clean

CC      = gcc
CFLAGS  = -Wall -g -pthread
GLFLAGS = -lGL -lGLU -lglut -lm
SRC     = src
BUILD   = build

all: $(BUILD)/server $(BUILD)/client

$(BUILD)/server: $(SRC)/server.c $(SRC)/common.c $(SRC)/gui.c
	$(CC) $(CFLAGS) $(SRC)/server.c $(SRC)/common.c $(SRC)/gui.c -o $(BUILD)/server $(GLFLAGS)

$(BUILD)/client: $(SRC)/client.c $(SRC)/common.c
	$(CC) $(CFLAGS) $(SRC)/client.c $(SRC)/common.c -o $(BUILD)/client

clean:
	rm -f $(BUILD)/server $(BUILD)/client

.PHONY: all clean