#!/bin/bash
# build.sh — compila o projeto sem precisar de make
set -e
cd "$(dirname "$0")"

echo "Compilando mandelbrot..."
g++ -O2 -std=c++17 -Wall -Wextra -march=native \
    -o mandelbrot main.cpp \
    -lSDL2 -lpthread -lm

echo "OK — binário gerado: ./mandelbrot"
echo ""
echo "Uso: ./mandelbrot <num_threads> <max_iter> <block_size>"
echo "Exemplo: ./mandelbrot 4 256 32"
