BUILD_DIR ?= tmp/build
CMAKE_ARGS ?=

.PHONY: build test lint package
build:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)
	cmake --build $(BUILD_DIR) --parallel 2

test: build
	dbus-run-session -- ctest --test-dir $(BUILD_DIR) --output-on-failure

lint:
	python3 tools/lint.py

package:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr $(CMAKE_ARGS)
	cmake --build $(BUILD_DIR) --parallel 2
	cpack -G DEB --config $(BUILD_DIR)/CPackConfig.cmake -B tmp/packages
