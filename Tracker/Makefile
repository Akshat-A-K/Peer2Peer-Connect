CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread

SRCS = tracker.cpp user.cpp tracker_sync.cpp
OBJS = $(SRCS:.cpp=.o)

TARGET = tracker

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

%.o: %.cpp user.h tracker_sync.h
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJS) $(TARGET)
