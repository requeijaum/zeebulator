#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <variant>
#include <vector>

#include "core/brew/gl_backend.h"

namespace zeebulator {

// One recorded texture-affecting GL call, host-resolved and
// self-contained (TexImage2D's pixel data is copied here, not
// referenced -- the real caller's own buffer, per GlTextureImage's own
// doc comment, is only valid for the duration of that one call).
//
// Deliberately doesn't cover any other GL state (viewport, matrices,
// blend/depth state, ...): those are real per-frame state a real app
// re-issues every frame anyway (confirmed live: Double Dragon's own
// real immediate-mode rendering re-sets its full draw state each
// frame), so only persistent GPU *objects* -- textures, referenced by
// ID from guest memory across frames -- need this treatment for a
// loaded save state to render correctly from a fresh GL context.
struct GenTexturesCall {
  std::vector<GLuint> assigned_ids;
};
struct DeleteTexturesCall {
  std::vector<GLuint> ids;
};
struct BindTextureCall {
  GLenum target;
  GLuint texture;
};
struct TexParameterCall {
  GLenum target;
  GLenum pname;
  GLint param;
};
struct TexImage2DCall {
  GLenum target;
  int level;
  GLenum internal_format;
  int width;
  int height;
  GLenum format;
  GLenum type;
  bool has_pixels;  // false = the real "reserve storage, no data yet" case
  std::vector<uint8_t> pixels;
};

using GlTextureLogEntry =
    std::variant<GenTexturesCall, DeleteTexturesCall, BindTextureCall, TexParameterCall,
                 TexImage2DCall>;

// Records every real texture-object-management call (GenTextures/
// DeleteTextures/BindTexture/TexParameter/TexImage2D) made through a
// GlBackend, forwarding every call -- these and all other, unrecorded
// GL calls -- unchanged to a wrapped real backend, so rendering behaves
// identically to using that backend directly, with recording as a pure
// side effect.
//
// TASKS_TOOLING.md Phase B stage 2: exists so a save state can capture
// (see Log()) and later replay (see ReplayGlTextureLog) the exact
// sequence of real texture uploads a session made up to that point,
// letting a freshly-launched process's own real GL context end up with
// the same real texture IDs/contents a loaded save state's guest
// memory expects -- a fresh, non-interactively-driven boot never
// recreates those on its own the way real gameplay would.
//
// Forward-declared here (full doc comment down by its own definition,
// near Serialize/DeserializeGlTextureLog) so GlTextureRecordingBackend's
// own CompactLog() can call it inline.
std::vector<GlTextureLogEntry> CompactGlTextureLog(const std::vector<GlTextureLogEntry>& log);

class GlTextureRecordingBackend : public GlBackend {
 public:
  explicit GlTextureRecordingBackend(GlBackend& real) : real_(real) {}

  const std::vector<GlTextureLogEntry>& Log() const { return log_; }

  // Replaces the recorded log with CompactGlTextureLog's own output --
  // see that function's doc comment for why this exists and what it
  // preserves. Safe to call at any time, repeatedly, during a live
  // session (unlike ClearLog, which only belongs at the one specific
  // boot/gameplay boundary): real callers should call this
  // periodically during long sessions, and always right before
  // serializing a save state, to keep both memory use and save-state
  // size bounded instead of growing for the rest of the process's
  // lifetime.
  void CompactLog() { log_ = CompactGlTextureLog(log_); }

  // Discards everything recorded so far. Real callers need this once,
  // right after a deterministic, always-the-same boot/setup sequence
  // finishes and before the interactive/gameplay portion of a session
  // starts: a save state's log is meant to cover only what a *fresh*
  // process's own identical boot sequence wouldn't already recreate on
  // its own -- replaying boot-time texture creation on top of a process
  // that just did that exact same boot sequence would double-create
  // those textures and desync every ID from there on (see
  // ReplayGlTextureLog's own doc comment on why ID alignment matters).
  void ClearLog() { log_.clear(); }

  bool CreateContext() override { return real_.CreateContext(); }
  void DestroyContext() override { real_.DestroyContext(); }
  void SwapBuffers() override { real_.SwapBuffers(); }

  void Clear(GLbitfield mask) override { real_.Clear(mask); }
  void ClearColor(float r, float g, float b, float a) override { real_.ClearColor(r, g, b, a); }
  void Viewport(int x, int y, int width, int height) override {
    real_.Viewport(x, y, width, height);
  }
  void Enable(GLenum cap) override { real_.Enable(cap); }
  void Disable(GLenum cap) override { real_.Disable(cap); }
  void MatrixMode(GLenum mode) override { real_.MatrixMode(mode); }
  void LoadIdentity() override { real_.LoadIdentity(); }
  void PushMatrix() override { real_.PushMatrix(); }
  void PopMatrix() override { real_.PopMatrix(); }
  void Ortho(float left, float right, float bottom, float top, float near_plane,
             float far_plane) override {
    real_.Ortho(left, right, bottom, top, near_plane, far_plane);
  }
  void Frustum(float left, float right, float bottom, float top, float near_plane,
               float far_plane) override {
    real_.Frustum(left, right, bottom, top, near_plane, far_plane);
  }
  void Translate(float x, float y, float z) override { real_.Translate(x, y, z); }
  void Rotate(float angle_degrees, float x, float y, float z) override {
    real_.Rotate(angle_degrees, x, y, z);
  }
  void Scale(float x, float y, float z) override { real_.Scale(x, y, z); }
  void Color4(float r, float g, float b, float a) override { real_.Color4(r, g, b, a); }
  void AlphaFunc(GLenum func, float ref) override { real_.AlphaFunc(func, ref); }
  void BlendFunc(GLenum sfactor, GLenum dfactor) override { real_.BlendFunc(sfactor, dfactor); }
  void DepthFunc(GLenum func) override { real_.DepthFunc(func); }
  void ClearDepth(float depth) override { real_.ClearDepth(depth); }
  void DepthMask(bool flag) override { real_.DepthMask(flag); }
  void DrawArrays(GLenum mode, const GlVertexArrays& arrays) override {
    real_.DrawArrays(mode, arrays);
  }

  void GenTextures(GLsizei n, GLuint* textures) override;
  void DeleteTextures(GLsizei n, const GLuint* textures) override;
  void BindTexture(GLenum target, GLuint texture) override;
  void TexParameter(GLenum target, GLenum pname, GLint param) override;
  void TexImage2D(GLenum target, const GlTextureImage& image) override;

 private:
  GlBackend& real_;
  std::vector<GlTextureLogEntry> log_;
};

// Replays a previously-recorded log against `target` -- issuing the
// exact same GenTextures/DeleteTextures/BindTexture/TexParameter/
// TexImage2D calls, in the same order, so `target`'s own real GL
// context ends up with the same real texture object IDs/contents the
// log was recorded against. Relies on a real, practical (if not
// formally spec-guaranteed) property of every real desktop GL driver:
// glGenTextures assigns sequential IDs deterministically when replayed
// from the same starting state (e.g. a fresh context) with the same
// call sequence. Each replayed GenTextures call's newly-assigned IDs
// are checked against the recorded ones; a mismatch means this
// property didn't hold on this driver, and is reported back (rather
// than silently producing wrong textures) via the returned bool.
bool ReplayGlTextureLog(const std::vector<GlTextureLogEntry>& log, GlBackend& target);

bool SerializeGlTextureLog(const std::vector<GlTextureLogEntry>& log, std::ostream& out);
bool DeserializeGlTextureLog(std::istream& in, std::vector<GlTextureLogEntry>& out);

// Compacts a log down to just what's needed to reproduce the *final*
// state of every texture still alive at the end of it: every
// GenTextures/DeleteTextures call is kept verbatim, in its original
// order (ReplayGlTextureLog's own real-driver ID-assignment trick
// depends on replaying that exact sequence -- reordering or dropping
// any of these would desync every texture ID from there on), but
// Bind/TexParameter/TexImage2D calls collapse down to one synthesized
// bind plus each texture's last-write-wins parameters and per-level
// images. A texture deleted by the end of the log contributes nothing
// to the output at all.
//
// Exists because this log has no size cap of its own (see
// GlTextureRecordingBackend's own doc comment on why `ClearLog` only
// runs once, at boot) -- a real, long play session re-uploading or
// reconfiguring the same live textures many times (menu transitions,
// animated UI, ...) otherwise makes both the in-memory log and any
// save state serializing it grow without bound for the rest of the
// process's lifetime, confirmed live: a 57-minute real session's own
// save state reached 470MB, almost entirely this log's own
// accumulated history rather than anything a fresh replay actually
// needs.
//
// Assumes each real texture ID is only ever used with one real GL
// target across its lifetime -- true for every real call site this
// project has traced so far (no cube maps or similar multi-target
// textures observed). If some future title's own real code violates
// that, the one synthesized bind per surviving texture may use the
// wrong target for a minority of its own parameter/image calls.
std::vector<GlTextureLogEntry> CompactGlTextureLog(const std::vector<GlTextureLogEntry>& log);

}  // namespace zeebulator
