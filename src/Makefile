# 直连宿主（Windows 版和 Linux 原生版）与 TCP⇄命名管道桥
#   make          同 make native：Linux 原生版 engined 和调试工具（链接 loader/ 的原生装载器）
#   make wine     Windows 版 host.exe、bridge.exe（旧的 Wine 方案用，需要 mingw-w64-gcc）
UNAME_S := $(shell uname -s)
ifeq ($(origin CC),default)
  ifeq ($(UNAME_S),Linux)
    CC := gcc
  else
    CC := cc
  endif
endif
CROSS   ?= x86_64-w64-mingw32-
# Windows 版的编译器。不叫 CC：环境里有 CC 时（rpmbuild 会设）make 会把它导出给 loader 的原生构建
WINCC   := $(CROSS)gcc
WINCFLAGS := -O2 -Wall -Wextra -std=gnu11
BUILD   := ../build
HDRS    := wxime.h sesscfg_official.h platform.h

all: native

wine: $(BUILD)/host.exe $(BUILD)/bridge.exe

$(BUILD)/host.exe: host.c $(HDRS) iat_trace.h
	mkdir -p $(BUILD)
	$(WINCC) $(WINCFLAGS) -o $@ host.c

$(BUILD)/bridge.exe: bridge.c
	mkdir -p $(BUILD)
	$(WINCC) $(WINCFLAGS) -o $@ bridge.c -lws2_32

NCFLAGS := $(CFLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -std=gnu11
LOADER  := $(BUILD)/native/pe.o $(BUILD)/native/kernel32.o $(BUILD)/native/files.o $(BUILD)/native/stub.o
LIBDL   := $(if $(filter Linux,$(UNAME_S)),-ldl,)
# 分配器用 jemalloc（实测比 glibc 快约 7%，见 docs/engine.md）；FreeBSD libc 已内置 jemalloc，其他平台没装就退回 libc
ifeq ($(UNAME_S),FreeBSD)
  JEMALLOC :=
else
  JEMALLOC := $(shell printf 'int main(void){return 0;}' | $(CC) -x c - -ljemalloc -o /dev/null 2>/dev/null && echo -ljemalloc)
endif

# Vulkan 检测：缺少 glslc 或 vulkan.h（或显式 MINIME_NO_VULKAN=1）时自动退回纯 CPU 后端
VK_CFLAGS := $(shell pkg-config --cflags vulkan 2>/dev/null || (test -f /usr/local/include/vulkan/vulkan.h && echo -I/usr/local/include))
ifneq ($(MINIME_NO_VULKAN),1)
  HAVE_GLSLC := $(shell command -v glslc >/dev/null 2>&1 && echo 1)
  HAVE_VK_H  := $(shell printf '#include <vulkan/vulkan.h>\nint main(void){return 0;}\n' | $(CC) $(VK_CFLAGS) -x c - -o /dev/null 2>/dev/null && echo 1)
  ifneq ($(HAVE_GLSLC)$(HAVE_VK_H),11)
    MINIME_NO_VULKAN := 1
  endif
endif

# 只有 CPU 后端（e2e_cpu.c）需要整个 TU 开启 AVX2/FMA/F16C；e2e.c / e2e_vk.c 用基线指令集，
# e2e.c 内的 logsumexp AVX2 版本通过 target 属性在运行时按 __builtin_cpu_supports 分发，避免老 CPU 在检测前 SIGILL
E2EBASEFLAGS := $(CFLAGS) -O3 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
E2ECPUFLAGS  := $(E2EBASEFLAGS) -mavx2 -mfma -mf16c
E2EOBJS  := $(BUILD)/native/e2e.o $(BUILD)/native/e2e_cpu.o $(BUILD)/native/e2e_vk.o
SHADERS  := $(patsubst shaders/%.comp,$(BUILD)/native/shaders/%.spv.h,$(wildcard shaders/*.comp)) \
           $(BUILD)/native/shaders/matmul_q4_sg.spv.h $(BUILD)/native/shaders/matmul_q8_sg.spv.h
NATIVE_BINS := $(BUILD)/native/engined $(BUILD)/native/userdata $(BUILD)/native/e2e_cli $(BUILD)/native/e2e_test $(if $(wildcard host.c),$(BUILD)/native/host)

loader_objs:
	$(MAKE) -C ../loader objs

$(BUILD)/native/shaders/%.spv.h: shaders/%.comp shaders/common.glsl
	@mkdir -p $(BUILD)/native/shaders
	glslc -O --target-env=vulkan1.1 -mfmt=c -o $@ $<

$(BUILD)/native/shaders/%_sg.spv.h: shaders/%.comp shaders/common.glsl
	@mkdir -p $(BUILD)/native/shaders
	glslc -O --target-env=vulkan1.1 -DUSE_SUBGROUP_ADD -mfmt=c -o $@ $<

$(BUILD)/native/e2e.o: e2e.c e2e.h e2e_internal.h
	@mkdir -p $(BUILD)/native
	$(CC) $(E2EBASEFLAGS) -c -o $@ e2e.c

$(BUILD)/native/e2e_cpu.o: e2e_cpu.c e2e.h e2e_internal.h
	@mkdir -p $(BUILD)/native
	$(CC) $(E2ECPUFLAGS) -c -o $@ e2e_cpu.c

ifeq ($(MINIME_NO_VULKAN),1)
$(BUILD)/native/e2e_vk.o: e2e_vk.c e2e.h e2e_internal.h
	@mkdir -p $(BUILD)/native
	$(CC) $(E2EBASEFLAGS) -DMINIME_NO_VULKAN -c -o $@ e2e_vk.c
else
$(BUILD)/native/e2e_vk.o: e2e_vk.c e2e.h e2e_internal.h $(SHADERS)
	@mkdir -p $(BUILD)/native
	$(CC) $(E2EBASEFLAGS) $(VK_CFLAGS) -I$(BUILD)/native/shaders -c -o $@ e2e_vk.c
endif

$(BUILD)/native/host: host.c loader_objs
	$(CC) $(NCFLAGS) $(LDFLAGS) -o $@ host.c $(LOADER) -lpthread

$(BUILD)/native/engined: engined.c engine.c netcompat.c $(wildcard tflite_trace.c) $(E2EOBJS) loader_objs $(HDRS)
	$(CC) $(NCFLAGS) $(LDFLAGS) -o $@ engined.c engine.c netcompat.c $(wildcard tflite_trace.c) $(E2EOBJS) $(LOADER) -lpthread -lm $(LIBDL) $(JEMALLOC)

$(BUILD)/native/userdata: userdata.c engine.c $(wildcard tflite_trace.c) loader_objs $(HDRS)
	$(CC) $(NCFLAGS) $(LDFLAGS) -o $@ userdata.c engine.c $(wildcard tflite_trace.c) $(LOADER) -lpthread -lm $(LIBDL) $(JEMALLOC)

$(BUILD)/native/e2e_cli: e2e_cli.c $(E2EOBJS)
	$(CC) $(NCFLAGS) $(LDFLAGS) -o $@ e2e_cli.c $(E2EOBJS) -lpthread -lm $(LIBDL)

$(BUILD)/native/e2e_test: e2e_test.c $(E2EOBJS)
	$(CC) $(NCFLAGS) $(LDFLAGS) -o $@ e2e_test.c $(E2EOBJS) -lpthread -lm $(LIBDL)

native: $(NATIVE_BINS)

clean:
	rm -rf $(BUILD)

.PHONY: all native wine clean loader_objs
