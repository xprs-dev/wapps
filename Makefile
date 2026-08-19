# XPRS wapps — top-level Makefile
#
# Targets:
#   make             — build and package every wapp into binaries/
#   make <wapp-id>   — build and package one wapp (e.g. make maps)
#   make clean       — remove binaries/
#   make examples    — build the sample modules under modules/
#   make install-sdk — install wasi-sdk locally for compiling

WASI_SDK_PATH ?= $(HOME)/wasi-sdk
export WASI_SDK_PATH

# Discover wapp source folders: any top-level directory with a
# manifest.json. Skips infrastructure folders (sdk/, hal/, modules/,
# binaries/) automatically.
WAPP_DIRS  := $(patsubst %/manifest.json,%,$(wildcard */manifest.json))
WAPP_NAMES := $(notdir $(WAPP_DIRS))

# Sample modules (echo_lib, hello_world) live separately under
# modules/ — they are *not* wapps but small demos of the HAL.
EXAMPLE_DIRS  := $(patsubst %/Makefile,%,$(wildcard modules/*/Makefile))
EXAMPLE_NAMES := $(notdir $(EXAMPLE_DIRS))

.PHONY: all clean install-sdk examples $(WAPP_NAMES) $(EXAMPLE_NAMES)

all:
	@./build-archive.sh

clean:
	@./build-archive.sh clean

install-sdk:
	@./install-wasi-sdk.sh

# Build one wapp by id — `make maps` etc.
$(WAPP_NAMES):
	@./build-archive.sh $@

# Sample modules
examples: $(EXAMPLE_NAMES)

$(EXAMPLE_NAMES):
	@$(MAKE) -C modules/$@ --no-print-directory
