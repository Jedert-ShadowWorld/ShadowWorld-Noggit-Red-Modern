#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_1_Core>
#include <QFile>
#include <QDir>
#include <QImage>
#include <noggit/ModelHeaders.h>
#include <noggit/rendering/RetailM2Shader.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <array>
#include <vector>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool v, char const* message) { if (!v) throw std::runtime_error(message); }
QByteArray read(QString const& path) { QFile f(path);require(f.open(QIODevice::ReadOnly),"Cannot read fixture/shader");return f.readAll(); }
template<class T> T value(QByteArray const& bytes, std::size_t offset) {
  require(offset <= std::size_t(bytes.size()) && sizeof(T) <= std::size_t(bytes.size())-offset,"Fixture range exceeds file");
  T out;std::memcpy(&out,bytes.constData()+offset,sizeof(T));return out;
}
template<class T> std::vector<T> array(QByteArray const& b, std::size_t offset, std::size_t count) {
  require(offset<=std::size_t(b.size()) && count <= (std::size_t(b.size())-offset)/sizeof(T),"Invalid fixture array");
  std::vector<T> out(count);std::memcpy(out.data(),b.constData()+offset,count*sizeof(T));return out;
}
}

int main(int argc, char** argv) {
  QGuiApplication app(argc,argv);
  QSurfaceFormat format;format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;context.setFormat(format);
  QOffscreenSurface surface;surface.setFormat(format);surface.create();
  if (!context.create() || !surface.isValid() || !context.makeCurrent(&surface)) return 77;
  auto* f=context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  if (!f || !f->initializeOpenGLFunctions()) return 77;
  try {
    require(argc==2 || argc==4,"Expected GLSL directory [--skin SKIN]");
    auto compile=[&](GLenum kind, QByteArray const& source) {
      auto shader=f->glCreateShader(kind);auto* text=source.constData();GLint n=source.size();
      f->glShaderSource(shader,1,&text,&n);f->glCompileShader(shader);GLint ok=0;f->glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
      if(!ok){GLint size=0;f->glGetShaderiv(shader,GL_INFO_LOG_LENGTH,&size);QByteArray log(size,'\0');f->glGetShaderInfoLog(shader,size,nullptr,log.data());throw std::runtime_error(log.constData());}return shader;
    };
    QDir shaders(QString::fromLocal8Bit(argv[1]));
    auto vs=compile(GL_VERTEX_SHADER,read(shaders.filePath("m2_vert.glsl")));
    auto fs=compile(GL_FRAGMENT_SHADER,read(shaders.filePath("m2_frag.glsl")));
    auto program=f->glCreateProgram();f->glAttachShader(program,vs);f->glAttachShader(program,fs);f->glLinkProgram(program);
    GLint linked=0;f->glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"Production M2 shader link failed");f->glUseProgram(program);
    auto uniform=[&](char const* name){return f->glGetUniformLocation(program,name);};
    GLuint vao=0,vbo=0,ebo=0,matricesBuffer=0,lightingBuffer=0,texture=0,fbo=0,color=0;
    f->glGenVertexArrays(1,&vao);f->glBindVertexArray(vao);
    f->glGenBuffers(1,&vbo);f->glGenBuffers(1,&ebo);f->glGenBuffers(1,&matricesBuffer);f->glGenBuffers(1,&lightingBuffer);
    auto attrib=[&](char const* name,int size,std::size_t offset) {auto loc=f->glGetAttribLocation(program,name);if(loc<0)return;f->glEnableVertexAttribArray(loc);f->glVertexAttribPointer(loc,size,GL_FLOAT,GL_FALSE,sizeof(ModelVertex),reinterpret_cast<void*>(offset));};
    f->glBindBuffer(GL_ARRAY_BUFFER,vbo);attrib("pos",3,offsetof(ModelVertex,position));attrib("normal",3,offsetof(ModelVertex,normal));attrib("texcoord1",2,offsetof(ModelVertex,texcoords));attrib("texcoord2",2,offsetof(ModelVertex,texcoords)+sizeof(glm::vec2));
    f->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glm::mat4 identity(1.f);
    for(auto name:{"transform","tex_matrix_1","tex_matrix_2"}) f->glUniformMatrix4fv(uniform(name),1,GL_FALSE,glm::value_ptr(identity));
    f->glUniform1i(uniform("anim_bones"),0);f->glUniform1i(uniform("bone_matrices"),2);f->glUniform1i(uniform("tex_unit_lookup_1"),1);f->glUniform1i(uniform("tex_unit_lookup_2"),2);
    f->glUniform1i(uniform("tex1"),0);f->glUniform1i(uniform("tex2"),0);f->glUniform1i(uniform("tex1_index"),0);f->glUniform1i(uniform("tex2_index"),0);
    f->glUniform1i(uniform("blend_mode"),1);f->glUniform1i(uniform("pixel_shader"),5);f->glUniform1i(uniform("unlit"),1);f->glUniform1i(uniform("unfogged"),1);
    f->glUniform4f(uniform("mesh_color"),1,1,1,1);
    std::array<float,32> lighting{};lighting[4]=lighting[5]=lighting[6]=1;
    f->glBindBuffer(GL_UNIFORM_BUFFER,lightingBuffer);f->glBufferData(GL_UNIFORM_BUFFER,sizeof(lighting),lighting.data(),GL_STATIC_DRAW);
    f->glUniformBlockBinding(program,f->glGetUniformBlockIndex(program,"lighting"),1);f->glBindBufferBase(GL_UNIFORM_BUFFER,1,lightingBuffer);
    f->glUniformBlockBinding(program,f->glGetUniformBlockIndex(program,"matrices"),0);f->glBindBufferBase(GL_UNIFORM_BUFFER,0,matricesBuffer);
    auto matrices=[&](glm::mat4 view,glm::mat4 projection){std::array<glm::mat4,2> m{view,projection};f->glBindBuffer(GL_UNIFORM_BUFFER,matricesBuffer);f->glBufferData(GL_UNIFORM_BUFFER,sizeof(m),m.data(),GL_DYNAMIC_DRAW);};
    matrices(identity,identity);
    f->glGenTextures(1,&texture);f->glActiveTexture(GL_TEXTURE0);f->glBindTexture(GL_TEXTURE_2D_ARRAY,texture);
    f->glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);f->glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);f->glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAX_LEVEL,0);
    std::array<std::array<unsigned char,4>,4> rgba{{{40,180,50,127},{40,180,50,128},{40,180,50,223},{40,180,50,225}}};
    f->glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RGBA8,4,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    f->glGenFramebuffers(1,&fbo);f->glBindFramebuffer(GL_FRAMEBUFFER,fbo);f->glGenTextures(1,&color);f->glBindTexture(GL_TEXTURE_2D,color);
    auto viewport=[&](int w,int h){f->glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);f->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);require(f->glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Incomplete framebuffer");f->glViewport(0,0,w,h);};
    viewport(4,1);
    f->glDisable(GL_BLEND);f->glDisable(GL_DEPTH_TEST);f->glDisable(GL_CULL_FACE);f->glDisable(GL_DITHER);f->glDisable(GL_FRAMEBUFFER_SRGB);
    std::vector<ModelVertex> vertices(4);std::array<glm::vec3,4> positions{{{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}}};std::array<glm::vec2,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
    for(int i=0;i<4;++i){vertices[i].position=positions[i];vertices[i].normal={0,0,1};vertices[i].texcoords[0]=uv[i];}
    std::vector<std::uint16_t> indices{0,1,2,0,2,3};
    auto uploadMesh=[&]{f->glBindBuffer(GL_ARRAY_BUFFER,vbo);f->glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(ModelVertex),vertices.data(),GL_STATIC_DRAW);f->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);f->glBufferData(GL_ELEMENT_ARRAY_BUFFER,indices.size()*sizeof(std::uint16_t),indices.data(),GL_STATIC_DRAW);};uploadMesh();
    auto draw=[&](bool retail,int w,int h){
      f->glUniform1i(uniform("retail_alpha_key"),retail);f->glUniform1i(uniform("retail_materials"),retail);f->glClearColor(0,0,0,0);f->glClear(GL_COLOR_BUFFER_BIT);f->glDrawElements(GL_TRIANGLES,GLsizei(indices.size()),GL_UNSIGNED_SHORT,nullptr);
      QImage pixels(w,h,QImage::Format_RGBA8888);f->glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.bits());unsigned count=0;
      for(int y=0;y<h;++y)for(int x=0;x<w;++x)if(pixels.constScanLine(y)[4*x+3])++count;
      return count;
    };
    for(float opacity:{1.f,.5f}){f->glUniform4f(uniform("mesh_color"),1,1,1,opacity);auto legacy=draw(false,4,1),retail=draw(true,4,1);require(legacy==1,"Legacy alpha-key changed");require(retail==3,"Retail midpoint alpha-key/opacity boundary failed");}
    f->glUniform4f(uniform("mesh_color"),1,1,1,1);
    // Exercise the real fragment shader with world lighting disabled and dimmed.
    // Unlit Retail materials must not black out, while legacy behavior is unchanged.
    f->glUniform1i(uniform("blend_mode"),0);
    auto lightCase=[&](bool retail,bool unlit,float ambient) {
      lighting[4]=lighting[5]=lighting[6]=ambient;
      // A nonzero direction keeps the lit control well-defined even with diffuse=0.
      lighting[12]=1.f;
      f->glBindBuffer(GL_UNIFORM_BUFFER,lightingBuffer);
      f->glBufferData(GL_UNIFORM_BUFFER,sizeof(lighting),lighting.data(),GL_DYNAMIC_DRAW);
      f->glUniform1i(uniform("unlit"),unlit);
      draw(retail,4,1);
      std::array<unsigned char,16> pixels{};
      f->glReadPixels(0,0,4,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
      return std::array<unsigned char,3>{pixels[0],pixels[1],pixels[2]};
    };
    for(float ambient:{0.f,.25f}) {
      auto retail=lightCase(true,true,ambient);
      require(retail==std::array<unsigned char,3>{40,180,50},"Retail unlit color depends on world ambient");
      auto legacy=lightCase(false,true,ambient);
      for(int channel=0;channel<3;++channel)
        require(std::abs(int(legacy[channel])-int(std::lround(rgba[0][channel]*ambient)))<=1,"Legacy unlit lighting changed");
    }
    require(lightCase(true,false,0.f)==std::array<unsigned char,3>{0,0,0},"Lit Retail material bypasses world lighting");
    require(lightCase(true,false,.25f)[1]>0 && lightCase(true,false,.25f)[1]<180,"Lit Retail material ignores dim ambient");
    lightCase(true,true,1.f);
    f->glUniform1i(uniform("blend_mode"),1);
    auto validStages=[](std::uint16_t combo,unsigned count,std::vector<std::uint16_t> const& lookup,
        std::vector<int> const& special,std::size_t textures) {
      return Noggit::Rendering::validRetailM2Textures(combo,count,lookup,special,textures,[](std::size_t type){return type==7;});
    };
    require(validStages(0,3,{0,1,2},{-1,-1,7},2),"Valid third-stage replacement rejected");
    require(!validStages(0,3,{0,1,2},{-1,-1,8},2),"Missing replacement can reuse stale binding");
    require(!validStages(0,3,{0,1,3},{-1,-1,7},3),"Bad third-stage lookup accepted");
    require(!validStages(0,3,{0,1,2},{-1,-1,-1},2),"Missing third-stage texture accepted");
    require(!validStages(1,3,{0,1,2},{-1,-1,-1},3),"Truncated stage combo accepted");
    require(!validStages(65535,2,std::vector<std::uint16_t>(65537,0),{-1},1),"Stage combo uint16 overflow accepted");
    // The production mapper selects authored shader IDs, all three UV stages,
    // and edge fade. Feed those contracts directly into the production shaders.
    for(auto id:{18,19,20,22,25,29,31,35,36,256})
      require(!Noggit::Rendering::retailM2Shader(0x8000|id),"Unsupported Retail shader silently mapped");
    require(!Noggit::Rendering::retailM2Shader(0x14),"Legacy shader entered Retail table");
    // ShaderMapper's Opaque_Mod_Add_Wgt uses Diffuse_T1_Env, not T1_T1.
    auto weighted = Noggit::Rendering::retailM2Shader(0x8010);
    require(weighted && weighted->uv == std::array<int,3>{1,0,3},
        "Weighted Retail material lost its environment coordinate stage");
    GLuint second=0,third=0;f->glGenTextures(1,&second);f->glGenTextures(1,&third);
    auto stageTexture=[&](int unit,GLuint handle,int width,unsigned char const* pixels) {
      f->glActiveTexture(GL_TEXTURE0+unit);f->glBindTexture(GL_TEXTURE_2D_ARRAY,handle);
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAX_LEVEL,0);
      f->glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RGBA8,width,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    };
    std::array<unsigned char,4> a{64,96,128,64}, b{128,64,32,128}, c{32,16,64,192};
    stageTexture(0,texture,1,a.data());stageTexture(1,second,1,b.data());stageTexture(3,third,1,c.data());
    f->glUniform1i(uniform("tex2"),1);f->glUniform1i(uniform("tex3"),3);
    f->glUniform1i(uniform("tex3_index"),0);
    f->glUniformMatrix4fv(uniform("tex_matrix_3"),1,GL_FALSE,glm::value_ptr(identity));
    auto material=[&](int id,bool fade=false) {
      auto mapped=Noggit::Rendering::retailM2Shader(0x8000|id);require(bool(mapped),"Expected Retail shader unavailable");
      f->glUniform1i(uniform("pixel_shader"),mapped->pixel);
      for(unsigned i=0;i<3;++i){auto name=QByteArray("tex_unit_lookup_")+QByteArray::number(i+1);f->glUniform1i(uniform(name.constData()),mapped->uv[i]);}
      f->glUniform1i(uniform("retail_edge_fade"),fade&&mapped->edgeFade);
      draw(true,4,1);
      std::array<unsigned char,16> result{};f->glReadPixels(0,0,4,1,GL_RGBA,GL_UNSIGNED_BYTE,result.data());
      return std::array<unsigned char,4>{result[0],result[1],result[2],result[3]};
    };
    auto expect=[&](int id, std::array<float,3> expected) {
      auto actual=material(id);
      for(unsigned channel=0;channel<3;++channel)
        require(std::abs(int(actual[channel])-int(std::lround(std::clamp(expected[channel],0.f,1.f)*255)))<=2,"Retail combiner RGB mismatch");
    };
    auto A=[&](int i){return a[i]/255.f;};auto B=[&](int i){return b[i]/255.f;};auto C=[&](int i){return c[i]/255.f;};
    f->glUniform1i(uniform("blend_mode"),0);
    for(int id:{0,1,2,3,9,12,17,23,24,33}) {
      std::array<float,3> expected{};
      for(int i=0;i<3;++i){float base=A(i)*B(i)*2*(1-A(3))+A(i)*A(3);
        switch(id){
          case 0: expected[i]=base;break;
          case 1: expected[i]=A(i)+B(i)*B(3);break;
          case 2: expected[i]=A(i)+B(i)*B(3)*(1-A(3));break;
          case 3: expected[i]=base+C(i)*C(3);break;
          case 9: expected[i]=A(i)*B(i)*2*(1-C(3))+C(i)*C(3);break;
          case 12: expected[i]=A(i)*B(i)*(1-A(3))+A(i)*A(3);break;
          case 17: case 24: expected[i]=base*(1-C(3))+C(i)*C(3);break;
          case 23: expected[i]=A(i)*(1-B(3))+B(i)*B(3);break;
          case 33: expected[i]=A(i)*B(i)*2;break;
        }
      }expect(id,expected);
    }
    // A glow stage stays visible without external lighting.
    lighting[4]=lighting[5]=lighting[6]=0;
    f->glBindBuffer(GL_UNIFORM_BUFFER,lightingBuffer);f->glBufferData(GL_UNIFORM_BUFFER,sizeof(lighting),lighting.data(),GL_DYNAMIC_DRAW);
    f->glUniform1i(uniform("unlit"),0);
    expect(3,{C(0)*C(3),C(1)*C(3),C(2)*C(3)});
    lightCase(true,true,1.f);
    // Two stages both use UV1 but only stage 2 is animated: it must select green.
    std::array<unsigned char,8> redGreen{255,0,0,255,0,255,0,255};stageTexture(1,second,2,redGreen.data());
    for(auto& v:vertices){v.texcoords[0]={.25f,.5f};v.texcoords[1]={.75f,.5f};}uploadMesh();
    glm::mat4 translated(1.f);translated[0][3]=.5f;
    f->glUniformMatrix4fv(uniform("tex_matrix_2"),1,GL_FALSE,glm::value_ptr(translated));
    auto animated=material(23);require(animated[0]==0&&animated[1]==255&&animated[2]==0,"Texture animation is attached to UV set instead of stage");
    f->glUniformMatrix4fv(uniform("tex_matrix_2"),1,GL_FALSE,glm::value_ptr(identity));
    auto stationary=material(23);require(stationary[0]==255&&stationary[1]==0,"Authored UV1 routing was replaced by UV2");
    // Third stage on authored ID24 selects UV2, not stage one's UV1.
    stageTexture(3,third,2,redGreen.data());a[3]=0;stageTexture(0,texture,1,a.data());
    auto thirdUV=material(24);require(thirdUV[0]==0&&thirdUV[1]==255&&thirdUV[2]==0,"Third material stage does not use authored UV2");
    // At a grazing view a real edge-fade shader removes coverage; without it the
    // same geometry remains covered. Normals/positions are deliberately orthogonal.
    f->glUniform1i(uniform("blend_mode"),2);stageTexture(1,second,1,b.data());stageTexture(0,texture,1,a.data());
    auto noFade=material(33,false),edgeFade=material(33,true);
    require(noFade[3]==0,"Zero authored alpha changed");
    a[3]=255;stageTexture(0,texture,1,a.data());
    noFade=material(33,false);edgeFade=material(33,true);
    require(noFade[3]>0&&edgeFade[3]==0,"Retail view-dependent edge fade not applied");
    if(argc==4){
      require(QByteArray(argv[2])=="--skin","Expected --skin fixture option");
      auto skin=read(QString::fromLocal8Bit(argv[3]));auto view=value<ModelView>(skin,0);
      require(std::memcmp(view.id,"SKIN",4)==0,"Not an actual SKIN fixture");
      auto batches=array<ModelTexUnit>(skin,view.ofs_texture_unit,view.n_texture_unit);
      unsigned authored=0;
      for(auto const& batch:batches)if(batch.shader_id&0x8000){
        auto mapped=Noggit::Rendering::retailM2Shader(batch.shader_id);
        require(bool(mapped)&&mapped->textures<=batch.texture_count&&batch.texture_count<=3,"Actual high-bit batch unsupported or wrong stage count");
        ++authored;
      }
      require(authored>0,"Expected actual authored high-bit batches");
    }
    require(f->glGetError()==GL_NO_ERROR,"OpenGL error in M2 alpha-key regression");
    std::cout<<"PASS: production M2 alpha-key cutoff and opacity, unlit lighting, Retail 2/3-stage combiners, emission, per-stage UV animation, third UV, edge fade and unsupported IDs; "
             <<reinterpret_cast<char const*>(f->glGetString(GL_RENDERER))<<'\n';return 0;
  }catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
