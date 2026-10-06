// SPDX-License-Identifier: MIT
// Software drawing into an XRGB8888 frame buffer. The display buffers are
// write-combined, so nothing here reads the destination: text is blended
// against a known background colour instead.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct Canvas {
	uint32_t *px = nullptr;
	int w = 0, h = 0;
	int stride = 0; // in pixels
};

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

void fill_rect(Canvas &c, int x, int y, int w, int h, uint32_t color);
void frame_rect(Canvas &c, int x, int y, int w, int h, int t, uint32_t color);

class Font {
public:
	bool load(const std::string &path);
	// Draw UTF-8 text with its baseline at y. Returns the advance in pixels.
	int draw(Canvas &c, int x, int y, int size, uint32_t fg, uint32_t bg, const std::string &s);
	int width(int size, const std::string &s);
	int ascent(int size);

private:
	struct Glyph {
		int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
		std::vector<uint8_t> alpha;
	};
	const Glyph &glyph(uint32_t cp, int size);

	std::vector<uint8_t> data_;
	void *info_ = nullptr; // stbtt_fontinfo
	std::unordered_map<uint64_t, Glyph> cache_;
};
