# Necronomicon: a CLAP synthesizer for the Akai MPC Sample (64-bit ARM Linux, Cortex-A55).
# Build inside a Linux arm64 image with ./build.sh, which runs `make all test`.
#   make            build/Necronomicon.clap
#   make test       the engine's own checks (tests/unit.cpp), the tests against the built plug-in (tests/host.cpp)
#                   and the sound's fingerprint (tests/fingerprint.cpp: the output to the bit, against
#                   tests/fingerprint.txt)
#   make bench      build/bench: renders a busy pattern; tools/bench.sh counts its instructions
#   make sanitize   the unit and host tests against a build with AddressSanitizer and UBSan
CXX ?= g++
OUT := build
WARN := -Wall -Wextra -Werror
# The MPC's cores. -ffp-contract=fast lets the compiler fuse multiply-adds; nothing here relies on IEEE errno,
# traps or exceptions.
OPT := -O3 -mcpu=cortex-a55 -ffp-contract=fast -fno-math-errno -fno-trapping-math
CXXFLAGS := -std=c++17 $(OPT) $(WARN) -fno-exceptions -fno-rtti -fvisibility=hidden -fPIC -Ithird_party/clap/include -Isrc
# Only clap_entry is exported; libstdc++ and libgcc are linked in so the MPC needs nothing beyond libc and libm. -s
# leaves out the symbol table (the code is the same; the exported clap_entry stays).
LDFLAGS := -shared -s -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-z,now -Wl,-O1 -Wl,--version-script=src/exports.map

SOURCES := src/necronomicon.cpp
HEADERS := $(wildcard src/*.h src/ui/*.h)
ART := $(OUT)/ui/liquid_art.h
CXXFLAGS += -I$(OUT)/ui

.DELETE_ON_ERROR:

.PHONY: all test bench clean fingerprint-update sanitize
all: $(OUT)/Necronomicon.clap

$(ART): tools/liquid_art.py assets/liquid-light/title-mask.pgm assets/liquid-light/silver-thorn.ppm assets/liquid-light/slipstream-eye.ppm assets/liquid-light/controls.ppm
	@mkdir -p $(OUT)/ui
	python3 tools/liquid_art.py $@

$(OUT)/Necronomicon.clap: $(SOURCES) $(HEADERS) $(ART) src/exports.map Makefile
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(OUT)/unit: tests/unit.cpp $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 $(OPT) $(WARN) -Isrc tests/unit.cpp -o $@

$(OUT)/host: tests/host.cpp tests/wav.h src/melodee_screen.h Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/host.cpp -ldl -o $@

$(OUT)/fingerprint: tests/fingerprint.cpp tests/fingerprint_scenarios.h Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/fingerprint.cpp -ldl -o $@

test: $(OUT)/Necronomicon.clap $(OUT)/unit $(OUT)/host $(OUT)/fingerprint
	$(OUT)/unit
	$(OUT)/host $(OUT)/Necronomicon.clap $(OUT)
	$(OUT)/fingerprint $(OUT)/Necronomicon.clap tests/fingerprint.txt > /dev/null

# The sound's hashes after a change meant to change the sound (see tests/fingerprint.cpp).
fingerprint-update: $(OUT)/Necronomicon.clap $(OUT)/fingerprint
	$(OUT)/fingerprint $(OUT)/Necronomicon.clap > tests/fingerprint.txt

# The unit and host tests against a build with AddressSanitizer and UBSan (any error stops it), in build/san/.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1
sanitize: $(ART) $(SOURCES) $(HEADERS) tests/host.cpp tests/unit.cpp tests/wav.h src/melodee_screen.h Makefile
	@mkdir -p $(OUT)/san
	$(CXX) -std=c++17 $(SAN) -mcpu=cortex-a55 $(WARN) -fPIC -shared -Ithird_party/clap/include -Isrc -I$(OUT)/ui $(SOURCES) -o $(OUT)/san/Necronomicon.clap
	$(CXX) -std=c++17 $(SAN) $(WARN) -Isrc tests/unit.cpp -o $(OUT)/san/unit
	$(CXX) -std=c++17 $(SAN) $(WARN) -Ithird_party/clap/include tests/host.cpp -ldl -o $(OUT)/san/host
	$(OUT)/san/unit
	$(OUT)/san/host $(OUT)/san/Necronomicon.clap $(OUT)/san

$(OUT)/bench: $(ART) tests/bench.cpp $(SOURCES) $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 $(OPT) $(WARN) -g -Ithird_party/clap/include -Isrc -I$(OUT)/ui tests/bench.cpp -o $@

bench: $(OUT)/bench

clean:
	rm -rf $(OUT)
