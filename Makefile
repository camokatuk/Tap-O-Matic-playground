# ---------------------------------------------------------------------------
# Firmware selection ("pluggable" module swap point).
#
# The board support (time_machine_hardware.*) is shared by every firmware.
# Each firmware is one application .cpp that provides its own main(). Pick one
# with  MODULE=<name>  on the make command line; Tap-O-Matic (the delay) is the
# default so a bare `make` builds exactly what it always did.
#
#   make                  -> Tap-O-Matic delay     (Tap-O-Matic.bin)  [default]
#   make MODULE=foxtail   -> Fox Tail oscillator    (Fox-Tail.bin)
# ---------------------------------------------------------------------------
MODULE ?= tapomatic

ifeq ($(MODULE),tapomatic)
  TARGET     = Tap-O-Matic
  MODULE_SRC = TimeMachine.cpp
else ifeq ($(MODULE),foxtail)
  TARGET     = Fox-Tail
  MODULE_SRC = FoxTail.cpp          # not created yet — see docs/claude/oscillator-impl.md
else
  $(error Unknown MODULE '$(MODULE)'. Use MODULE=tapomatic or MODULE=foxtail)
endif

USE_DAISYSP_LGPL=1
#DEBUG=1

# Sources: shared board support + the selected firmware.
CPP_SOURCES = time_machine_hardware.cpp $(MODULE_SRC)
LDFLAGS = -u _printf_float
OPT = -Os

# Library Locations
LIBDAISY_DIR = ../DaisyExamples/libDaisy
DAISYSP_DIR = ../DaisyExamples/DaisySP

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
