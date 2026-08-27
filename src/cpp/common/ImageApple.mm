// ImageApple.mm — PNG/JPEG codecs for the Apple targets, via ImageIO
// SPDX-License-Identifier: GPL-3.0+
//
// [F4] libpng and libjpeg are not linked on iOS or the iPSX2 macOS build, so Image.cpp
// compiled its PNG and JPEG handlers out entirely and left WebP as the only format. That is
// not a niche gap: cover art accepts .png/.jpg/.jpeg/.webp and virtually every cover in the
// wild is PNG or JPEG, screenshots are written as PNG, every RetroAchievements image is a
// PNG, HD texture replacement packs are PNG, and FullscreenUI refuses to initialise at all
// because it cannot load fullscreenui/placeholder.png.
//
// ImageIO is part of the base system on both platforms, so this adds no third-party
// dependency and nothing for App Store review to weigh.
//
// Pixel format contract, matching the WebP path in Image.cpp: RGBA8Image holds u32 pixels
// whose bytes are R,G,B,A in memory, with STRAIGHT (non-premultiplied) alpha — that is what
// WebPDecodeRGBAInto produces and what upstream's libpng loader produces. CoreGraphics
// bitmap contexts cannot render straight alpha, so the loader draws premultiplied and
// converts back; the saver goes the other way.

#include "Image.h"
#include "Console.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	template <typename T>
	struct CFDeleter
	{
		void operator()(T ref) const
		{
			if (ref)
				CFRelease(ref);
		}
	};
	template <typename T>
	using CFPtr = std::unique_ptr<std::remove_pointer_t<T>, CFDeleter<T>>;

	// CoreGraphics only renders premultiplied alpha, so undo it to match the straight-alpha
	// convention the rest of the image code uses. Skips the common fully-opaque case.
	void UnpremultiplyInPlace(u32* pixels, size_t count)
	{
		for (size_t i = 0; i < count; i++)
		{
			u8* p = reinterpret_cast<u8*>(&pixels[i]);
			const u32 a = p[3];
			if (a == 255 || a == 0)
				continue;
			for (int c = 0; c < 3; c++)
				p[c] = static_cast<u8>((p[c] * 255 + a / 2) / a);
		}
	}

	bool DecodeImageSource(RGBA8Image* image, CGImageSourceRef source, const char* what)
	{
		if (!source)
		{
			Console.Error("ImageIO: could not create image source for %s", what);
			return false;
		}

		CFPtr<CGImageRef> cg(CGImageSourceCreateImageAtIndex(source, 0, nullptr));
		if (!cg)
		{
			Console.Error("ImageIO: could not decode %s", what);
			return false;
		}

		const size_t width = CGImageGetWidth(cg.get());
		const size_t height = CGImageGetHeight(cg.get());
		if (width == 0 || height == 0)
		{
			Console.Error("ImageIO: %s has zero extent", what);
			return false;
		}

		std::vector<u32> pixels;
		pixels.resize(width * height);

		// Fast path, and the only correct one when alpha varies. A CGBitmapContext cannot hold
		// straight alpha, so drawing through one premultiplies; recovering the original colour
		// afterwards divides by alpha and loses most of the precision where alpha is small (a
		// 256x256 sweep over every alpha value shows errors up to 127 per channel). When ImageIO
		// has already decoded to 8-bit straight-alpha RGBA we take its bytes untouched instead.
		if (CGImageGetBitsPerComponent(cg.get()) == 8 && CGImageGetBitsPerPixel(cg.get()) == 32)
		{
			const CGBitmapInfo info = CGImageGetBitmapInfo(cg.get());
			const CGImageAlphaInfo alpha = static_cast<CGImageAlphaInfo>(info & kCGBitmapAlphaInfoMask);
			const uint32_t order = info & kCGBitmapByteOrderMask;
			CGColorSpaceRef space = CGImageGetColorSpace(cg.get());
			const bool rgba_last = (alpha == kCGImageAlphaLast || alpha == kCGImageAlphaNoneSkipLast);
			const bool big_endian = (order == kCGBitmapByteOrderDefault || order == kCGBitmapByteOrder32Big);
			if (rgba_last && big_endian && space && CGColorSpaceGetModel(space) == kCGColorSpaceModelRGB)
			{
				CFPtr<CFDataRef> raw(CGDataProviderCopyData(CGImageGetDataProvider(cg.get())));
				const size_t stride = CGImageGetBytesPerRow(cg.get());
				if (raw && static_cast<size_t>(CFDataGetLength(raw.get())) >= stride * height &&
					stride >= width * sizeof(u32))
				{
					const u8* base = CFDataGetBytePtr(raw.get());
					for (size_t y = 0; y < height; y++)
						std::memcpy(&pixels[y * width], base + y * stride, width * sizeof(u32));

					if (alpha == kCGImageAlphaNoneSkipLast)
					{
						for (u32& px : pixels)
							reinterpret_cast<u8*>(&px)[3] = 255;
					}

					image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), std::move(pixels));
					return true;
				}
			}
		}

		CFPtr<CGColorSpaceRef> cs(CGColorSpaceCreateDeviceRGB());
		CFPtr<CGContextRef> ctx(CGBitmapContextCreate(pixels.data(), width, height, 8, width * sizeof(u32), cs.get(),
			kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
		if (!ctx)
		{
			Console.Error("ImageIO: could not create bitmap context for %s", what);
			return false;
		}

		CGContextSetBlendMode(ctx.get(), kCGBlendModeCopy);
		CGContextDrawImage(ctx.get(), CGRectMake(0.0, 0.0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)),
			cg.get());

		UnpremultiplyInPlace(pixels.data(), pixels.size());
		image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), std::move(pixels));
		return true;
	}

	// Shared encoder. `dest` is already pointed at its destination (a CFData or a file URL).
	bool EncodeTo(CGImageDestinationRef dest, const RGBA8Image& image, u8 quality, const char* what)
	{
		if (!dest)
		{
			Console.Error("ImageIO: could not create image destination for %s", what);
			return false;
		}

		// CGImage (unlike CGBitmapContext) accepts straight alpha, so hand our pixels over as-is
		// rather than premultiplying and making ImageIO undo it — that round trip loses precision
		// at low alpha for no reason.
		const size_t count = static_cast<size_t>(image.GetWidth()) * static_cast<size_t>(image.GetHeight());
		CFPtr<CGDataProviderRef> provider(
			CGDataProviderCreateWithData(nullptr, image.GetPixels(), count * sizeof(u32), nullptr));
		CFPtr<CGColorSpaceRef> cs(CGColorSpaceCreateDeviceRGB());
		CFPtr<CGImageRef> cg(CGImageCreate(image.GetWidth(), image.GetHeight(), 8, 32, image.GetWidth() * sizeof(u32),
			cs.get(), kCGImageAlphaLast | kCGBitmapByteOrder32Big, provider.get(), nullptr, false,
			kCGRenderingIntentDefault));
		if (!cg)
		{
			Console.Error("ImageIO: could not create CGImage for %s", what);
			return false;
		}

		const double q = static_cast<double>(quality) / 100.0;
		CFPtr<CFNumberRef> qnum(CFNumberCreate(nullptr, kCFNumberDoubleType, &q));
		const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
		const void* values[] = {qnum.get()};
		CFPtr<CFDictionaryRef> props(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks));

		CGImageDestinationAddImage(dest, cg.get(), props.get());
		if (!CGImageDestinationFinalize(dest))
		{
			Console.Error("ImageIO: could not finalize %s", what);
			return false;
		}
		return true;
	}

	bool ReadWholeFile(std::FILE* fp, std::vector<u8>* out)
	{
		if (std::fseek(fp, 0, SEEK_END) != 0)
			return false;
		const long size = std::ftell(fp);
		if (size < 0 || std::fseek(fp, 0, SEEK_SET) != 0)
			return false;
		out->resize(static_cast<size_t>(size));
		return out->empty() || std::fread(out->data(), out->size(), 1, fp) == 1;
	}

	bool BufferLoad(RGBA8Image* image, const void* buffer, size_t buffer_size, const char* what)
	{
		CFPtr<CFDataRef> data(
			CFDataCreateWithBytesNoCopy(nullptr, static_cast<const UInt8*>(buffer), buffer_size, kCFAllocatorNull));
		CFPtr<CGImageSourceRef> src(data ? CGImageSourceCreateWithData(data.get(), nullptr) : nullptr);
		return DecodeImageSource(image, src.get(), what);
	}

	bool FileLoad(RGBA8Image* image, std::FILE* fp, const char* what)
	{
		std::vector<u8> bytes;
		if (!ReadWholeFile(fp, &bytes) || bytes.empty())
		{
			Console.Error("ImageIO: could not read %s", what);
			return false;
		}
		return BufferLoad(image, bytes.data(), bytes.size(), what);
	}

	bool BufferSave(const RGBA8Image& image, std::vector<u8>* buffer, u8 quality, CFStringRef type, const char* what)
	{
		CFPtr<CFMutableDataRef> data(CFDataCreateMutable(nullptr, 0));
		CFPtr<CGImageDestinationRef> dest(
			data ? CGImageDestinationCreateWithData(data.get(), type, 1, nullptr) : nullptr);
		if (!EncodeTo(dest.get(), image, quality, what))
			return false;

		const CFIndex len = CFDataGetLength(data.get());
		buffer->resize(static_cast<size_t>(len));
		CFDataGetBytes(data.get(), CFRangeMake(0, len), buffer->data());
		return true;
	}

	bool FileSave(const RGBA8Image& image, std::FILE* fp, u8 quality, CFStringRef type, const char* what)
	{
		std::vector<u8> bytes;
		if (!BufferSave(image, &bytes, quality, type, what))
			return false;
		return bytes.empty() || std::fwrite(bytes.data(), bytes.size(), 1, fp) == 1;
	}
} // namespace

// Declared in Image.cpp under the Apple guard.
bool AppleImage_PNGBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size)
{
	return BufferLoad(image, buffer, buffer_size, "PNG buffer");
}
bool AppleImage_PNGFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp)
{
	return FileLoad(image, fp, filename);
}
bool AppleImage_PNGBufferSaver(const RGBA8Image& image, std::vector<u8>* buffer, u8 quality)
{
	return BufferSave(image, buffer, quality, CFSTR("public.png"), "PNG buffer");
}
bool AppleImage_PNGFileSaver(const RGBA8Image& image, const char* filename, std::FILE* fp, u8 quality)
{
	return FileSave(image, fp, quality, CFSTR("public.png"), filename);
}

bool AppleImage_JPEGBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size)
{
	return BufferLoad(image, buffer, buffer_size, "JPEG buffer");
}
bool AppleImage_JPEGFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp)
{
	return FileLoad(image, fp, filename);
}
bool AppleImage_JPEGBufferSaver(const RGBA8Image& image, std::vector<u8>* buffer, u8 quality)
{
	return BufferSave(image, buffer, quality, CFSTR("public.jpeg"), "JPEG buffer");
}
bool AppleImage_JPEGFileSaver(const RGBA8Image& image, const char* filename, std::FILE* fp, u8 quality)
{
	return FileSave(image, fp, quality, CFSTR("public.jpeg"), filename);
}
