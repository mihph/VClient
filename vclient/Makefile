CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
TARGET = vclient
SRCS = main.cpp interface.cpp config.cpp reader.cpp writer.cpp communicator.cpp auth.cpp client.cpp
OBJS = $(SRCS:.cpp=.o)
LIBS = -lboost_program_options -lcryptopp

TEST_TARGET = test_main
TEST_SRCS = test_main.cpp interface.cpp config.cpp reader.cpp writer.cpp communicator.cpp auth.cpp client.cpp
TEST_OBJS = $(TEST_SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -pthread

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET) $(TEST_TARGET)

run: $(TARGET)
	./$(TARGET) -a 127.0.0.1 -p 33333 -i input.txt -o output.txt -c vclient.conf

.PHONY: all clean run test
