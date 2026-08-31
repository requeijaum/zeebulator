#pragma once

#include <cstdint>
#include <cstring>

#include "core/memory/memory.h"

namespace zeebulator {

// Minimal scalar type aliases matching the Khronos GL ES 1.x / EGL 1.x
// spec's standard typedefs (industry-standard fixed-width aliases, not
// Qualcomm-specific -- declared fresh here rather than including any real
// AEEGL.h/gl.h/egl.h, per the project's clean-room policy: only the real
// header's *vtable slot order* was read, never its text copied). Handle
// types (EGLDisplay/EGLSurface/EGLContext/EGLConfig/Native*Type) are all
// opaque 32-bit values on the ARM side, so a single alias covers them --
// GlHle invents its own sentinel values for these rather than mirroring
// real host EGL handles.
using GLenum = uint32_t;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLint = int32_t;
using GLsizei = int32_t;
using GLuint = uint32_t;
using GLfixed = int32_t;   // 16.16 fixed point
using GLclampx = GLfixed;  // 16.16 fixed point, clamped to [0,1] (0..0x10000)
using GLubyte = uint8_t;

using EGLint = int32_t;
using EGLBoolean = uint32_t;
using EGLHandle = uint32_t;  // EGLDisplay / EGLSurface / EGLContext / EGLConfig

constexpr EGLBoolean kEglTrue = 1;
constexpr EGLBoolean kEglFalse = 0;

// GLfixed <-> float conversion (16.16 fixed point, as used throughout the
// real IGL vtable's "x"-suffixed entry points -- e.g. glClearColorx,
// glTranslatex).
constexpr float FixedToFloat(GLfixed value) {
  return static_cast<float>(value) / 65536.0f;
}

// Standard Khronos GL enum values needed to interpret glVertexPointer/
// glColorPointer/glTexCoordPointer/glNormalPointer's runtime `type`
// argument and glEnableClientState/glDisableClientState's `array`
// argument. These are the same fixed numeric values on every real GL/GLES
// implementation (part of the public OpenGL specification, not
// Qualcomm-specific expression) -- only the handful this project actually
// needs to branch on are declared here.
constexpr GLenum kGlByte = 0x1400;
constexpr GLenum kGlUnsignedByte = 0x1401;
constexpr GLenum kGlShort = 0x1402;
constexpr GLenum kGlUnsignedShort = 0x1403;
constexpr GLenum kGlFloat = 0x1406;
constexpr GLenum kGlFixed = 0x140C;

constexpr GLenum kGlVertexArray = 0x8074;
constexpr GLenum kGlNormalArray = 0x8075;
constexpr GLenum kGlColorArray = 0x8076;
constexpr GLenum kGlTextureCoordArray = 0x8078;

// Byte size of one component of the given vertex-array element type.
constexpr int GlTypeSize(GLenum type) {
  switch (type) {
    case kGlByte:
    case kGlUnsignedByte:
      return 1;
    case kGlShort:
    case kGlUnsignedShort:
      return 2;
    case kGlFloat:
    case kGlFixed:
    default:
      return 4;
  }
}

// Reads one vertex-array component at `addr` per the real GLES1.x
// semantics for `type`, returning it as a host float. Integer types
// (BYTE/SHORT and their unsigned variants) are NOT normalized here --
// per spec that's correct for position/texcoord/normal arrays; color
// arrays additionally normalize GL_UNSIGNED_BYTE by /255 themselves
// (see gl_hle.cpp), since normalization is per-attribute, not per-type.
inline float ReadGlComponent(Memory& memory, uint32_t addr, GLenum type) {
  switch (type) {
    case kGlByte:
      return static_cast<float>(static_cast<int8_t>(memory.Read8(addr)));
    case kGlUnsignedByte:
      return static_cast<float>(memory.Read8(addr));
    case kGlShort:
      return static_cast<float>(static_cast<int16_t>(memory.Read16(addr)));
    case kGlUnsignedShort:
      return static_cast<float>(memory.Read16(addr));
    case kGlFixed:
      return FixedToFloat(static_cast<GLfixed>(memory.Read32(addr)));
    case kGlFloat:
    default: {
      uint32_t bits = memory.Read32(addr);
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
  }
}

// Standard Khronos pixel formats/types needed to size a glTexImage2D/
// glTexSubImage2D upload -- again the same fixed numeric values on every
// real GL/GLES implementation, only the ones this project branches on.
constexpr GLenum kGlAlpha = 0x1906;
constexpr GLenum kGlRgb = 0x1907;
constexpr GLenum kGlRgba = 0x1908;
constexpr GLenum kGlLuminance = 0x1909;
constexpr GLenum kGlLuminanceAlpha = 0x190A;
constexpr GLenum kGlUnsignedShort565 = 0x8363;
constexpr GLenum kGlUnsignedShort4444 = 0x8033;
constexpr GLenum kGlUnsignedShort5551 = 0x8034;

// GL_ATI_texture_compression_atitc's two real internalformat tokens
// (standard, publicly-registered Khronos extension enum values, not
// Qualcomm-specific text -- same status as the kGl* constants above).
// See core/loader/atitc.h for the real compressed block format these
// select and how it was derived.
constexpr GLenum kGlCompressedRgbAtiTc = 0x8C92;
constexpr GLenum kGlCompressedRgbaAtiTc = 0x8C93;

constexpr int GlFormatComponents(GLenum format) {
  switch (format) {
    case kGlAlpha:
    case kGlLuminance:
      return 1;
    case kGlLuminanceAlpha:
      return 2;
    case kGlRgb:
      return 3;
    case kGlRgba:
    default:
      return 4;
  }
}

// Bytes needed for one pixel of (format, type) -- the packed 16-bit types
// are always 2 bytes regardless of the component count their format
// implies; GL_UNSIGNED_BYTE (the common case) is one byte per component.
constexpr int GlPixelSize(GLenum format, GLenum type) {
  switch (type) {
    case kGlUnsignedShort565:
    case kGlUnsignedShort4444:
    case kGlUnsignedShort5551:
      return 2;
    case kGlUnsignedByte:
    default:
      return GlFormatComponents(format);
  }
}

}  // namespace zeebulator
