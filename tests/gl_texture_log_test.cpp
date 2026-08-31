#include "core/gl_texture_log.h"

#include <sstream>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

using zeebulator::BindTextureCall;
using zeebulator::CompactGlTextureLog;
using zeebulator::DeleteTexturesCall;
using zeebulator::DeserializeGlTextureLog;
using zeebulator::GenTexturesCall;
using zeebulator::GLenum;
using zeebulator::GLint;
using zeebulator::GLsizei;
using zeebulator::GLuint;
using zeebulator::GlBackend;
using zeebulator::GlTextureImage;
using zeebulator::GlTextureLogEntry;
using zeebulator::GlTextureRecordingBackend;
using zeebulator::GlVertexArrays;
using zeebulator::ReplayGlTextureLog;
using zeebulator::SerializeGlTextureLog;
using zeebulator::TexImage2DCall;
using zeebulator::TexParameterCall;

namespace {

// Minimal fake covering exactly what these tests need to observe --
// real texture object bookkeeping (sequential ID assignment, matching
// every real desktop GL driver's own practical behavior from a fresh
// context -- see ReplayGlTextureLog's own doc comment) plus enough
// bound-texture/pixel tracking to verify a replay actually reproduced
// the recorded state. Every other GlBackend method is a no-op; nothing
// here exercises them.
class FakeGlBackend : public GlBackend {
 public:
  explicit FakeGlBackend(GLuint first_id = 1) : next_id_(first_id) {}

  bool CreateContext() override { return true; }
  void DestroyContext() override {}
  void SwapBuffers() override {}
  void Clear(zeebulator::GLbitfield) override {}
  void ClearColor(float, float, float, float) override {}
  void Viewport(int, int, int, int) override {}
  void Enable(GLenum) override {}
  void Disable(GLenum) override {}
  void MatrixMode(GLenum) override {}
  void LoadIdentity() override {}
  void PushMatrix() override {}
  void PopMatrix() override {}
  void Ortho(float, float, float, float, float, float) override {}
  void Frustum(float, float, float, float, float, float) override {}
  void Translate(float, float, float) override {}
  void Rotate(float, float, float, float) override {}
  void Scale(float, float, float) override {}
  void Color4(float, float, float, float) override {}
  void AlphaFunc(GLenum, float) override {}
  void BlendFunc(GLenum, GLenum) override {}
  void DepthFunc(GLenum) override {}
  void ClearDepth(float) override {}
  void DepthMask(bool) override {}
  void DrawArrays(GLenum, const GlVertexArrays&) override {}

  void GenTextures(GLsizei n, GLuint* textures) override {
    for (GLsizei i = 0; i < n; ++i) textures[i] = next_id_++;
  }
  void DeleteTextures(GLsizei n, const GLuint* textures) override {
    for (GLsizei i = 0; i < n; ++i) live_textures_.erase(textures[i]);
  }
  void BindTexture(GLenum, GLuint texture) override { bound_texture_ = texture; }
  void TexParameter(GLenum, GLenum, GLint) override {}
  void TexImage2D(GLenum, const GlTextureImage& image) override {
    std::vector<uint8_t> pixels;
    if (image.pixels != nullptr) {
      size_t count = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                     static_cast<size_t>(zeebulator::GlPixelSize(image.format, image.type));
      pixels.assign(image.pixels, image.pixels + count);
    }
    live_textures_[bound_texture_] = std::move(pixels);
  }

  GLuint bound_texture_ = 0;
  std::unordered_map<GLuint, std::vector<uint8_t>> live_textures_;

 private:
  GLuint next_id_;
};

}  // namespace

TEST(GlTextureLog, RecordsGenTexturesWithTheRealAssignedIds) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);

  GLuint ids[2] = {0, 0};
  recorder.GenTextures(2, ids);

  ASSERT_EQ(recorder.Log().size(), 1u);
  const auto* call = std::get_if<GenTexturesCall>(&recorder.Log()[0]);
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->assigned_ids, (std::vector<GLuint>{1, 2}));
  EXPECT_EQ(ids[0], 1u);
  EXPECT_EQ(ids[1], 2u);
}

TEST(GlTextureLog, ClearLogDiscardsEverythingRecordedSoFar) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);  // simulates boot-time texture creation
  ASSERT_FALSE(recorder.Log().empty());

  recorder.ClearLog();
  EXPECT_TRUE(recorder.Log().empty());

  // Recording resumes normally afterward.
  recorder.BindTexture(0x0DE1, id);
  EXPECT_EQ(recorder.Log().size(), 1u);
}

TEST(GlTextureLog, RecordsTexImage2DPixelDataByValueNotByPointer) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1 /* GL_TEXTURE_2D-ish */, id);

  std::vector<uint16_t> pixels = {0x1234, 0x5678};  // 2x1 RGB565-shaped
  GlTextureImage image;
  image.width = 2;
  image.height = 1;
  image.format = 0x1907;  // arbitrary stand-in, not checked by the fake
  image.type = 0x8363;
  image.pixels = reinterpret_cast<const uint8_t*>(pixels.data());
  recorder.TexImage2D(0x0DE1, image);

  // Mutate the original buffer -- the recorded copy must be unaffected,
  // matching GlTextureImage's own doc comment that `pixels` is only
  // valid for the call's own duration.
  pixels[0] = 0xFFFF;

  ASSERT_EQ(recorder.Log().size(), 3u);
  const auto* tex_call = std::get_if<TexImage2DCall>(&recorder.Log()[2]);
  ASSERT_NE(tex_call, nullptr);
  ASSERT_EQ(tex_call->pixels.size(), 4u);
  EXPECT_EQ(tex_call->pixels[0], 0x34);
  EXPECT_EQ(tex_call->pixels[1], 0x12);
}

TEST(GlTextureLog, SerializeThenDeserializeRoundTrips) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  recorder.TexParameter(0x0DE1, 0x2801, 0x2600);
  uint8_t pixel_bytes[4] = {0x11, 0x22, 0x33, 0x44};
  GlTextureImage image;
  image.width = 2;
  image.height = 1;
  image.type = 0x8363;  // kGlUnsignedShort565 -- 2 bytes/pixel, matches pixel_bytes' size
  image.pixels = pixel_bytes;
  recorder.TexImage2D(0x0DE1, image);
  GLuint delete_id = id;
  recorder.DeleteTextures(1, &delete_id);

  std::stringstream stream;
  ASSERT_TRUE(SerializeGlTextureLog(recorder.Log(), stream));

  std::vector<GlTextureLogEntry> restored;
  ASSERT_TRUE(DeserializeGlTextureLog(stream, restored));
  ASSERT_EQ(restored.size(), recorder.Log().size());

  ASSERT_TRUE(std::holds_alternative<GenTexturesCall>(restored[0]));
  EXPECT_EQ(std::get<GenTexturesCall>(restored[0]).assigned_ids, (std::vector<GLuint>{1}));

  ASSERT_TRUE(std::holds_alternative<BindTextureCall>(restored[1]));
  EXPECT_EQ(std::get<BindTextureCall>(restored[1]).texture, 1u);

  ASSERT_TRUE(std::holds_alternative<TexParameterCall>(restored[2]));
  EXPECT_EQ(std::get<TexParameterCall>(restored[2]).param, 0x2600);

  ASSERT_TRUE(std::holds_alternative<TexImage2DCall>(restored[3]));
  const auto& tex = std::get<TexImage2DCall>(restored[3]);
  EXPECT_TRUE(tex.has_pixels);
  ASSERT_EQ(tex.pixels.size(), 4u);
  EXPECT_EQ(tex.pixels[0], 0x11);
  EXPECT_EQ(tex.pixels[3], 0x44);

  ASSERT_TRUE(std::holds_alternative<DeleteTexturesCall>(restored[4]));
  EXPECT_EQ(std::get<DeleteTexturesCall>(restored[4]).ids, (std::vector<GLuint>{1}));
}

TEST(GlTextureLog, ReplayReproducesTheSameTexturesOnAFreshBackend) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  uint8_t pixel_bytes[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  GlTextureImage image;
  image.width = 2;
  image.height = 1;
  image.type = 0x8363;  // kGlUnsignedShort565 -- 2 bytes/pixel, matches pixel_bytes' size
  image.pixels = pixel_bytes;
  recorder.TexImage2D(0x0DE1, image);

  FakeGlBackend fresh;  // a second, independent "cold" backend
  bool ok = ReplayGlTextureLog(recorder.Log(), fresh);
  EXPECT_TRUE(ok);

  ASSERT_EQ(fresh.live_textures_.count(1u), 1u);
  const std::vector<uint8_t>& replayed_pixels = fresh.live_textures_.at(1u);
  ASSERT_EQ(replayed_pixels.size(), 4u);
  EXPECT_EQ(replayed_pixels[0], 0xAA);
  EXPECT_EQ(replayed_pixels[3], 0xDD);
}

TEST(GlTextureLog, ReplayReportsFailureWhenTheTargetAssignsDifferentIds) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);  // recorded as ID 1

  FakeGlBackend mismatched(/*first_id=*/50);  // will assign ID 50, not 1
  EXPECT_FALSE(ReplayGlTextureLog(recorder.Log(), mismatched));
}

namespace {
GlTextureImage MakeImage(int width, int height, const uint8_t* pixels) {
  GlTextureImage image;
  image.width = width;
  image.height = height;
  image.type = 0x8363;  // kGlUnsignedShort565 -- 2 bytes/pixel
  image.pixels = pixels;
  return image;
}
}  // namespace

TEST(GlTextureLogCompact, DropsAllButTheLastTexImage2DForARepeatedlyUploadedTexture) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  uint8_t first[4] = {1, 1, 1, 1};
  uint8_t second[4] = {2, 2, 2, 2};
  uint8_t third[4] = {3, 3, 3, 3};
  // Real gameplay re-uploading the same live texture many times over a
  // long session -- exactly the pattern that made a real 57-minute
  // session's own save state reach 470MB (see CompactGlTextureLog's
  // own doc comment).
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, first));
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, second));
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, third));

  auto compacted = CompactGlTextureLog(recorder.Log());

  int tex_image_count = 0;
  for (const auto& entry : compacted) {
    if (const auto* tex = std::get_if<TexImage2DCall>(&entry)) {
      ++tex_image_count;
      EXPECT_EQ(tex->pixels[0], 3u) << "only the last upload should survive";
    }
  }
  EXPECT_EQ(tex_image_count, 1);
}

TEST(GlTextureLogCompact, DropsAllButTheLastValuePerDistinctTexParameterPname) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  constexpr GLenum kWrapS = 0x2802;
  constexpr GLenum kMinFilter = 0x2801;
  recorder.TexParameter(0x0DE1, kWrapS, 0x2900);
  recorder.TexParameter(0x0DE1, kMinFilter, 0x2600);
  recorder.TexParameter(0x0DE1, kWrapS, 0x812F);  // supersedes the first kWrapS

  auto compacted = CompactGlTextureLog(recorder.Log());

  std::unordered_map<GLenum, GLint> params;
  for (const auto& entry : compacted) {
    if (const auto* param = std::get_if<TexParameterCall>(&entry)) {
      params[param->pname] = param->param;
    }
  }
  ASSERT_EQ(params.size(), 2u) << "both distinct pnames survive, just deduplicated";
  EXPECT_EQ(params[kWrapS], 0x812F);
  EXPECT_EQ(params[kMinFilter], 0x2600);
}

TEST(GlTextureLogCompact, DeletedTextureContributesNothingToTheOutput) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  uint8_t pixels[4] = {9, 9, 9, 9};
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, pixels));
  GLuint delete_id = id;
  recorder.DeleteTextures(1, &delete_id);

  auto compacted = CompactGlTextureLog(recorder.Log());

  for (const auto& entry : compacted) {
    EXPECT_FALSE(std::holds_alternative<TexImage2DCall>(entry))
        << "a texture deleted by the end of the log needs no image data restored";
    EXPECT_FALSE(std::holds_alternative<BindTextureCall>(entry))
        << "no reason to bind a texture nothing else references anymore";
  }
}

TEST(GlTextureLogCompact, PreservesEveryGenAndDeleteCallVerbatimAndInOrder) {
  // Real-driver ID assignment during replay depends on this exact
  // sequence (ReplayGlTextureLog's own doc comment) -- compaction must
  // never touch it, no matter how much of the rest it collapses.
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint first = 0, second = 0;
  recorder.GenTextures(1, &first);
  recorder.GenTextures(1, &second);
  GLuint delete_first = first;
  recorder.DeleteTextures(1, &delete_first);
  recorder.BindTexture(0x0DE1, second);
  uint8_t pixels[4] = {5, 5, 5, 5};
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, pixels));

  auto compacted = CompactGlTextureLog(recorder.Log());

  std::vector<GlTextureLogEntry> gen_and_delete_only;
  for (const auto& entry : compacted) {
    if (std::holds_alternative<GenTexturesCall>(entry) ||
        std::holds_alternative<DeleteTexturesCall>(entry)) {
      gen_and_delete_only.push_back(entry);
    }
  }
  ASSERT_EQ(gen_and_delete_only.size(), 3u);
  EXPECT_EQ(std::get<GenTexturesCall>(gen_and_delete_only[0]).assigned_ids,
            (std::vector<GLuint>{first}));
  EXPECT_EQ(std::get<GenTexturesCall>(gen_and_delete_only[1]).assigned_ids,
            (std::vector<GLuint>{second}));
  EXPECT_EQ(std::get<DeleteTexturesCall>(gen_and_delete_only[2]).ids,
            (std::vector<GLuint>{first}));
}

TEST(GlTextureLogCompact, ReusedIdAfterDeleteDoesNotInheritTheOldTexturesState) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  uint8_t old_pixels[4] = {7, 7, 7, 7};
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, old_pixels));
  GLuint delete_id = id;
  recorder.DeleteTextures(1, &delete_id);
  // A real driver can hand the same numeric ID back out after it's
  // freed -- simulate that directly rather than relying on FakeGlBackend's
  // own monotonic counter.
  GenTexturesCall regen;
  regen.assigned_ids = {id};
  // Push the raw entries this round needs beyond what the recorder's
  // own real calls would produce, to exercise the specific "same ID,
  // fresh generation" case regardless of the fake backend's own ID
  // policy.
  std::vector<GlTextureLogEntry> log = recorder.Log();
  log.push_back(regen);
  log.push_back(BindTextureCall{0x0DE1, id});
  uint8_t new_pixels[4] = {8, 8, 8, 8};
  TexImage2DCall new_image;
  new_image.target = 0x0DE1;
  new_image.level = 0;
  new_image.internal_format = 0;
  new_image.width = 2;
  new_image.height = 1;
  new_image.format = 0;
  new_image.type = 0x8363;
  new_image.has_pixels = true;
  new_image.pixels.assign(new_pixels, new_pixels + 4);
  log.push_back(new_image);

  auto compacted = CompactGlTextureLog(log);

  int tex_image_count = 0;
  for (const auto& entry : compacted) {
    if (const auto* tex = std::get_if<TexImage2DCall>(&entry)) {
      ++tex_image_count;
      EXPECT_EQ(tex->pixels[0], 8u) << "only the fresh generation's own upload should survive";
    }
  }
  EXPECT_EQ(tex_image_count, 1);
}

TEST(GlTextureLogCompact, ReplayingTheCompactedLogReproducesTheSameFinalStateAsTheOriginal) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  uint8_t stale[4] = {1, 1, 1, 1};
  uint8_t final_pixels[4] = {42, 42, 42, 42};
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, stale));
  recorder.TexParameter(0x0DE1, 0x2801, 0x2600);
  recorder.TexImage2D(0x0DE1, MakeImage(2, 1, final_pixels));

  auto compacted = CompactGlTextureLog(recorder.Log());

  FakeGlBackend original_replay_target;
  ASSERT_TRUE(ReplayGlTextureLog(recorder.Log(), original_replay_target));
  FakeGlBackend compacted_replay_target;
  ASSERT_TRUE(ReplayGlTextureLog(compacted, compacted_replay_target));

  ASSERT_EQ(original_replay_target.live_textures_.size(),
            compacted_replay_target.live_textures_.size());
  EXPECT_EQ(original_replay_target.live_textures_.at(1u),
            compacted_replay_target.live_textures_.at(1u));
  EXPECT_EQ(compacted_replay_target.live_textures_.at(1u)[0], 42u);
}
