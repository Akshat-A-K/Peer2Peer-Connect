CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread

SRCS = client.cpp
OBJS = $(SRCS:.cpp=.o)

TARGET = client

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJS) $(TARGET)
