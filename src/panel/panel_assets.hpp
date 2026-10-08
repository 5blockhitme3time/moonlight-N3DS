/*
 * The panel's font bytes. On the console they are compiled into the binary
 * (see the .ttf rule in the Makefile); the PC preview tool provides its own
 * definition that reads the same file from disk.
 */
#pragma once

#include <cstddef>
#include <cstdint>

const uint8_t *panel_font_data();
size_t panel_font_size();
