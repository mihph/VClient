CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
TARGET = vclient
SRCS = main.cpp interface.cpp config.cpp reader.cpp writer.cpp communicator.cpp auth.cpp client.cpp
OBJS = $(SRCS:.cpp=.o)
LIBS = -lboost_program_options -lcryptopp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET) -a 127.0.0.1 -p 33333 -i input.txt -o output.txt -c vclient.conf

.PHONY: all clean run
