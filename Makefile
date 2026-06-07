# Makefile — Fractal de Mandelbrot
# Compilação no WSL2 (Ubuntu) com g++ e SDL2

CXX      = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -march=native
LDFLAGS  = -lSDL2 -lpthread -lm

TARGET = mandelbrot
SRC    = main.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

# Executa com parâmetros padrão: 4 threads, 256 iterações, blocos 32×32
run: $(TARGET)
	./$(TARGET) --threads 4 --max-iter 256 --block-size 32

clean:
	rm -f $(TARGET)
