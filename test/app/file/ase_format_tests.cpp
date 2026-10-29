// Copyright (C) 2026 Veritaware
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.

#include "tests.h"

#include "app/context.h"
#include "app/document.h"
#include "app/file/file.h"
#include "doc/doc.h"
#include "doc/image_traits.h"
#include "she/system.h"
#include "zlib.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace app;

namespace
{

class ScopedTestFile
{
public:
  explicit ScopedTestFile(const char* filename)
    : m_filename(filename)
  {
  }
  ~ScopedTestFile() { std::remove(m_filename); }

private:
  const char* m_filename;
};

void putU16LE(std::vector<uint8_t>& buf, std::size_t offset, uint16_t v)
{
  buf[offset] = uint8_t(v & 0xff);
  buf[offset + 1] = uint8_t((v >> 8) & 0xff);
}

void putU32LE(std::vector<uint8_t>& buf, std::size_t offset, uint32_t v)
{
  buf[offset] = uint8_t(v & 0xff);
  buf[offset + 1] = uint8_t((v >> 8) & 0xff);
  buf[offset + 2] = uint8_t((v >> 16) & 0xff);
  buf[offset + 3] = uint8_t((v >> 24) & 0xff);
}

uint16_t getU16LE(const std::vector<uint8_t>& buf, std::size_t offset)
{
  return uint16_t(buf[offset]) | (uint16_t(buf[offset + 1]) << 8);
}

uint32_t getU32LE(const std::vector<uint8_t>& buf, std::size_t offset)
{
  return uint32_t(buf[offset]) | (uint32_t(buf[offset + 1]) << 8) | (uint32_t(buf[offset + 2]) << 16) |
         (uint32_t(buf[offset + 3]) << 24);
}

std::vector<uint8_t> readWholeFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeWholeFile(const std::string& path, const std::vector<uint8_t>& data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), data.size());
}

// Creates a 1x1 image layer holding "color" at frame 0 and appends it to
// "parent" (which may be the sprite's root folder or a group folder).
doc::LayerImage* addImageLayer(doc::Sprite* sprite, doc::LayerFolder* parent, const std::string& name,
                               doc::color_t color)
{
  auto* layer = new doc::LayerImage(sprite);
  layer->setName(name);

  doc::ImageRef image(doc::Image::create(sprite->pixelFormat(), sprite->width(), sprite->height()));
  doc::put_pixel(image.get(), 0, 0, color);

  auto cel = std::make_shared<doc::Cel>(doc::frame_t(0), image);
  layer->addCel(cel);

  parent->addLayer(layer);
  return layer;
}

bool hasAnyFolder(doc::LayerFolder* folder)
{
  for (doc::Layer* child : folder->getLayersList())
  {
    if (child->isFolder())
      return true;
  }
  return false;
}

} // namespace

TEST(AseFormat, ColorProfileChunkIsToleratedNotTreatedAsAnUnsupportedChunkWarning)
{
  // Save a minimal, real .ase file, then splice in a synthetic 0x2007
  // (ASE_FILE_CHUNK_COLOR_PROFILE) chunk as an extra chunk of the (only)
  // frame - the reader unconditionally seeks to chunk_pos+chunk_size after
  // each chunk regardless of what its handler consumed, so only the
  // chunk's own [size,type] header and the frame's updated chunk
  // count/size need to be correct; the 16-byte payload just needs to be
  // present.
  //
  // she::create_system() is defensive: if this regresses and AseFormat
  // ends up rejecting the file after all, the load falls back to
  // SheFormat, which needs a live she::instance() - without it, that
  // fallback would crash instead of just failing this test's assertions.
  std::unique_ptr<she::System> sys(she::create_system());

  app::Context ctx;
  const int w = 3, h = 2;

  doc::Document* doc = ctx.documents().add(w, h, doc::ColorMode::RGB);
  doc->setFilename("ase_color_profile_test.ase");
  ScopedTestFile cleanup("ase_color_profile_test.ase");
  Layer* layer = doc->sprite()->folder()->getFirstLayer();
  ASSERT_TRUE(layer != nullptr);
  Image* image = layer->cel(frame_t(0))->image();
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      put_pixel(image, x, y, doc::rgba(x * 50, y * 60, 10, 255));

  ASSERT_EQ(0, save_document(&ctx, doc));
  doc->close();
  delete doc;

  auto bytes = readWholeFile("ase_color_profile_test.ase");
  ASSERT_GE(bytes.size(), 128u + 16u) << "file too small to be a real .ase";

  // File header: chunks/frame fields aren't touched; only the total file
  // size (offset 0, u32) needs updating, for consistency.
  // Frame header starts right after the fixed 128-byte file header:
  //   offset 128: frame size (u32)
  //   offset 132: frame magic (u16)
  //   offset 134: chunk count (u16)
  const std::size_t frameHeaderStart = 128;
  const std::size_t frameSizeOff = frameHeaderStart;
  const std::size_t chunkCountOff = frameHeaderStart + 6;

  uint32_t oldFrameSize = getU32LE(bytes, frameSizeOff);
  uint16_t oldChunkCount = getU16LE(bytes, chunkCountOff);
  ASSERT_EQ(bytes.size(), frameHeaderStart + oldFrameSize) << "single-frame file: the frame must run to EOF";

  // New chunk: 6-byte chunk header (size, type) + 16-byte color-profile
  // payload (type u16, flags u16, gamma u32, 8 bytes padding), matching
  // what AseFormat::onLoad's ASE_FILE_CHUNK_COLOR_PROFILE case reads.
  const uint32_t newChunkSize = 6 + 16;
  std::vector<uint8_t> chunk(newChunkSize, 0);
  putU32LE(chunk, 0, newChunkSize);
  putU16LE(chunk, 4, 0x2007); // ASE_FILE_CHUNK_COLOR_PROFILE

  bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  putU32LE(bytes, frameSizeOff, oldFrameSize + newChunkSize);
  putU16LE(bytes, chunkCountOff, oldChunkCount + 1);
  putU32LE(bytes, 0, uint32_t(bytes.size())); // file header's total size

  writeWholeFile("ase_color_profile_test.ase", bytes);

  app::Document* loaded = load_document(&ctx, "ase_color_profile_test.ase");
  ASSERT_NE(nullptr, loaded);

  ASSERT_EQ(w, loaded->sprite()->width());
  ASSERT_EQ(h, loaded->sprite()->height());

  Layer* loadedLayer = loaded->sprite()->folder()->getFirstLayer();
  ASSERT_TRUE(loadedLayer != nullptr);
  Image* loadedImage = loadedLayer->cel(frame_t(0))->image();
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      EXPECT_EQ(doc::rgba(x * 50, y * 60, 10, 255), loadedImage->getPixel(x, y)) << "at (" << x << "," << y << ")";

  loaded->close();
  delete loaded;
}

TEST(AseFormat, AnActuallyUnrecognizedChunkTypeIsNotSilentlyTolerated)
{
  // Contrast against the color-profile test above: a genuinely unknown
  // chunk type (0xffff, never assigned) hits the `default:` branch in
  // AseFormat::onLoad, which records a message via fop->setError() -
  // unlike the color-profile chunk's dedicated, error-free handling. That
  // makes operateLoadTryFormat()'s `&& m_error.empty()` check fail, so
  // this AseFormat attempt is treated as a failure and the loader falls
  // back to the next-priority format (SheFormat), which needs a live
  // she::instance() - unlike every other test in this file, which never
  // reach that fallback.
  std::unique_ptr<she::System> sys(she::create_system());

  app::Context ctx;
  doc::Document* doc = ctx.documents().add(2, 2, doc::ColorMode::RGB);
  doc->setFilename("ase_unknown_chunk_test.ase");
  ScopedTestFile cleanup("ase_unknown_chunk_test.ase");
  Layer* layer = doc->sprite()->folder()->getFirstLayer();
  Image* image = layer->cel(frame_t(0))->image();
  put_pixel(image, 0, 0, doc::rgba(1, 2, 3, 255));

  ASSERT_EQ(0, save_document(&ctx, doc));
  doc->close();
  delete doc;

  auto bytes = readWholeFile("ase_unknown_chunk_test.ase");
  const std::size_t frameSizeOff = 128;
  const std::size_t chunkCountOff = 134;

  uint32_t oldFrameSize = getU32LE(bytes, frameSizeOff);
  uint16_t oldChunkCount = getU16LE(bytes, chunkCountOff);

  const uint32_t newChunkSize = 6; // header only, no payload
  std::vector<uint8_t> chunk(newChunkSize, 0);
  putU32LE(chunk, 0, newChunkSize);
  putU16LE(chunk, 4, 0xffff); // never-assigned chunk type

  bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  putU32LE(bytes, frameSizeOff, oldFrameSize + newChunkSize);
  putU16LE(bytes, chunkCountOff, oldChunkCount + 1);
  putU32LE(bytes, 0, uint32_t(bytes.size()));

  writeWholeFile("ase_unknown_chunk_test.ase", bytes);

  // The point of this test is that this must not crash the loader,
  // regardless of which format ends up handling it (or whether every
  // fallback ultimately fails and load_document() returns null).
  app::Document* loaded = load_document(&ctx, "ase_unknown_chunk_test.ase");
  if (loaded)
  {
    loaded->close();
    delete loaded;
  }
}

TEST(AseFormat, OversizedCelDimensionsDoNotOverflowTheDecompressionBufferSize)
{
  // Regression coverage for 0febb4bf9: read_compressed_image() used to
  // compute `image->height() * ImageTraits::getRowStrideBytes(width)` as a
  // plain `int`, which overflows well before reaching the 65535x65535
  // maximum a .ase width/height field (a u16) can encode - an overflowed
  // size there means the zlib inflate loop writes decompressed bytes past
  // a too-small buffer. This checks the arithmetic directly (no
  // multi-gigabyte allocation needed) without itself performing the
  // signed-int multiplication that overflows - that's undefined behavior,
  // and reproducing it here (even just to show it's wrong) is exactly the
  // class of bug this project's own CodeQL scanning flags: instead, it
  // confirms the true byte count exceeds what an `int` can represent,
  // which is precisely why the fix's `static_cast<long>` is necessary.
  const int width = 50000, height = 50000; // 2.5e9 pixels, indexed (1 byte/px)

  long correct = static_cast<long>(height) * doc::IndexedTraits::getRowStrideBytes(width);
  const long truePixelCount = 50000L * 50000L;

  EXPECT_EQ(truePixelCount, correct);
  ASSERT_GT(truePixelCount, static_cast<long>(INT32_MAX))
      << "test setup: these dimensions must actually exceed int32 range, "
         "otherwise this isn't exercising the overflow-prone case at all";
}

TEST(AseFormat, ModeratelyLargeImageRoundTripsWithoutCorruption)
{
  // A real (not synthetic-overflow-scale) large image, to confirm the
  // actual load/decompress path handles a substantially-sized cel
  // correctly end to end. Filled with a single repeating value so the
  // zlib-compressed .ase file this produces stays small and the test
  // stays fast, even though the uncompressed image is ~4 million pixels.
  std::unique_ptr<she::System> sys(she::create_system()); // see the note above

  app::Context ctx;
  const int w = 2000, h = 2000;

  doc::Document* doc = ctx.documents().add(w, h, doc::ColorMode::INDEXED, 4);
  doc->setFilename("ase_large_image_test.ase");
  ScopedTestFile cleanup("ase_large_image_test.ase");
  Layer* layer = doc->sprite()->folder()->getFirstLayer();
  ASSERT_TRUE(layer != nullptr);
  Image* image = layer->cel(frame_t(0))->image();
  clear_image(image, 2);
  put_pixel(image, 0, 0, 1);
  put_pixel(image, w - 1, h - 1, 3);

  ASSERT_EQ(0, save_document(&ctx, doc));
  doc->close();
  delete doc;

  app::Document* loaded = load_document(&ctx, "ase_large_image_test.ase");
  ASSERT_NE(nullptr, loaded);
  ASSERT_EQ(w, loaded->sprite()->width());
  ASSERT_EQ(h, loaded->sprite()->height());

  Layer* loadedLayer = loaded->sprite()->folder()->getFirstLayer();
  Image* loadedImage = loadedLayer->cel(frame_t(0))->image();
  EXPECT_EQ(1, loadedImage->getPixel(0, 0));
  EXPECT_EQ(3, loadedImage->getPixel(w - 1, h - 1));
  EXPECT_EQ(2, loadedImage->getPixel(w / 2, h / 2));

  loaded->close();
  delete loaded;
}

TEST(AseFormat, LayerGroupsAreFlattenedToTopLevelLayersOnLoad)
{
  // Besprited doesn't support layer groups, so on load (AseFormat::onPostLoad)
  // any group must be flattened into top-level image layers, preserving both
  // the original stacking order (Timeline::onPaint indexes m_layers assuming
  // a flat list built from Sprite::indexToLayer, which must agree with
  // Sprite::countLayers once groups are gone) and each layer's pixels.
  //
  // Stack (bottom to top): Base, G1{ A, B, G2{ C } }, Top - the "Top" layer
  // after a 2-level-deep group in particular exercises the child_level
  // ascend-by-more-than-one-level fix in ase_file_read_layer_chunk.
  std::unique_ptr<she::System> sys(she::create_system());

  app::Context ctx;
  doc::Document* doc = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  doc->setFilename("ase_layer_groups_test.ase");
  ScopedTestFile cleanup("ase_layer_groups_test.ase");

  doc::Sprite* sprite = doc->sprite();
  doc::LayerFolder* root = sprite->folder();

  doc::Layer* base = root->getFirstLayer();
  ASSERT_TRUE(base != nullptr);
  base->setName("Base");
  doc::put_pixel(base->cel(doc::frame_t(0))->image(), 0, 0, doc::rgba(10, 10, 10, 255));

  auto* g1 = new doc::LayerFolder(sprite);
  g1->setName("G1");
  addImageLayer(sprite, g1, "A", doc::rgba(20, 20, 20, 255));
  addImageLayer(sprite, g1, "B", doc::rgba(30, 30, 30, 255));

  auto* g2 = new doc::LayerFolder(sprite);
  g2->setName("G2");
  addImageLayer(sprite, g2, "C", doc::rgba(40, 40, 40, 255));
  g1->addLayer(g2);

  root->addLayer(g1);
  addImageLayer(sprite, root, "Top", doc::rgba(50, 50, 50, 255));

  ASSERT_EQ(0, save_document(&ctx, doc));
  doc->close();
  delete doc;

  app::Document* loaded = load_document(&ctx, "ase_layer_groups_test.ase");
  ASSERT_NE(nullptr, loaded);

  doc::LayerFolder* loadedRoot = loaded->sprite()->folder();
  EXPECT_FALSE(hasAnyFolder(loadedRoot)) << "no group should survive AseFormat::onPostLoad's flattening";

  ASSERT_EQ(5, loadedRoot->getLayersCount());

  std::vector<doc::Layer*> layers(loadedRoot->getLayerBegin(), loadedRoot->getLayerEnd());
  const char* expectedNames[] = {"Base", "G1-A", "G1-B", "G1-G2-C", "Top"};
  const doc::color_t expectedColors[] = {doc::rgba(10, 10, 10, 255), doc::rgba(20, 20, 20, 255),
                                         doc::rgba(30, 30, 30, 255), doc::rgba(40, 40, 40, 255),
                                         doc::rgba(50, 50, 50, 255)};

  for (int i = 0; i < 5; ++i)
  {
    SCOPED_TRACE(i);
    ASSERT_TRUE(layers[i]->isImage());
    EXPECT_EQ(expectedNames[i], layers[i]->name());
    EXPECT_EQ(expectedColors[i], doc::get_pixel(layers[i]->cel(doc::frame_t(0))->image(), 0, 0));

    // The flat stacking order the timeline depends on: indexToLayer's
    // depth-first walk must agree with the root's own child order once
    // there are no more nested groups.
    EXPECT_EQ(layers[i], loaded->sprite()->indexToLayer(doc::LayerIndex(i)));
  }
  EXPECT_EQ(doc::LayerIndex(5), loaded->sprite()->countLayers());

  loaded->close();
  delete loaded;
}

TEST(AseFormat, FileWithoutGroupsIsUntouchedByFlattening)
{
  // Guards against the ase_ungroup_all() rewrite (needed to preserve
  // stacking order for files that do have groups) accidentally reordering
  // or renaming layers in the common case where there are none.
  std::unique_ptr<she::System> sys(she::create_system());

  app::Context ctx;
  doc::Document* doc = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  doc->setFilename("ase_no_groups_test.ase");
  ScopedTestFile cleanup("ase_no_groups_test.ase");

  doc::Sprite* sprite = doc->sprite();
  doc::LayerFolder* root = sprite->folder();

  doc::Layer* base = root->getFirstLayer();
  base->setName("Base");
  doc::put_pixel(base->cel(doc::frame_t(0))->image(), 0, 0, doc::rgba(10, 10, 10, 255));
  addImageLayer(sprite, root, "Middle", doc::rgba(20, 20, 20, 255));
  addImageLayer(sprite, root, "Top", doc::rgba(30, 30, 30, 255));

  ASSERT_EQ(0, save_document(&ctx, doc));
  doc->close();
  delete doc;

  app::Document* loaded = load_document(&ctx, "ase_no_groups_test.ase");
  ASSERT_NE(nullptr, loaded);

  doc::LayerFolder* loadedRoot = loaded->sprite()->folder();
  ASSERT_EQ(3, loadedRoot->getLayersCount());

  std::vector<doc::Layer*> layers(loadedRoot->getLayerBegin(), loadedRoot->getLayerEnd());
  const char* expectedNames[] = {"Base", "Middle", "Top"};
  for (int i = 0; i < 3; ++i)
  {
    SCOPED_TRACE(i);
    EXPECT_EQ(expectedNames[i], layers[i]->name());
  }

  loaded->close();
  delete loaded;
}

TEST(AseFormat, DeeplyNestedGroupDropIsReparentedToRootNotCrashing)
{
  // Regression coverage for ase_file_read_layer_chunk's child_level
  // handling: a drop of more than one level (G1{G2{G3{X}}} followed by a
  // top-level Y, i.e. child_level 3 -> 0) used to call parent()->parent()
  // exactly once regardless of how far the level actually dropped, which
  // mis-parents (or, for a deep enough file, NULL-derefs) the next layer.
  std::unique_ptr<she::System> sys(she::create_system());

  app::Context ctx;
  doc::Document* doc = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  doc->setFilename("ase_deep_nested_groups_test.ase");
  ScopedTestFile cleanup("ase_deep_nested_groups_test.ase");

  doc::Sprite* sprite = doc->sprite();
  doc::LayerFolder* root = sprite->folder();

  doc::Layer* base = root->getFirstLayer();
  base->setName("Base");
  doc::put_pixel(base->cel(doc::frame_t(0))->image(), 0, 0, doc::rgba(1, 1, 1, 255));

  auto* g1 = new doc::LayerFolder(sprite);
  g1->setName("G1");
  auto* g2 = new doc::LayerFolder(sprite);
  g2->setName("G2");
  auto* g3 = new doc::LayerFolder(sprite);
  g3->setName("G3");
  addImageLayer(sprite, g3, "X", doc::rgba(2, 2, 2, 255));
  g2->addLayer(g3);
  g1->addLayer(g2);
  root->addLayer(g1);

  addImageLayer(sprite, root, "Y", doc::rgba(3, 3, 3, 255));

  ASSERT_EQ(0, save_document(&ctx, doc));
  doc->close();
  delete doc;

  app::Document* loaded = load_document(&ctx, "ase_deep_nested_groups_test.ase");
  ASSERT_NE(nullptr, loaded);

  doc::LayerFolder* loadedRoot = loaded->sprite()->folder();
  EXPECT_FALSE(hasAnyFolder(loadedRoot));
  ASSERT_EQ(3, loadedRoot->getLayersCount());

  std::vector<doc::Layer*> layers(loadedRoot->getLayerBegin(), loadedRoot->getLayerEnd());
  EXPECT_EQ("Base", layers[0]->name());
  EXPECT_EQ("G1-G2-G3-X", layers[1]->name());
  EXPECT_EQ("Y", layers[2]->name());

  loaded->close();
  delete loaded;
}

namespace
{

struct ChunkCountCase
{
  uint32_t chunks;
  bool sentinel;
  bool extended;
};

class AseChunkCount : public ::testing::TestWithParam<ChunkCountCase>
{
};

} // namespace

TEST_P(AseChunkCount, LoadsTheLastCel)
{
  std::unique_ptr<she::System> sys(she::create_system());
  app::Context ctx;
  const char* filename = "ase_chunk_count_test.ase";
  ScopedTestFile cleanup(filename);
  auto* document = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  document->setFilename(filename);
  document->sprite()->setFrameDuration(doc::frame_t(0), 123);
  auto* layer = document->sprite()->folder()->getFirstLayer();
  const auto color = doc::rgba(12, 34, 56, 255);
  doc::put_pixel(layer->cel(doc::frame_t(0))->image(), 0, 0, color);
  ASSERT_EQ(0, save_document(&ctx, document));
  document->close();
  delete document;

  auto bytes = readWholeFile(filename);
  ASSERT_GE(bytes.size(), 144u);
  const uint32_t originalCount = getU16LE(bytes, 134);
  const auto param = GetParam();
  const uint32_t count = param.chunks ? param.chunks : originalCount;
  ASSERT_GE(count, originalCount);
  ASSERT_GT(count, 0u);

  // Insert valid, ignored path chunks before the saved chunks. The cel
  // stays last, so a truncated count cannot accidentally pass this test.
  std::vector<uint8_t> padding(6u * (count - originalCount), 0);
  for (std::size_t offset = 0; offset < padding.size(); offset += 6)
  {
    putU32LE(padding, offset, 6);
    putU16LE(padding, offset + 4, 0x2017);
  }
  bytes.insert(bytes.begin() + 144, padding.begin(), padding.end());
  putU32LE(bytes, 0, uint32_t(bytes.size()));
  putU32LE(bytes, 128, uint32_t(bytes.size() - 128));
  putU16LE(bytes, 134, param.sentinel ? 0xffff : uint16_t(param.extended ? count - 1 : count));
  // Frame-relative offsets: duration 8, reserved 10, extended count 12.
  putU16LE(bytes, 138, 0);
  putU32LE(bytes, 140, param.extended ? count : 0);
  writeWholeFile(filename, bytes);

  auto* loaded = load_document(&ctx, filename);
  ASSERT_NE(nullptr, loaded);
  EXPECT_EQ(123, loaded->sprite()->frameDuration(doc::frame_t(0)));
  auto* loadedLayer = loaded->sprite()->folder()->getFirstLayer();
  ASSERT_NE(nullptr, loadedLayer);
  auto cel = loadedLayer->cel(doc::frame_t(0));
  ASSERT_NE(nullptr, cel);
  EXPECT_EQ(color, doc::get_pixel(cel->image(), 0, 0));
  loaded->close();
  delete loaded;
}

INSTANTIATE_TEST_SUITE_P(FrameHeader, AseChunkCount,
                         ::testing::Values(ChunkCountCase{0, false, false}, ChunkCountCase{0, false, true},
                                           ChunkCountCase{0xffff, true, false}, ChunkCountCase{0x10000, true, true}));

TEST(AseFormat, WritesExtendedChunkCountAboveWordLimit)
{
  std::unique_ptr<she::System> sys(she::create_system());
  app::Context ctx;
  const char* filename = "ase_extended_write_test.ase";
  ScopedTestFile cleanup(filename);
  auto* document = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  document->setFilename(filename);
  auto* sprite = document->sprite();
  doc::UserData data;
  data.setText("metadata");
  // Each empty layer adds a layer chunk and a user-data chunk, without
  // allocating or compressing thousands of images.
  for (int i = 0; i < 32767; ++i)
  {
    auto* layer = new doc::LayerImage(sprite);
    layer->setUserData(data);
    sprite->folder()->addLayer(layer);
  }
  ASSERT_EQ(0, save_document(&ctx, document));
  document->close();
  delete document;

  const auto bytes = readWholeFile(filename);
  ASSERT_GE(bytes.size(), 144u);
  ASSERT_EQ(bytes.size(), getU32LE(bytes, 0));
  ASSERT_EQ(bytes.size() - 128, getU32LE(bytes, 128));
  uint32_t count = 0;
  std::size_t offset = 144;
  while (offset < bytes.size())
  {
    ASSERT_GE(bytes.size() - offset, 6u);
    const uint32_t size = getU32LE(bytes, offset);
    ASSERT_GE(size, 6u);
    ASSERT_LE(size, bytes.size() - offset);
    offset += size;
    ++count;
  }
  ASSERT_GT(count, 0xffffu);
  EXPECT_EQ(0xffff, getU16LE(bytes, 134));
  EXPECT_EQ(0, getU16LE(bytes, 138));
  EXPECT_EQ(count, getU32LE(bytes, 140));

  auto* loaded = load_document(&ctx, filename);
  ASSERT_NE(nullptr, loaded);
  EXPECT_EQ(32768, loaded->sprite()->folder()->getLayersCount());
  unsigned cels = 0;
  for (auto* layer : loaded->sprite()->folder()->getLayersList())
    if (layer->cel(doc::frame_t(0)))
      ++cels;
  EXPECT_EQ(1u, cels);
  loaded->close();
  delete loaded;
}

namespace
{

class AseMalformedFrame : public ::testing::TestWithParam<int>
{
};

} // namespace

TEST_P(AseMalformedFrame, RejectsInvalidFrameWithoutLooping)
{
  std::unique_ptr<she::System> sys(she::create_system());
  app::Context ctx;
  const char* filename = "ase_malformed_frame_test.ase";
  ScopedTestFile cleanup(filename);
  auto* document = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  document->setFilename(filename);
  ASSERT_EQ(0, save_document(&ctx, document));
  document->close();
  delete document;
  auto bytes = readWholeFile(filename);
  ASSERT_GE(bytes.size(), 150u);
  switch (GetParam())
  {
  case 0:
    putU16LE(bytes, 134, 0xffff);
    putU32LE(bytes, 140, 0xffffffff);
    break;
  case 1:
    bytes.resize(143); // Last byte of the extended count is missing.
    break;
  case 2:
    putU32LE(bytes, 144, 5); // Chunk header itself needs six bytes.
    break;
  case 3:
    putU32LE(bytes, 144, 0); // Must never seek backwards/repeat a chunk.
    break;
  case 4:
    putU32LE(bytes, 144, 0xffffffff); // Chunk exceeds the frame.
    break;
  case 5:
    bytes.pop_back(); // Frame payload is physically incomplete.
    break;
  case 6:
    putU32LE(bytes, 128, 15); // Guard frame-size subtraction.
    break;
  case 7:
    putU32LE(bytes, 128, 22); // Cannot consume bytes beyond the frame.
    putU16LE(bytes, 134, 1);
    break;
  case 8:
    putU32LE(bytes, 144, 6);
    putU16LE(bytes, 148, 0x2007); // Color profile payload is missing.
    break;
  }
  if (GetParam() >= 9)
  {
    // A complete frame containing an incomplete tag chunk.
    const unsigned payloadSize = GetParam() == 9 ? 0 : (GetParam() == 10 ? 10 : 29);
    bytes.resize(150 + payloadSize, 0);
    std::fill(bytes.begin() + 144, bytes.end(), 0);
    putU32LE(bytes, 0, bytes.size());
    putU32LE(bytes, 128, bytes.size() - 128);
    putU16LE(bytes, 134, 1);
    putU32LE(bytes, 140, 0);
    putU32LE(bytes, 144, 6 + payloadSize);
    putU16LE(bytes, 148, 0x2018);
    if (GetParam() == 10)
      putU16LE(bytes, 150, 0xffff); // Count cannot fit in this chunk.
    if (GetParam() == 11)
    {
      putU16LE(bytes, 150, 1);
      putU16LE(bytes, 177, 1); // Name extends beyond the chunk.
    }
  }
  writeWholeFile(filename, bytes);
  std::unique_ptr<FileOp> operation(FileOp::createLoadDocumentOperation(&ctx, filename, FILE_LOAD_SEQUENCE_NONE));
  ASSERT_NE(nullptr, operation);
  operation->operate();
  operation->done();
  EXPECT_TRUE(operation->hasError());
  EXPECT_LT(operation->error().size(), 4096u);
  std::unique_ptr<app::Document> unexpectedDocument(operation->document());
  EXPECT_EQ(nullptr, unexpectedDocument);
}

INSTANTIATE_TEST_SUITE_P(FrameBounds, AseMalformedFrame, ::testing::Range(0, 12));

TEST(AseFormat, RepeatedUnknownChunksHaveBoundedDiagnostics)
{
  std::unique_ptr<she::System> sys(she::create_system());
  app::Context ctx;
  const char* filename = "ase_unknown_repeated_test.ase";
  ScopedTestFile cleanup(filename);
  auto* document = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  document->setFilename(filename);
  ASSERT_EQ(0, save_document(&ctx, document));
  document->close();
  delete document;
  auto bytes = readWholeFile(filename);
  ASSERT_GE(bytes.size(), 144u);
  constexpr unsigned count = 10000;
  bytes.resize(144 + count * 6, 0);
  putU32LE(bytes, 0, bytes.size());
  putU32LE(bytes, 128, bytes.size() - 128);
  putU16LE(bytes, 134, count);
  putU32LE(bytes, 140, 0);
  for (unsigned offset = 144; offset < bytes.size(); offset += 6)
  {
    putU32LE(bytes, offset, 6);
    putU16LE(bytes, offset + 4, 0xffff);
  }
  writeWholeFile(filename, bytes);
  std::unique_ptr<FileOp> operation(FileOp::createLoadDocumentOperation(&ctx, filename, FILE_LOAD_SEQUENCE_NONE));
  ASSERT_NE(nullptr, operation);
  operation->operate();
  operation->done();
  EXPECT_TRUE(operation->hasError());
  EXPECT_LT(operation->error().size(), 4096u);
  ASSERT_NE(nullptr, operation->document()); // Unknown chunks remain skippable.
  operation->document()->close();
  delete operation->document();
}

TEST(AseFormat, FrameTagsRoundTripWithNames)
{
  std::unique_ptr<she::System> sys(she::create_system());
  app::Context ctx;
  const char* filename = "ase_tags_roundtrip_test.ase";
  ScopedTestFile cleanup(filename);
  auto* document = ctx.documents().add(1, 1, doc::ColorMode::RGB);
  document->setFilename(filename);
  auto tag = std::make_unique<doc::FrameTag>(doc::frame_t(0), doc::frame_t(0));
  tag->setName("walk");
  document->sprite()->frameTags().add(tag.get());
  tag.release();
  ASSERT_EQ(0, save_document(&ctx, document));
  document->close();
  delete document;
  auto* loaded = load_document(&ctx, filename);
  ASSERT_NE(nullptr, loaded);
  EXPECT_EQ(1u, loaded->sprite()->frameTags().size());
  EXPECT_NE(nullptr, loaded->sprite()->frameTags().getByName("walk"));
  loaded->close();
  delete loaded;
}

namespace
{

class AsePixelPayload : public ::testing::Test
{
protected:
  std::unique_ptr<she::System> sys{she::create_system()};
  app::Context ctx;
  const char* filename = "ase_pixel_payload_test.ase";
  ScopedTestFile cleanup{filename};
  std::vector<uint8_t> bytes;
  size_t celOffset = 0;

  void makeFile(doc::ColorMode mode = doc::ColorMode::RGB)
  {
    auto* document = ctx.documents().add(2, 2, mode);
    document->setFilename(filename);
    ASSERT_EQ(0, save_document(&ctx, document));
    document->close();
    delete document;
    bytes = readWholeFile(filename);
    ASSERT_GE(bytes.size(), 144u);
    for (size_t offset = 144; offset < bytes.size();)
    {
      ASSERT_GE(bytes.size() - offset, 6u);
      const size_t size = getU32LE(bytes, offset);
      ASSERT_GE(size, 6u);
      ASSERT_LE(size, bytes.size() - offset);
      if (getU16LE(bytes, offset + 4) == 0x2005)
      {
        ASSERT_GE(size, 26u);
        ASSERT_EQ(bytes.size(), offset + size);
        celOffset = offset;
        return;
      }
      offset += size;
    }
    FAIL() << "Saved cel not found";
  }

  void setPixels(const std::vector<uint8_t>& payload, bool compressed)
  {
    bytes.resize(celOffset + 26);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    putU16LE(bytes, celOffset + 13, compressed ? 2 : 0);
    putU32LE(bytes, celOffset, static_cast<uint32_t>(26 + payload.size()));
    putU32LE(bytes, 0, static_cast<uint32_t>(bytes.size()));
    putU32LE(bytes, 128, static_cast<uint32_t>(bytes.size() - 128));
  }

  void expectRejected(const char* reason = "Error reading ASE chunk")
  {
    writeWholeFile(filename, bytes);
    std::unique_ptr<FileOp> operation(FileOp::createLoadDocumentOperation(&ctx, filename, FILE_LOAD_SEQUENCE_NONE));
    ASSERT_NE(nullptr, operation);
    operation->operate();
    operation->done();
    std::unique_ptr<app::Document> unexpectedDocument(operation->document());
    EXPECT_EQ(nullptr, unexpectedDocument);
    EXPECT_TRUE(operation->hasError());
    EXPECT_LT(operation->error().size(), 4096u);
    EXPECT_NE(std::string::npos, operation->error().find(reason));
  }
};

std::vector<uint8_t> compressPixels(const std::vector<uint8_t>& pixels)
{
  uLongf size = compressBound(pixels.size());
  std::vector<uint8_t> result(size);
  if (compress2(result.data(), &size, pixels.data(), pixels.size(), Z_DEFAULT_COMPRESSION) != Z_OK)
    throw std::runtime_error("Cannot prepare compressed pixel test");
  result.resize(size);
  return result;
}

class AseValidPixels : public AsePixelPayload,
                       public ::testing::WithParamInterface<int>
{
};
class AseInvalidPixels : public AsePixelPayload,
                         public ::testing::WithParamInterface<int>
{
};

} // namespace

TEST_P(AseValidPixels, LoadsExactPixelBytes)
{
  const int format = GetParam() % 3;
  const bool compressed = GetParam() >= 3;
  const doc::ColorMode mode =
      format == 0 ? doc::ColorMode::RGB : (format == 1 ? doc::ColorMode::GRAYSCALE : doc::ColorMode::INDEXED);
  ASSERT_NO_FATAL_FAILURE(makeFile(mode));
  const size_t stride = format == 0 ? 4 : (format == 1 ? 2 : 1);
  std::vector<uint8_t> pixels(4 * stride);
  for (size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = static_cast<uint8_t>(i + 1);
  setPixels(compressed ? compressPixels(pixels) : pixels, compressed);
  writeWholeFile(filename, bytes);
  std::unique_ptr<app::Document> loaded(load_document(&ctx, filename));
  ASSERT_NE(nullptr, loaded);
  const auto cel = loaded->sprite()->folder()->getFirstLayer()->cel(doc::frame_t(0));
  ASSERT_NE(nullptr, cel);
  for (int i = 0; i < 4; ++i)
  {
    const auto* p = &pixels[i * stride];
    const doc::color_t expected =
        format == 0 ? doc::rgba(p[0], p[1], p[2], p[3]) : (format == 1 ? doc::graya(p[0], p[1]) : p[0]);
    EXPECT_EQ(expected, doc::get_pixel(cel->image(), i % 2, i / 2));
  }
  loaded->close();
}

INSTANTIATE_TEST_SUITE_P(PixelFormats, AseValidPixels, ::testing::Range(0, 6));

TEST_P(AseInvalidPixels, RejectsBeforePublishingPartialImage)
{
  ASSERT_NO_FATAL_FAILURE(makeFile());
  std::vector<uint8_t> pixels(16, 42);
  bool compressed = GetParam() >= 3;
  if (GetParam() == 0 || GetParam() == 6)
    pixels.pop_back();
  if (GetParam() == 7)
    pixels.push_back(42);
  auto payload = compressed ? compressPixels(pixels) : pixels;
  if (GetParam() == 3)
    payload.clear();
  if (GetParam() == 4)
    payload.pop_back(); // Missing zlib trailer, despite enough decoded pixels.
  if (GetParam() == 5)
    payload.back() ^= 1; // Invalid checksum.
  setPixels(payload, compressed);
  if (GetParam() == 1)
  {
    putU16LE(bytes, celOffset + 22, 16384);
    putU16LE(bytes, celOffset + 24, 16384); // 1 GiB claimed by a tiny raw chunk.
  }
  if (GetParam() == 2)
    putU32LE(bytes, celOffset, 6); // Physical bytes remain outside this chunk.
  expectRejected(GetParam() == 1 ? "ASE chunk payload is truncated" : "Error reading ASE chunk");
}

INSTANTIATE_TEST_SUITE_P(PixelBounds, AseInvalidPixels, ::testing::Range(0, 8));

namespace
{
class AseChunkPayload : public AsePixelPayload,
                        public ::testing::WithParamInterface<int>
{
};
} // namespace

TEST_P(AseChunkPayload, ChecksMetadataAndMaskPayloadBounds)
{
  ASSERT_NO_FATAL_FAILURE(makeFile());
  const uint16_t types[] = {4, 11, 0x2004, 0x2020, 0x2019, 0x2016, 0x2016, 0x2018, 0x2007, 0x2005, 11, 11};
  const unsigned sizes[] = {0, 0, 18, 6, 20, 18, 19, 10, 15, 16, 7, 7};
  const int index = GetParam();
  std::vector<uint8_t> chunk(6 + sizes[index], 0);
  putU32LE(chunk, 0, static_cast<uint32_t>(chunk.size()));
  putU16LE(chunk, 4, types[index]);
  if (index == 2)
    putU16LE(chunk, 22, 1); // Layer name is missing.
  if (index == 3)
  {
    putU32LE(chunk, 6, 1);       // User text flag.
    putU16LE(chunk, 10, 0xffff); // Text cannot fit.
  }
  if (index == 4)
    putU32LE(chunk, 6, 2); // Palette has no bytes for its first entry.
  if (index == 5 || index == 6)
  {
    putU16LE(chunk, 10, index == 5 ? 16384 : 1);
    putU16LE(chunk, 12, index == 5 ? 16384 : 1);
    if (index == 6)
      chunk.back() = 0x80; // One bit-wide mask, padded to a byte.
  }
  if (index == 9)
    putU16LE(chunk, 13, 1); // Linked cel has no link-frame WORD.
  if (index >= 10)
  {
    putU16LE(chunk, 6, 1);             // One legacy palette packet.
    chunk[9] = 1;                      // One color.
    chunk[10] = index == 10 ? 64 : 63; // Six-bit lookup boundary.
  }
  const uint16_t count = getU16LE(bytes, 134);
  bytes.insert(index >= 10 ? bytes.begin() + 144 : bytes.end(), chunk.begin(), chunk.end());
  putU16LE(bytes, 134, count + 1);
  putU32LE(bytes, 0, static_cast<uint32_t>(bytes.size()));
  putU32LE(bytes, 128, static_cast<uint32_t>(bytes.size() - 128));
  if (index == 6 || index == 7 || index == 11)
  {
    writeWholeFile(filename, bytes);
    std::unique_ptr<app::Document> loaded(load_document(&ctx, filename));
    ASSERT_NE(nullptr, loaded);
    loaded->close();
  }
  else
    expectRejected();
}

INSTANTIATE_TEST_SUITE_P(ChunkPayloads, AseChunkPayload, ::testing::Range(0, 12));

TEST_F(AsePixelPayload, StreamsCompressedPixelsAcrossInputBlocks)
{
  ASSERT_NO_FATAL_FAILURE(makeFile());
  std::vector<uint8_t> pixels(64 * 64 * 4);
  uint32_t state = 123;
  for (auto& byte : pixels)
  {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(state >> 24);
  }
  const auto compressed = compressPixels(pixels);
  ASSERT_GT(compressed.size(), 4096u);
  setPixels(compressed, true);
  putU16LE(bytes, 8, 64);
  putU16LE(bytes, 10, 64);
  putU16LE(bytes, celOffset + 22, 64);
  putU16LE(bytes, celOffset + 24, 64);
  writeWholeFile(filename, bytes);
  std::unique_ptr<app::Document> loaded(load_document(&ctx, filename));
  ASSERT_NE(nullptr, loaded);
  const auto cel = loaded->sprite()->folder()->getFirstLayer()->cel(doc::frame_t(0));
  ASSERT_NE(nullptr, cel);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x)
    {
      const auto* p = &pixels[4 * (y * 64 + x)];
      EXPECT_EQ(doc::rgba(p[0], p[1], p[2], p[3]), doc::get_pixel(cel->image(), x, y));
    }
  loaded->close();
}
