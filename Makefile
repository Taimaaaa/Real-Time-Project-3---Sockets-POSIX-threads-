# Makefile
# Builds the update server and client for the distributed software update framework.
# Usage: make all, make server, make client, make clean

CC      = gcc
CFLAGS  = -Wall -g -pthread
SRC     = src
BUILD   = build

all: $(BUILD)/server $(BUILD)/client

$(BUILD)/server: $(SRC)/server.c $(SRC)/common.c
	$(CC) $(CFLAGS) $(SRC)/server.c $(SRC)/common.c -o $(BUILD)/server

$(BUILD)/client: $(SRC)/client.c $(SRC)/common.c
	$(CC) $(CFLAGS) $(SRC)/client.c $(SRC)/common.c -o $(BUILD)/client

clean:
	rm -f $(BUILD)/server $(BUILD)/client

.PHONY: all clean