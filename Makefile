# Convenience wrapper around the CMake presets.
#
# Nothing here is load-bearing: every target maps to a `cmake --preset` command
# you can run directly, and CMakePresets.json remains the source of truth. This
# exists so the common actions are one word instead of three.
#
# Run `make` on its own for the list.

# Reconfiguring is cheap and getting it wrong is expensive -- a stale cache
# produces errors that point anywhere but at the cause. Each build target
# therefore configures first.
CMAKE   ?= cmake
CTEST   ?= ctest
PRESET  ?= dev
JOBS    ?=

BUILD_DIR = build/$(PRESET)
DIST_DIR  = $(BUILD_DIR)/dist

ifneq ($(JOBS),)
  BUILD_FLAGS = --parallel $(JOBS)
endif

# Everything after the target name on the command line is passed through, so
# `make sweep ARGS="--start 88M --stop 108M"` works.
ARGS ?=

.DEFAULT_GOAL := help

# ---------------------------------------------------------------- building

.PHONY: build
build: ## Configure and build the default (dev) preset
	@$(CMAKE) --preset $(PRESET)
	@$(CMAKE) --build --preset $(PRESET) $(BUILD_FLAGS)

.PHONY: debug
debug: ## Build the debug preset
	@$(MAKE) build PRESET=debug

.PHONY: release
release: ## Build the optimised release preset
	@$(MAKE) build PRESET=release

.PHONY: headless
headless: ## Build without the GUI, for a remote node
	@$(MAKE) build PRESET=headless

.PHONY: all
all: build debug release ## Build dev, debug and release

# ----------------------------------------------------------- module builds

# One target per plugin does not scale, and there are going to be more plugins
# than targets worth reading. WITHOUT names what to switch off instead.
#
#   make without WITHOUT=bladerf              -> build/without-bladerf
#   make without WITHOUT="gui server" BUILD_TYPE=Release
#
# The directory is derived from the sorted list, so one combination always
# lands in the same tree and two combinations never share one. That is what
# keeps a switched-off module from lingering in a stale cache.
#
# Valid names are read out of CMakeLists.txt, so a new plugin needs no edit
# here; `make modules` lists them.
BUILD_TYPE ?= RelWithDebInfo

.PHONY: modules
modules: ## List the modules `make without` can switch off
	@echo 'switch off with: make without WITHOUT="name name"'
	@echo ""
	@sed -nE 's/^option\(SWEEPPP_(WITH|BUILD)_([A-Z0-9_]+)[[:space:]]+"([^"]*)".*/\2|\3/p' \
		CMakeLists.txt \
		| awk -F'|' '{ n = tolower($$1); gsub(/_/, "-", n); printf "  %-12s %s\n", n, $$2 }'

.PHONY: without
without: ## Build with modules off (pass WITHOUT="gui server")
	@test -n "$(WITHOUT)" || { \
		echo 'usage: make without WITHOUT="gui server"   (see: make modules)' >&2; \
		exit 2; }
	@set -e; \
	flags=""; suffix=""; \
	for m in $(sort $(WITHOUT)); do \
		u=$$(printf '%s' "$$m" | tr 'a-z-' 'A-Z_'); \
		o=$$(grep -oE "SWEEPPP_(WITH|BUILD)_$$u"'\b' CMakeLists.txt | head -1); \
		if [ -z "$$o" ]; then \
			echo "unknown module '$$m' -- run: make modules" >&2; exit 2; \
		fi; \
		flags="$$flags -D$$o=OFF"; suffix="$$suffix-$$m"; \
	done; \
	dir="build/without$$suffix"; \
	$(CMAKE) -S . -B "$$dir" -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $$flags; \
	$(CMAKE) --build "$$dir" $(BUILD_FLAGS); \
	echo "binaries in $$dir/dist/"

# ----------------------------------------------------------------- running

.PHONY: run
run: build ## Build and launch the desktop application
	@app=$$(ls -d "$(DIST_DIR)"/*.app 2>/dev/null | head -1); \
	if [ -n "$$app" ]; then \
		"$$app/Contents/MacOS/$$(basename "$$app" .app)"; \
	else \
		"$(DIST_DIR)/sweeppp"; \
	fi

.PHONY: cli
cli: build ## Build and run sweeppp-cli (pass ARGS="...")
	@$(DIST_DIR)/sweeppp-cli $(ARGS)

.PHONY: info
info: build ## List the FFT backends and SDR devices this build can see
	@$(DIST_DIR)/sweeppp-cli info

.PHONY: sweep
sweep: build ## Headless synthetic sweep with telemetry (pass ARGS="...")
	@$(DIST_DIR)/sweeppp-cli sweep --device synthetic --sample-rate 20e6 \
		--duration 5 --stats $(ARGS)

.PHONY: throughput
throughput: build ## The 100 MS/s acceptance run: rate held, sample accounting exact
	@$(DIST_DIR)/sweeppp-cli sweep --device synthetic --sample-rate 100e6 \
		--duration 30 --stats

.PHONY: serve
serve: build ## Run the remote node (loopback only unless ARGS says otherwise)
	@$(DIST_DIR)/sweeppp-cli serve --device synthetic $(ARGS)

# ----------------------------------------------------------------- testing

# --test-dir rather than --preset: only dev/asan/tsan have test presets, and
# those add nothing but --output-on-failure, so this form works for every
# preset instead of failing on `make test PRESET=release`.
.PHONY: test
test: build ## Build and run the test suite
	@$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

.PHONY: asan
asan: ## Run the tests under AddressSanitizer + UBSan
	@$(MAKE) test PRESET=asan

.PHONY: tsan
tsan: ## Run the tests under ThreadSanitizer (the acquisition pipeline)
	@$(MAKE) test PRESET=tsan

# libsweepsfile checks itself.
#
# The library is MIT, dependency-free and meant to be usable by projects that
# have never heard of Sweep++, so it owns its own build, test and install
# round-trips -- see lib/libsweepsfile/Makefile. Duplicating any of that here
# would mean two places to keep in step, and the copy in this file would be the
# one nobody ran.
#
# Delegated rather than dropped: built only as a subdirectory, "still compiles
# as C++17" and "still builds standalone" are properties that rot within a
# month, because the parent sets CMAKE_CXX_STANDARD 23, provides doctest and
# Threads, and quietly supplies every include path. Something in `make check`
# has to notice.
.PHONY: sweepsfile
sweepsfile: ## Build, test and install-check libsweepsfile standalone (C++17, C++23, shared, bindings)
	@$(MAKE) -C lib/libsweepsfile check JOBS=$(JOBS)

.PHONY: check
check: test sweepsfile asan tsan ## Everything CI would run: tests, standalone lib, both sanitizers
	@echo "all checks passed"

# ------------------------------------------------------------------ linting

# The scripts in tools/ hold the logic: which files are in scope, and the SDK
# sysroot Homebrew's clang-tidy needs and the compile database does not record.
# These targets exist so the common ones are one word.

.PHONY: format
format: ## Apply .clang-format across the tree
	@tools/format.sh

.PHONY: format-check
format-check: ## Verify formatting without rewriting anything
	@tools/format.sh --check

.PHONY: tidy
tidy: build ## Run clang-tidy (PRESET picks the compile database)
	@tools/tidy.sh

.PHONY: check-headers
check-headers: ## Verify every source carries its zone's SPDX licence header
	@tools/check-headers.sh

.PHONY: lint
lint: format-check check-headers ## Formatting and licence headers, no build required

# ------------------------------------------------------------------ upkeep

.PHONY: compile-commands
compile-commands: build ## Refresh the compile_commands.json symlink for clangd
	@ln -sf $(BUILD_DIR)/compile_commands.json compile_commands.json
	@echo "compile_commands.json -> $(BUILD_DIR)/compile_commands.json"

# compile_commands.json is left alone: it is a relative symlink into the tree
# being removed, so it dangles until the next build and then resolves again on
# its own. Deleting it would cost clangd its flags for no gain.
.PHONY: clean
clean: ## Remove the build trees, fetched dependencies included
	@rm -rf build
	@echo "build/ removed"

.PHONY: rebuild
rebuild: clean build ## Clean build tree and rebuild from scratch

.PHONY: help
help: ## Show this help
	@echo "Sweep++ -- make targets"
	@echo ""
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}'
	@echo ""
	@echo "Variables:"
	@echo "  PRESET=<name>   dev (default), debug, release, headless, asan, tsan"
	@echo "  JOBS=<n>        parallel build jobs"
	@echo "  ARGS=\"...\"      extra arguments for run/cli/sweep targets"
	@echo "  WITHOUT=\"...\"   modules to switch off for 'make without'"
	@echo "  BUILD_TYPE=<t>  build type for 'make without' (default RelWithDebInfo)"
	@echo ""
	@echo "Examples:"
	@echo "  make run"
	@echo "  make test"
	@echo "  make cli ARGS=\"info\""
	@echo "  make sweep ARGS=\"--start 88M --stop 108M --rbw 10k\""
	@echo "  make without WITHOUT=bladerf"
	@echo "  make build PRESET=release JOBS=8"
