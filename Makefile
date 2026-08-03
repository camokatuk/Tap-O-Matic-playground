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
  # Delay stays stock: whinebug.md says it is already at its optimum, so its
  # build flags are not something to go poking at.
  OPT        = -Os
else ifeq ($(MODULE),foxtail)
  TARGET     = Fox-Tail
  MODULE_SRC = FoxTail.cpp
  # SERIAL_LOG=1 turns on the once-a-second status line (CV values, V/oct, CPU
  # load) without editing FoxTail.cpp and remembering to revert it.
  ifdef SERIAL_LOG
    C_DEFS  += -DFOXTAIL_SERIAL_LOG=$(SERIAL_LOG)
  endif
  # CV_NULL=1 captures the unpatched idle reading of every summing jack at
  # startup and stores it, for when only the nulls need redoing (step 3 of the
  # calibration gesture does the same). One-shot: flash it once with nothing
  # patched, then build without it again.
  ifdef CV_NULL
    C_DEFS  += -DFOXTAIL_CV_NULL=$(CV_NULL)
  endif
  # NO_NORM=1 defeats the Cluster collapse compensation, for A/B-ing it against
  # a symptom on hardware. tests/run.sh builds the same flag.
  ifdef NO_NORM
    C_DEFS  += -DFOXTAIL_CLUSTER_NORM=0
  endif
  # QUIRKS=1 compensates for this unit's hardware: a software centre detent on
  # the spectral shift pot, which has no mechanical one. Per-unit, so it is a
  # flag rather than the default.
  ifdef QUIRKS
    C_DEFS  += -DFOXTAIL_QUIRKS=$(QUIRKS)
  endif
  # Optimise for SPEED, not size. The additive engine's inner loop runs
  # kNumPartials x 48000 times a second; -Os leaves it unrolled and badly
  # scheduled, which is enough on its own to blow the audio budget.
  # -fno-math-errno is not cosmetic: without it every std::sqrt compiles to
  # VSQRT *plus* a compare and a branch to libm's sqrtf to set errno, which is
  # both a function call and the vcmpe/vmrs pipeline stall we work to avoid.
  OPT        = -O3 -fno-math-errno -fno-trapping-math
else
  $(error Unknown MODULE '$(MODULE)'. Use MODULE=tapomatic or MODULE=foxtail)
endif

USE_DAISYSP_LGPL=1
#DEBUG=1

# Sources: shared board support + the selected firmware.
CPP_SOURCES = time_machine_hardware.cpp $(MODULE_SRC)
LDFLAGS = -u _printf_float

# Library Locations
LIBDAISY_DIR = ../DaisyExamples/libDaisy
DAISYSP_DIR = ../DaisyExamples/DaisySP

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
