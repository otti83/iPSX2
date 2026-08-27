// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSPng.h"
#include "GSExtra.h"
#include "common/FileSystem.h"
#include <zlib.h>
#include <cstdio>
#include <vector>
#include <algorithm>
#if !TARGET_OS_IPHONE && !defined(iPSX2_MACOS)
#include <png.h>
#endif

namespace GSPng
{

#if !TARGET_OS_IPHONE && !defined(iPSX2_MACOS)
	bool SaveFile(const std::string& file, const Format fmt, const u8* const image,
		u8* const row, const int width, const int height, const int pitch,
		const int compression, const bool rb_swapped = false, const bool first_image = false)
	{
        // ... (existing code remains as is, will be wrapped)
#else
	// [iPSX2] Apple ビルドは libpng を張っていない (common/PNGStub.cpp) ため、
	// 上流の GS ダンプ (SaveRT / SaveTexture / SaveDepth / SaveAlpha / SaveFrame) が
	// **まるごと無効化されていた**。draw 単位で何が描かれたかを見る手段が無いので、
	// ライブラリ不要の 24bpp BMP で書き出すようにする。拡張子は .bmp になる。
	//
	// 除去条件: libpng を Apple ビルドにも張るなら不要。それまでは診断の生命線。
	static bool WriteBMP24(const std::string& file, const u8* image, int width, int height,
		int pitch, int bytes_per_pixel_in, bool rb_swapped, bool alpha_as_gray)
	{
		auto fp = FileSystem::OpenManagedCFile(file.c_str(), "wb");
		if (!fp)
			return false;

		const u32 row_bytes = ((static_cast<u32>(width) * 3u) + 3u) & ~3u;
		const u32 img_bytes = row_bytes * static_cast<u32>(height);
		u8 hdr[54] = {};
		auto put32 = [&hdr](u32 at, u32 v) {
			hdr[at] = u8(v); hdr[at + 1] = u8(v >> 8); hdr[at + 2] = u8(v >> 16); hdr[at + 3] = u8(v >> 24);
		};
		hdr[0] = 'B'; hdr[1] = 'M';
		put32(2, 54u + img_bytes);
		put32(10, 54);
		put32(14, 40);
		put32(18, static_cast<u32>(width));
		put32(22, static_cast<u32>(height));
		hdr[26] = 1; hdr[28] = 24;
		put32(34, img_bytes);
		if (std::fwrite(hdr, 1, sizeof(hdr), fp.get()) != sizeof(hdr))
			return false;

		std::vector<u8> line(row_bytes, 0);
		for (int y = height - 1; y >= 0; y--) // BMP は下から上
		{
			const u8* src = image + static_cast<size_t>(y) * static_cast<size_t>(pitch);
			for (int x = 0; x < width; x++)
			{
				const u8* p = src + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel_in);
				u8 r, g, b;
				if (alpha_as_gray)
				{
					// α (RGBA8 の 4 byte 目) か、1 byte 形式ならその値をグレースケールにする
					const u8 v = (bytes_per_pixel_in >= 4) ? p[3] : p[0];
					r = g = b = v;
				}
				else if (bytes_per_pixel_in >= 3)
				{
					r = p[0]; g = p[1]; b = p[2];
					if (rb_swapped)
						std::swap(r, b);
				}
				else
				{
					r = g = b = p[0];
				}
				line[static_cast<size_t>(x) * 3 + 0] = b; // BMP は BGR 順
				line[static_cast<size_t>(x) * 3 + 1] = g;
				line[static_cast<size_t>(x) * 3 + 2] = r;
			}
			if (std::fwrite(line.data(), 1, row_bytes, fp.get()) != row_bytes)
				return false;
		}
		return true;
	}

	bool SaveFile(const std::string& file, const Format fmt, const u8* const image,
		u8* const row, const int width, const int height, const int pitch,
		const int compression, const bool rb_swapped = false, const bool first_image = false)
	{
		// 呼び出し元は .png を期待するので、ここで .bmp に付け替える
		std::string out = file;
		const size_t dot = out.rfind('.');
		if (dot != std::string::npos)
			out.replace(dot, out.length() - dot, ".bmp");

		switch (fmt)
		{
			case Format::RGBA_PNG:
			case Format::RGB_PNG:
				return WriteBMP24(out, image, width, height, pitch, 4, rb_swapped, false);
			case Format::RGB_A_PNG:
				// 1 枚目 = 色、2 枚目 = α
				return WriteBMP24(out, image, width, height, pitch, 4, rb_swapped, !first_image);
			case Format::ALPHA_PNG:
				return WriteBMP24(out, image, width, height, pitch, 4, rb_swapped, true);
			case Format::R8I_PNG:
				return WriteBMP24(out, image, width, height, pitch, 1, false, false);
			default:
				// R16I / R32I (深度) はスケールの決め方が自明でないので出さない
				return false;
		}
	}
#endif

	bool Save(GSPng::Format fmt, const std::string& file, const u8* image, int w, int h, int pitch, int compression, bool rb_swapped)
	{
		std::string root = file;
		root.replace(file.length() - 4, 4, "");

		pxAssert(fmt >= Format::START && fmt < Format::COUNT);

		if (compression < 0 || compression > Z_BEST_COMPRESSION)
			compression = Z_BEST_SPEED;

#if !TARGET_OS_IPHONE && !defined(iPSX2_MACOS)
		std::unique_ptr<u8[]> row(new u8[pixel[fmt].bytes_per_pixel_out * w]);

		std::string filename = root + pixel[fmt].extension[0];
		if (!SaveFile(filename, fmt, image, row.get(), w, h, pitch, compression, rb_swapped, true))
			return false;

		// Second image
		if (pixel[fmt].extension[1] == nullptr)
			return true;

		filename = root + pixel[fmt].extension[1];
		return SaveFile(filename, fmt, image, row.get(), w, h, pitch, compression);
#else
		// [iPSX2] Apple: BMP で書く。1 枚目 = 本体、RGB_A_PNG のみ 2 枚目に α を出す。
		if (!SaveFile(root + ".png", fmt, image, nullptr, w, h, pitch, compression, rb_swapped, true))
			return false;
		if (fmt == Format::RGB_A_PNG)
			return SaveFile(root + "_alpha.png", fmt, image, nullptr, w, h, pitch, compression, rb_swapped, false);
		return true;
#endif
	}

	Transaction::Transaction(GSPng::Format fmt, const std::string& file, const u8* image, int w, int h, int pitch, int compression)
		: m_fmt(fmt), m_file(file), m_w(w), m_h(h), m_pitch(pitch), m_compression(compression)
	{
		// Note: yes it would be better to use shared pointer
		m_image = (u8*)_aligned_malloc(pitch * h, 32);
		if (m_image)
			memcpy(m_image, image, pitch * h);
	}

	Transaction::~Transaction()
	{
		if (m_image)
			_aligned_free(m_image);
	}

	void Process(std::shared_ptr<Transaction>& item)
	{
		Save(item->m_fmt, item->m_file, item->m_image, item->m_w, item->m_h, item->m_pitch, item->m_compression);
	}

} // namespace GSPng
