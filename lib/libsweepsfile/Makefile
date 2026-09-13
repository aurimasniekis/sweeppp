# libsweepsfile, on its own.
#
# This library is MIT, C++17 and dependency-free, and it is meant to be usable
# by projects that have never heard of Sweep++. So it builds, tests and
# install-checks itself from this directory, with nothing above it involved --
# which is also what keeps "still builds standalone" from being a claim that
# quietly stops being true.
#
# Nothing here is load-bearing: every target is a `cmake` command you can run
# directly. This exists so the common actions are one word instead of four.
#
# Run `make` on its own for the list.

CMAKE ?= cmake
CTEST ?= ctest
JOBS  ?=

BUILD        ?= build
BUILD_TYPE   ?= RelWithDebInfo
GENERATOR    ?= Ninja

# Three build trees, because they check three different things and sharing one
# would mean reconfiguring between every pair of targets.
BUILD_DEFAULT = $(BUILD)/default
BUILD_SHARED  = $(BUILD)/shared
BUILD_CXX23   = $(BUILD)/cxx23

CONFIGURE = $(CMAKE) -S . -G $(GENERATOR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

ifneq ($(JOBS),)
  BUILD_FLAGS = --parallel $(JOBS)
endif

.DEFAULT_GOAL := help

# ---------------------------------------------------------------- building

.PHONY: build
build: ## Configure and build the static library, CLI and tests
	@$(CONFIGURE) -B $(BUILD_DEFAULT)
	@$(CMAKE) --build $(BUILD_DEFAULT) $(BUILD_FLAGS)

.PHONY: shared
shared: ## Build as a shared library -- what a C or ctypes consumer wants
	@$(CONFIGURE) -B $(BUILD_SHARED) -DBUILD_SHARED_LIBS=ON
	@$(CMAKE) --build $(BUILD_SHARED) $(BUILD_FLAGS)

# ----------------------------------------------------------------- testing

# The suite includes the install round-trips: `install`, `consumer-cxx` and
# `consumer-c` are ctest cases, so this one command covers the build tree and
# the installed package both.
.PHONY: test
test: build ## Build and run the conformance suite, install round-trips included
	@$(CTEST) --test-dir $(BUILD_DEFAULT) --output-on-failure

# The Python binding is ctypes over the C ABI, so it needs a shared library --
# there is nothing in a static archive for it to load. That is the only reason
# this is a separate target from `test`, and the `python` ctest case registers
# itself only in a shared build for the same reason.
.PHONY: python
python: shared ## Build shared and test the Python binding against it
	@$(CTEST) --test-dir $(BUILD_SHARED) --output-on-failure

# The other half of the `expected` switch.
#
# Below C++23 the library uses its vendored expected; at C++23 it uses
# std::expected, and that is a genuinely different type behind every signature
# in the API. Only one of the two is exercised by `test`, so without this the
# other branch compiles for exactly as long as nobody changes anything near it.
.PHONY: cxx23
cxx23: ## Build and test at C++23, where the API uses std::expected
	@$(CONFIGURE) -B $(BUILD_CXX23) -DSWEEPSFILE_CXX_STANDARD=23
	@$(CMAKE) --build $(BUILD_CXX23) $(BUILD_FLAGS)
	@$(CTEST) --test-dir $(BUILD_CXX23) --output-on-failure

.PHONY: check
check: test cxx23 python ## Everything: C++17, C++23, shared, bindings, install round-trips
	@echo "libsweepsfile: all checks passed"

# ------------------------------------------------------------------ using

.PHONY: install
install: build ## Install to PREFIX (default /usr/local)
	@$(CMAKE) --install $(BUILD_DEFAULT) $(if $(PREFIX),--prefix $(PREFIX),)

.PHONY: cli
cli: build ## Build and run the `sweeps` tool (pass ARGS="info file.sweeps")
	@$(BUILD_DEFAULT)/cli/sweeps $(ARGS)

# ------------------------------------------------------------------ upkeep

.PHONY: clean
clean: ## Remove the build trees
	@rm -rf $(BUILD)
	@echo "$(BUILD)/ removed"

.PHONY: help
help: ## Show this help
	@echo "libsweepsfile -- make targets"
	@echo ""
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-10s\033[0m %s\n", $$1, $$2}'
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE=<type>  RelWithDebInfo (default), Debug, Release"
	@echo "  GENERATOR=<name>   Ninja (default), \"Unix Makefiles\", ..."
	@echo "  PREFIX=<path>      install destination"
	@echo "  JOBS=<n>           parallel build jobs"
	@echo "  ARGS=\"...\"         arguments for the cli target"
