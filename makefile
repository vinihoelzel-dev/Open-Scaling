.RECIPEPREFIX = >
.DEFAULT_GOAL := all
CC      := gcc
CXX     := g++
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc -Iexternal -Iexternal/ffx $(shell pkg-config --cflags sdl2)
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc -Iexternal/ffx -Iexternal/imgui -Iexternal/imgui/backends -Ilibrife-ncnn-vulkan/src $(shell pkg-config --cflags sdl2)
LDLIBS  := -lX11 -lXext -lm -lXcomposite $(shell pkg-config --libs sdl2) -lvulkan -lSDL2 -lstdc++ -fopenmp

# --- ncnn: ajuste conforme o diagnóstico ---
NCNN_PREFIX := $(shell pkg-config --variable=prefix ncnn 2>/dev/null)
ifeq ($(NCNN_PREFIX),)
  # fallback: tenta /usr/local (onde o build from source costuma instalar)
  NCNN_PREFIX := /usr/local
endif
CXXFLAGS += -I$(NCNN_PREFIX)/include/ncnn
LDLIBS   += -L$(NCNN_PREFIX)/lib -lncnn

# --- fontes C ---
SRC := src/main.c src/capture_x11.c src/input_uinput.c src/overlay_input.c \
       src/timer.c src/vk_triangle.c src/vk_preview.c src/vk_upscale.c \
       src/vk_framegen.c
OBJ := $(SRC:src/%.c=build/%.o)

# --- fontes C++ locais (rife_bridge vira .cpp) ---
GUI_OBJ       := build/gui.o
RIFE_BRIDGE_O := build/rife_bridge.o

# --- librife sources ---
RIFE_SRC := $(wildcard librife-ncnn-vulkan/src/*.cpp)
RIFE_OBJ := $(patsubst librife-ncnn-vulkan/src/%.cpp,build/librife/%.o,$(RIFE_SRC))

# --- imgui ---
IMGUI_SRC := external/imgui/imgui.cpp external/imgui/imgui_draw.cpp \
             external/imgui/imgui_tables.cpp external/imgui/imgui_widgets.cpp \
             external/imgui/backends/imgui_impl_sdl2.cpp \
             external/imgui/backends/imgui_impl_vulkan.cpp
IMGUI_OBJ := $(addprefix build/,$(notdir $(IMGUI_SRC:.cpp=.o)))

BIN := build/open-scaling

GLSLC := $(shell command -v glslc 2>/dev/null)
ifeq ($(GLSLC),)
  GLSLC    := glslangValidator
  SPV_FLAG := -V
else
  SPV_FLAG :=
endif

SHADER_VERTS := $(wildcard shaders/*.vert)
SHADER_FRAGS := $(wildcard shaders/*.frag)
SHADER_COMPS := $(wildcard shaders/*.comp)
SPVS := $(patsubst shaders/%.vert,build/shaders/%.vert.spv,$(SHADER_VERTS)) \
        $(patsubst shaders/%.frag,build/shaders/%.frag.spv,$(SHADER_FRAGS)) \
        $(patsubst shaders/%.comp,build/shaders/%.comp.spv,$(SHADER_COMPS))

# --- dependências de header (evita o bug do capture_data) ---
HEADERS := $(wildcard src/*.h)
$(OBJ): $(HEADERS)
$(GUI_OBJ) $(RIFE_BRIDGE_O): $(HEADERS)

all: $(BIN) $(SPVS)

$(BIN): $(OBJ) $(GUI_OBJ) $(RIFE_BRIDGE_O) $(RIFE_OBJ) $(IMGUI_OBJ)
> $(CXX) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c | build
> $(CC) $(CFLAGS) -c $< -o $@

build/gui.o: src/gui.cpp | build
> $(CXX) $(CXXFLAGS) -c $< -o $@

build/rife_bridge.o: src/rife_bridge.cpp | build
> $(CXX) $(CXXFLAGS) -c $< -o $@

build/librife/%.o: librife-ncnn-vulkan/src/%.cpp | build/librife
> $(CXX) $(CXXFLAGS) -c $< -o $@

build/%.o: external/imgui/%.cpp | build
> $(CXX) $(CXXFLAGS) -c $< -o $@

build/%.o: external/imgui/backends/%.cpp | build
> $(CXX) $(CXXFLAGS) -c $< -o $@

build/shaders/%.vert.spv: shaders/%.vert | build/shaders
> $(GLSLC) $(SPV_FLAG) $< -o $@

build/shaders/%.frag.spv: shaders/%.frag | build/shaders
> $(GLSLC) $(SPV_FLAG) $< -o $@

build/shaders/%.comp.spv: shaders/%.comp | build/shaders
> $(GLSLC) $(SPV_FLAG) -Iexternal/ffx $< -o $@

build:
> mkdir -p build

build/shaders:
> mkdir -p build/shaders

build/librife:
> mkdir -p build/librife

clean:
> rm -rf build

.PHONY: all clean