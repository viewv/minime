# 原生 PE 装载器（路线 B）
# make            构建 build/native/ 下的测试程序
# make test-tflite 原生和 Wine 各跑一次 TFLite 冒烟测试并比对输出
CC      ?= gcc
override CFLAGS += -O2 -g -Wall -Wextra -std=gnu11 -fno-strict-aliasing -Wno-unused-parameter
BUILD   := ../build/native
VENDOR  := ../vendor/win/3.0.1.35
OBJS    := $(BUILD)/pe.o $(BUILD)/kernel32.o $(BUILD)/files.o $(BUILD)/stub.o

all: $(BUILD)/tflite_smoke $(BUILD)/tflite_smoke.exe

objs: $(OBJS)

$(BUILD)/%.o: %.c pe.h winabi.h k32_internal.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/stub.o: stub.S
	@mkdir -p $(BUILD)
	$(CC) -c -o $@ $<

$(BUILD)/tflite_smoke: tests/tflite_smoke.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

$(BUILD)/tflite_smoke.exe: tests/tflite_smoke.c
	@mkdir -p $(BUILD)
	x86_64-w64-mingw32-gcc -O2 -o $@ $<

test-tflite: all
	$(BUILD)/tflite_smoke $(VENDOR)/tensorflowlite_c.dll $(VENDOR)/BuildIn/nlrgpt2_nnlm.bin > $(BUILD)/tflite_native.txt
	WINEDEBUG=-all wine $(BUILD)/tflite_smoke.exe "Z:$(abspath $(VENDOR))/tensorflowlite_c.dll" "Z:$(abspath $(VENDOR))/BuildIn/nlrgpt2_nnlm.bin" | tr -d '\r' > $(BUILD)/tflite_wine.txt
	diff $(BUILD)/tflite_native.txt $(BUILD)/tflite_wine.txt && echo "原生与 Wine 输出一致"

clean:
	rm -rf $(BUILD)

.PHONY: all objs clean test-tflite
