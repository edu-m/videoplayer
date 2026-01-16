#!/bin/sh

gcc -O2 -Wall main.c -o v4l2_sdl_view $(pkg-config --cflags --libs sdl3) -pthread
