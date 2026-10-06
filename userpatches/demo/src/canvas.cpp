// SPDX-License-Identifier: MIT
#include "canvas.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

void fill_rect(Canvas &c, int x, int y, int w, int h, uint32_t color)
{
	int x0 = std::max(x, 0), y0 = std::max(y, 0);
	int x1 = std::min(x + w, c.w), y1 = std::min(y + h, c.h);
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = c.px + size_t(yy) * c.stride;
		std::fill(row + x0, row + x1, color);
	}
}

void frame_rect(Canvas &c, int x, int y, int w, int h, int t, uint32_t color)
{
	fill_rect(c, x, y, w, t, color);
	fill_rect(c, x, y + h - t, w, t, color);
	fill_rect(c, x, y, t, h, color);
	fill_rect(c, x + w - t, y, t, h, color);
}

static uint32_t blend(uint32_t fg, uint32_t bg, unsigned a)
{
	unsigned r = ((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * (255 - a);
	unsigned g = ((fg >> 8) & 0xff) * a + ((bg >> 8) & 0xff) * (255 - a);
	unsigned b = (fg & 0xff) * a + (bg & 0xff) * (255 - a);
	return 0xff000000u | ((r / 255) << 16) | ((g / 255) << 8) | (b / 255);
}

static uint32_t next_cp(const std::string &s, size_t &i)
{
	unsigned char c = s[i++];
	if (c < 0x80)
		return c;
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
	uint32_t cp = c & (0x3f >> n);
	while (n-- && i < s.size())
		cp = (cp << 6) | (s[i++] & 0x3f);
	return cp;
}

bool Font::load(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	data_.assign(std::istreambuf_iterator<char>(f), {});
	auto *info = new stbtt_fontinfo;
	if (!stbtt_InitFont(info, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), 0))) {
		delete info;
		return false;
	}
	info_ = info;
	return true;
}

const Font::Glyph &Font::glyph(uint32_t cp, int size)
{
	uint64_t key = (uint64_t(cp) << 16) | uint64_t(size);
	auto it = cache_.find(key);
	if (it != cache_.end())
		return it->second;

	auto *info = static_cast<stbtt_fontinfo *>(info_);
	Glyph g;
	float scale = stbtt_ScaleForPixelHeight(info, size);
	int adv, lsb;
	stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
	g.advance = int(adv * scale + 0.5f);
	unsigned char *bm = stbtt_GetCodepointBitmap(info, 0, scale, cp, &g.w, &g.h, &g.xoff, &g.yoff);
	if (bm) {
		g.alpha.assign(bm, bm + size_t(g.w) * g.h);
		stbtt_FreeBitmap(bm, nullptr);
	}
	return cache_.emplace(key, std::move(g)).first->second;
}

int Font::ascent(int size)
{
	auto *info = static_cast<stbtt_fontinfo *>(info_);
	int asc, desc, gap;
	stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
	return int(asc * stbtt_ScaleForPixelHeight(info, size) + 0.5f);
}

int Font::width(int size, const std::string &s)
{
	if (!info_)
		return 0;
	int w = 0;
	for (size_t i = 0; i < s.size();)
		w += glyph(next_cp(s, i), size).advance;
	return w;
}

int Font::draw(Canvas &c, int x, int y, int size, uint32_t fg, uint32_t bg, const std::string &s)
{
	if (!info_)
		return 0;
	int x0 = x;
	for (size_t i = 0; i < s.size();) {
		const Glyph &g = glyph(next_cp(s, i), size);
		for (int gy = 0; gy < g.h; gy++) {
			int py = y + g.yoff + gy;
			if (py < 0 || py >= c.h)
				continue;
			uint32_t *row = c.px + size_t(py) * c.stride;
			const uint8_t *a = g.alpha.data() + size_t(gy) * g.w;
			for (int gx = 0; gx < g.w; gx++) {
				int px = x + g.xoff + gx;
				if (px < 0 || px >= c.w || !a[gx])
					continue;
				row[px] = a[gx] == 255 ? fg : blend(fg, bg, a[gx]);
			}
		}
		x += g.advance;
	}
	return x - x0;
}
