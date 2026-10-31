#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_1_Core>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <noggit/rendering/WMOGroupRender.hpp>
#include <noggit/formats/WmoVertexColors.hpp>
#include <noggit/formats/WmoMaterial.hpp>

namespace
{
  void require(bool ok, char const* message)
  {
    if (!ok) throw std::runtime_error(message);
  }
  char const* vertexSource = R"GLSL(#version 410 core
uniform uint fixtureFlags;
uniform uint fixtureShader;
uniform uint fixtureAlphaMode;
uniform vec4 fixtureWeights;
uniform float fixtureBlendAlpha;
out vec3 f_position;
out vec3 f_normal;
out vec2 f_texcoord;
out vec2 f_texcoord_2;
out vec2 f_texcoord_3;
out vec2 f_texcoord_4;
out vec4 f_material_weights;
out vec2 f_env_texcoord;
out vec4 f_vertex_color;
out float f_material_blend_alpha;
flat out uint flags;
flat out uint shader;
flat out uint tex_array0;
flat out uint tex_array1;
flat out uint tex0;
flat out uint tex1;
flat out uint alpha_test_mode;
flat out uint sidn_color;
flat out uvec2 extra_textures[7];
void main()
{
  vec2 positions[3] = vec2[3](vec2(-1.,-1.), vec2(3.,-1.), vec2(-1.,3.));
  gl_Position = vec4(positions[gl_VertexID], 0., 1.);
  f_position = vec3(0.);
  f_normal = vec3(0.,0.,1.);
  f_texcoord = vec2(.5);
  f_texcoord_2 = vec2(.5);
  f_texcoord_3 = vec2(.5);
  f_texcoord_4 = vec2(.5);
  f_env_texcoord = vec2(.5);
  f_material_weights = fixtureWeights;
  for (uint i = 0u; i < 7u; ++i) extra_textures[i] = uvec2(i + 2u, 0u);
  f_vertex_color = vec4(1., 1., 1., fixtureBlendAlpha);
  f_material_blend_alpha = fixtureBlendAlpha;
  flags = fixtureFlags;
  shader = fixtureShader;
  tex_array0 = 0u;
  tex_array1 = 1u;
  tex0 = 0u;
  tex1 = 0u;
  alpha_test_mode = fixtureAlphaMode;
  sidn_color = 0u;
}
)GLSL";
}

int main(int argc, char** argv)
{
  QGuiApplication app(argc, argv);
  QSurfaceFormat format;
  format.setVersion(4, 1);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  if (!context.create() || !surface.isValid() || !context.makeCurrent(&surface))
  {
    std::cout << "SKIP: OpenGL 4.1 context unavailable\n";
    return 77;
  }
  auto* f = context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  if (!f || !f->initializeOpenGLFunctions()) return 77;
  try
  {
    using Noggit::Format::wmo_color_streams;
    auto const soleBlend = wmo_color_streams(false, true, 1);
    require(!soleBlend.lighting && soleBlend.blending == 0, "Sole CVERTS2 must bypass lighting fixups");
    auto const bothColors = wmo_color_streams(true, true, 2);
    require(bothColors.lighting == 0 && bothColors.blending == 1, "Lighting and blend streams must stay separate");
    auto const soleLighting = wmo_color_streams(true, false, 1);
    require(soleLighting.lighting == 0 && !soleLighting.blending, "Lighting-only MOCV must not drive material blending");
    require(!wmo_color_streams(true, true, 0).lighting && !wmo_color_streams(true, true, 1).blending,
        "Absent streams must use defaults rather than indexing absent chunks");
    require(argc == 2, "Expected production WMO fragment shader path");
    QFile fragment(QString::fromLocal8Bit(argv[1]));
    require(fragment.open(QIODevice::ReadOnly), "Cannot read production WMO fragment shader");
    // Compile the production source directly, without Qt's GLSL preamble rewriting.
    auto compile = [&](GLenum type, QByteArray const& source)
    {
      GLuint const shader = f->glCreateShader(type);
      auto const* text = source.constData();
      GLint const size = source.size();
      f->glShaderSource(shader, 1, &text, &size);
      f->glCompileShader(shader);
      GLint compiled = 0;
      f->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
      if (!compiled)
      {
        GLint length = 0;
        f->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        QByteArray log(length, '\0');
        f->glGetShaderInfoLog(shader, length, nullptr, log.data());
        f->glDeleteShader(shader);
        throw std::runtime_error(log.constData());
      }
      return shader;
    };
    GLuint const vertex = compile(GL_VERTEX_SHADER, vertexSource);
    GLuint const pixel = compile(GL_FRAGMENT_SHADER, fragment.readAll());
    GLuint const program = f->glCreateProgram();
    f->glAttachShader(program, vertex);
    f->glAttachShader(program, pixel);
    f->glLinkProgram(program);
    GLint linked = 0;
    f->glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
      GLint length = 0;
      f->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
      QByteArray log(length, '\0');
      f->glGetProgramInfoLog(program, length, nullptr, log.data());
      throw std::runtime_error(log.constData());
    }
    QFile productionVertex(QFileInfo(fragment).dir().filePath("wmo_vert.glsl"));
    require(productionVertex.open(QIODevice::ReadOnly), "Cannot read production WMO vertex shader");
    GLuint const realVertex = compile(GL_VERTEX_SHADER, productionVertex.readAll());
    GLuint const realProgram = f->glCreateProgram();
    f->glAttachShader(realProgram, realVertex);
    f->glAttachShader(realProgram, pixel);
    f->glLinkProgram(realProgram);
    f->glGetProgramiv(realProgram, GL_LINK_STATUS, &linked);
    if (!linked)
    {
      GLint length = 0;
      f->glGetProgramiv(realProgram, GL_INFO_LOG_LENGTH, &length);
      QByteArray log(length, '\0');
      f->glGetProgramInfoLog(realProgram, length, nullptr, log.data());
      throw std::runtime_error(log.constData());
    }
    f->glDeleteShader(realVertex);
    f->glDeleteShader(vertex);
    f->glDeleteShader(pixel);
    f->glUseProgram(program);
    f->glUniform1f(f->glGetUniformLocation(program, "fixtureBlendAlpha"), 1.f);

    GLuint vao = 0, lightingBuffer = 0, framebuffer = 0, color = 0;
    std::array<GLuint, 9> textures{};
    f->glGenVertexArrays(1, &vao);
    f->glBindVertexArray(vao);
    f->glGenBuffers(1, &lightingBuffer);
    f->glBindBuffer(GL_UNIFORM_BUFFER, lightingBuffer);
    std::array<float, 32> lighting{};
    f->glBufferData(GL_UNIFORM_BUFFER, sizeof(lighting), lighting.data(), GL_STATIC_DRAW);
    auto const block = f->glGetUniformBlockIndex(program, "lighting");
    require(block != GL_INVALID_INDEX, "Production lighting block is missing");
    f->glUniformBlockBinding(program, block, 0);
    f->glBindBufferBase(GL_UNIFORM_BUFFER, 0, lightingBuffer);
    f->glUniform1f(f->glGetUniformLocation(program, "preview_texture_lod_bias"), 0.f);
    f->glUniform1f(f->glGetUniformLocation(program, "sidn_intensity"), 0.f);
    f->glUniform3f(f->glGetUniformLocation(program, "camera"), 0.f, 0.f, 1.f);
    f->glUniform3f(f->glGetUniformLocation(program, "ambient_color"), 0.f, 0.f, 0.f);

    std::array<unsigned char, 4> const diffuse{64, 128, 192, 255}, darkLightmap{0, 0, 0, 255};
    std::array<std::array<unsigned char, 4>, 9> pixels{diffuse, darkLightmap,
      std::array<unsigned char, 4>{180, 20, 40, 255}, {20, 180, 40, 255}, {20, 40, 180, 255},
      {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}};
    f->glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
    for (unsigned index = 0; index < textures.size(); ++index)
    {
      f->glActiveTexture(GL_TEXTURE0 + index);
      f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[index]);
      auto const& pixel = pixels[index];
      f->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    }
    std::array<GLint, 15> samplers{};
    for (unsigned unit = 0; unit < samplers.size(); ++unit)
    {
      samplers[unit] = unit;
      f->glActiveTexture(GL_TEXTURE0 + unit);
      f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[unit < textures.size() ? unit : 1]);
    }
    f->glUniform1iv(f->glGetUniformLocation(program, "texture_samplers[0]"),
        static_cast<GLsizei>(samplers.size()), samplers.data());

    f->glGenTextures(1, &color);
    f->glBindTexture(GL_TEXTURE_2D, color);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    f->glGenFramebuffers(1, &framebuffer);
    f->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    require(f->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "WMO test framebuffer incomplete");
    GLenum const colorAttachment = GL_COLOR_ATTACHMENT0;
    f->glDrawBuffers(1, &colorAttachment);
    f->glViewport(0, 0, 4, 4);
    f->glDisable(GL_DEPTH_TEST);
    f->glDisable(GL_BLEND);
    f->glDisable(GL_DITHER);
    f->glDisable(GL_FRAMEBUFFER_SRGB);
    auto draw = [&](unsigned shader, unsigned flags, auto const& expected)
    {
      f->glUniform1ui(f->glGetUniformLocation(program, "fixtureShader"), shader);
      f->glUniform1ui(f->glGetUniformLocation(program, "fixtureFlags"), flags);
      f->glClearColor(1.f, 0.f, 1.f, 1.f);
      f->glClear(GL_COLOR_BUFFER_BIT);
      f->glDrawArrays(GL_TRIANGLES, 0, 3);
      std::array<unsigned char, 4> actual{};
      f->glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
      for (std::size_t channel = 0; channel < actual.size(); ++channel)
        if (std::abs(int(actual[channel]) - int(expected[channel])) > 1)
          throw std::runtime_error("WMO shader " + std::to_string(shader) + " flags " + std::to_string(flags)
              + " channel " + std::to_string(channel) + ": got " + std::to_string(actual[channel])
              + ", expected " + std::to_string(expected[channel]));
    };
    constexpr unsigned unlitUnfogged = 0x4u | 0x8u;
    constexpr unsigned retail = 0x40u;
    draw(21, unlitUnfogged | retail, diffuse);
    draw(21, unlitUnfogged, darkLightmap);
    draw(0, unlitUnfogged | retail, diffuse);
    draw(0, unlitUnfogged, diffuse);
    // Legacy 23 keeps the second texture, independent of Retail material data.
    draw(23, unlitUnfogged, darkLightmap);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[1]); // black environment, isolate diffuse
    auto weights = [&](float r, float g, float b, float a = 0.f)
    { f->glUniform4f(f->glGetUniformLocation(program, "fixtureWeights"), r, g, b, a); };
    weights(1, 0, 0); draw(23, unlitUnfogged | retail, pixels[1]);
    weights(0, 1, 0); draw(23, unlitUnfogged | retail, pixels[2]);
    weights(0, 0, 1); draw(23, unlitUnfogged | retail, pixels[3]);
    weights(0, 0, 0); draw(23, unlitUnfogged | retail, pixels[4]);
    weights(0, .5f, .5f); draw(23, unlitUnfogged | retail, std::array<unsigned char, 4>{100, 100, 40, 255});
    // The red layer has low height while green is high: height blending selects green.
    f->glActiveTexture(GL_TEXTURE6);
    auto lowHeight = pixels[6]; lowHeight[3] = 0;
    f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lowHeight.data());
    weights(0, .5f, .5f); draw(23, unlitUnfogged | retail, std::array<unsigned char, 4>{20, 180, 40, 255});
    // Height alpha 0 must remain finite; MOC2 alpha must not darken unlit materials.
    for (unsigned i = 5; i < 9; ++i)
    {
      f->glActiveTexture(GL_TEXTURE0 + i);
      auto pixel = pixels[i]; pixel[3] = 0;
      f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    }
    weights(0, 1, 0); draw(23, unlitUnfogged | retail, pixels[2]);
    weights(0, 1, 0, 1); draw(23, unlitUnfogged | retail, pixels[2]);

    auto setLighting = [&](float ambient, float direct)
    {
      lighting.fill(0.f);
      for(unsigned channel=0;channel<3;++channel) {lighting[channel]=direct;lighting[4+channel]=ambient;}
      lighting[13]=1.f; // Shader's world-light transform gives a +Z direction.
      f->glBindBuffer(GL_UNIFORM_BUFFER,lightingBuffer);
      f->glBufferSubData(GL_UNIFORM_BUFFER,0,sizeof(lighting),lighting.data());
    };
    constexpr unsigned exteriorUnfogged = 0x1u | 0x8u;
    setLighting(.5f,0.f);
    weights(0,1,0,0);draw(23,retail|exteriorUnfogged,std::array<unsigned char,4>{99,11,22,255});
    weights(0,1,0,1);draw(23,retail|exteriorUnfogged,darkLightmap);
    setLighting(0.f,.5f);
    weights(0,1,0,0);draw(23,retail|exteriorUnfogged,std::array<unsigned char,4>{90,10,20,255});
    weights(0,1,0,1);draw(23,retail|exteriorUnfogged,std::array<unsigned char,4>{90,10,20,255});
    // Authored MOCV lighting is independent of ambient visibility.
    setLighting(0.f,0.f);
    weights(0,1,0,0);draw(23,retail|exteriorUnfogged|0x2u,std::array<unsigned char,4>{198,22,44,255});
    weights(0,1,0,1);draw(23,retail|exteriorUnfogged|0x2u,std::array<unsigned char,4>{198,22,44,255});
    // The same ambient-only rule applies to interior ambient_color.
    f->glUniform3f(f->glGetUniformLocation(program,"ambient_color"),.5f,.5f,.5f);
    weights(0,1,0,0);draw(23,retail|0x8u,std::array<unsigned char,4>{99,11,22,255});
    weights(0,1,0,1);draw(23,retail|0x8u,darkLightmap);
    f->glUniform3f(f->glGetUniformLocation(program,"ambient_color"),0.f,0.f,0.f);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D_ARRAY,textures[0]);
    weights(0,1,0,0);draw(23,retail|exteriorUnfogged,std::array<unsigned char,4>{45,10,30,255});
    weights(0,1,0,1);draw(23,retail|exteriorUnfogged,std::array<unsigned char,4>{45,10,30,255});

    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[0]);
    weights(0, 1, 0);
    draw(23, unlitUnfogged | retail | 0x80u, pixels[2]); // absent environment stays black
    auto blendAlpha = [&](float alpha)
    { f->glUniform1f(f->glGetUniformLocation(program, "fixtureBlendAlpha"), alpha); };
    constexpr unsigned hasMocv = 0x2u;
    blendAlpha(1.f);
    draw(7, unlitUnfogged | retail | hasMocv | 0x80u, diffuse);
    draw(7, unlitUnfogged | retail | hasMocv, std::array<unsigned char, 4>{109, 138, 222, 255});
    blendAlpha(0.f);
    draw(7, unlitUnfogged | retail | hasMocv | 0x80u, darkLightmap);
    draw(7, unlitUnfogged | retail | hasMocv, darkLightmap);
    blendAlpha(.5f);
    draw(7, unlitUnfogged | retail | hasMocv | 0x80u, std::array<unsigned char, 4>{32, 64, 96, 255});
    draw(7, unlitUnfogged | retail | hasMocv, std::array<unsigned char, 4>{55, 69, 111, 255});
    // MOMT 13 is an opaque two-layer blend, including when layer 2 has zero alpha.
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[1]);
    std::array<unsigned char, 4> const secondLayer{20, 40, 60, 0};
    f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, secondLayer.data());
    blendAlpha(0.f);
    draw(13, unlitUnfogged | retail | hasMocv, std::array<unsigned char, 4>{20, 40, 60, 255});
    blendAlpha(.5f);
    draw(13, unlitUnfogged | retail | hasMocv, std::array<unsigned char, 4>{42, 84, 126, 255});
    blendAlpha(1.f);
    draw(13, unlitUnfogged | retail | hasMocv, diffuse);
    draw(13, unlitUnfogged | hasMocv, diffuse); // legacy behavior is unchanged
    // MOMT 9 emissive must remain visible with both ambient and diffuse at zero.
    // No MOCV lighting contribution: the default material blend alpha is one.
    std::array<unsigned char, 4> const emissiveLayer{80, 120, 160, 128};
    blendAlpha(1.f);
    f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, emissiveLayer.data());
    draw(9, retail | 0x8u, std::array<unsigned char, 4>{40, 60, 80, 255});
    draw(9, 0x8u, darkLightmap);
    blendAlpha(.5f);
    draw(9, unlitUnfogged | retail | hasMocv, std::array<unsigned char, 4>{84, 158, 232, 255});
    f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, darkLightmap.data());
    // Distinct color and alpha values separate each MOMT composition from a
    // diffuse fallback; legacy uses diffuse.
    std::array<std::array<unsigned char,4>,3> const compositionPixels{{{64,128,192,128},{96,48,24,64},{32,160,80,192}}};
    for(unsigned i=0;i<3;++i){f->glActiveTexture(GL_TEXTURE0+i);f->glBindTexture(GL_TEXTURE_2D_ARRAY,textures[i]);f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,0,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,compositionPixels[i].data());}
    blendAlpha(.25f);
    struct Composition {unsigned shader,count;std::array<unsigned char,4> expected;};
    std::array<Composition,8> const compositions{{
      {8,2,{88,68,66,128}}, {11,3,{55,99,118,128}}, {12,3,{82,170,216,128}},
      {15,2,{88,68,66,128}}, {17,3,{59,103,122,255}}, {18,3,{18,142,101,128}},
      {19,2,{60,108,153,128}}, {20,3,{17,154,114,128}}}};
    for(auto const& test:compositions){
      require(Noggit::Format::wmo_texture_count(test.shader,true)==test.count,"Material texture policy missing a used slot");
      require(Noggit::Format::wmo_texture_count(test.shader,false)==1,"Legacy texture count changed");
      draw(test.shader,retail|unlitUnfogged,test.expected);
      draw(test.shader,unlitUnfogged,std::array<unsigned char,4>{64,128,192,255});
    }
    draw(3,retail|unlitUnfogged,std::array<unsigned char,4>{112,152,204,255});
    f->glUniform1ui(f->glGetUniformLocation(program,"fixtureAlphaMode"),1u);
    draw(3,retail|unlitUnfogged,std::array<unsigned char,4>{112,152,204,255}); // Env alpha is reflection strength, not opacity.
    draw(0,retail|unlitUnfogged,std::array<unsigned char,4>{255,0,255,255}); // Diffuse alpha-key still discards.
    f->glUniform1ui(f->glGetUniformLocation(program,"fixtureAlphaMode"),0u);
    draw(3,unlitUnfogged,std::array<unsigned char,4>{88,152,210,255});
    // Production vertex shader and CPU batch layout. The second batch is used so
    // a wrong batch stride cannot pass.
    for(unsigned i=0;i<textures.size();++i){f->glActiveTexture(GL_TEXTURE0+i);f->glBindTexture(GL_TEXTURE_2D_ARRAY,textures[i]);f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,0,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels[i].data());}

    f->glUseProgram(realProgram);
    auto const realLighting = f->glGetUniformBlockIndex(realProgram, "lighting");
    f->glUniformBlockBinding(realProgram, realLighting, 0);
    f->glUniform1iv(f->glGetUniformLocation(realProgram, "texture_samplers[0]"), 15, samplers.data());
    f->glUniform1f(f->glGetUniformLocation(realProgram, "preview_texture_lod_bias"), 0.f);
    f->glUniform1f(f->glGetUniformLocation(realProgram, "sidn_intensity"), 0.f);
    f->glUniform3f(f->glGetUniformLocation(realProgram, "camera"), 0.f, 0.f, 1.f);
    f->glUniform3f(f->glGetUniformLocation(realProgram, "ambient_color"), 0.f, 0.f, 0.f);
    std::array<float, 16> identity{};
    for (unsigned i = 0; i < 4; ++i) identity[i * 5] = 1.f;
    f->glUniformMatrix4fv(f->glGetUniformLocation(realProgram, "transform"), 1, GL_FALSE, identity.data());
    std::array<float, 32> matrices{};
    std::copy(identity.begin(), identity.end(), matrices.begin());
    std::copy(identity.begin(), identity.end(), matrices.begin() + 16);
    GLuint matrixBuffer = 0, positionBuffer = 0, batchBuffer = 0, batchTexture = 0;
    f->glGenBuffers(1, &matrixBuffer);
    f->glBindBuffer(GL_UNIFORM_BUFFER, matrixBuffer);
    f->glBufferData(GL_UNIFORM_BUFFER, sizeof(matrices), matrices.data(), GL_STATIC_DRAW);
    f->glUniformBlockBinding(realProgram, f->glGetUniformBlockIndex(realProgram, "matrices"), 1);
    f->glBindBufferBase(GL_UNIFORM_BUFFER, 1, matrixBuffer);
    std::array<float, 12> const positions{-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    f->glGenBuffers(1, &positionBuffer);
    f->glBindBuffer(GL_ARRAY_BUFFER, positionBuffer);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions.data(), GL_STATIC_DRAW);
    auto attribute = [&](char const* name) { auto a=f->glGetAttribLocation(realProgram,name);require(a>=0,"Production vertex attribute missing");return GLuint(a); };
    auto const positionAttribute = attribute("position");
    f->glEnableVertexAttribArray(positionAttribute);
    f->glVertexAttribPointer(positionAttribute, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    std::vector<GLuint> attributeBuffers;
    auto floatAttribute = [&](char const* name, std::array<float,4> value)
    {
      GLuint buffer=0;f->glGenBuffers(1,&buffer);attributeBuffers.push_back(buffer);
      std::array<std::array<float,4>,3> values{value,value,value};
      f->glBindBuffer(GL_ARRAY_BUFFER,buffer);
      f->glBufferData(GL_ARRAY_BUFFER,sizeof(values),values.data(),GL_DYNAMIC_DRAW);
      auto const index=attribute(name);f->glEnableVertexAttribArray(index);
      f->glVertexAttribPointer(index,4,GL_FLOAT,GL_FALSE,0,nullptr);
      return buffer;
    };
    floatAttribute("normal",{0.f,0.f,1.f,0.f});
    floatAttribute("vertex_color",{1.f,1.f,1.f,1.f});
    floatAttribute("material_blend_alpha",{1.f,0.f,0.f,0.f});
    std::array<char const*,4> const uvNames{"texcoord","texcoord_2","texcoord_3","texcoord_4"};
    for(unsigned i=0;i<4;++i) floatAttribute(uvNames[i],{i%2 ? .75f : .25f,.5f,0.f,0.f});
    auto const weightsBuffer=floatAttribute("material_weights",{0.f,0.f,0.f,0.f});
    GLuint mappingBuffer=0;f->glGenBuffers(1,&mappingBuffer);attributeBuffers.push_back(mappingBuffer);
    std::array<GLuint,3> mapping{2,2,2};
    f->glBindBuffer(GL_ARRAY_BUFFER,mappingBuffer);
    f->glBufferData(GL_ARRAY_BUFFER,sizeof(mapping),mapping.data(),GL_STATIC_DRAW);
    f->glEnableVertexAttribArray(attribute("batch_mapping"));
    f->glVertexAttribIPointer(attribute("batch_mapping"),1,GL_UNSIGNED_INT,0,nullptr);
    std::array<Noggit::Rendering::WMORenderBatch,2> batches{};
    auto& batch=batches[1];
    batch.flags=unlitUnfogged|retail|0x80u;batch.shader=23;
    for(unsigned slot=0;slot<9;++slot) {batch.sampler(slot)=8-slot;batch.layer(slot)=1;}
    f->glGenBuffers(1,&batchBuffer);
    f->glBindBuffer(GL_TEXTURE_BUFFER,batchBuffer);
    f->glBufferData(GL_TEXTURE_BUFFER,sizeof(batches),batches.data(),GL_STATIC_DRAW);
    f->glGenTextures(1,&batchTexture);
    f->glActiveTexture(GL_TEXTURE15);
    f->glBindTexture(GL_TEXTURE_BUFFER,batchTexture);
    f->glTexBuffer(GL_TEXTURE_BUFFER,GL_RGBA32UI,batchBuffer);
    f->glUniform1i(f->glGetUniformLocation(realProgram,"render_batches_tex"),15);
    for(unsigned slot=0;slot<9;++slot)
    {
      f->glActiveTexture(GL_TEXTURE0+8-slot);
      f->glBindTexture(GL_TEXTURE_2D_ARRAY,textures[slot]);
      std::array<std::array<unsigned char,4>,4> data{};
      for(auto& p:data)p={255,0,255,255}; // wrong array layer or UV remains visible
      if(slot>=1 && slot<=4) data[2+(slot-1)%2]=pixels[slot];
      else {data[2]=pixels[slot];data[3]=pixels[slot];}
      f->glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RGBA8,2,1,2,0,GL_RGBA,GL_UNSIGNED_BYTE,data.data());
    }
    for(unsigned layer=0;layer<4;++layer)
    {
      std::array<float,4> weight{layer==0?1.f:0.f,layer==1?1.f:0.f,layer==2?1.f:0.f,0.f};
      std::array<std::array<float,4>,3> values{weight,weight,weight};
      f->glBindBuffer(GL_ARRAY_BUFFER,weightsBuffer);
      f->glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(values),values.data());
      f->glClear(GL_COLOR_BUFFER_BIT);
      f->glDrawArrays(GL_TRIANGLES,0,3);
      std::array<unsigned char,4> actual{};
      f->glReadPixels(1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
      require(actual==pixels[layer+1],"Production WMO vertex/batch stride, UV, sampler or array-layer mapping failed");
    }
    batch.shader = 7;
    batch.flags = unlitUnfogged | retail | hasMocv;
    f->glBindBuffer(GL_TEXTURE_BUFFER, batchBuffer);
    f->glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(batches), batches.data());
    auto layerPixels = [&](unsigned slot, std::array<unsigned char, 4> left, std::array<unsigned char, 4> right)
    {
      f->glActiveTexture(GL_TEXTURE0 + batch.sampler(slot));
      std::array<std::array<unsigned char, 4>, 2> data{left, right};
      f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    };
    layerPixels(0, diffuse, diffuse);
    layerPixels(1, darkLightmap, darkLightmap);
    // At the sampled fragment reflection X is negative and repeats into the
    // right texel; the incorrect sphere projection would select black at left.
    layerPixels(2, darkLightmap, pixels[2]);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glDrawArrays(GL_TRIANGLES, 0, 3);
    std::array<unsigned char, 4> reflected{};
    f->glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, reflected.data());
    std::array<unsigned char, 4> const expectedReflection{109, 138, 222, 255};
    for (unsigned channel = 0; channel < 4; ++channel)
      require(std::abs(int(reflected[channel]) - int(expectedReflection[channel])) <= 1,
          "Production shader 7 must use reflection XY rather than sphere-map UVs");
    f->glDisableVertexAttribArray(positionAttribute);
    f->glDeleteBuffers(1,&matrixBuffer);f->glDeleteBuffers(1,&positionBuffer);f->glDeleteBuffers(1,&batchBuffer);
    f->glDeleteBuffers(static_cast<GLsizei>(attributeBuffers.size()),attributeBuffers.data());
    f->glDeleteTextures(1,&batchTexture);
    require(f->glGetError() == GL_NO_ERROR, "OpenGL error during WMO material regression");
    f->glDeleteFramebuffers(1, &framebuffer);
    f->glDeleteTextures(1, &color);
    f->glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    f->glDeleteBuffers(1, &lightingBuffer);
    f->glDeleteVertexArrays(1, &vao);
    f->glUseProgram(0);
    f->glDeleteProgram(program);
    f->glDeleteProgram(realProgram);
    std::cout << "PASS: production WMO shaders link; Retail MOMT layers/compositions, opacity, emission, height blend, environment and legacy paths render correctly; "
              << reinterpret_cast<char const*>(f->glGetString(GL_RENDERER)) << '\n';
    return 0;
  }
  catch (std::exception const& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
