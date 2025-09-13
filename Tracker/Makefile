CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread

SRCS = tracker.cpp user.cpp
OBJS = $(SRCS:.cpp=.o)

TARGET = tracker

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

%.o: %.cpp user.h
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJS) $(TARGET)
