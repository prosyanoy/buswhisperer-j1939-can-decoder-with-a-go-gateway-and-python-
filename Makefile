CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -g
INC     := -Ic/include
LIBSRC  := c/src/id.c c/src/signals.c c/src/tp.c c/src/candump.c
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
BUILD   := build
VENV    := .venv
GO      ?= go

.PHONY: demo-live all test test-c test-go test-hil test-san fuzz coverage venv demo clean

all: $(BUILD)/bw_decode $(BUILD)/gateway

$(BUILD)/libbuswhisperer.a: $(LIBSRC) c/include/bw_j1939.h
	@mkdir -p $(BUILD)/obj
	for f in $(LIBSRC); do $(CC) $(CFLAGS) $(INC) -c $$f -o $(BUILD)/obj/$$(basename $$f .c).o || exit 1; done
	ar rcs $@ $(BUILD)/obj/*.o

$(BUILD)/bw_decode: c/tools/bw_decode.c $(BUILD)/libbuswhisperer.a
	$(CC) $(CFLAGS) $(INC) $< $(BUILD)/libbuswhisperer.a -o $@

$(BUILD)/gateway: $(shell find gateway -name '*.go') gateway/go.mod
	cd gateway && $(GO) build -o ../$@ ./cmd/gateway

# C unit tests, plain and under ASan+UBSan
test-c: $(BUILD)/test_bw
	$(BUILD)/test_bw
$(BUILD)/test_bw: c/tests/test_bw.c $(LIBSRC) c/include/bw_j1939.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) c/tests/test_bw.c $(LIBSRC) -lm -o $@

test-san: $(BUILD)/test_bw_san
	$(BUILD)/test_bw_san
$(BUILD)/test_bw_san: c/tests/test_bw.c $(LIBSRC) c/include/bw_j1939.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -O1 $(SAN) $(INC) c/tests/test_bw.c $(LIBSRC) -lm -o $@

test-go:
	cd gateway && $(GO) vet ./... && $(GO) test ./...

$(VENV)/bin/pytest:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install -q -r harness/requirements.txt
venv: $(VENV)/bin/pytest

test-hil: all $(VENV)/bin/pytest
	$(VENV)/bin/python -m pytest harness -q

test: test-c test-san test-go test-hil

# Fuzzing: sanitized mutational fuzzer (gcc) with gcov coverage report.
fuzz:
	FUZZ_ITERS=$${FUZZ_ITERS:-300000} scripts/fuzz.sh

# Optional: real libFuzzer build (needs clang)
fuzz-libfuzzer:
	clang -g -O1 -fsanitize=fuzzer,address,undefined $(INC) fuzz/fuzz_target.c $(LIBSRC) -o $(BUILD)/fuzz_libfuzzer
	@mkdir -p $(BUILD)/corpus && $(BUILD)/fuzz_libfuzzer $(BUILD)/corpus -max_total_time=60

coverage: fuzz

demo: all
	BW_LOG=data/sample.log BW_SINK=file BW_OUT=$(BUILD)/demo.pb $(BUILD)/gateway
	@ls -l $(BUILD)/demo.pb

demo-live: all
	python3 scripts/live_demo.py data/sample.log $${SPEED:-1}

clean:
	rm -rf $(BUILD) $(VENV) gateway/gateway
