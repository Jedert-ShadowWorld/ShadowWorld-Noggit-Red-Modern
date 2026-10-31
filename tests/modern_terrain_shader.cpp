// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <QGuiApplication>
#include <QFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_1_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
  QGuiApplication application(argc, argv);
  QSurfaceFormat format;
  format.setVersion(4, 1);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  if (!context.create() || !context.makeCurrent(&surface))
  {
    std::cerr << "SKIP: Cannot create OpenGL 4.1 context.\n";
    return 77;
  }
  auto* gl = context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  if (!gl || !gl->initializeOpenGLFunctions())
  {
    std::cerr << "SKIP: OpenGL 4.1 functions are unavailable.\n";
    return 77;
  }
  QOpenGLShaderProgram program;
  if (argc != 2)
  {
    std::cerr << "Usage: modern-terrain-shader-test <shader-directory>\n";
    return 1;
  }
  QString const path = QString::fromLocal8Bit(argv[1]) + "/";
  auto read_shader = [&](QString const& name)
  {
    QFile file(path + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
      return QByteArray{};
    auto source = file.readAll();
    auto const version = source.indexOf("#version");
    return version >= 0 ? source.mid(version) : source;
  };
  if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, read_shader("terrain_vert.glsl"))
      || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, read_shader("terrain_frag.glsl"))
      || !program.link())
  {
    std::cerr << "FAIL: " << program.log().toStdString() << '\n';
    return 1;
  }
  auto const block = gl->glGetUniformBlockIndex(program.programId(), "chunk_instances");
  GLint size = 0;
  gl->glGetActiveUniformBlockiv(program.programId(), block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
  if (size != 240 * 256)
  {
    std::cerr << "FAIL: Terrain uniform layout size " << size << '\n';
    return 1;
  }
  auto const ext_block = gl->glGetUniformBlockIndex(program.programId(), "chunk_layer_ext");
  gl->glGetActiveUniformBlockiv(program.programId(), ext_block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
  if (size != 16 * 3072)
  {
    std::cerr << "FAIL: Extended terrain uniform layout size " << size << '\n';
    return 1;
  }
  std::cout << "PASS: Terrain shaders compile/link on " << gl->glGetString(GL_RENDERER)
            << "; uniform block matches 240-byte CPU stride.\n";

  // Keep the real blend function, isolating it from lighting and terrain geometry.
  auto fragment = read_shader("terrain_frag.glsl");
  fragment.truncate(fragment.indexOf("void main()"));
  fragment += "void main() { out_color = terrain_texture_blend(); }\n";
  QOpenGLShaderProgram blend;
  char const* vertex = R"(#version 410 core
out vec2 vary_texcoord, vary_t0_uv, vary_t1_uv, vary_t2_uv, vary_t3_uv;
flat out int instanceID;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  vary_texcoord = vary_t0_uv = vary_t1_uv = vary_t2_uv = vary_t3_uv = vec2(0.5);
  instanceID = 0;
}
)";
  if (!blend.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
      || !blend.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !blend.link())
  {
    std::cerr << "FAIL: blend fixture " << blend.log().toStdString() << '\n';
    return 1;
  }
  blend.bind();
  blend.setUniformValue("enable_mists_heightmapping", true);
  blend.setUniformValue("retail_terrain", true);
  auto upload_texture = [&](int unit, int depth, float const* pixels)
  {
    GLuint texture;
    gl->glGenTextures(1, &texture);
    gl->glActiveTexture(GL_TEXTURE0 + unit);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, 1, 1, depth, 0, GL_RGBA, GL_FLOAT, pixels);
    return texture;
  };
  float const diffuse[] = {1,0,0,1, 0,1,0,1, 0,0,1,1, 1,1,1,1};
  std::array<float, 16 * 4> heights{};
  heights[1 * 4 + 3] = heights[15 * 4 + 3] = 1.0f;
  heights[7 * 4 + 3] = 0.25f;
  std::vector<float> alpha(1280 * 4, 0.0f);
  alpha[0] = 0.25f;
  auto const diffuse_texture = upload_texture(5, 4, diffuse);
  auto const height_texture = upload_texture(6, 16, heights.data());
  auto const alpha_texture = upload_texture(3, 1280, alpha.data());
  int samplers[13];
  for (int i = 0; i < 13; ++i)
  {
    samplers[i] = 5 + i;
    if (i < 2) continue;
    gl->glActiveTexture(GL_TEXTURE0 + samplers[i]);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, diffuse_texture);
  }
  blend.setUniformValueArray("textures", samplers, 13);
  blend.setUniformValue("alphamap", 3);

  std::array<std::array<std::uint32_t, 60>, 256> instances{};
  auto set_float = [&](int word, float value) { std::memcpy(&instances[0][word], &value, 4); };
  instances[0][4] = 0; instances[0][5] = 1; // Diffuse array layers.
  instances[0][10] = 2; // Two terrain layers.
  instances[0][32] = instances[0][33] = 1; // Height sampler.
  instances[0][36] = 1; instances[0][37] = 0; // Independent, reversed height layers.
  for (int i = 0; i < 4; ++i) set_float(48 + i, 1.0f);
  std::array<std::array<std::uint32_t, 4>, 3072> ext_layers{};
  auto set_ext_float = [&](int layer, int word, float value)
  {
    std::memcpy(&ext_layers[layer][word], &value, sizeof(value));
  };
  for (int i = 0; i < 12; ++i)
  {
    ext_layers[i][1] = 2; // Blue, no animation, neutral height.
    set_ext_float(i, 3, 1.0f);
  }
  auto set_layer = [&](int layer, int diffuse_index, int height_index, float height_scale, float height_offset)
  {
    if (layer < 4)
    {
      instances[0][layer] = 0;
      instances[0][4 + layer] = diffuse_index;
      instances[0][32 + layer] = 1;
      instances[0][36 + layer] = height_index;
      instances[0][40 + layer] = 0;
      set_float(44 + layer, height_scale);
      set_float(48 + layer, height_offset);
    }
    else
    {
      ext_layers[layer - 4][0] = (1 << 15) | (height_index << 19);
      ext_layers[layer - 4][1] = diffuse_index;
      set_ext_float(layer - 4, 2, height_scale);
      set_ext_float(layer - 4, 3, height_offset);
    }
  };
  auto set_weight = [&](int layer, float weight)
  {
    if (layer > 0 && layer < 4) alpha[layer - 1] = weight;
    else if (layer >= 4) alpha[(256 + (layer - 4) / 3) * 4 + (layer - 4) % 3] = weight;
  };
  GLuint buffer, ext_buffer, vao;
  gl->glGenBuffers(1, &buffer);
  gl->glBindBuffer(GL_UNIFORM_BUFFER, buffer);
  gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(instances), instances.data(), GL_DYNAMIC_DRAW);
  gl->glUniformBlockBinding(blend.programId(), gl->glGetUniformBlockIndex(blend.programId(), "chunk_instances"), 0);
  gl->glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffer);
  gl->glGenBuffers(1, &ext_buffer);
  gl->glBindBuffer(GL_UNIFORM_BUFFER, ext_buffer);
  gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(ext_layers), ext_layers.data(), GL_DYNAMIC_DRAW);
  gl->glUniformBlockBinding(blend.programId(), gl->glGetUniformBlockIndex(blend.programId(), "chunk_layer_ext"), 1);
  gl->glBindBufferBase(GL_UNIFORM_BUFFER, 1, ext_buffer);
  gl->glGenVertexArrays(1, &vao);
  gl->glBindVertexArray(vao);
  QOpenGLFramebufferObjectFormat framebuffer_format;
  framebuffer_format.setInternalTextureFormat(GL_RGBA32F);
  QOpenGLFramebufferObject framebuffer(2, 2, framebuffer_format);
  framebuffer.bind();
  gl->glViewport(0, 0, 2, 2);
  auto check_pixel = [&](float red, float green, float blue = 0.0f, float specular = -1.0f)
  {
    gl->glBindBuffer(GL_UNIFORM_BUFFER, ext_buffer);
    gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ext_layers), ext_layers.data());
    gl->glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(instances), instances.data());
    gl->glActiveTexture(GL_TEXTURE3);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, alpha_texture);
    gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1280, GL_RGBA, GL_FLOAT, alpha.data());
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    std::array<float, 4> pixel{};
    gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel.data());
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2])
      && std::abs(pixel[0] - red) < 0.002f && std::abs(pixel[1] - green) < 0.002f
      && std::abs(pixel[2] - blue) < 0.002f
      && (specular < 0.0f || (std::isfinite(pixel[3]) && std::abs(pixel[3] - specular) < 0.002f));
  };
  bool const fallback_ok = check_pixel(0.75f, 0.25f);
  set_float(44, 1.0f); set_float(45, 1.0f);
  set_float(48, 0.0f); set_float(49, 0.0f);
  bool const height_ok = check_pixel(1.0f, 0.0f);
  set_float(44, 0.0f); set_float(45, 0.0f);
  set_float(50, 0.0f); set_float(51, 0.0f);
  bool const zero_ok = check_pixel(0.75f, 0.25f);

  for (int i = 0; i < 4; ++i)
  {
    instances[0][4 + i] = i;
    set_float(44 + i, 0.0f);
    set_float(48 + i, 1.0f);
  }
  instances[0][10] = 4;
  alpha[0] = 0.3f; alpha[1] = 0.2f; alpha[2] = 0.1f;
  bool const four_ok = check_pixel(0.5f, 0.4f, 0.3f);
  blend.setUniformValue("retail_terrain", false);
  bool non_retail_ok = check_pixel(0.5f, 0.4f, 0.3f);
  blend.setUniformValue("retail_terrain", true);
  instances[0][10] = 5;
  alpha[1] = alpha[2] = 0.0f;
  alpha[256 * 4] = 0.5f;
  bool const five_ok = check_pixel(0.2f, 0.3f, 0.5f);
  // The non-Retail modern-features option retains its historical first-four
  // blend and overlays extension alpha afterwards: (.7, .3, 0) * .5 + blue * .5.
  blend.setUniformValue("retail_terrain", false);
  non_retail_ok &= check_pixel(0.35f, 0.15f, 0.5f);
  blend.setUniformValue("retail_terrain", true);
  blend.setUniformValue("enable_mists_heightmapping", false);
  bool const regular_ok = check_pixel(0.2f, 0.3f, 0.5f);
  blend.setUniformValue("enable_mists_heightmapping", true);
  alpha[0] = 0.5f;
  bool const zero_base_ok = check_pixel(0.0f, 0.5f, 0.5f);
  alpha[0] = 0.0f; alpha[256 * 4] = 1.0f;
  bool const extended_only_ok = check_pixel(0.0f, 0.0f, 1.0f);

  // Moving a texture across the fourth/fifth layer boundary must preserve
  // the neutral-height result when its absolute alpha is unchanged.
  instances[0][4] = 2;
  ext_layers[0][1] = 0;
  alpha[0] = 0.3f; alpha[256 * 4] = 0.2f;
  bool const permutation_ok = check_pixel(0.2f, 0.3f, 0.5f);
  instances[0][4] = 0;
  ext_layers[0][1] = 2;
  alpha[256 * 4] = 0.5f;
  set_float(44, 1.0f); set_float(45, 1.0f);
  set_float(48, 0.0f); set_float(49, 0.0f);
  // Scores .2/0/.5 become .14/0/.5 after global height competition.
  bool const global_height_ok = check_pixel(0.21875f, 0.0f, 0.78125f);
  blend.setUniformValue("retail_terrain", false);
  non_retail_ok &= check_pixel(0.5f, 0.0f, 0.5f);
  blend.setUniformValue("retail_terrain", true);
  for (int i = 0; i < 4; ++i)
  {
    set_float(44 + i, 0.0f);
    set_float(48 + i, 1.0f);
  }
  instances[0][10] = 16;
  alpha[0] = alpha[1] = alpha[2] = 1.0f / 16.0f;
  for (int i = 0; i < 12; ++i)
    alpha[(256 + i / 3) * 4 + i % 3] = 1.0f / 16.0f;
  bool const sixteen_ok = check_pixel(0.125f, 0.125f, 0.875f);

  // A height-dominant material with equal alpha must give the same result
  // in slots four and five; the former split formula returned a 50/50 mix.
  bool height_permutation_ok = true;
  for (int dominant_layer : {3, 4})
  {
    std::fill(alpha.begin(), alpha.end(), 0.0f);
    instances[0][10] = 5;
    for (int i = 0; i < 5; ++i) set_layer(i, 0, 0, 0.0f, 1.0f);
    set_weight(dominant_layer, 0.5f);
    set_layer(dominant_layer, 2, 15, 10.0f, 1.0f);
    height_permutation_ok &= check_pixel(0.0f, 0.0f, 1.0f);
    blend.setUniformValue("retail_terrain", false);
    non_retail_ok &= dominant_layer == 3 ? check_pixel(0.0f, 0.0f, 1.0f)
                                         : check_pixel(0.5f, 0.0f, 0.5f);
    blend.setUniformValue("enable_mists_heightmapping", false);
    non_retail_ok &= check_pixel(0.5f, 0.0f, 0.5f);
    blend.setUniformValue("retail_terrain", true);
    height_permutation_ok &= check_pixel(0.5f, 0.0f, 0.5f);
    blend.setUniformValue("enable_mists_heightmapping", true);
  }

  // Exercise all slices/channels and an implicit layer-zero weight. With 16
  // equal weights, scores are 11/16 and 1/16; normalized high weight is 88/133.
  bool many_height_ok = true;
  for (int count : {8, 16})
    for (int dominant_layer = 0; dominant_layer < count; ++dominant_layer)
    {
      std::fill(alpha.begin(), alpha.end(), 0.0f);
      instances[0][10] = count;
      for (int i = 0; i < count; ++i)
      {
        set_layer(i, 0, 0, 0.0f, 1.0f);
        set_weight(i, 1.0f / count);
      }
      set_layer(dominant_layer, 2, 15, 10.0f, 1.0f);
      float const blue = count == 8 ? 1.0f : 88.0f / 133.0f;
      many_height_ok &= check_pixel(1.0f - blue, 0.0f, blue);
    }

  // Azeroth 41_16, chunk (6,6)/(6,7), edge texel 21: leaf 6268514 moves
  // from slot four to five while MCAL stays 132/255 (road 6195745: 123/255).
  // Use those real weights/MTXP values with controlled height samples, so
  // this regression needs no copyrighted client texture assets.
  bool seam_ok = true;
  for (int height_slice : {0, 7, 15})
    for (int leaf_layer : {3, 4})
    {
      std::fill(alpha.begin(), alpha.end(), 0.0f);
      instances[0][10] = leaf_layer == 3 ? 5 : 6;
      for (int i = 0; i < 6; ++i) set_layer(i, 1, 0, 0.0f, 1.0f);
      set_layer(0, 0, height_slice, 8.0f, 1.0f);
      set_layer(leaf_layer, 2, height_slice, 10.0f, 1.0f);
      instances[0][40] = 2; // Road MTXP UV scale.
      if (leaf_layer < 4) instances[0][40 + leaf_layer] = 1;
      else ext_layers[leaf_layer - 4][0] |= 1 << 11;
      set_weight(leaf_layer, 132.0f / 255.0f);
      double const h = heights[height_slice * 4 + 3];
      double const road = (123.0 / 255.0) * (8.0 * h + 1.0);
      double const leaf = (132.0 / 255.0) * (10.0 * h + 1.0);
      double const surviving_road = road * std::max(0.0, 1.0 - (leaf - road));
      float const blue = float(leaf / (leaf + surviving_road));
      seam_ok &= check_pixel(1.0f - blue, 0.0f, blue);
    }

  // Missing/all-zero height contributions retain absolute alpha for every
  // layer, including an extended-only material and inactive base materials.
  bool extended_zero_ok = true;
  for (int i = 0; i < 16; ++i) set_layer(i, 0, 0, 0.0f, 0.0f);
  set_layer(15, 2, 0, 0.0f, 0.0f);
  instances[0][10] = 16;
  std::fill(alpha.begin(), alpha.end(), 0.0f);
  set_weight(15, 0.75f);
  extended_zero_ok &= check_pixel(0.25f, 0.0f, 0.75f);
  set_weight(15, 1.0f);
  extended_zero_ok &= check_pixel(0.0f, 0.0f, 1.0f);

  // Signed slice zero must stay non-specular after sharing an array page.
  bool signed_slice_ok = true;
  for (int layer : {0, 4})
    for (bool retail : {false, true})
      for (bool regular : {false, true})
        for (int slice : {0, 1})
        {
          std::fill(alpha.begin(), alpha.end(), 0.0f);
          instances[0][10] = layer + 1;
          for (int i = 0; i <= layer; ++i) set_layer(i, 0, 0, 0.0f, 1.0f);
          set_layer(layer, -(slice + 1), 0, 0.0f, 1.0f);
          set_weight(layer, 1.0f);
          blend.setUniformValue("retail_terrain", retail);
          blend.setUniformValue("enable_mists_heightmapping", !regular);
          signed_slice_ok &= check_pixel(slice == 0 ? 1.0f : 0.0f, slice == 1 ? 1.0f : 0.0f, 0.0f, 0.0f);
        }
  blend.setUniformValue("enable_mists_heightmapping", true);
  std::fill(alpha.begin(), alpha.end(), 0.0f);
  for (int i = 0; i < 16; ++i) set_layer(i, 0, 0, 0.0f, 1.0f);
  instances[0][10] = 1;
  signed_slice_ok &= check_pixel(1.0f, 0.0f, 0.0f, 1.0f);

  // The last terrain samplers use units 16/17; fragment texture units 0/1
  // remain available to the production vertex-only height/MCCV samplers.
  instances[0][10] = 1;
  instances[0][0] = 11; instances[0][4] = 1;
  bool const sampler11_ok = check_pixel(0.0f, 1.0f, 0.0f);
  instances[0][0] = 12; instances[0][4] = 2;
  bool const sampler12_ok = check_pixel(0.0f, 0.0f, 1.0f);
  instances[0][0] = 0;

  // A fully dominant texture must keep its MTXP scale and world phase when
  // moved from the fourth to the fifth layer. Neutral heights isolate UVs.
  std::array<float, 16 * 16 * 4> pattern{};
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x)
    {
      auto const pixel = (y * 16 + x) * 4;
      pattern[pixel] = x / 15.0f;
      pattern[pixel + 1] = y / 15.0f;
      pattern[pixel + 2] = 0.25f;
      pattern[pixel + 3] = 1.0f;
    }
  GLuint pattern_texture;
  gl->glGenTextures(1, &pattern_texture);
  gl->glActiveTexture(GL_TEXTURE5);
  gl->glBindTexture(GL_TEXTURE_2D_ARRAY, pattern_texture);
  gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, 16, 16, 1, 0, GL_RGBA, GL_FLOAT, pattern.data());
  std::fill(alpha.begin(), alpha.end(), 0.0f);
  for (int i = 0; i < 4; ++i) instances[0][4 + i] = 0;
  instances[0][24] = 3; instances[0][25] = 5;
  ext_layers[0][0] = ext_layers[0][1] = 0;
  bool uv_scale_ok = true;
  for (int scale : {1, 2, 3, 4})
  {
    auto expected = [scale](int chunk)
    {
      float const uv = (0.5f + 8.0f * chunk) / float(1 << scale);
      return std::floor((uv - std::floor(uv)) * 16.0f) / 15.0f;
    };
    instances[0][10] = 4;
    instances[0][43] = scale;
    alpha[2] = 1.0f; alpha[256 * 4] = 0.0f;
    uv_scale_ok &= check_pixel(expected(3), expected(5), 0.25f);
    instances[0][10] = 5;
    ext_layers[0][0] = scale << 11;
    alpha[2] = 0.0f; alpha[256 * 4] = 1.0f;
    uv_scale_ok &= check_pixel(expected(3), expected(5), 0.25f);
    blend.setUniformValue("retail_terrain", false);
    non_retail_ok &= check_pixel(8.0f / 15.0f, 8.0f / 15.0f, 0.25f);
    blend.setUniformValue("retail_terrain", true);
    blend.setUniformValue("enable_mists_heightmapping", false);
    uv_scale_ok &= check_pixel(8.0f / 15.0f, 8.0f / 15.0f, 0.25f);
    blend.setUniformValue("enable_mists_heightmapping", true);
  }
  ext_layers[0][0] = (7 << 5) | (2 << 8) | (2 << 11); // Speed/direction without enable bit.
  blend.setUniformValue("animtime", 800);
  bool const ext_animation_disabled_ok = check_pixel(2.0f / 15.0f, 2.0f / 15.0f, 0.25f);
  ext_layers[0][0] |= 1 << 4;
  bool const ext_animation_enabled_ok = check_pixel(0.0f, 2.0f / 15.0f, 0.25f);

  // Exercise the production vertex path too: the blend fixture above supplies
  // fixed UVs and cannot detect disabled or stale per-layer animation metadata.
  QOpenGLShaderProgram animated_vertex;
  bool animation_ok = animated_vertex.addShaderFromSourceCode(QOpenGLShader::Vertex, read_shader("terrain_vert.glsl"))
    && animated_vertex.addShaderFromSourceCode(QOpenGLShader::Fragment,
      "#version 410 core\nout vec4 color; void main(){color=vec4(1.0);}\n");
  char const* feedback_names[]{"vary_t0_uv", "vary_t1_uv", "vary_t2_uv", "vary_t3_uv", "gl_Position"};
  gl->glTransformFeedbackVaryings(animated_vertex.programId(), 5, feedback_names, GL_INTERLEAVED_ATTRIBS);
  animation_ok = animation_ok && animated_vertex.link();
  bool holes_ok = animation_ok;
  if (animation_ok)
  {
    animated_vertex.bind();
    animated_vertex.setUniformValue("heightmap", 0);
    animated_vertex.setUniformValue("mccv", 1);
    animated_vertex.setUniformValue("base_instance", 0);
    animated_vertex.setUniformValue("lod_level", 0);
    animated_vertex.setUniformValue("animtime", 800);
    gl->glUniformBlockBinding(animated_vertex.programId(), gl->glGetUniformBlockIndex(animated_vertex.programId(), "chunk_instances"), 0);
    gl->glUniformBlockBinding(animated_vertex.programId(), gl->glGetUniformBlockIndex(animated_vertex.programId(), "matrices"), 2);
    std::array<float, 32> matrices{};
    for (int i = 0; i < 4; ++i) matrices[i * 5] = matrices[16 + i * 5] = 1.0f;
    GLuint matrices_buffer, feedback_buffer, vertex_textures[2];
    gl->glGenBuffers(1, &matrices_buffer);
    gl->glBindBuffer(GL_UNIFORM_BUFFER, matrices_buffer);
    gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(matrices), matrices.data(), GL_STATIC_DRAW);
    gl->glBindBufferBase(GL_UNIFORM_BUFFER, 2, matrices_buffer);
    gl->glGenTextures(2, vertex_textures);
    std::array<float, 145 * 4> vertex_data{};
    for (int i = 0; i < 145; ++i) vertex_data[i * 4 + 1] = 1.0f;
    for (int i = 0; i < 2; ++i)
    {
      gl->glActiveTexture(GL_TEXTURE0 + i);
      gl->glBindTexture(GL_TEXTURE_2D, vertex_textures[i]);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 145, 1, 0, GL_RGBA, GL_FLOAT, vertex_data.data());
    }
    gl->glGenBuffers(1, &feedback_buffer);
    gl->glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffer);
    gl->glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 145 * 12 * sizeof(float), nullptr, GL_STREAM_READ);
    gl->glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback_buffer);
    animated_vertex.setAttributeValue("texcoord", 0.25f, 0.75f);
    instances[0][8] = 0; // No holes.
    for (int i = 0; i < 4; ++i)
    {
      instances[0][12 + i] = 0; // Disabled; stale speed/direction must not matter.
      instances[0][16 + i] = 7;
      instances[0][28 + i] = 999;
    }
    auto capture_uvs = [&]
    {
      gl->glBindBuffer(GL_UNIFORM_BUFFER, buffer);
      gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(instances), instances.data());
      gl->glEnable(GL_RASTERIZER_DISCARD);
      gl->glBeginTransformFeedback(GL_POINTS);
      gl->glDrawArrays(GL_POINTS, 0, 1);
      gl->glEndTransformFeedback();
      gl->glDisable(GL_RASTERIZER_DISCARD);
      std::array<float, 8> result{};
      gl->glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(result), result.data());
      return result;
    };
    auto const disabled = capture_uvs();
    for (int i = 0; i < 4; ++i)
      animation_ok &= std::abs(disabled[i * 2] - 0.25f) < 0.0001f
        && std::abs(disabled[i * 2 + 1] - 0.75f) < 0.0001f;
    instances[0][12] = 1; instances[0][28] = 2;
    instances[0][13] = 1; instances[0][29] = 8; // Invalid direction must be safe too.
    auto const enabled = capture_uvs();
    animation_ok &= std::abs(enabled[0] + 0.25f) < 0.0001f
      && std::abs(enabled[1] - 0.75f) < 0.0001f
      && std::abs(enabled[2] - 0.25f) < 0.0001f
      && std::abs(enabled[3] - 0.75f) < 0.0001f;

    // Capture all production vertices, not a reimplementation of the shader.
    // A fine hole must remove only its fan center, never the shared corners
    // or the other three cells of its old coarse block.
    auto check_holes = [&](std::uint64_t mask, bool retail, unsigned coarse, int lod)
    {
      animated_vertex.setUniformValue("retail_terrain", retail);
      animated_vertex.setUniformValue("lod_level", lod);
      instances[0][8] = coarse;
      instances[0][21] = static_cast<std::int32_t>(mask);
      instances[0][22] = static_cast<std::int32_t>(mask >> 32);
      gl->glBindBuffer(GL_UNIFORM_BUFFER, buffer);
      gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(instances), instances.data());
      gl->glEnable(GL_RASTERIZER_DISCARD);
      gl->glBeginTransformFeedback(GL_POINTS);
      gl->glDrawArrays(GL_POINTS, 0, 145);
      gl->glEndTransformFeedback();
      gl->glDisable(GL_RASTERIZER_DISCARD);
      std::array<float, 145 * 12> result{};
      gl->glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(result), result.data());
      for (int vertex = 0; vertex < 145; ++vertex)
      {
        int const row = vertex / 17;
        int const column = vertex % 17;
        bool expected = false;
        if (retail && row < 8 && column >= 9)
          expected = (mask & (std::uint64_t{1} << (row * 8 + column - 9))) != 0;
        else if (!retail && lod == 0 && row < 8 && column >= 9)
          expected = (coarse & (1u << ((row / 2) * 4 + (column - 9) / 2))) != 0;
        else if (!retail && lod == 1 && row < 8 && row % 2 == 1
                 && column < 9 && column % 2 == 1)
          expected = (coarse & (1u << ((row / 2) * 4 + column / 2))) != 0;
        bool const removed = !std::isfinite(result[vertex * 12 + 8]);
        if (removed != expected)
        {
          std::cerr << "FAIL: hole vertex " << vertex << " retail=" << retail << " lod=" << lod << '\n';
          return false;
        }
      }
      return true;
    };
    holes_ok &= check_holes(0, true, 0xffff, 0); // Ignore stale coarse bits.
    holes_ok &= check_holes(~std::uint64_t{0}, true, 0, 0);
    for (int cell = 0; cell < 64; ++cell)
      holes_ok &= check_holes(std::uint64_t{1} << cell, true, 0xffff, 0);
    for (int cell = 0; cell < 16; ++cell)
      for (int lod = 0; lod < 2; ++lod)
        holes_ok &= check_holes(~std::uint64_t{0}, false, 1u << cell, lod);

    // Rasterize the original 145-vertex grid and its four-triangle cell fans.
    // This catches drivers retaining a triangle whose center becomes NaN,
    // which transform feedback alone cannot establish.
    std::array<std::array<float, 2>, 145> grid{};
    std::array<std::uint16_t, 768> indices{};
    for (int y = 0; y < 9; ++y)
    {
      for (int x = 0; x < 9; ++x)
        grid[y * 17 + x] = {x / 4.0f - 1.0f, y / 4.0f - 1.0f};
      if (y == 8) continue;
      for (int x = 0; x < 8; ++x)
      {
        auto const center = static_cast<std::uint16_t>(y * 17 + 9 + x);
        grid[center] = {(x + 0.5f) / 4.0f - 1.0f, (y + 0.5f) / 4.0f - 1.0f};
        std::uint16_t const corners[]{static_cast<std::uint16_t>(y * 17 + x),
          static_cast<std::uint16_t>((y + 1) * 17 + x),
          static_cast<std::uint16_t>((y + 1) * 17 + x + 1),
          static_cast<std::uint16_t>(y * 17 + x + 1)};
        for (int triangle = 0; triangle < 4; ++triangle)
        {
          int const start = (y * 8 + x) * 12 + triangle * 3;
          indices[start] = center;
          indices[start + 1] = corners[triangle];
          indices[start + 2] = corners[(triangle + 1) % 4];
        }
      }
    }
    GLuint geometry_buffers[2];
    gl->glGenBuffers(2, geometry_buffers);
    gl->glBindBuffer(GL_ARRAY_BUFFER, geometry_buffers[0]);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(grid), grid.data(), GL_STATIC_DRAW);
    animated_vertex.enableAttributeArray("position");
    animated_vertex.setAttributeBuffer("position", GL_FLOAT, 0, 2, 2 * sizeof(float));
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry_buffers[1]);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices.data(), GL_STATIC_DRAW);
    // Top-down view: world X/Z become screen X/Y, with zero terrain height.
    std::fill(matrices.begin(), matrices.begin() + 16, 0.0f);
    matrices[0] = matrices[6] = matrices[9] = matrices[15] = 1.0f;
    gl->glBindBuffer(GL_UNIFORM_BUFFER, matrices_buffer);
    gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(matrices), matrices.data());
    for (int i = 24; i < 28; ++i) instances[0][i] = 0;
    animated_vertex.setUniformValue("retail_terrain", true);
    animated_vertex.setUniformValue("lod_level", 0);
    QOpenGLFramebufferObject hole_framebuffer(64, 64);
    holes_ok &= hole_framebuffer.isValid() && hole_framebuffer.bind();
    gl->glViewport(0, 0, 64, 64);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_CULL_FACE);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glClearColor(0, 0, 0, 1);
    for (int i = 0; i < 2; ++i)
    {
      gl->glActiveTexture(GL_TEXTURE0 + i);
      gl->glBindTexture(GL_TEXTURE_2D, vertex_textures[i]);
    }
    for (std::uint64_t const mask : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 31,
          std::uint64_t{1} << 32, std::uint64_t{3} << 31, std::uint64_t{1} << 63, ~std::uint64_t{0}})
    {
      instances[0][21] = static_cast<std::uint32_t>(mask);
      instances[0][22] = static_cast<std::uint32_t>(mask >> 32);
      gl->glBindBuffer(GL_UNIFORM_BUFFER, buffer);
      gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(instances), instances.data());
      gl->glClear(GL_COLOR_BUFFER_BIT);
      gl->glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr);
      std::array<std::uint8_t, 64 * 64 * 4> pixels{};
      gl->glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      bool coverage_ok = true;
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
          bool const hole = (mask & (std::uint64_t{1} << ((y / 8) * 8 + x / 8))) != 0;
          coverage_ok &= pixels[(y * 64 + x) * 4] == (hole ? 0 : 255);
        }
      if (!coverage_ok)
        std::cerr << "FAIL: rasterized terrain hole coverage for mask " << mask << '\n';
      holes_ok &= coverage_ok;
    }
    animated_vertex.disableAttributeArray("position");
    gl->glDeleteBuffers(2, geometry_buffers);
    framebuffer.bind();
    gl->glDeleteTextures(2, vertex_textures);
    gl->glDeleteBuffers(1, &feedback_buffer);
    gl->glDeleteBuffers(1, &matrices_buffer);
  }
  else
    std::cerr << "FAIL: terrain vertex animation fixture " << animated_vertex.log().toStdString() << '\n';
  bool const gl_ok = gl->glGetError() == GL_NO_ERROR;
  framebuffer.release();
  gl->glDeleteTextures(1, &diffuse_texture);
  gl->glDeleteTextures(1, &height_texture);
  gl->glDeleteTextures(1, &alpha_texture);
  gl->glDeleteTextures(1, &pattern_texture);
  gl->glDeleteBuffers(1, &buffer);
  gl->glDeleteBuffers(1, &ext_buffer);
  gl->glDeleteVertexArrays(1, &vao);
  if (!fallback_ok || !height_ok || !zero_ok || !four_ok || !five_ok || !regular_ok
      || !zero_base_ok || !extended_only_ok || !permutation_ok || !global_height_ok || !sixteen_ok
      || !height_permutation_ok || !many_height_ok || !seam_ok || !extended_zero_ok || !signed_slice_ok || !non_retail_ok
      || !sampler11_ok || !sampler12_ok || !uv_scale_ok || !ext_animation_disabled_ok || !ext_animation_enabled_ok
      || !animation_ok || !holes_ok || !gl_ok)
  {
    std::cerr << "FAIL: GPU pixels: alpha=" << fallback_ok << " height=" << height_ok
              << " zero=" << zero_ok << " four=" << four_ok << " five=" << five_ok
              << " regular=" << regular_ok << " zero-base=" << zero_base_ok
              << " extended-only=" << extended_only_ok << " permutation=" << permutation_ok
              << " global-height=" << global_height_ok << " sixteen=" << sixteen_ok
              << " height-permutation=" << height_permutation_ok << " height8/16=" << many_height_ok
              << " non-retail=" << non_retail_ok << " retail-seam=" << seam_ok << " ext-zero=" << extended_zero_ok << " signed-slice=" << signed_slice_ok
              << " sampler11/12=" << sampler11_ok << '/' << sampler12_ok << " UV-scale=" << uv_scale_ok
              << " ext-animation=" << ext_animation_disabled_ok << '/' << ext_animation_enabled_ok
              << " vertex-animation=" << animation_ok << " holes=" << holes_ok << " gl=" << gl_ok << '\n';
    return 1;
  }
  std::cout << "PASS: GPU pixels verify MHID, zero-height safety, 4/5/16-layer weights, "
               "zero-base/extended-only blending, global height competition, real seam layer permutation, extended UV scale, "
               "Retail-only dispatch, original non-Retail height blending, 13 samplers, production vertex animation "
               "and exact Retail/legacy hole vertices.\n";
  return 0;
}
