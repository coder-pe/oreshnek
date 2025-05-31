# Makefile for Mini REST Framework
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -pthread -fPIC
DEBUG_FLAGS = -std=c++17 -Wall -Wextra -g -pthread -fPIC -DDEBUG

# Directories
SRC_DIR = src
BUILD_DIR = build
INCLUDE_DIR = include

# Source files
FRAMEWORK_SOURCES = $(SRC_DIR)/oreshnek.cpp $(SRC_DIR)/server.cpp
EXAMPLE_SOURCES = example.cpp

# Object files
FRAMEWORK_OBJECTS = $(FRAMEWORK_SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
EXAMPLE_OBJECTS = $(EXAMPLE_SOURCES:%.cpp=$(BUILD_DIR)/%.o)

# Targets
STATIC_LIB = $(BUILD_DIR)/liboreshnek.a
SHARED_LIB = $(BUILD_DIR)/liboreshnek.so
EXAMPLE_BIN = $(BUILD_DIR)/oreshnek_example

# Default target
all: directories $(STATIC_LIB) $(SHARED_LIB) $(EXAMPLE_BIN)

# Debug build
debug: CXXFLAGS = $(DEBUG_FLAGS)
debug: all

# Create build directories
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(SRC_DIR)
	@mkdir -p $(INCLUDE_DIR)

# Static library
$(STATIC_LIB): $(FRAMEWORK_OBJECTS)
	@echo "Creating static library..."
	ar rcs $@ $^

# Shared library
$(SHARED_LIB): $(FRAMEWORK_OBJECTS)
	@echo "Creating shared library..."
	$(CXX) -shared -o $@ $^

# Example executable
$(EXAMPLE_BIN): $(EXAMPLE_OBJECTS) $(STATIC_LIB)
	@echo "Building example..."
	$(CXX) $(CXXFLAGS) -o $@ $^

# Compile framework source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# Compile example
$(BUILD_DIR)/%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# Setup project structure
setup:
	@echo "Setting up project structure..."
	@mkdir -p $(SRC_DIR) $(INCLUDE_DIR) $(BUILD_DIR)
	@cp oreshnek.h $(INCLUDE_DIR)/
	@echo "// Implementation files" > $(SRC_DIR)/oreshnek.cpp
	@echo "#include \"../include/oreshnek.h\"" >> $(SRC_DIR)/oreshnek.cpp
	@echo "" >> $(SRC_DIR)/oreshnek.cpp
	@echo "// Add the implementation from oreshnek_impl.cpp here" >> $(SRC_DIR)/oreshnek.cpp
	@echo "" > $(SRC_DIR)/server.cpp
	@echo "#include \"../include/oreshnek.h\"" >> $(SRC_DIR)/server.cpp
	@echo "" >> $(SRC_DIR)/server.cpp
	@echo "// Add the server implementation here" >> $(SRC_DIR)/server.cpp
	@echo "Project structure created!"

# Performance test
benchmark: $(EXAMPLE_BIN)
	@echo "Running performance benchmark..."
	@echo "Starting server in background..."
	@./$(EXAMPLE_BIN) &
	@SERVER_PID=$$!; \
	sleep 2; \
	echo "Running Apache Bench test..."; \
	ab -n 10000 -c 100 http://localhost:8080/api/hello || echo "Apache Bench not installed"; \
	echo "Running curl test..."; \
	time curl -s http://localhost:8080/api/health > /dev/null; \
	kill $$SERVER_PID 2>/dev/null || true

# Load test with multiple endpoints
load_test: $(EXAMPLE_BIN)
	@echo "Running comprehensive load test..."
	@./$(EXAMPLE_BIN) &
	@SERVER_PID=$$!; \
	sleep 2; \
	echo "Testing GET endpoints..."; \
	curl -s http://localhost:8080/api/hello | head -c 100; echo; \
	curl -s http://localhost:8080/api/users | head -c 100; echo; \
	curl -s http://localhost:8080/api/user/123 | head -c 100; echo; \
	echo "Testing POST endpoint..."; \
	curl -s -X POST -H "Content-Type: application/json" \
		-d '{"name":"John Doe","email":"john@example.com"}' \
		http://localhost:8080/api/users | head -c 100; echo; \
	echo "Load test completed"; \
	kill $$SERVER_PID 2>/dev/null || true

# Memory analysis with Valgrind
memcheck: debug
	@echo "Running memory analysis..."
	valgrind --leak-check=full --show-leak-kinds=all \
		--track-origins=yes --verbose \
		./$(BUILD_DIR)/rest_server_example

# Performance profiling
profile: debug
	@echo "Running performance profiling..."
	perf record -g ./$(BUILD_DIR)/rest_server_example &
	@sleep 5
	@curl -s http://localhost:8080/api/hello > /dev/null
	@pkill rest_server_example
	perf report

# Install system-wide
install: $(STATIC_LIB) $(SHARED_LIB)
	@echo "Installing Mini REST Framework..."
	sudo cp $(INCLUDE_DIR)/oreshnek.h /usr/local/include/
	sudo cp $(STATIC_LIB) /usr/local/lib/
	sudo cp $(SHARED_LIB) /usr/local/lib/
	sudo ldconfig
	@echo "Installation completed!"

# Uninstall
uninstall:
	@echo "Uninstalling Mini REST Framework..."
	sudo rm -f /usr/local/include/oreshnek.h
	sudo rm -f /usr/local/lib/liboreshnek.a
	sudo rm -f /usr/local/lib/liboreshnek.so
	sudo ldconfig
	@echo "Uninstallation completed!"

# Documentation
docs:
	@echo "Generating documentation..."
	doxygen Doxyfile 2>/dev/null || echo "Doxygen not installed"

# Clean build files
clean:
	@echo "Cleaning build files..."
	rm -rf $(BUILD_DIR)/*

# Deep clean (including setup files)
distclean: clean
	@echo "Deep cleaning..."
	rm -rf $(BUILD_DIR)

# Help
help:
	@echo "Mini REST Framework - Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all        - Build static library, shared library, and example"
	@echo "  debug      - Build with debug symbols and no optimization"
	@echo "  setup      - Setup project directory structure"
	@echo "  benchmark  - Run performance benchmark with Apache Bench"
	@echo "  load_test  - Run comprehensive load test"
	@echo "  memcheck   - Run memory analysis with Valgrind"
	@echo "  profile    - Run performance profiling with perf"
	@echo "  install    - Install system-wide"
	@echo "  uninstall  - Remove system installation"
	@echo "  docs       - Generate documentation"
	@echo "  clean      - Clean build files"
	@echo "  distclean  - Deep clean all generated files"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make all           # Build everything"
	@echo "  make debug         # Build with debug info"
	@echo "  make benchmark     # Test performance"

# Check dependencies
check_deps:
	@echo "Checking build dependencies..."
	@which $(CXX) > /dev/null || echo "ERROR: C++ compiler not found"
	@which ar > /dev/null || echo "ERROR: ar (archiver) not found"
	@echo "Required dependencies check completed"

.PHONY: all debug directories setup benchmark load_test memcheck profile install uninstall docs clean distclean help check_deps
