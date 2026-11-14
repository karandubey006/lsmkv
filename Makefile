# Thin wrapper over the CMake presets. `make test` is what CI and a fresh clone run.
.PHONY: test tsan debug release clean

# On macOS, prefer Homebrew's LLVM: the sanitizer runtimes in older Xcode
# toolchains abort at startup on newer macOS releases. An explicit CXX wins.
# (The compiler is cached on first configure; `make clean` to switch.)
BREW_CXX := /opt/homebrew/opt/llvm/bin/clang++
ifeq ($(shell uname -s),Darwin)
  ifneq ($(wildcard $(BREW_CXX)),)
    ifeq ($(origin CXX),default)
      export CXX := $(BREW_CXX)
    endif
  endif
endif

test:            ## ASan + UBSan build, all tests
	cmake --preset asan
	cmake --build --preset asan
	ctest --preset asan

tsan:            ## ThreadSanitizer build, all tests
	cmake --preset tsan
	cmake --build --preset tsan
	ctest --preset tsan

debug:
	cmake --preset debug
	cmake --build --preset debug
	ctest --preset debug

release:
	cmake --preset release
	cmake --build --preset release

clean:
	rm -rf build
