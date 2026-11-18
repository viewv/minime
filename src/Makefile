# 直连宿主（Windows 版和 Linux 原生版）与 TCP⇄命名管道桥
#   make          同 make native：Linux 原生版 engined 和调试工具（链接 loader/ 的原生装载器）
#   make wine     Windows 版 host.exe、bridge.exe（旧的 Wine 方案用，需要 mingw-w64-gcc）
CROSS   ?= x86_64-w64-mingw32-
# Windows 版的编译器。不叫 CC：环境里有 CC 时（rpmbuild 会设）make 会把它导出给 loader 的原生构建
WINCC   := $(CROSS)gcc
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
BUILD   := ../build
HDRS    := wxime.h sesscfg_official.h platform.h

all: native

wine: $(BUILD)/host.exe $(BUILD)/bridge.exe

$(BUILD)/host.exe: host.c $(HDRS) iat_trace.h
	mkdir -p $(BUILD)
	$(WINCC) $(CFLAGS) -o $@ host.c

$(BUILD)/bridge.exe: bridge.c
	mkdir -p $(BUILD)
	$(WINCC) $(CFLAGS) -o $@ bridge.c -lws2_32

NCFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -std=gnu11
LOADER  := $(BUILD)/native/pe.o $(BUILD)/native/kernel32.o $(BUILD)/native/files.o $(BUILD)/native/stub.o
# 分配器用 jemalloc（实测比 glibc 快约 7%，见 docs/engine.md）；没装就退回 glibc
JEMALLOC := $(shell printf 'int main(void){return 0;}' | gcc -x c - -ljemalloc -o /dev/null 2>/dev/null && echo -ljemalloc)

# 只有 CPU 后端（e2e_cpu.c）需要整个 TU 开启 AVX2/FMA/F16C；e2e.c / e2e_vk.c 用基线指令集，
# e2e.c 内的 logsumexp AVX2 版本通过 target 属性在运行时按 __builtin_cpu_supports 分发，避免老 CPU 在检测前 SIGILL
E2EBASEFLAGS := -O3 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
E2ECPUFLAGS  := $(E2EBASEFLAGS) -mavx2 -mfma -mf16c
E2EOBJS  := $(BUILD)/native/e2e.o $(BUILD)/native/e2e_cpu.o $(BUILD)/native/e2e_vk.o

native:
	$(MAKE) -C ../loader objs
	mkdir -p $(BUILD)/native
	gcc $(E2EBASEFLAGS) -c -o $(BUILD)/native/e2e.o e2e.c
	gcc $(E2ECPUFLAGS) -c -o $(BUILD)/native/e2e_cpu.o e2e_cpu.c
	mkdir -p $(BUILD)/native/shaders
	for f in shaders/*.comp; do glslc -O --target-env=vulkan1.1 -mfmt=c -o $(BUILD)/native/shaders/`basename $$f .comp`.spv.h $$f || exit 1; done
	gcc $(E2EBASEFLAGS) -I$(BUILD)/native/shaders -c -o $(BUILD)/native/e2e_vk.o e2e_vk.c
	[ ! -f host.c ] || gcc $(NCFLAGS) -o $(BUILD)/native/host host.c $(LOADER) -lpthread   # 调试用宿主（公开仓库里没有）
	gcc $(NCFLAGS) -o $(BUILD)/native/engined engined.c engine.c netcompat.c tflite_trace.c $(E2EOBJS) $(LOADER) -lpthread -lm -ldl $(JEMALLOC)
	gcc $(NCFLAGS) -o $(BUILD)/native/userdata userdata.c engine.c tflite_trace.c $(LOADER) -lpthread -lm -ldl $(JEMALLOC)
	gcc $(NCFLAGS) -o $(BUILD)/native/e2e_cli e2e_cli.c $(E2EOBJS) -lpthread -lm -ldl
	gcc $(NCFLAGS) -o $(BUILD)/native/e2e_test e2e_test.c $(E2EOBJS) -lpthread -lm -ldl

clean:
	rm -rf $(BUILD)

.PHONY: all native wine clean
