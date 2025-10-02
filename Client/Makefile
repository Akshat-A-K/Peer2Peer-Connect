CXX=g++
CXXFLAGS=-std=c++17 -pthread -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -g
LDFLAGS=-lcrypto

SRCS=client.cpp
OBJS=$(SRCS:.cpp=.o)
TARGET=client

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJS) $(TARGET)
