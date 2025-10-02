# Compiler and flags
CXX=g++
CXXFLAGS=-std=c++17 -pthread -Wall -Wextra -Wno-unused-parameter -g
LDFLAGS=-lcrypto

# Source files (all .cpp in the directory)
SRCS=tracker.cpp files.cpp user.cpp tracker_sync.cpp

# Object files
OBJS=$(SRCS:.cpp=.o)

# Target executable
TARGET=tracker

# Default rule
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files to object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build files
clean:
	rm -f $(OBJS) $(TARGET)
