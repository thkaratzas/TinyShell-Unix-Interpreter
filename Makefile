# Variables
CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++11
TARGET = tsh
SRC = src/tinyshell.cpp

# Default rule
all: $(TARGET)

# Link the executable
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

# Clean rule to remove the binary
clean:
	rm -f $(TARGET)
