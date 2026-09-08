#include <cstdio>
#include "core/brew/soft_gl_backend.h"

#include <algorithm>
#include <cmath>

#include "core/brew/gl_types.h"

namespace zeebulator {
namespace {

// Enums Khronos padrão que este backend precisa comparar (mesmos valores
// numéricos em toda implementação GL/GLES -- ver gl_types.h). Declarados
// aqui em vez de incluir qualquer gl.h proprietário.
constexpr GLenum kMatrixModelview = 0x1700;
constexpr GLenum kMatrixProjection = 0x1701;

constexpr GLbitfield kColorBufferBit = 0x00004000;
constexpr GLbitfield kDepthBufferBit = 0x00000100;

constexpr GLenum kCapBlend = 0x0BE2;
constexpr GLenum kCapDepthTest = 0x0B71;
constexpr GLenum kCapAlphaTest = 0x0BC0;
constexpr GLenum kCapTexture2d = 0x0DE1;

constexpr GLenum kTexWrapS = 0x2802;
constexpr GLenum kTexWrapT = 0x2803;
constexpr GLenum kTexMinFilter = 0x2801;
constexpr GLenum kTexMagFilter = 0x2800;
constexpr GLenum kClampToEdge = 0x812F;
constexpr GLenum kRepeat = 0x2901;
constexpr GLenum kNearest = 0x2600;

constexpr GLenum kTriangles = 0x0004;
constexpr GLenum kTriangleStrip = 0x0005;
constexpr GLenum kTriangleFan = 0x0006;

// Funções de comparação (depth/alpha).
constexpr GLenum kNever = 0x0200;
constexpr GLenum kLess = 0x0201;
constexpr GLenum kEqual = 0x0202;
constexpr GLenum kLequal = 0x0203;
constexpr GLenum kGreater = 0x0204;
constexpr GLenum kNotequal = 0x0205;
constexpr GLenum kGequal = 0x0206;
constexpr GLenum kAlways = 0x0207;

// Fatores de blend.
constexpr GLenum kZero = 0;
constexpr GLenum kOne = 1;
constexpr GLenum kSrcColor = 0x0300;
constexpr GLenum kOneMinusSrcColor = 0x0301;
constexpr GLenum kSrcAlpha = 0x0302;
constexpr GLenum kOneMinusSrcAlpha = 0x0303;
constexpr GLenum kDstAlpha = 0x0304;
constexpr GLenum kOneMinusDstAlpha = 0x0305;
constexpr GLenum kDstColor = 0x0306;
constexpr GLenum kOneMinusDstColor = 0x0307;

using Matrix = std::array<float, 16>;

Matrix Identity() {
  Matrix m{};
  m[0] = m[5] = m[10] = m[15] = 1.0f;
  return m;
}

// out = a * b, ambas column-major (m[col*4+row]).
Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
      out[col * 4 + row] = s;
    }
  }
  return out;
}

// Aplica a matriz a um ponto homogéneo (v em coluna).
std::array<float, 4> Transform(const Matrix& m, const std::array<float, 4>& v) {
  std::array<float, 4> out{};
  for (int row = 0; row < 4; ++row) {
    float s = 0.0f;
    for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * v[k];
    out[row] = s;
  }
  return out;
}

// Dobro da área com sinal do triângulo projetado (a,b,c) na tela.
float Edge(const std::array<float, 4>& a, const std::array<float, 4>& b,
           const std::array<float, 4>& c) {
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

bool Compare(GLenum func, float value, float reference) {
  switch (func) {
    case kNever: return false;
    case kLess: return value < reference;
    case kEqual: return value == reference;
    case kLequal: return value <= reference;
    case kGreater: return value > reference;
    case kNotequal: return value != reference;
    case kGequal: return value >= reference;
    case kAlways: default: return true;
  }
}

// Peso de um fator de mistura para o canal `c` (0..3, com 3 = alpha).
float BlendFactor(GLenum kind, const std::array<float, 4>& src,
                  const std::array<float, 4>& dst, int c) {
  switch (kind) {
    case kZero: return 0.0f;
    case kOne: return 1.0f;
    case kSrcColor: return src[c];
    case kOneMinusSrcColor: return 1.0f - src[c];
    case kSrcAlpha: return src[3];
    case kOneMinusSrcAlpha: return 1.0f - src[3];
    case kDstAlpha: return dst[3];
    case kOneMinusDstAlpha: return 1.0f - dst[3];
    case kDstColor: return dst[c];
    case kOneMinusDstColor: return 1.0f - dst[c];
    default: return 1.0f;
  }
}

int WrapIndex(GLenum mode, int index, int size) {
  if (index >= 0 && index < size) return index;
  if (mode == kClampToEdge) return std::max(0, std::min(size - 1, index));
  // GL_REPEAT: resto euclidiano (sempre não-negativo).
  int r = index % size;
  if (r < 0) r += size;
  return r;
}

uint16_t ToRgb565(float r, float g, float b) {
  int ri = static_cast<int>(std::max(0.0f, std::min(1.0f, r)) * 255.0f + 0.5f);
  int gi = static_cast<int>(std::max(0.0f, std::min(1.0f, g)) * 255.0f + 0.5f);
  int bi = static_cast<int>(std::max(0.0f, std::min(1.0f, b)) * 255.0f + 0.5f);
  return static_cast<uint16_t>(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

std::array<float, 3> FromRgb565(uint16_t p) {
  float r = static_cast<float>(((p >> 11) & 0x1F) << 3) / 255.0f;
  float g = static_cast<float>(((p >> 5) & 0x3F) << 2) / 255.0f;
  float b = static_cast<float>((p & 0x1F) << 3) / 255.0f;
  return {r, g, b};
}

}  // namespace

SoftGlBackend::SoftGlBackend(std::vector<uint16_t>& framebuffer, int width, int height)
    : framebuffer_(framebuffer),
      width_(width),
      height_(height),
      depth_(static_cast<size_t>(width) * height, 1.0f) {
  vp_w_ = width;
  vp_h_ = height;
  modelview_[0] = Identity();
  projection_[0] = Identity();
}

std::vector<SoftGlBackend::Matrix>& SoftGlBackend::CurrentStack() {
  return matrix_mode_ == kMatrixProjection ? projection_ : modelview_;
}

SoftGlBackend::Matrix& SoftGlBackend::Top() { return CurrentStack().back(); }

void SoftGlBackend::MultTop(const Matrix& m) {
  Matrix& t = Top();
  t = Multiply(t, m);
}

void SoftGlBackend::SwapBuffers() {
  if (on_present_) on_present_(present_user_);
}

void SoftGlBackend::Clear(GLbitfield mask) {
  if (mask & kColorBufferBit) {
    uint16_t c = ToRgb565(clear_color_[0], clear_color_[1], clear_color_[2]);
    std::fill(framebuffer_.begin(), framebuffer_.end(), c);
  }
  if (mask & kDepthBufferBit) {
    std::fill(depth_.begin(), depth_.end(), clear_depth_);
  }
}

void SoftGlBackend::ClearColor(float r, float g, float b, float a) {
  clear_color_ = {r, g, b, a};
}

void SoftGlBackend::Viewport(int x, int y, int width, int height) {
  // GLES inicializa o viewport com o tamanho da surface. Headless (sem SDL/EGL
  // real dimensionando a surface) alguns titulos emitem um glViewport(0,0,0,0)
  // espurio antes do primeiro frame; aceita-lo zeraria o pipeline (todo
  // triangulo recortado em vp_w_==0). Ignora dimensoes degeneradas e mantem o
  // default full-screen do construtor -- comportamento verificado com ironsight
  // (folder 280221), que so emite viewport 0x0 e nunca um valido no boot.
  if (width <= 0 || height <= 0) return;
  vp_x_ = x;
  vp_y_ = y;
  vp_w_ = width;
  vp_h_ = height;
}

void SoftGlBackend::Enable(GLenum cap) {
  switch (cap) {
    case kCapBlend: blend_ = true; break;
    case kCapDepthTest: depth_test_ = true; break;
    case kCapAlphaTest: alpha_test_ = true; break;
    case kCapTexture2d: texture_2d_ = true; break;
    default: break;
  }
}

void SoftGlBackend::Disable(GLenum cap) {
  switch (cap) {
    case kCapBlend: blend_ = false; break;
    case kCapDepthTest: depth_test_ = false; break;
    case kCapAlphaTest: alpha_test_ = false; break;
    case kCapTexture2d: texture_2d_ = false; break;
    default: break;
  }
}

void SoftGlBackend::MatrixMode(GLenum mode) { matrix_mode_ = mode; }
void SoftGlBackend::LoadIdentity() { Top() = Identity(); }
void SoftGlBackend::LoadMatrix(const float m[16]) {
  Matrix mat;
  for (int i = 0; i < 16; ++i) mat[i] = m[i];
  Top() = mat;  // column-major, mesma convenção interna
}
void SoftGlBackend::MultMatrix(const float m[16]) {
  Matrix mat;
  for (int i = 0; i < 16; ++i) mat[i] = m[i];
  MultTop(mat);
}

void SoftGlBackend::PushMatrix() {
  auto& s = CurrentStack();
  s.push_back(s.back());
}

void SoftGlBackend::PopMatrix() {
  auto& s = CurrentStack();
  if (s.size() > 1) s.pop_back();
}

void SoftGlBackend::Ortho(float l, float r, float b, float t, float n, float f) {
  Matrix m = Identity();
  m[0] = 2.0f / (r - l);
  m[5] = 2.0f / (t - b);
  m[10] = -2.0f / (f - n);
  m[12] = -(r + l) / (r - l);
  m[13] = -(t + b) / (t - b);
  m[14] = -(f + n) / (f - n);
  MultTop(m);
}

void SoftGlBackend::Frustum(float l, float r, float b, float t, float n, float f) {
  Matrix m{};
  m[0] = 2.0f * n / (r - l);
  m[5] = 2.0f * n / (t - b);
  m[8] = (r + l) / (r - l);
  m[9] = (t + b) / (t - b);
  m[10] = -(f + n) / (f - n);
  m[11] = -1.0f;
  m[14] = -2.0f * f * n / (f - n);
  MultTop(m);
}

void SoftGlBackend::Translate(float x, float y, float z) {
  Matrix m = Identity();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  MultTop(m);
}

void SoftGlBackend::Rotate(float angle_degrees, float x, float y, float z) {
  float len = std::sqrt(x * x + y * y + z * z);
  if (len == 0.0f) return;
  x /= len;
  y /= len;
  z /= len;
  float rad = angle_degrees * 3.14159265358979323846f / 180.0f;
  float s = std::sin(rad);
  float c = std::cos(rad);
  float t = 1.0f - c;
  Matrix m{};
  m[0] = t * x * x + c;
  m[1] = t * x * y + s * z;
  m[2] = t * x * z - s * y;
  m[4] = t * x * y - s * z;
  m[5] = t * y * y + c;
  m[6] = t * y * z + s * x;
  m[8] = t * x * z + s * y;
  m[9] = t * y * z - s * x;
  m[10] = t * z * z + c;
  m[15] = 1.0f;
  MultTop(m);
}

void SoftGlBackend::Scale(float x, float y, float z) {
  Matrix m = Identity();
  m[0] = x;
  m[5] = y;
  m[10] = z;
  MultTop(m);
}

void SoftGlBackend::Color4(float r, float g, float b, float a) {
  current_color_ = {r, g, b, a};
}

void SoftGlBackend::AlphaFunc(GLenum func, float ref) {
  alpha_func_ = func;
  alpha_ref_ = ref;
}

void SoftGlBackend::BlendFunc(GLenum sfactor, GLenum dfactor) {
  blend_src_ = sfactor;
  blend_dst_ = dfactor;
}

void SoftGlBackend::DepthFunc(GLenum func) { depth_func_ = func; }
void SoftGlBackend::ClearDepth(float depth) { clear_depth_ = depth; }
void SoftGlBackend::DepthMask(bool flag) { depth_mask_ = flag; }

void SoftGlBackend::GenTextures(GLsizei n, GLuint* textures) {
  for (GLsizei i = 0; i < n; ++i) {
    GLuint id = next_texture_++;
    textures_[id];  // cria entrada vazia
    textures[i] = id;
  }
}

void SoftGlBackend::DeleteTextures(GLsizei n, const GLuint* textures) {
  for (GLsizei i = 0; i < n; ++i) {
    textures_.erase(textures[i]);
    if (bound_texture_ == textures[i]) bound_texture_ = 0;
  }
}

void SoftGlBackend::BindTexture(GLenum /*target*/, GLuint texture) {
  bound_texture_ = texture;
  if (texture != 0) textures_[texture];  // garante existência
}

void SoftGlBackend::TexParameter(GLenum /*target*/, GLenum pname, GLint param) {
  if (bound_texture_ == 0) return;
  Texture& tex = textures_[bound_texture_];
  switch (pname) {
    case kTexWrapS: tex.wrap_s = static_cast<GLenum>(param); break;
    case kTexWrapT: tex.wrap_t = static_cast<GLenum>(param); break;
    case kTexMinFilter: tex.min_filter = static_cast<GLenum>(param); break;
    case kTexMagFilter: tex.mag_filter = static_cast<GLenum>(param); break;
    default: break;
  }
}

void SoftGlBackend::TexImage2D(GLenum /*target*/, const GlTextureImage& image) {
  if (bound_texture_ == 0) return;
  Texture& tex = textures_[bound_texture_];
  tex.width = image.width;
  tex.height = image.height;
  tex.pixels.assign(static_cast<size_t>(image.width) * image.height, {0, 0, 0, 255});
  if (image.pixels == nullptr) return;  // reserva de storage, sem dados

  // Desempacota os formatos que os títulos GL do Zeebo realmente usam para
  // RGBA host-native. O GlHle já copiou os bytes para host; aqui só
  // interpretamos (format, type) -- os mesmos valores Khronos padrão.
  int comps = GlFormatComponents(image.format);
  const uint8_t* p = image.pixels;
  size_t count = static_cast<size_t>(image.width) * image.height;
  if (image.type == kGlUnsignedShort565 || image.type == kGlUnsignedShort4444 ||
      image.type == kGlUnsignedShort5551) {
    for (size_t i = 0; i < count; ++i) {
      uint16_t v = static_cast<uint16_t>(p[i * 2] | (p[i * 2 + 1] << 8));
      uint8_t r, g, b, a;
      if (image.type == kGlUnsignedShort565) {
        r = static_cast<uint8_t>(((v >> 11) & 0x1F) << 3);
        g = static_cast<uint8_t>(((v >> 5) & 0x3F) << 2);
        b = static_cast<uint8_t>((v & 0x1F) << 3);
        a = 255;
      } else if (image.type == kGlUnsignedShort4444) {
        r = static_cast<uint8_t>(((v >> 12) & 0xF) * 17);
        g = static_cast<uint8_t>(((v >> 8) & 0xF) * 17);
        b = static_cast<uint8_t>(((v >> 4) & 0xF) * 17);
        a = static_cast<uint8_t>((v & 0xF) * 17);
      } else {  // 5551
        r = static_cast<uint8_t>(((v >> 11) & 0x1F) << 3);
        g = static_cast<uint8_t>(((v >> 6) & 0x1F) << 3);
        b = static_cast<uint8_t>(((v >> 1) & 0x1F) << 3);
        a = (v & 1) ? 255 : 0;
      }
      tex.pixels[i] = {r, g, b, a};
    }
    return;
  }
  // GL_UNSIGNED_BYTE: 1..4 componentes por texel.
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* t = p + i * comps;
    std::array<uint8_t, 4> out{0, 0, 0, 255};
    switch (image.format) {
      case kGlAlpha: out = {255, 255, 255, t[0]}; break;
      case kGlLuminance: out = {t[0], t[0], t[0], 255}; break;
      case kGlLuminanceAlpha: out = {t[0], t[0], t[0], t[1]}; break;
      case kGlRgb: out = {t[0], t[1], t[2], 255}; break;
      case kGlRgba: default: out = {t[0], t[1], t[2], t[3]}; break;
    }
    tex.pixels[i] = out;
  }
}

std::array<float, 4> SoftGlBackend::Texture::Texel(int x, int y) const {
  x = WrapIndex(wrap_s, x, width);
  y = WrapIndex(wrap_t, y, height);
  const std::array<uint8_t, 4>& p = pixels[static_cast<size_t>(y) * width + x];
  return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
}

std::array<float, 4> SoftGlBackend::Texture::Sample(float u, float v) const {
  if (width == 0 || height == 0) return {1, 1, 1, 1};
  float x = u * width - 0.5f;
  float y = v * height - 0.5f;
  if (mag_filter == kNearest) {
    return Texel(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
  }
  // Bilinear (GL_LINEAR).
  float x0 = std::floor(x);
  float y0 = std::floor(y);
  float fx = x - x0;
  float fy = y - y0;
  int ix = static_cast<int>(x0);
  int iy = static_cast<int>(y0);
  std::array<float, 4> c00 = Texel(ix, iy);
  std::array<float, 4> c10 = Texel(ix + 1, iy);
  std::array<float, 4> c01 = Texel(ix, iy + 1);
  std::array<float, 4> c11 = Texel(ix + 1, iy + 1);
  std::array<float, 4> out{};
  for (int c = 0; c < 4; ++c) {
    float top = c00[c] + (c10[c] - c00[c]) * fx;
    float bot = c01[c] + (c11[c] - c01[c]) * fx;
    out[c] = top + (bot - top) * fy;
  }
  return out;
}

void SoftGlBackend::DrawArrays(GLenum mode, const GlVertexArrays& arrays) {
  if (!arrays.has_position || arrays.vertex_count < 3) return;

  // Constrói vértices em clip-space combinando MODELVIEW e PROJECTION.
  Matrix mvp = Multiply(projection_.back(), modelview_.back());
  int psz = arrays.position_size;
  std::vector<Vertex> verts(arrays.vertex_count);
  for (int i = 0; i < arrays.vertex_count; ++i) {
    std::array<float, 4> obj{0, 0, 0, 1};
    obj[0] = arrays.positions[i * psz + 0];
    obj[1] = psz > 1 ? arrays.positions[i * psz + 1] : 0.0f;
    obj[2] = psz > 2 ? arrays.positions[i * psz + 2] : 0.0f;
    obj[3] = psz > 3 ? arrays.positions[i * psz + 3] : 1.0f;
    verts[i].position = Transform(mvp, obj);
    if (arrays.has_color) {
      verts[i].color = {arrays.colors[i * 4 + 0], arrays.colors[i * 4 + 1],
                        arrays.colors[i * 4 + 2], arrays.colors[i * 4 + 3]};
    } else {
      verts[i].color = current_color_;
    }
    if (arrays.has_texcoord) {
      verts[i].uv = {arrays.texcoords[i * arrays.texcoord_size + 0],
                     arrays.texcoord_size > 1 ? arrays.texcoords[i * arrays.texcoord_size + 1]
                                              : 0.0f};
    } else {
      verts[i].uv = {0.0f, 0.0f};
    }
  }

  auto emit = [&](int a, int b, int c) { ClipAndDraw(verts[a], verts[b], verts[c]); };
  if (mode == kTriangles) {
    for (int i = 0; i + 2 < arrays.vertex_count; i += 3) emit(i, i + 1, i + 2);
  } else if (mode == kTriangleStrip) {
    for (int i = 0; i + 2 < arrays.vertex_count; ++i) {
      if (i & 1) emit(i + 1, i, i + 2);
      else emit(i, i + 1, i + 2);
    }
  } else if (mode == kTriangleFan) {
    for (int i = 1; i + 1 < arrays.vertex_count; ++i) emit(0, i, i + 1);
  }
}

// Recorte no plano próximo (z + w >= 0) antes de projetar. Um triângulo
// pode gerar 0, 1 (mesmo) ou 2 triângulos após o corte.
void SoftGlBackend::ClipAndDraw(Vertex a, Vertex b, Vertex c) {
  Vertex tri[3] = {a, b, c};
  auto dist = [](const Vertex& v) { return v.position[2] + v.position[3]; };
  auto lerp = [](const Vertex& p, const Vertex& q, float t) {
    Vertex r;
    for (int i = 0; i < 4; ++i) r.position[i] = p.position[i] + (q.position[i] - p.position[i]) * t;
    for (int i = 0; i < 4; ++i) r.color[i] = p.color[i] + (q.color[i] - p.color[i]) * t;
    for (int i = 0; i < 2; ++i) r.uv[i] = p.uv[i] + (q.uv[i] - p.uv[i]) * t;
    return r;
  };
  auto clip = [&](const Vertex& p, const Vertex& q) {
    float dp = dist(p), dq = dist(q);
    float t = dp / (dp - dq);
    return lerp(p, q, t);
  };
  int in[3];
  int n = 0;
  for (int i = 0; i < 3; ++i)
    if (dist(tri[i]) >= 0.0f) in[n++] = i;
  if (n == 0) return;
  if (n == 3) {
    DrawTriangle(tri[0], tri[1], tri[2]);
    return;
  }
  if (n == 1) {
    int i = in[0];
    int ia = (i + 1) % 3, ib = (i + 2) % 3;
    DrawTriangle(tri[i], clip(tri[i], tri[ia]), clip(tri[i], tri[ib]));
    return;
  }
  // n == 2: um vértice fora.
  int out = 0;
  for (int i = 0; i < 3; ++i) {
    bool inside = (i == in[0] || i == in[1]);
    if (!inside) out = i;
  }
  int ia = (out + 1) % 3, ib = (out + 2) % 3;
  Vertex ea = clip(tri[ia], tri[out]);
  Vertex eb = clip(tri[ib], tri[out]);
  DrawTriangle(tri[ia], tri[ib], eb);
  DrawTriangle(tri[ia], eb, ea);
}

void SoftGlBackend::DrawTriangle(const Vertex& a, const Vertex& b, const Vertex& c) {
  RasterizePrepared(a, b, c);
}

void SoftGlBackend::RasterizePrepared(const Vertex& v0, const Vertex& v1, const Vertex& v2) {
  if (vp_w_ <= 0 || vp_h_ <= 0) return;
  const Vertex* v[3] = {&v0, &v1, &v2};

  // Divisão pela perspectiva -> NDC -> tela. Guarda inv_w (=1/w) para
  // interpolação perspectiva-correta.
  std::array<float, 4> scr[3];
  for (int i = 0; i < 3; ++i) {
    float w = v[i]->position[3];
    if (w == 0.0f) return;
    float inv_w = 1.0f / w;
    float x = v[i]->position[0] * inv_w;
    float y = v[i]->position[1] * inv_w;
    float z = v[i]->position[2] * inv_w;
    scr[i] = {vp_x_ + (x * 0.5f + 0.5f) * vp_w_,
              vp_y_ + (0.5f - y * 0.5f) * vp_h_,  // Y invertido (topo = 0)
              z * 0.5f + 0.5f, inv_w};
  }

  float area = Edge(scr[0], scr[1], scr[2]);
  if (area == 0.0f) return;

  // Caixa envolvente recortada ao viewport e à tela.
  float fminx = std::min({scr[0][0], scr[1][0], scr[2][0]});
  float fmaxx = std::max({scr[0][0], scr[1][0], scr[2][0]});
  float fminy = std::min({scr[0][1], scr[1][1], scr[2][1]});
  float fmaxy = std::max({scr[0][1], scr[1][1], scr[2][1]});
  int min_x = std::max({static_cast<int>(std::floor(fminx)), vp_x_, 0});
  int max_x = std::min({static_cast<int>(std::ceil(fmaxx)), vp_x_ + vp_w_, width_});
  int min_y = std::max({static_cast<int>(std::floor(fminy)), vp_y_, 0});
  int max_y = std::min({static_cast<int>(std::ceil(fmaxy)), vp_y_ + vp_h_, height_});
  if (min_x >= max_x || min_y >= max_y) return;

  float inv_area = 1.0f / area;
  // Atributos divididos por w para corrigir perspectiva; voltam multiplicados
  // pelo w interpolado no fragmento.
  std::array<float, 6> ow[3];
  for (int i = 0; i < 3; ++i) {
    float w = scr[i][3];
    ow[i] = {v[i]->color[0] * w, v[i]->color[1] * w, v[i]->color[2] * w,
             v[i]->color[3] * w, v[i]->uv[0] * w, v[i]->uv[1] * w};
  }

  const Texture* tex = nullptr;
  if (texture_2d_ && bound_texture_ != 0) {
    auto it = textures_.find(bound_texture_);
    if (it != textures_.end() && it->second.width > 0) tex = &it->second;
  }

  for (int y = min_y; y < max_y; ++y) {
    for (int x = min_x; x < max_x; ++x) {
      std::array<float, 4> p{x + 0.5f, y + 0.5f, 0.0f, 0.0f};
      float w0 = Edge(scr[1], scr[2], p) * inv_area;
      float w1 = Edge(scr[2], scr[0], p) * inv_area;
      float w2 = Edge(scr[0], scr[1], p) * inv_area;
      if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

      float z = w0 * scr[0][2] + w1 * scr[1][2] + w2 * scr[2][2];
      size_t idx = static_cast<size_t>(y) * width_ + x;
      if (depth_test_ && !Compare(depth_func_, z, depth_[idx])) continue;

      float inv_w = w0 * scr[0][3] + w1 * scr[1][3] + w2 * scr[2][3];
      if (inv_w == 0.0f) continue;
      float rw = 1.0f / inv_w;
      auto attr = [&](int k) {
        return (w0 * ow[0][k] + w1 * ow[1][k] + w2 * ow[2][k]) * rw;
      };
      std::array<float, 4> src{attr(0), attr(1), attr(2), attr(3)};
      if (tex) {
        std::array<float, 4> texel = tex->Sample(attr(4), attr(5));
        // GL_MODULATE (padrão fixed-function; o que o ddragonz usa).
        for (int c = 0; c < 4; ++c) src[c] *= texel[c];
      }

      if (alpha_test_ && !Compare(alpha_func_, src[3], alpha_ref_)) continue;

      std::array<float, 3> dst_rgb = FromRgb565(framebuffer_[idx]);
      std::array<float, 4> out;
      if (blend_) {
        std::array<float, 4> dst{dst_rgb[0], dst_rgb[1], dst_rgb[2], 1.0f};
        for (int c = 0; c < 4; ++c) {
          float s = BlendFactor(blend_src_, src, dst, c);
          float d = BlendFactor(blend_dst_, src, dst, c);
          out[c] = std::max(0.0f, std::min(1.0f, src[c] * s + dst[c] * d));
        }
      } else {
        out = src;
      }
      framebuffer_[idx] = ToRgb565(out[0], out[1], out[2]);
      if (depth_test_ && depth_mask_) depth_[idx] = z;
    }
  }
}

}  // namespace zeebulator
