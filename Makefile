# =====================================================================
# Makefile -- Distributed Software Update Framework
# ---------------------------------------------------------------------
# BUILD TARGETS
#   make              build server (with OpenGL GUI) + client
#   make server       build only the GUI server
#   make client       build only the client
#   make headless     build server WITHOUT OpenGL (for ssh / no display)
#   make all-targets  build everything
#
# RUN TARGETS
#   make run-server          start the GUI server (foreground)
#   make run-server-headless start the headless server (foreground)
#   make run-client          run one client (uses config/version.txt)
#   make run-outdated        run one client pretending to be v1
#   make run-uptodate        run one client pretending to be v5
#   make run-many            fire 5 concurrent clients (mixed versions)
#   make run-stress          fire 10 concurrent clients
#
# UTILITY TARGETS
#   make test         run the full automated test suite (9 scenarios)
#   make package      generate a 20 MB random update package
#   make small-pkg    swap in a small text update package (fast demo)
#   make reset        reset state: clear downloads, logs, version=1
#   make stop         kill any running server
#   make clean        remove built binaries
#   make help         show this help
# =====================================================================

CC       = gcc
CFLAGS   = -Wall -Wextra -g -pthread -O2
GLFLAGS  = -lGL -lGLU -lglut -lm
SRC      = src
BUILD    = build
CFG      = config/config.txt

COMMON   = $(SRC)/common.c
SVR_SRC  = $(SRC)/server.c $(COMMON) $(SRC)/gui.c
CLI_SRC  = $(SRC)/client.c $(COMMON)

# ---------- default ----------
all: $(BUILD)/server $(BUILD)/client

all-targets: $(BUILD)/server $(BUILD)/client $(BUILD)/server_headless

# ---------- build targets ----------
server: $(BUILD)/server

$(BUILD)/server: $(SVR_SRC) $(SRC)/common.h | $(BUILD)
	$(CC) $(CFLAGS) $(SVR_SRC) -o $@ $(GLFLAGS)

client: $(BUILD)/client

$(BUILD)/client: $(CLI_SRC) $(SRC)/common.h | $(BUILD)
	$(CC) $(CFLAGS) $(CLI_SRC) -o $@ -lm

headless: $(BUILD)/server_headless

$(BUILD)/server_headless: $(SVR_SRC) $(SRC)/common.h | $(BUILD)
	$(CC) $(CFLAGS) -DNO_GUI $(SVR_SRC) -o $@ -lm

$(BUILD):
	@mkdir -p $(BUILD)

# ---------- run targets ----------
run-server: $(BUILD)/server
	@echo ">>> Starting GUI server (Ctrl+C to stop)..."
	./$(BUILD)/server $(CFG)

run-server-headless: $(BUILD)/server_headless
	@echo ">>> Starting headless server (Ctrl+C to stop)..."
	./$(BUILD)/server_headless $(CFG)

run-client: $(BUILD)/client
	@echo ">>> Running one client (using config/version.txt)..."
	./$(BUILD)/client $(CFG)

run-outdated: $(BUILD)/client
	@echo ">>> Running one client pretending to be v1 (outdated)..."
	./$(BUILD)/client $(CFG) 1

run-uptodate: $(BUILD)/client
	@echo ">>> Running one client pretending to be v5 (up to date)..."
	./$(BUILD)/client $(CFG) 5

run-many: $(BUILD)/client
	@echo ">>> Firing 5 concurrent clients (mixed versions)..."
	@./$(BUILD)/client $(CFG) 1 & \
	 ./$(BUILD)/client $(CFG) 2 & \
	 ./$(BUILD)/client $(CFG) 3 & \
	 ./$(BUILD)/client $(CFG) 4 & \
	 ./$(BUILD)/client $(CFG) 5 & \
	 wait
	@echo ">>> All 5 clients done."

run-stress: $(BUILD)/client
	@echo ">>> Firing 10 concurrent clients..."
	@for i in 1 2 3 4 1 2 3 4 1 2; do \
	   ./$(BUILD)/client $(CFG) $$i & \
	 done; wait
	@echo ">>> All 10 clients done."

# ---------- testing ----------
test: $(BUILD)/server_headless $(BUILD)/client
	@chmod +x tests/test_scenarios.sh
	@./tests/test_scenarios.sh

# ---------- utility ----------
package: | update_files
	@dd if=/dev/urandom of=update_files/update_v5.pkg bs=1M count=20 status=none
	@echo ">>> Created 20 MB random package at update_files/update_v5.pkg"

small-pkg: | update_files
	@printf 'small update v5 -- fast demo package\n' > update_files/update_v5.pkg
	@echo ">>> Wrote small text package (37 bytes) for instant transfers"

reset:
	@rm -f downloads/*.pkg
	@echo "1" > config/version.txt
	@: > logs/server.log
	@echo ">>> State reset: downloads cleared, version=v1, log truncated"

stop:
	@pkill -f $(BUILD)/server || true
	@pkill -f $(BUILD)/server_headless || true
	@echo ">>> Killed any running server processes"

update_files:
	@mkdir -p update_files

clean:
	@rm -f $(BUILD)/server $(BUILD)/client $(BUILD)/server_headless
	@echo ">>> Cleaned binaries"

help:
	@echo "Distributed Software Update Framework -- Makefile targets:"
	@echo ""
	@echo "  BUILD:"
	@echo "    make                  build GUI server + client"
	@echo "    make server           build GUI server only"
	@echo "    make client           build client only"
	@echo "    make headless         build server without OpenGL"
	@echo "    make all-targets      build everything"
	@echo ""
	@echo "  RUN (start server in one terminal, clients in another):"
	@echo "    make run-server       GUI server"
	@echo "    make run-server-headless"
	@echo "    make run-client       one client (uses config/version.txt)"
	@echo "    make run-outdated     one client as v1"
	@echo "    make run-uptodate     one client as v5"
	@echo "    make run-many         5 concurrent clients"
	@echo "    make run-stress       10 concurrent clients"
	@echo ""
	@echo "  TEST:"
	@echo "    make test             full 9-scenario automated suite"
	@echo ""
	@echo "  UTILITY:"
	@echo "    make package          generate 20 MB random update package"
	@echo "    make small-pkg        swap in small text package (fast demo)"
	@echo "    make reset            clear downloads, log, reset version"
	@echo "    make stop             kill any running server"
	@echo "    make clean            remove built binaries"

.PHONY: all all-targets server client headless \
        run-server run-server-headless run-client run-outdated run-uptodate \
        run-many run-stress test package small-pkg reset stop clean help
