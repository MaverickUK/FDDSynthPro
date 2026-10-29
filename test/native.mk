# Builds the host-side unit tests and the tools the Python tests run, one
# object per source, in parallel, rebuilding only what changed (each object
# records its headers in a .d file). test/run_native_tests.sh runs it:
#
#   make -f test/native.mk -j8 [SANITIZE=thread] [CXX=g++]
#
# Objects for each sanitizer setting live apart, so switching doesn't mix them.

CXX ?= c++
SANITIZE ?= address,undefined
OUT := .pio/native-tests

# The firmware's pure C++: everything the tests and tools link against.
SRC := src/fdd_signals.cpp src/trace_format.cpp src/short_test.cpp src/head.cpp src/rotation.cpp \
       src/drive.cpp src/ibm_track.cpp src/rdata_stream.cpp src/cylinder_loader.cpp src/activity.cpp \
       src/mfm_decode.cpp src/track_write.cpp src/write_burst.cpp src/synth.cpp src/wav.cpp src/ui.cpp \
       src/screens.cpp src/canvas.cpp src/settings.cpp src/image_cache.cpp src/bus.cpp src/img_cfg.cpp src/amiga_track.cpp
TESTS := $(wildcard test/native/*.cpp)

comma := ,
TEST_OBJ := $(OUT)/obj-$(subst $(comma),-,$(SANITIZE))
TOOL_OBJ := $(OUT)/obj-tools
TEST_FLAGS := -std=c++17 -O2 -g -Wall -Wextra -pthread -fsanitize=$(SANITIZE) -Isrc -Itest/native -MMD -MP
TOOL_FLAGS := -std=c++17 -O2 -Wall -Wextra -Isrc -Itest/native -MMD -MP

obj = $(patsubst %.cpp,$(1)/%.o,$(2))

TOOLS := build_track decode_write render_events dos_session showreel
build_track_SRC := test/tools/build_track.cpp src/amiga_track.cpp src/ibm_track.cpp src/rdata_stream.cpp src/img_cfg.cpp
decode_write_SRC := test/tools/decode_write.cpp src/amiga_track.cpp src/ibm_track.cpp src/mfm_decode.cpp src/track_write.cpp
render_events_SRC := test/tools/render_events.cpp src/synth.cpp src/wav.cpp
dos_session_SRC := test/tools/dos_session.cpp src/amiga_track.cpp src/bus.cpp src/fdd_signals.cpp src/head.cpp src/rotation.cpp src/drive.cpp \
                   src/ibm_track.cpp src/rdata_stream.cpp src/cylinder_loader.cpp src/activity.cpp \
                   src/mfm_decode.cpp src/track_write.cpp src/write_burst.cpp src/image_cache.cpp
showreel_SRC := $(subst test/tools/dos_session.cpp,test/tools/showreel.cpp,$(dos_session_SRC)) \
                src/synth.cpp src/wav.cpp src/ui.cpp src/screens.cpp src/canvas.cpp

.PHONY: all
all: $(TEST_OBJ)/run_tests $(addprefix $(OUT)/,$(TOOLS))

# The tests, sanitized: one binary per sanitizer setting, beside its objects.
$(TEST_OBJ)/run_tests: $(call obj,$(TEST_OBJ),$(TESTS) $(SRC))
	$(CXX) $(TEST_FLAGS) $^ -o $@

$(TEST_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_FLAGS) -c $< -o $@

# The tools, plain -O2.
define tool
$(OUT)/$(1): $(call obj,$(TOOL_OBJ),$($(1)_SRC))
	$$(CXX) $$(TOOL_FLAGS) $$^ -o $$@
endef
$(foreach t,$(TOOLS),$(eval $(call tool,$(t))))

$(TOOL_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TOOL_FLAGS) -c $< -o $@

# Rebuild when a header changes.
-include $(shell find $(OUT) -name '*.d' 2>/dev/null)
