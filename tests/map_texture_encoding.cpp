#include <external/PNG2BLP/Png2Blp.h>
#include <QBuffer>
#include <QImage>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
  void require(bool ok, char const* message)
  { if (!ok) throw std::runtime_error(message); }

  QImage fixture(int size)
  {
    QImage image(size, size, QImage::Format_RGBA8888);
    std::uint32_t noise = 12345;
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x)
      {
        noise = noise * 1664525u + 1013904223u;
        auto* pixel = image.scanLine(y) + x * 4;
        pixel[0] = (x * 255 / size + (noise >> 27)) & 255;
        pixel[1] = (y * 255 / size + (noise >> 27)) & 255;
        pixel[2] = ((x + y) * 127 / size + (noise >> 27)) & 255;
        pixel[3] = (x / 8 + y / 8) % 3 ? 255 : 96;
      }
    return image;
  }

  QByteArray encode(QImage const& image, int format, bool pngRoundtrip, bool mipmaps = true)
  {
    Png2Blp blp;
    if (pngRoundtrip)
    {
      QByteArray png;
      QBuffer buffer(&png);
      require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "PNG encoding failed");
      blp.load(png.constData(), static_cast<uint32_t>(png.size()));
    }
    else
      blp.loadRgba(image.constBits(), image.sizeInBytes(), image.width(), image.height(), image.bytesPerLine());

    uint32_t size = 0;
    std::unique_ptr<void, decltype(&std::free)> result(blp.createBlpDxtInMemory(mipmaps, format, size), &std::free);
    require(result && size, "BLP encoding failed");
    return QByteArray(static_cast<char const*>(result.get()), size);
  }
}

int main(int argc, char** argv)
try
{
  for (auto const [size, format] : {std::pair{512, FORMAT_DXT5}, std::pair{256, FORMAT_DXT1}})
  {
    auto image = fixture(size);
    auto const expected = encode(image, format, true);
    require(encode(image, format, false) == expected, "Direct RGBA changed BLP pixels, alpha or mipmaps");
    require(encode(image, format, false, false) == encode(image, format, true, false), "Non-mipmapped BLP differs");

    // Padded rows exercise byte stride independently of QImage's usual packed RGBA.
    int const stride = size * 4 + 16;
    std::vector<uchar> padded(std::size_t(stride) * size, 0xCD);
    for (int y = 0; y < size; ++y)
      std::memcpy(padded.data() + std::size_t(y) * stride, image.constScanLine(y), size * 4);
    QImage paddedImage(padded.data(), size, size, stride, QImage::Format_RGBA8888);
    require(encode(paddedImage, format, false) == expected, "RGBA row padding changed BLP output");

    if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0)
    {
      // Alternate equivalent paths; fixture creation is outside timing. Two warmups, nine samples.
      for (int run = -2; run < 9; ++run)
        for (int path = 0; path < 2; ++path)
        {
          bool png = (run + path) % 2 == 0;
          auto const start = std::chrono::steady_clock::now();
          auto result = encode(image, format, png);
          auto const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
          require(result == expected, "Benchmark output differs");
          if (run >= 0)
            std::cout << size << ',' << (png ? "png" : "rgba") << ',' << run << ',' << ms << '\n';
        }
    }
  }

  Png2Blp invalid;
  std::vector<uchar> bytes(64);
  for (int test = 0; test < 4; ++test)
  {
    bool rejected = false;
    try
    {
      invalid.loadRgba(test == 0 ? nullptr : bytes.data(), test == 1 ? 3 : bytes.size(),
        test == 2 ? 0 : 4, 4, test == 3 ? 8 : 16);
    }
    catch (std::invalid_argument const&) { rejected = true; }
    require(rejected, "Invalid RGBA input was accepted");
  }
  return 0;
}
catch (std::exception const& error)
{
  std::cerr << error.what() << '\n';
  return 1;
}
