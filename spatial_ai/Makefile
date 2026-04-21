CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude -std=c11
LDFLAGS = -lm

SRC_DIR         = src
INC_DIR         = include
TEST_DIR        = tests
TOOLS_DIR       = tools
THIRD_PARTY_DIR = third_party
BUILD_DIR       = build

# ─── Host-portable shell helpers ─────────────────────────
# mingw32-make under PowerShell defaults to cmd.exe, which doesn't
# know `mkdir -p`, `rm -rf`, or sh-style `for/do/done`. Pick a shell
# and command shapes per host so the same Makefile runs under both
# POSIX sh and cmd.exe.
ifeq ($(OS),Windows_NT)
    MKDIR_BUILD = if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)
    RMRF_BUILD  = if exist $(BUILD_DIR) rmdir /s /q $(BUILD_DIR)
    EXE         = .exe
else
    MKDIR_BUILD = mkdir -p $(BUILD_DIR)
    RMRF_BUILD  = rm -rf $(BUILD_DIR)
    EXE         =
endif

# ─── Engine sources ──────────────────────────────────────
# Text-side (v8 "완성본"): adds spatial_clock alongside the
# existing stack. Image-side (img-canvas): self-contained CE
# pipeline + bimodal binding. Both compile into the same
# engine objects below.
SRCS = $(SRC_DIR)/spatial_grid.c \
       $(SRC_DIR)/spatial_morpheme.c \
       $(SRC_DIR)/spatial_layers.c \
       $(SRC_DIR)/spatial_match.c \
       $(SRC_DIR)/spatial_keyframe.c \
       $(SRC_DIR)/spatial_context.c \
       $(SRC_DIR)/spatial_generate.c \
       $(SRC_DIR)/spatial_io.c \
       $(SRC_DIR)/spatial_canvas.c \
       $(SRC_DIR)/spatial_clock.c \
       $(SRC_DIR)/spatial_subtitle.c \
       $(SRC_DIR)/img_ce.c \
       $(SRC_DIR)/img_delta_memory.c \
       $(SRC_DIR)/img_delta_compute.c \
       $(SRC_DIR)/img_tier_table.c \
       $(SRC_DIR)/img_delta_tables_data.c \
       $(SRC_DIR)/img_set16.c \
       $(SRC_DIR)/img_render.c \
       $(SRC_DIR)/img_pipeline.c \
       $(SRC_DIR)/img_delta_learn.c \
       $(SRC_DIR)/img_ce_diff.c \
       $(SRC_DIR)/img_drawing.c \
       $(SRC_DIR)/img_noise_memory.c \
       $(SRC_DIR)/spatial_bimodal.c

OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

TESTS = test_grid test_morpheme test_layers test_match test_keyframe \
        test_context test_integration test_io test_cascade test_canvas \
        test_adaptive test_subtitle \
        test_img_ce test_img_delta_memory test_img_set16 test_img_render \
        test_img_pipeline test_img_tier_table test_img_delta_learn \
        test_img_ce_diff test_img_drawing test_img_noise_memory test_bimodal

.PHONY: all clean test gpu_train_help demo train gen-tables wiki bench \
        bench_word bench_qa chat stream

all: $(BUILD_DIR) $(OBJS)
	@echo "Build complete."

$(BUILD_DIR):
	$(MKDIR_BUILD)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ─── Tests ───────────────────────────────────────────────
# Each suite becomes a `run-<name>` phony that runs the built binary.
# The `test` target depends on every `run-<name>`, so make orchestrates
# sequencing with no shell loop — works identically under /bin/sh and
# cmd.exe.
RUN_TESTS = $(addprefix run-,$(TESTS))

.PHONY: $(RUN_TESTS)

$(RUN_TESTS): run-%: $(BUILD_DIR)/%
	@echo --- $* ---
	@$<

test: $(addprefix $(BUILD_DIR)/,$(TESTS)) $(RUN_TESTS)
	@echo === ALL TESTS PASSED ===

# Text-engine tests
$(BUILD_DIR)/test_grid:        $(TEST_DIR)/test_grid.c        $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_morpheme:    $(TEST_DIR)/test_morpheme.c    $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_layers:      $(TEST_DIR)/test_layers.c      $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_match:       $(TEST_DIR)/test_match.c       $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_keyframe:    $(TEST_DIR)/test_keyframe.c    $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_context:     $(TEST_DIR)/test_context.c     $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_integration: $(TEST_DIR)/test_integration.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_io:          $(TEST_DIR)/test_io.c          $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_cascade:     $(TEST_DIR)/test_cascade.c     $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_canvas:      $(TEST_DIR)/test_canvas.c      $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_adaptive:    $(TEST_DIR)/test_adaptive.c    $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_subtitle:    $(TEST_DIR)/test_subtitle.c    $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)

# Image-side tests
$(BUILD_DIR)/test_img_ce:           $(TEST_DIR)/test_img_ce.c           $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_delta_memory: $(TEST_DIR)/test_img_delta_memory.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_set16:        $(TEST_DIR)/test_img_set16.c        $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_render:       $(TEST_DIR)/test_img_render.c       $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_pipeline:     $(TEST_DIR)/test_img_pipeline.c     $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_tier_table:   $(TEST_DIR)/test_img_tier_table.c   $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_delta_learn:  $(TEST_DIR)/test_img_delta_learn.c  $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_ce_diff:      $(TEST_DIR)/test_img_ce_diff.c      $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_drawing:      $(TEST_DIR)/test_img_drawing.c      $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_img_noise_memory: $(TEST_DIR)/test_img_noise_memory.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/test_bimodal:          $(TEST_DIR)/test_bimodal.c          $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)

# ─── Wikipedia integration test (manual run) ─────────────────
$(BUILD_DIR)/test_wiki: $(TEST_DIR)/test_wiki.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)

wiki: $(BUILD_DIR)/test_wiki
	@echo "Built test_wiki. Run with:"
	@echo "  ./build/test_wiki data/sample_ko.txt"
	@echo "  ./build/test_wiki data/sample_en.txt"
	@echo "  ./build/test_wiki data/sample_en.txt 500"

# ─── Benchmarks: STS-B + Perplexity ──────────────────────────
$(BUILD_DIR)/bench_stsb:         $(TEST_DIR)/bench_stsb.c         $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/bench_perplexity:   $(TEST_DIR)/bench_perplexity.c   $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/bench_word_predict: $(TEST_DIR)/bench_word_predict.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)
$(BUILD_DIR)/bench_qa:           $(TEST_DIR)/bench_qa.c           $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)

bench_word: $(BUILD_DIR)/bench_word_predict
	@echo "Built bench_word_predict. Run with:"
	@echo "  ./build/bench_word_predict data/sample_ko.txt"
	@echo "  ./build/bench_word_predict data/sample_en.txt 1000"

bench_qa: $(BUILD_DIR)/bench_qa
	@echo "Built bench_qa. Run with:"
	@echo "  ./build/bench_qa data/qa.tsv"

gpu_train_help:
	@echo "Kaggle GPU trainer (experimental):"
	@echo "  pip install -r requirements-gpu.txt"
	@echo "  python tools/kaggle_gpu_train.py --input data/sample_en.txt --max-clauses 50000 --checkpoint-every 5000"
	@echo "  output: build/gpu_models/gpu_model_final.pt"

# ─── Streaming trainer (line-by-line, no full-file buffering) ────
$(BUILD_DIR)/stream_train: $(TOOLS_DIR)/stream_train.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)

stream: $(BUILD_DIR)/stream_train
	@echo "Built stream_train. Example:"
	@echo "  ./build/stream_train --input data/sample_en.txt --max 50000 \\"
	@echo "                       --save build/models/wiki50k.spai --verify"

# ─── Interactive chat REPL (with --session + query router) ───
$(BUILD_DIR)/chat: $(TOOLS_DIR)/chat.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)

chat: $(BUILD_DIR)/chat
	@echo "Built chat. Example:"
	@echo "  ./build/chat --load build/models/wiki5k.spai --session build/chat.session"
	@echo "  ./build/chat --train data/wiki5k.txt --max 5000"

# ─── Vendored stb_image (built with relaxed warnings) ────────
$(BUILD_DIR)/stb_image_impl.o: $(THIRD_PARTY_DIR)/stb_image_impl.c | $(BUILD_DIR)
	$(CC) -O2 -std=c11 -I$(THIRD_PARTY_DIR) -w -c $< -o $@

# ─── CE delta table generator (offline) ──────────────────────
$(BUILD_DIR)/gen_delta_tables: $(TOOLS_DIR)/gen_delta_tables.c \
                                $(BUILD_DIR)/img_delta_compute.o \
                                $(BUILD_DIR)/img_tier_table.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(BUILD_DIR)/img_delta_compute.o $(BUILD_DIR)/img_tier_table.o \
	      -o $@ $(LDFLAGS)

gen-tables: $(BUILD_DIR)/gen_delta_tables
	$(BUILD_DIR)/gen_delta_tables > $(SRC_DIR)/img_delta_tables_data.c
	@echo "Regenerated $(SRC_DIR)/img_delta_tables_data.c"

# ─── Image pipeline demo (PPM + PNG) ─────────────────────────
$(BUILD_DIR)/demo_pipeline: $(TOOLS_DIR)/demo_pipeline.c $(OBJS) \
                             $(BUILD_DIR)/stb_image_impl.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(THIRD_PARTY_DIR) $< $(OBJS) \
	      $(BUILD_DIR)/stb_image_impl.o -o $@ $(LDFLAGS)

demo: $(BUILD_DIR)/demo_pipeline
	@echo "Built demo_pipeline. Usage:"
	@echo "  ./build/demo_pipeline [--adapt] input.png [out_prefix]"

# ─── Image training CLI (manifest → .spai + .imem) ──────────
$(BUILD_DIR)/train: $(TOOLS_DIR)/train.c $(OBJS) \
                    $(BUILD_DIR)/stb_image_impl.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(THIRD_PARTY_DIR) $< $(OBJS) \
	      $(BUILD_DIR)/stb_image_impl.o -o $@ $(LDFLAGS)

train: $(BUILD_DIR)/train
	@echo "Built train. Usage:"
	@echo "  ./build/train [--resume] [--model foo.spai] [--memory foo.imem] manifest.tsv"

# ─── Frame-by-frame drawing CLI (.imem + optional seed → PNG frames) ──
$(BUILD_DIR)/draw: $(TOOLS_DIR)/draw.c $(OBJS) \
                   $(BUILD_DIR)/stb_image_impl.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(THIRD_PARTY_DIR) $< $(OBJS) \
	      $(BUILD_DIR)/stb_image_impl.o -o $@ $(LDFLAGS)

draw: $(BUILD_DIR)/draw
	@echo "Built draw. Usage:"
	@echo "  ./build/draw --memory out/models/out.imem --out out/draw"
	@echo "               [--seed-image path | --seed-kf ID --model file.spai]"
	@echo "               [--frames N] [--top-g N] [--penalty F]"
	@echo "  Writes out/draw/frames/frame_NNN.{png,ppm} + out/draw/final.{png,ppm}."

# ─── Aggregate bench runner ──────────────────────────────────
bench: $(BUILD_DIR)/bench_stsb $(BUILD_DIR)/bench_perplexity \
       $(BUILD_DIR)/bench_word_predict $(BUILD_DIR)/bench_qa
	@echo "=== Benchmarks built ==="
	@echo "  ./build/bench_stsb          data/stsb.tsv"
	@echo "  ./build/bench_perplexity    data/sample_ko.txt"
	@echo "  ./build/bench_word_predict  data/sample_ko.txt"
	@echo "  ./build/bench_qa            data/qa.tsv"
	@echo ""
	@if [ -f data/stsb.tsv ]; then \
		echo "=== Running bench_stsb ==="; \
		./$(BUILD_DIR)/bench_stsb data/stsb.tsv || true; \
	else \
		echo "(skip bench_stsb: data/stsb.tsv not found)"; \
	fi
	@echo ""
	@if [ -f data/sample_ko.txt ]; then \
		echo "=== Running bench_perplexity on sample_ko.txt ==="; \
		./$(BUILD_DIR)/bench_perplexity data/sample_ko.txt 500 || true; \
		echo "=== Running bench_word_predict on sample_ko.txt ==="; \
		./$(BUILD_DIR)/bench_word_predict data/sample_ko.txt 500 || true; \
	elif [ -f data/sample_en.txt ]; then \
		echo "=== Running bench_perplexity on sample_en.txt ==="; \
		./$(BUILD_DIR)/bench_perplexity data/sample_en.txt 500 || true; \
		echo "=== Running bench_word_predict on sample_en.txt ==="; \
		./$(BUILD_DIR)/bench_word_predict data/sample_en.txt 500 || true; \
	else \
		echo "(skip bench_perplexity/word: no sample_*.txt)"; \
	fi
	@echo ""
	@if [ -f data/qa.tsv ]; then \
		echo "=== Running bench_qa ==="; \
		./$(BUILD_DIR)/bench_qa data/qa.tsv || true; \
	else \
		echo "(skip bench_qa: data/qa.tsv not found — run data/make_qa.ps1)"; \
	fi

clean:
	$(RMRF_BUILD)
