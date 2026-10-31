// Execute production upload/unload/setupVao/draw against a recording GL device.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <glm/glm.hpp>
#include <noggit/formats/WmoMaterial.hpp>
#include <noggit/formats/WmoAmbient.hpp>
using GLuint=unsigned;
using GLenum=unsigned;
using GLint=int;
using GLsizei=int;
constexpr unsigned GL_ARRAY_BUFFER=1,GL_ELEMENT_ARRAY_BUFFER=2,GL_TEXTURE_BUFFER=3,
 GL_TEXTURE_2D_ARRAY=4,GL_STATIC_DRAW=5,GL_RGBA32UI=6,GL_FLOAT=7,GL_FALSE=0,
 GL_UNSIGNED_INT=8,GL_TEXTURE0=100,GL_CULL_FACE=9,GL_TRIANGLES=10,GL_UNSIGNED_SHORT=11,
 GL_RGBA8=12,GL_RGBA=13,GL_UNSIGNED_BYTE=14,GL_TEXTURE_MAX_LEVEL=15,GL_TEXTURE_MIN_FILTER=16,
 GL_TEXTURE_MAG_FILTER=17,GL_LINEAR_MIPMAP_LINEAR=18,GL_LINEAR=19,
 GL_BLEND=20,GL_TRUE=1,GL_SRC_ALPHA=21,GL_ONE_MINUS_SRC_ALPHA=22,GL_ONE=23,GL_ZERO=24,
 GL_DST_COLOR=25,GL_SRC_COLOR=26,GL_DST_ALPHA=27,GL_CONSTANT_ALPHA=28,GL_ONE_MINUS_CONSTANT_ALPHA=29,GL_ONE_MINUS_SRC_COLOR=30,GL_MAX_ARRAY_TEXTURE_LAYERS=31,
 GL_TEXTURE_BINDING_2D_ARRAY=32,GL_COMPRESSED_RGBA_S3TC_DXT5_EXT=33;
#define LogError std::cerr
void require(bool ok,char const* message){if(!ok)throw std::runtime_error(message);}
// ACTUAL_BATCH_LAYOUT
struct FakeGL
{
 unsigned next=1000,unit=0,vao=0;
 bool blend=false,depth=true;std::array<unsigned,4> factors{};
 std::map<unsigned,unsigned> bindings,indices,tbo;
 std::map<std::pair<unsigned,unsigned>,unsigned> textures;
 std::map<unsigned,std::vector<unsigned char>> bytes;
 std::map<unsigned,std::map<std::string,unsigned>> attributes;
 struct Draw {unsigned count,start,elementBuffer;std::vector<unsigned char> batches;std::array<unsigned,15> samplers;bool blend,depth;std::array<unsigned,4> factors;};
 std::vector<Draw> draws;
  std::vector<unsigned> deletedTextures;
 std::set<unsigned> liveTextures;
 int uploadCalls=0,failUploadCall=-1;
 void genTextures(int n,unsigned* ids){while(n--){liveTextures.insert(next);*ids++=next++;}}
 void deleteTextures(int n,unsigned const* ids){while(n--){deletedTextures.push_back(*ids);liveTextures.erase(*ids);tbo.erase(*ids++);}}
 bool isTexture(unsigned id)const{return liveTextures.count(id)!=0;}
 void bindBuffer(unsigned target,unsigned id){bindings[target]=id;if(target==GL_ELEMENT_ARRAY_BUFFER)indices[vao]=id;}
 void bufferData(unsigned target,std::size_t size,void const* data,unsigned)
 {auto& out=bytes[bindings[target]];out.resize(size);if(size)std::memcpy(out.data(),data,size);}
 template<unsigned target> void bufferData(unsigned id,std::size_t size,void const* data,unsigned usage)
 {bindBuffer(target,id);bufferData(target,size,data,usage);}
 template<unsigned target,class T> void bufferData(unsigned id,std::vector<T> const& data,unsigned usage)
 {bufferData<target>(id,data.size()*sizeof(T),data.data(),usage);}
 void activeTexture(unsigned value){unit=value-GL_TEXTURE0;}
 void bindTexture(unsigned target,unsigned id){textures[{unit,target}]=id;}
 void texBuffer(unsigned,unsigned,unsigned id){tbo[textures[{unit,GL_TEXTURE_BUFFER}]]=id;}
 void texImage3D(unsigned,int,int,int,int,int,int,unsigned,unsigned,void const*){}
 void compressedTexImage3D(unsigned,int,int,int,int,int,int,int,void const*){}
 void getIntegerv(unsigned target,GLint* value)
 {if(target==GL_MAX_ARRAY_TEXTURE_LAYERS)*value=256;else if(target==GL_TEXTURE_BINDING_2D_ARRAY)*value=textures[{unit,GL_TEXTURE_2D_ARRAY}];else throw std::logic_error("Unexpected GL query");}
 void uploadMip(){if(++uploadCalls==failUploadCall)throw std::runtime_error("Injected mip upload failure");}
 void texSubImage3D(unsigned,int,int,int,int,int,int,int,unsigned,unsigned,void const*){uploadMip();}
 void compressedTexSubImage3D(unsigned,int,int,int,int,int,int,int,unsigned,int,void const*){uploadMip();}
 void texParameteri(unsigned,unsigned,int){}
 void enable(unsigned target){if(target==GL_BLEND)blend=true;} void disable(unsigned target){if(target==GL_BLEND)blend=false;}
 void depthMask(unsigned value){depth=value;}
 void blendFunc(unsigned src,unsigned dst){factors={src,dst,src,dst};}
 void blendFuncSeparate(unsigned src,unsigned dst,unsigned srcA,unsigned dstA){factors={src,dst,srcA,dstA};}
 void blendColor(float,float,float,float){}

 void drawElements(unsigned,unsigned count,unsigned,void const* offset)
 {
  Draw draw{count,unsigned(reinterpret_cast<std::uintptr_t>(offset)/2),indices[vao],bytes[tbo[textures[{0,GL_TEXTURE_BUFFER}]]],{},blend,depth,factors};
  for(unsigned i=0;i<15;++i)draw.samplers[i]=textures[{i+1,GL_TEXTURE_2D_ARRAY}];
  draws.push_back(std::move(draw));
 }
} gl;
namespace Noggit{enum NoggitRenderContext{map,preview,count};}
namespace BlizzardArchive
{
 enum class ClientVersion{RETAIL,LEGACY};
 struct ClientFile
 {
  std::vector<std::uint32_t> colors;std::size_t pos=0;
  void const* getPointer()const{return reinterpret_cast<char const*>(colors.data())+pos;}
  void seekRelative(std::size_t size){pos+=size;}
 };
}
namespace Noggit::Application
{
 struct NoggitApplication
 {
  bool modern_features=true;
  BlizzardArchive::ClientVersion client_version=BlizzardArchive::ClientVersion::LEGACY;
  static NoggitApplication* instance(){static NoggitApplication app;return &app;}
  NoggitApplication* getConfiguration(){return this;}
  NoggitApplication* clientData(){return this;}
  BlizzardArchive::ClientVersion version()const{return client_version;}
 };
}
namespace BlizzardArchive::Listfile{struct FileKey{std::string stringRepr()const{return "fixture.blp";}std::string filepath()const{return stringRepr();}};}
struct blp_texture{unsigned unloads=0;void unload(){++unloads;}};
struct Pool
{
 std::array<blp_texture,2> textures;
 template<class F>void context_aware_apply(F function,Noggit::NoggitRenderContext context)
 {function(BlizzardArchive::Listfile::FileKey{},textures[context]);}
};
// ACTUAL_TEXTURE_ARRAY_LAYOUT
struct TextureManager
{
 static inline Pool _;
 static inline std::array<std::map<std::tuple<int,int,int,int>,TexArrayParams>,2> _tex_arrays;
  static inline std::mutex _retired_mutex;
  static inline std::array<std::uint64_t,2> _texture_generations{};
  static inline std::array<std::vector<TexArrayAllocation>,2> _retired_arrays;
  static void retire_texture(Noggit::NoggitRenderContext,GLuint,unsigned,std::uint64_t);
  static std::uint64_t texture_generation(Noggit::NoggitRenderContext);
  static void release_retired_textures(Noggit::NoggitRenderContext);
 static void unload_all(Noggit::NoggitRenderContext);
 static TexArrayAllocation get_tex_array(int,int,int,Noggit::NoggitRenderContext);
 static TexArrayAllocation get_tex_array(GLint,int,int,int,std::map<int,std::vector<std::uint8_t>>const&,Noggit::NoggitRenderContext);
 static TexArrayAllocation allocate_texture_array(GLint,int,int,std::vector<std::size_t>const&,Noggit::NoggitRenderContext);
};
// ACTUAL_TEXTURE_LIFECYCLE
// ACTUAL_TEXTURE_BINDING_GUARD
struct UploadTexture
{
 bool finished=true,_uploaded=false;
 int _width=8,_height=8,_array_index=-1;
 GLuint _texture_array=0;
 std::uint64_t _texture_generation=0;
 Noggit::NoggitRenderContext _context=Noggit::map;
 std::optional<GLint> _compression_format;
 std::map<int,std::vector<std::uint32_t>> _data;
 std::map<int,std::vector<std::uint8_t>> _compressed_data;
 bool loading_failed()const{return false;}
 void upload();
};
// ACTUAL_BLP_UPLOAD
template<unsigned count>struct Handles
{
 std::array<unsigned,count> ids{};
 void upload(){for(auto& id:ids)id=gl.next++;}
 void unload(){for(auto id:ids)gl.bytes.erase(id);ids.fill(0);}
 unsigned const& operator[](unsigned i)const{return ids.at(i);}
};
namespace OpenGL::Scoped
{
 struct use_program
 {
  void attrib(char const* name,unsigned buffer,int,unsigned,unsigned,int,int){gl.attributes[gl.vao][name]=buffer;}
  void attribi(char const* name,unsigned buffer,int,unsigned,int,int){gl.attributes[gl.vao][name]=buffer;}
 };
 struct vao_binder{unsigned old=gl.vao;vao_binder(unsigned id){gl.vao=id;}~vao_binder(){gl.vao=old;}};
 struct index_buffer_manual_binder{unsigned id;index_buffer_manual_binder(unsigned value):id(value){}void bind(){gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER,id);}};
}
namespace math{struct frustum{};}
namespace Noggit::Project
{
 enum class ProjectVersion{RETAIL,LEGACY,SL,FOREVER};
 struct Project{ProjectVersion projectVersion=ProjectVersion::RETAIL;} project;
 struct CurrentProject{static Project* get(){return &project;}};
}
struct Texture
{
 unsigned array,layer,coldUploads=0;bool resident=false,ready=true,failed=false,specular=true;
 std::shared_ptr<Texture> heightMap;
 Texture(unsigned a,unsigned l):array(a),layer(l){}
 void wait_until_loaded(){}
 bool finishedLoading()const{return ready;}
 bool loading_failed()const{return failed;}
 void upload(){if(ready&&!resident){++coldUploads;resident=true;gl.bindTexture(GL_TEXTURE_2D_ARRAY,array);}}
 unsigned texture_array()const{return array;}unsigned array_index()const{return layer;}
 bool is_uploaded()const{return resident;}bool is_specular()const{return specular;}
 Texture* get(){return this;}Texture* getHeightMap(){return heightMap.get();}
 BlizzardArchive::Listfile::FileKey file_key()const{return {};}
};
// ACTUAL_TILE_DRAW_CALL
constexpr int BASE_RENDER_TEXTURE_LAYERS=4,EXT_RENDER_TEXTURE_LAYERS=12;
struct HeightData{int uvScale=0;float heightScale=1, heightOffset=0;};
namespace OpenGL{
// ACTUAL_EXT_LAYER_LAYOUT
}
struct TileTextureSet
{
 std::vector<std::shared_ptr<Texture>> textures;
 std::array<std::shared_ptr<Texture>,16> heightTextures{};
 std::array<bool,16> heightTextureOverrides=[] {std::array<bool,16> flags;flags.fill(true);return flags;}();
 std::array<unsigned,16> flags{};
 std::array<HeightData,16> mapping{};
 auto* getTextures(){return &textures;}
 int num()const{return static_cast<int>(textures.size());}
 unsigned flag(int layer)const{return flags[layer];}
 Texture* heightTexture(std::size_t layer);
 bool hasHeightMappingData(int)const{return true;}
 HeightData getHeightMappingData(int layer)const{return mapping[layer];}
};
// ACTUAL_HEIGHT_TEXTURE_LOOKUP
struct TileMap;
struct TileChunk{std::unique_ptr<TileTextureSet> texture_set=std::make_unique<TileTextureSet>();TileMap* mt=nullptr;unsigned px=0,py=0;};
using MapChunk=TileChunk;
struct TileMap
{
 std::array<std::array<std::unique_ptr<TileChunk>,16>,16> mChunks;
 struct{unsigned x=40,z=16;}index;
 HeightData GetTextureHeightMappingData(std::string const&)const{return {};}
};
struct TileRender
{
 TileMap* _map_tile;
 std::vector<MapTileDrawCall> _draw_calls;
 struct Instance
 {
  int ChunkTexDoAnim[4]{},ChunkTexAnimSpeed[4]{},ChunkTexAnimDir[4]{};
  int ChunkHoles_DrawImpass_TexLayerCount_CantPaint[4]{},ChunkTextureSamplers[4]{},ChunkTextureArrayIDs[4]{};
  int ChunkHeightTextureSamplers[4]{},ChunkHeightTextureArrayIDs[4]{},ChunkTextureUVScale[4]{};
  float ChunkTextureHeightScale[4]{},ChunkTextureHeightOffset[4]{};
 };
 Instance _chunk_instance_data[256]{};
 OpenGL::ChunkExtLayerParams _chunk_layer_ext_data[256*12]{};
 bool _texture_not_loaded=false,_sampler_error=false,_split_drawcall=true,_requires_sampler_reset=false;
 void pruneUnusedSamplers(bool skip_upload_alphamap=false);
 void updateTextureAnimation(MapChunk*,unsigned);
 bool fillSamplers(MapChunk*,unsigned,unsigned);
 void rebuildDrawCalls();
 // ACTUAL_TILE_READINESS
};
// ACTUAL_TILE_SAMPLER_PRUNING
struct Material
{
 unsigned shader=0;
 struct{bool unculled=false,unlit=false,unfogged=false,sidn=false;}flags;
 unsigned blend_mode=0,texture1=0,texture2=0;
 unsigned texture_offset_1=1,texture_offset_2=1,texture_offset_3=1;
 struct{unsigned char a=0,r=0,g=0,b=0;}sidn_color;
};
using WMOMaterial=Material;
struct Root
{
 bool modern=true;
 std::vector<Material> materials;
 std::vector<std::shared_ptr<Texture>> textures;
 std::vector<std::array<unsigned,9>> material_textures;
 std::vector<std::uint16_t> material_missing_textures;
 struct{bool do_not_fix_vertex_color_alpha=false,use_unified_render_path=false;}flags;
 glm::vec4 ambient_light_color{};
 bool uses_file_data_ids()const{return modern;}
};
struct Batch{unsigned texture=0,flags=0;std::array<unsigned,6> unused{};unsigned index_start=0,index_count=3;unsigned vertex_start=0,vertex_end=0;};
struct Group;
struct WMOGroupRender
{
 Group* _wmo_group;
 std::vector<WMORenderBatch> _render_batches;
 std::vector<unsigned> _render_batch_mapping;
 std::vector<WMOCombinedDrawCall> _draw_calls;
 Handles<1> _vertex_array;
 unsigned const& _vao=_vertex_array[0];
 Handles<12> _buffers;
 unsigned const& _vertices_buffer=_buffers[0];unsigned const& _normals_buffer=_buffers[1];
 unsigned const& _texcoords_buffer=_buffers[2];unsigned const& _texcoords_buffer_2=_buffers[3];
 unsigned const& _vertex_colors_buffer=_buffers[4];unsigned const& _indices_buffer=_buffers[5];
 unsigned const& _render_batch_mapping_buffer=_buffers[6];unsigned const& _render_batch_tex_buffer=_buffers[7];
 unsigned const& _texcoords_buffer_3=_buffers[8];unsigned const& _texcoords_buffer_4=_buffers[9];
 unsigned const& _material_weights_buffer=_buffers[10];unsigned const& _material_blend_alpha_buffer=_buffers[11];
 unsigned _render_batch_tex=0;
 bool _uploaded=false,_upload_attempted=false,_vao_is_setup=false,_reload_before_upload=false;
 explicit WMOGroupRender(Group* group):_wmo_group(group){}
 void upload();void packLegacyBatches();bool packRetailBatches();void unload();void setupVao(OpenGL::Scoped::use_program&);
 void initRenderBatches();
 void draw(OpenGL::Scoped::use_program&,math::frustum const&,float const&,glm::vec3 const&,bool,bool);
};
struct Group
{
 Root* wmo;WMOGroupRender renderer{this};unsigned loads=0;bool failLoad=false;
 struct{struct{bool has_two_motv=true,has_vertex_color=true,use_mocv2_for_texture_blending=true,exterior=false,exterior_lit=false;}flags;unsigned transparency_batches_count=0;}header;
 std::vector<Batch> _batches;
 std::vector<glm::vec3> _vertices,_normals;
 std::vector<glm::vec2> _texcoords,_texcoords_2,_texcoords_3,_texcoords_4;
 std::vector<glm::vec4> _vertex_colors,_material_weights;
 std::vector<float> _material_blend_alpha;
 std::vector<std::uint16_t> _indices;
 explicit Group(Root* root):wmo(root){}
 void load_mocv(BlizzardArchive::ClientFile&,std::uint32_t,bool);
 void fix_vertex_color_alpha(bool);
 void load()
 {
  ++loads;if(failLoad)throw std::runtime_error("fixture source unavailable");
  auto n=_batches.size()*3;_vertices.assign(n,glm::vec3(.2f));_normals.assign(n,glm::vec3(0,0,1));
  _texcoords.assign(n,glm::vec2(.1f));_texcoords_2.assign(n,glm::vec2(.2f));
  _texcoords_3.assign(n,glm::vec2(.3f));_texcoords_4.assign(n,glm::vec2(.4f));
  _vertex_colors.assign(n,glm::vec4(.5f));_material_weights.assign(n,glm::vec4(.25f));_material_blend_alpha.assign(n,.7f);
  _indices.resize(n);for(unsigned i=0;i<n;++i)_indices[i]=std::uint16_t(i);
  renderer._render_batches.assign(_batches.size(),WMORenderBatch{});renderer._render_batch_mapping.resize(n);
  for(unsigned i=0;i<_batches.size();++i){renderer._render_batches[i].shader=wmo->materials[_batches[i].texture].shader;for(unsigned j=0;j<3;++j)renderer._render_batch_mapping[i*3+j]=i+1;}
 }
};
// ACTUAL_RENDER_FUNCTIONS
// ACTUAL_BATCH_INIT
Root root(unsigned seed)
{
 Root r;
 // Distinct arrays deliberately exceed the 15-sampler budget; reused array
 // layers and repeated references additionally exercise deduplication.
 for(unsigned i=0;i<22;++i)r.textures.push_back(std::make_shared<Texture>(seed+i,i+7));
 std::array<unsigned,5> shaders{23,7,23,9,13};
 for(unsigned i=0;i<shaders.size();++i){r.materials.push_back({shaders[i],{i==3}});std::array<unsigned,9> refs{};for(unsigned j=0;j<9;++j)refs[j]=(i*7+j)%22;r.materials.back().texture1=refs[0];r.materials.back().texture2=refs[1];r.material_textures.push_back(refs);}
 r.material_textures[2][8]=r.material_textures[2][0];
 return r;
}
void populate(Group& group)
{for(unsigned i=0;i<group.wmo->materials.size();++i)group._batches.push_back(Batch{i,0,{},i*3,3});group.load();}
void draw(Group& group)
{OpenGL::Scoped::use_program program;group.renderer.draw(program,math::frustum{},1000.f,glm::vec3{},false,true);}
void verify(Group const& group,std::size_t firstDraw,bool retail=true)
{
 unsigned total=0;
 for(std::size_t d=firstDraw;d<gl.draws.size();++d)
 {
  auto const& call=gl.draws[d];require(call.batches.size()==group._batches.size()*sizeof(WMORenderBatch),"GPU batch table is empty/truncated after upload");
  require(gl.bytes.at(call.elementBuffer).size()==group._batches.size()*3*sizeof(std::uint16_t),"Wrong EBO after another group upload");
  for(unsigned i=call.start/3;i<(call.start+call.count)/3;++i)
  {
   WMORenderBatch batch;std::memcpy(&batch,call.batches.data()+i*sizeof(batch),sizeof(batch));
   auto const material=Noggit::Format::wmo_material_index(group._batches[i],group.wmo->modern,group.wmo->materials.size());
   unsigned count=Noggit::Format::wmo_texture_count(batch.shader, retail);
   require(batch.shader==group.wmo->materials[material].shader,"Wrong batch shader");
   for(unsigned s=0;s<count;++s){auto const& texture=group.wmo->textures[group.wmo->material_textures[material][s]];require(batch.sampler(s)<15,"Sampler budget exceeded");require(call.samplers[batch.sampler(s)]==texture->array,"Wrong cold/warm sampler array");require(batch.layer(s)==texture->layer,"Wrong layer in packed material");}
  }
  total+=call.count;
 }
 require(total==group._batches.size()*3,"Batches omitted or drawn twice");
}
int main()
{
 try
 {
  // Execute the loader's normalized MOCV path with BGRA bytes. Retail uses
  // client precomputed-light gain, while older clients retain their baseline.
  Root colorRoot;Group colorGroup(&colorRoot);
  colorRoot.flags.do_not_fix_vertex_color_alpha=true;
  for(bool retail : {false,true})
  {
   BlizzardArchive::ClientFile file{{0xA0402010u}};
   colorGroup.load_mocv(file,4,retail);
   auto const color=colorGroup._vertex_colors[0];float const gain=retail?2.f:1.f;
   require(glm::length(glm::vec3(color)-glm::vec3(64,32,16)*(gain/255.f))<.00001f,"MOCV bypass path lost RGB channels or precomputed-light gain");
   require(color.w==0.f&&file.pos==4,"MOCV interior alpha/stream cursor changed");
  }
  colorGroup.header.flags.exterior_lit=true;
  BlizzardArchive::ClientFile exteriorFile{{0xA0402010u}};
  colorGroup.load_mocv(exteriorFile,4,true);
  require(colorGroup._vertex_colors[0].w==1.f,"Retail exterior-lit group lost light alpha");
  colorGroup.header.flags.exterior_lit=false;
  colorGroup.header.transparency_batches_count=1;
  colorGroup._batches.push_back(Batch{});
  BlizzardArchive::ClientFile alphaFile{{0xA0402010u,0xFF402010u}};
  colorGroup.load_mocv(alphaFile,8,true);
  require(std::abs(colorGroup._vertex_colors[0].w-160.f/255.f)<.00001f&&colorGroup._vertex_colors[1].w==0.f,"MOCV transition alpha was multiplied with RGB");
  colorRoot.flags.do_not_fix_vertex_color_alpha=false;
  colorRoot.ambient_light_color=glm::vec4(16.f/255.f,8.f/255.f,4.f/255.f,0);
  colorGroup.header.transparency_batches_count=0;
  for(bool retail : {false,true})
  {
   BlizzardArchive::ClientFile file{{0x80402010u,0xFFFFFFFFu}};
   colorGroup.load_mocv(file,8,retail);
   float const intensity=retail?2.f:(128.f/255.f)/64.f;
   auto const expected=glm::vec3(64,32,16)/255.f*(1.f+intensity)-glm::vec3(colorRoot.ambient_light_color);
   require(glm::length(glm::vec3(colorGroup._vertex_colors[0])-expected)<.00001f,"MOCV fixup used normalized alpha as a byte intensity");
   if(retail)require(glm::vec3(colorGroup._vertex_colors[1])==glm::vec3(2.f),"Retail MOCV intensity exceeded normalized limit");
  }
  // Missing optional environment maps must not render the placeholder BLP.
  for(auto project : {Noggit::Project::ProjectVersion::RETAIL,Noggit::Project::ProjectVersion::LEGACY})
  {
   Noggit::Project::project.projectVersion=project;
   for(unsigned shader : {3u,5u,7u,12u,23u})
   {
    Root envRoot;envRoot.materials.resize(1);envRoot.material_missing_textures.push_back(0x101);envRoot.materials[0].shader=shader;
    envRoot.materials[0].texture_offset_1=envRoot.materials[0].texture_offset_2=envRoot.materials[0].texture_offset_3=0;
    Group envGroup(&envRoot);envGroup._vertices.resize(1);envGroup._batches.push_back(Batch{});
    envGroup.renderer.initRenderBatches();
    require(bool(envGroup.renderer._render_batches[0].flags&eWMOBatch_NoEnvironment)==(project==Noggit::Project::ProjectVersion::RETAIL),"Optional WMO environment handling crossed client boundary");
    require((envGroup.renderer._render_batches[0].flags>>8)==(project==Noggit::Project::ProjectVersion::RETAIL?0x101u:0u),"Absent texture mask crossed client boundary or lost high slots");
   }
  }
  Noggit::Project::project.projectVersion=Noggit::Project::ProjectVersion::RETAIL;
  // Material42 in the reported building: shader12 has an intentionally absent
  // emissive stage. Nonzero references are preserved even if they later fail.
  require(Noggit::Format::wmo_missing_texture_mask({6256152,1267167,0},3)==4,"Missing emissive stage was interpreted as a placeholder texture");
  require(Noggit::Format::wmo_missing_texture_mask({1,2,3,4,5,6,7,0,0},9)==0x180,"Raw high texture slots were mistaken for runtime aliases");
  // Authored Retail ambient is BGRA, not the legacy CArgb struct order. These
  // bytes are representative of Sunfury Spire and the directional building02.
  std::array<std::uint8_t,144> ambientBytes{};
  auto storeColor=[&](unsigned at,std::uint32_t bgra)
  {for(unsigned byte=0;byte<4;++byte)ambientBytes[at+byte]=std::uint8_t(bgra>>(8*byte));};
  storeColor(20,0xff37114c);storeColor(24,0xffffffff);storeColor(28,0xffffffff);
  ambientBytes[36]=2;
  storeColor(48+20,0xff505794);storeColor(48+24,0xff4b4376);storeColor(48+28,0xff382f6b);
  ambientBytes[48+32]=1;ambientBytes[48+36]=3;ambientBytes[48+37]=1;
  storeColor(96+20,0xff818181); // DefaultGlobal follows the selected-set records.
  auto volumes=Noggit::Format::read_wmo_ambient_volumes(ambientBytes);
  auto near=[](glm::vec3 a,glm::vec3 b){return glm::length(a-b)<.00001f;};
  require(volumes.size()==3 && volumes[1].doodad_set==259,"Ambient parser lost the 16-bit doodad-set selector");
  require(near(volumes[0].colors[0],glm::vec3(55,17,76)/255.f)
    && volumes[0].colors[0]==volumes[0].colors[1] && volumes[0].colors[0]==volumes[0].colors[2],
    "Uniform ambient used the wrong color order or unused horizon/ground fields");
  require(near(volumes[1].colors[0],glm::vec3(80,87,148)/255.f)
    && near(volumes[1].colors[1],glm::vec3(75,67,118)/255.f)
    && near(volumes[1].colors[2],glm::vec3(56,47,107)/255.f),"Directional ambient lost sky/horizon/ground");
  std::array<std::uint16_t,2> sets{259,2};
  auto ambient=Noggit::Format::wmo_ambient_colors(volumes,sets,glm::vec3(0));
  require(ambient==volumes[0].colors,"Active-set ambient ignored authored record order");
  ambient=Noggit::Format::wmo_ambient_colors(volumes,std::span(sets).first(1),glm::vec3(0));
  require(ambient==volumes[1].colors,"Selected Retail doodad set did not choose its ambient");
  ambient=Noggit::Format::wmo_ambient_colors(volumes,{},glm::vec3(0));
  require(ambient==volumes[2].colors,"DefaultGlobal ambient was not active without a selected set");
  std::rotate(volumes.begin(),volumes.begin()+2,volumes.end());
  require(Noggit::Format::wmo_ambient_colors(volumes,sets,glm::vec3(0))==volumes[0].colors,
    "Selected-set ambient incorrectly overrode an earlier active DefaultGlobal record");
  require(Noggit::Format::wmo_ambient_colors(std::span(volumes).subspan(1),{},glm::vec3(0))==volumes[1].colors,
    "Unmatched ambient selectors did not fall back to the first record");
  auto fallback=glm::vec3(.1f,.2f,.3f);
  ambient=Noggit::Format::wmo_ambient_colors({},sets,fallback);
  require(ambient[0]==fallback && ambient[1]==fallback && ambient[2]==fallback,
    "WMO without ambient volumes did not retain all MOHD ambient channels");
  require(Noggit::Format::read_wmo_ambient_volumes({}).empty(),"Empty ambient chunk was not accepted");
  for(unsigned size:{1u,47u,49u,143u})
  {
    bool rejected=false;
    try{Noggit::Format::read_wmo_ambient_volumes(std::span(ambientBytes).first(size));}
    catch(std::runtime_error const&){rejected=true;}
    require(rejected,"Truncated ambient records were silently accepted");
  }

  // wait_until_loaded is nonblocking; a late referenced texture must defer
  // the whole group without publishing invalid bindings.
  for(unsigned delayedSlot : {0u,8u})
  {
   auto pendingRoot=root(400);Group pending(&pendingRoot);populate(pending);
   auto& delayed=*pendingRoot.textures[pendingRoot.material_textures[0][delayedSlot]];
   delayed.ready=false;auto first=gl.draws.size();draw(pending);draw(pending);
   require(gl.draws.size()==first&&!pending.renderer._uploaded,"Pending BLP published an incomplete group");
   require(!pending.renderer._upload_attempted&&!pending._normals.empty(),"Pending upload lost retry state or CPU streams");
   require(pendingRoot.textures[0]->coldUploads==0,"Pending group partially uploaded textures");
   delayed.ready=true;draw(pending);verify(pending,first);
   first=gl.draws.size();draw(pending);verify(pending,first);
   pending.renderer.unload();delayed.ready=false;draw(pending);draw(pending);
   require(pending.loads==2&&!pending.renderer._uploaded,"Pending context restoration repeats IO or uploads too soon");
   delayed.ready=true;first=gl.draws.size();draw(pending);verify(pending,first);
  }
  auto unusedRoot=root(500);unusedRoot.textures.push_back(std::make_shared<Texture>(599,0));unusedRoot.textures.back()->ready=false;
  Group unused(&unusedRoot);populate(unused);auto first=gl.draws.size();draw(unused);verify(unused,first);
  auto failedRoot=root(600);failedRoot.textures[0]->failed=true;
  Group failed(&failedRoot);populate(failed);first=gl.draws.size();draw(failed);draw(failed);
  require(gl.draws.size()==first&&failed.renderer._upload_attempted&&!failed.renderer._uploaded,"Failed texture drew invalid resources or remained retryable");
  auto originalRoot=root(100),otherRoot=root(200);Group original(&originalRoot),other(&otherRoot);populate(original);populate(other);
  gl.activeTexture(GL_TEXTURE0+12);auto begin=gl.draws.size();draw(original);verify(original,begin);
  require(gl.draws.size()-begin>=3,"Sampler limit/cull boundary was not exercised");
  auto cold=originalRoot.textures[0]->coldUploads;begin=gl.draws.size();draw(original);verify(original,begin);require(originalRoot.textures[0]->coldUploads==cold,"Warm draw unexpectedly uploads textures");
  // Cold upload/setup with another cached VAO still bound must not replace
  // that VAO's EBO.
  gl.vao=original.renderer._vao;auto originalEbo=gl.indices[gl.vao];
  draw(other);require(gl.indices[original.renderer._vao]==originalEbo,"Cold WMO upload/setup replaced another VAO's EBO");
  gl.vao=0;begin=gl.draws.size();draw(original);verify(original,begin);
  Group fresh(&originalRoot);populate(fresh);begin=gl.draws.size();draw(fresh);verify(fresh,begin);
  // Same sampler/cull/contiguous geometry must still split on every blend mode.
  auto blendedRoot=root(300);blendedRoot.materials.clear();blendedRoot.material_textures.clear();
  for(unsigned mode=0;mode<14;++mode){blendedRoot.materials.push_back({0,{false},mode});blendedRoot.material_textures.push_back({});}
  Group blended(&blendedRoot);populate(blended);begin=gl.draws.size();draw(blended);verify(blended,begin);
  require(gl.draws.size()-begin==14,"Different blend modes were merged into one GPU draw");
  for(unsigned mode=0;mode<14;++mode){auto const& d=gl.draws[begin+mode];require(d.blend==(mode>1),"Wrong blend enable");require(d.depth==(mode<2||mode==8||mode==9),"Wrong material depth-write");}
  require(gl.draws[begin+2].factors==std::array<unsigned,4>{GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA},"Alpha blend factors differ from material policy");
  require(gl.draws[begin+3].factors==std::array<unsigned,4>{GL_SRC_ALPHA,GL_ONE,GL_ZERO,GL_ONE},"Additive blend factors incorrect");
  require(!gl.blend&&gl.depth,"WMO leaves transparent state in the next renderer");
  Noggit::Project::project.projectVersion=Noggit::Project::ProjectVersion::LEGACY;
  Group legacy(&blendedRoot);populate(legacy);gl.blend=true;gl.depth=false;begin=gl.draws.size();draw(legacy);
  require(gl.draws.size()-begin==1&&gl.blend&&!gl.depth,"Legacy rendering state changed");
  require(!gl.attributes[legacy.renderer._vao].count("material_weights")
      && !gl.attributes[legacy.renderer._vao].count("texcoord_3"),"Retail vertex streams leaked into a non-Retail VAO");
  require(!gl.bytes.count(legacy.renderer._material_weights_buffer),"Non-Retail upload allocated Retail vertex data");
  Noggit::Project::project.projectVersion=Noggit::Project::ProjectVersion::RETAIL;
  for(auto version : {Noggit::Project::ProjectVersion::LEGACY,Noggit::Project::ProjectVersion::SL,Noggit::Project::ProjectVersion::FOREVER})
  {
   Noggit::Project::project.projectVersion=version;
   auto nonRetailRoot=root(700);nonRetailRoot.materials.resize(1);nonRetailRoot.material_textures.resize(1);
   for(unsigned slot=2;slot<9;++slot)nonRetailRoot.textures[slot]->ready=false;
   Group nonRetail(&nonRetailRoot);populate(nonRetail);first=gl.draws.size();draw(nonRetail);verify(nonRetail,first,false);
   require(!gl.attributes[nonRetail.renderer._vao].count("material_weights"),"Retail weights enabled for another version");
  }
  Noggit::Project::project.projectVersion=Noggit::Project::ProjectVersion::RETAIL;
  require(original.loads==1,"Normal drawing unexpectedly rereads the asset");
  original.renderer.unload();require(original.loads==1,"Context cleanup must not perform file IO");
  begin=gl.draws.size();draw(original);verify(original,begin);require(original.loads==2,"CPU geometry was not rehydrated for context recreation");
  auto const normal=gl.attributes[original.renderer._vao]["normal"];
  require(gl.bytes[normal].size()==original._vertices.size()*sizeof(glm::vec3),"Recreated VAO has no normal data");
  original.renderer.unload();original.failLoad=true;begin=gl.draws.size();draw(original);require(gl.draws.size()==begin,"Failed reload must not draw partial resources");
  draw(original);require(original.loads==3,"Failed reload was retried every frame");
  auto array=TextureManager::get_tex_array(64,64,7,Noggit::map);auto oldArray=array.array;
  auto otherContext=TextureManager::get_tex_array(32,32,6,Noggit::preview);auto otherArray=otherContext.array;
  TextureManager::unload_all(Noggit::map);
  require(TextureManager::_tex_arrays[Noggit::map].empty(),"Deleted texture arrays remain in allocator cache");
  require(TextureManager::_tex_arrays[Noggit::preview].begin()->second.pages[0].array==otherArray,"Cleanup changed another context");
  auto recreated=TextureManager::get_tex_array(64,64,7,Noggit::map);
  require(recreated.layer==0 && recreated.array!=oldArray,"Texture allocator reused deleted array/layer position");
  require(TextureManager::_.textures[Noggit::map].unloads==1 && TextureManager::_.textures[Noggit::preview].unloads==0,"Texture cleanup crossed context boundary");
  for(unsigned iteration=0;iteration<100;++iteration)
  {
    auto previous=TextureManager::_tex_arrays[Noggit::map].begin()->second.pages[0].array;
    auto deleted=gl.deletedTextures.size();
    auto generation=TextureManager::texture_generation(Noggit::map);
    std::thread release([&]{TextureManager::retire_texture(Noggit::map,previous,0,generation);});
    release.join();
    require(gl.deletedTextures.size()==deleted,"Texture destructor used GL on a worker thread");
    TextureManager::release_retired_textures(Noggit::map);
    auto replacement=TextureManager::get_tex_array(64,64,7,Noggit::map);
    require(TextureManager::_tex_arrays[Noggit::map].begin()->second.pages.size()==1 && replacement.layer==0,"Retired allocations accumulated across repeated loads");
    require(gl.deletedTextures.size()==deleted+1 && gl.deletedTextures.back()==previous,"Retired texture was not deleted on allocation");
    require(TextureManager::_tex_arrays[Noggit::preview].begin()->second.pages[0].array==otherArray,"Retirement crossed a context boundary");
  }
  auto deleted=gl.deletedTextures.size();
  TextureManager::retire_texture(Noggit::map,oldArray,0,TextureManager::texture_generation(Noggit::map));
  TextureManager::release_retired_textures(Noggit::map);
  require(gl.deletedTextures.size()==deleted,"Unknown retired handle was deleted after context reset");
  auto stale=TextureManager::_tex_arrays[Noggit::map].begin()->second.pages[0].array;
  auto staleGeneration=TextureManager::texture_generation(Noggit::map);
  TextureManager::unload_all(Noggit::map);
  // A driver is allowed to reuse the same GLuint after a context reset.
  gl.next=stale;
  auto reused=TextureManager::get_tex_array(64,64,7,Noggit::map);
  require(reused.array==stale,"Fixture did not recycle the GL name");
  deleted=gl.deletedTextures.size();
  TextureManager::retire_texture(Noggit::map,stale,0,staleGeneration);
  TextureManager::release_retired_textures(Noggit::map);
  require(gl.deletedTextures.size()==deleted && TextureManager::_tex_arrays[Noggit::map].begin()->second.pages.size()==1,"Stale retirement deleted a current-generation GL texture");
  // A layer removal can precede reclamation in another tile's upload. Execute
  // the terrain cache pruning against diffuse, height and extended references.
  TileMap terrain;
  terrain.mChunks[0][0]=std::make_unique<TileChunk>();
  terrain.mChunks[1][0]=std::make_unique<TileChunk>();
  auto& layers=*terrain.mChunks[0][0]->texture_set;
  for(unsigned id : {2001u,2002u,2003u,2004u,2005u})
    layers.textures.push_back(std::make_shared<Texture>(id,0));
  layers.heightTextures[2]=std::make_shared<Texture>(2006,0);
  terrain.mChunks[1][0]->texture_set->textures.push_back(std::make_shared<Texture>(2010,0));
  TileRender tileRenderer{&terrain,{}};
  tileRenderer._draw_calls.push_back({{2001,2002,2003,2004,2005,2006,static_cast<int>(stale),2010,-1,-1,-1,-1,-1},0,1});
  tileRenderer._draw_calls.push_back({{2010,2001,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},1,1});
  TextureManager::retire_texture(Noggit::map,stale,0,TextureManager::texture_generation(Noggit::map));
  TextureManager::release_retired_textures(Noggit::map);
  require(gl.deletedTextures.back()==stale,"Fixture did not retire the cached terrain name");
  tileRenderer.pruneUnusedSamplers();
  auto const& bindings=tileRenderer._draw_calls[0].samplers;
  require(bindings==std::array<int,13>{2001,2002,2003,2004,2005,2006,-1,-1,-1,-1,-1,-1,-1},"Terrain pruning lost live layer bindings or retained a retired name");
  require(tileRenderer._draw_calls[1].samplers[0]==2010 && tileRenderer._draw_calls[1].samplers[1]==-1,"Terrain pruning crossed draw-call chunk ranges");
  auto unchanged=bindings;
  tileRenderer.pruneUnusedSamplers();
  require(bindings==unchanged,"Alpha-only update changed live sampler indices");
  layers.textures[1]=std::make_shared<Texture>(2020,0);
  layers.heightTextures[2].reset();
  tileRenderer.pruneUnusedSamplers();
  require(bindings[1]==-1 && bindings[5]==-1 && bindings[4]==2005,"Layer replacement retained obsolete diffuse/height names");
  layers.flags[0]=(3<<3)|7; // Disabled animation may retain direction/speed.
  layers.flags[1]=(1<<6)|(7<<3)|2;
  layers.flags[4]=(1<<6)|(3<<3)|7;
  int const nonAnimationBits=9|(3<<11)|(12<<15)|(15<<19);
  tileRenderer._chunk_layer_ext_data[0].params_packed=nonAnimationBits;
  tileRenderer.updateTextureAnimation(terrain.mChunks[0][0].get(),0);
  auto const& animation=tileRenderer._chunk_instance_data[0];
  require(animation.ChunkTexDoAnim[0]==0 && animation.ChunkTexDoAnim[1]==1
    && animation.ChunkTexAnimSpeed[1]==7 && animation.ChunkTexAnimDir[1]==2,"Animation flags did not follow the current base layers");
  require(tileRenderer._chunk_layer_ext_data[0].params_packed==(nonAnimationBits|(1<<4)|(3<<5)|(7<<8)),"Extended animation update changed UV/height packing or lost its flags");
  std::swap(layers.flags[1],layers.flags[4]);
  tileRenderer.updateTextureAnimation(terrain.mChunks[0][0].get(),0);
  require(animation.ChunkTexAnimSpeed[1]==3 && animation.ChunkTexAnimDir[1]==7,"Reordered layer retained stale animation metadata");
  layers.textures.resize(1);
  tileRenderer.updateTextureAnimation(terrain.mChunks[0][0].get(),0);
  require(animation.ChunkTexDoAnim[1]==0 && animation.ChunkTexAnimSpeed[1]==0 && animation.ChunkTexAnimDir[1]==0
    && tileRenderer._chunk_layer_ext_data[0].params_packed==nonAnimationBits,"Removed layer retained animation flags");
  layers.textures.clear();
  tileRenderer.pruneUnusedSamplers();
  tileRenderer.updateTextureAnimation(terrain.mChunks[0][0].get(),0);
  require(std::all_of(bindings.begin(),bindings.end(),[](int id){return id==-1;}),"Empty chunk retained texture names");
  require(animation.ChunkTexDoAnim[0]==0 && animation.ChunkTexAnimSpeed[0]==0 && animation.ChunkTexAnimDir[0]==0,"Empty chunk retained animation metadata");
  // Retail reuses bounded, compatible pages. Releasing one slice must not
  // delete or renumber any live sibling, and the final release frees the page.
  auto* application=Noggit::Application::NoggitApplication::instance();
  application->client_version=BlizzardArchive::ClientVersion::RETAIL;
  std::vector<TexArrayAllocation> pageSlots;
  for(unsigned i=0;i<17;++i)pageSlots.push_back(TextureManager::get_tex_array(64,64,7,Noggit::map));
  for(unsigned i=0;i<16;++i)require(pageSlots[i].array==pageSlots[0].array && pageSlots[i].layer==i,"Compatible Retail textures did not share stable slices");
  require(pageSlots[16].array!=pageSlots[0].array && pageSlots[16].layer==0,"Retail page exceeded its bounded slice count");
  // Switching client targets can leave Retail pages alive in this context.
  // Non-Retail allocations must still use their historical single-slice pages.
  application->client_version=BlizzardArchive::ClientVersion::LEGACY;
  auto legacyFirst=TextureManager::get_tex_array(64,64,7,Noggit::map);
  auto legacySecond=TextureManager::get_tex_array(64,64,7,Noggit::map);
  require(legacyFirst.array!=pageSlots[0].array && legacyFirst.array!=pageSlots[16].array
    && legacySecond.array!=pageSlots[0].array && legacySecond.array!=pageSlots[16].array
    && legacySecond.array!=legacyFirst.array && legacyFirst.layer==0 && legacySecond.layer==0,
    "Non-Retail allocation reused a free slice in a live Retail page");
  auto const& boundaryPages=TextureManager::_tex_arrays[Noggit::map].at(std::make_tuple(-1,64,64,7)).pages;
  require(boundaryPages.size()==4 && boundaryPages[2].used.size()==1 && boundaryPages[3].used.size()==1
    && std::count(boundaryPages[0].used.begin(),boundaryPages[0].used.end(),true)==16
    && std::count(boundaryPages[1].used.begin(),boundaryPages[1].used.end(),true)==1,
    "Client-version switch changed live siblings or allocated shared legacy pages");
  TextureManager::retire_texture(Noggit::map,legacyFirst.array,0,TextureManager::texture_generation(Noggit::map));
  TextureManager::retire_texture(Noggit::map,legacySecond.array,0,TextureManager::texture_generation(Noggit::map));
  TextureManager::release_retired_textures(Noggit::map);
  require(boundaryPages.size()==2,"Releasing legacy pages changed live Retail pages");
  application->client_version=BlizzardArchive::ClientVersion::RETAIL;
  deleted=gl.deletedTextures.size();
  auto generation=TextureManager::texture_generation(Noggit::map);
  std::thread releaseSlice([&]{TextureManager::retire_texture(Noggit::map,pageSlots[5].array,pageSlots[5].layer,generation);});
  releaseSlice.join();
  require(gl.deletedTextures.size()==deleted,"Worker released shared GPU storage");
  TextureManager::release_retired_textures(Noggit::map);
  require(gl.deletedTextures.size()==deleted,"Releasing one slice deleted its live siblings");
  auto reusedSlice=TextureManager::get_tex_array(64,64,7,Noggit::map);
  require(reusedSlice.array==pageSlots[5].array && reusedSlice.layer==5,"Released slice was not reused without renumbering siblings");
  for(unsigned i=0;i<16;++i)TextureManager::retire_texture(Noggit::map,pageSlots[i].array,pageSlots[i].layer,generation);
  TextureManager::release_retired_textures(Noggit::map);
  require(gl.deletedTextures.size()==deleted+1 && gl.deletedTextures.back()==pageSlots[0].array,"Final shared-slice release did not free exactly its page");
  TextureManager::retire_texture(Noggit::map,pageSlots[16].array,0,generation);
  TextureManager::release_retired_textures(Noggit::map);
  auto large=TextureManager::get_tex_array(2048,2048,12,Noggit::map);
  auto const& largePage=TextureManager::_tex_arrays[Noggit::map].at(std::make_tuple(-1,2048,2048,12)).pages.front();
  require(largePage.array==large.array && largePage.used.size()==1,"Large texture reserved an unbounded multi-slice page");
  TextureManager::retire_texture(Noggit::map,large.array,0,generation);
  TextureManager::release_retired_textures(Noggit::map);
  // An untouched Retail tile can use eight distinct diffuse arrays and four
  // distinct height arrays in one chunk. The old 11-slot budget dropped layer 8.
  TileMap layered;
  for(unsigned i=0;i<256;++i)
  {
    auto& chunk=layered.mChunks[i%16][i/16];
    chunk=std::make_unique<TileChunk>();chunk->mt=&layered;chunk->px=i%16;chunk->py=i/16;
  }
  auto& eight=*layered.mChunks[1][1]->texture_set;
  for(unsigned i=0;i<8;++i)
  {
    eight.textures.push_back(std::make_shared<Texture>(3000+i,10+i));
    eight.mapping[i]={int(i%3+1),float(i+2),float(i)/10};
    if(i<4)eight.heightTextures[i]=std::make_shared<Texture>(3100+i,20+i);
  }
  TileRender layeredRenderer{&layered,{}};
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady(),"Eight-layer Retail chunk exceeded the sampler budget or retained a stale reset flag");
  auto& assigned=layeredRenderer._draw_calls.front().samplers;
  auto const& instance=layeredRenderer._chunk_instance_data[17];
  for(unsigned i=0;i<8;++i)
  {
    auto const& extra=layeredRenderer._chunk_layer_ext_data[17*12+(i<4?0:i-4)];
    auto sampler=i<4?instance.ChunkTextureSamplers[i]:(extra.params_packed&15);
    auto index=i<4?instance.ChunkTextureArrayIDs[i]:extra.array_index;
    require(assigned[sampler]==3000+i && index==10+i,"Retail layer mapped to the wrong diffuse array or slice");
    require((i<4?instance.ChunkTextureUVScale[i]:(extra.params_packed>>11)&15)==int(i%3+1),"MTXP scale was lost for a base or extended layer");
    if(i<4)
    {
      require(assigned[instance.ChunkHeightTextureSamplers[i]]==3100+i && instance.ChunkHeightTextureArrayIDs[i]==20+i,"Retail height layer lost its array or slice");
      require(instance.ChunkTextureHeightScale[i]==float(i+2),"Height parameters were lost");
    }
  }
  require(std::count_if(assigned.begin(),assigned.end(),[](int id){return id>=0;})==12,"Eight-layer fixture did not use twelve distinct arrays");
  // Pruning can leave a hole before a matching live name. Do not insert a
  // duplicate there or shift the other chunks' existing sampler indices.
  auto live=assigned;
  std::swap(assigned[0],assigned[12]);
  require(layeredRenderer.fillSamplers(layered.mChunks[1][1].get(),17,0),"A free hole caused sampler exhaustion");
  require(assigned[0]==-1 && assigned[12]==live[0],"Diffuse matching filled an earlier hole instead of reusing a live binding");
  std::swap(assigned[0],assigned[12]);
  std::swap(assigned[1],assigned[12]);
  require(layeredRenderer.fillSamplers(layered.mChunks[1][1].get(),17,0),"A free hole caused height sampler exhaustion");
  require(assigned[1]==-1 && assigned[12]==live[1],"Height matching filled an earlier hole instead of reusing a live binding");
  // A genuine over-budget chunk must not become render/export ready when the
  // retry in its own draw call also fails. A later layer edit can recover.
  auto& crowded=*layered.mChunks[2][1]->texture_set;
  for(unsigned i=0;i<10;++i)
  {
    crowded.textures.push_back(std::make_shared<Texture>(3200+i,i));
    if(i<4)crowded.heightTextures[i]=std::make_shared<Texture>(3300+i,i);
  }
  auto const beforeFailedAppend=layeredRenderer._draw_calls.front().samplers;
  require(!layeredRenderer.fillSamplers(layered.mChunks[2][1].get(),18,0),"Fixture did not exhaust the existing draw call");
  require(layeredRenderer._draw_calls.front().samplers==beforeFailedAppend,"Failed append left partial bindings in the previous draw call");
  layeredRenderer._split_drawcall=true;
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer._sampler_error && !layeredRenderer._texture_not_loaded,"Unrepresentable chunk was published as ready or marked as an asynchronous load");
  require(!layeredRenderer.texturesReady() && !layeredRenderer._requires_sampler_reset,"Permanent sampler overflow became ready or scheduled endless resets");
  crowded.textures.resize(8);
  layeredRenderer.rebuildDrawCalls();
  require(!layeredRenderer._sampler_error && layeredRenderer._draw_calls.size()==2,"Sampler overflow did not recover after reducing the layer set");
  unsigned chunksDrawn=0;for(auto const& call:layeredRenderer._draw_calls)chunksDrawn+=call.n_chunks;
  require(chunksDrawn==256,"Draw-call split skipped or duplicated chunks");
  eight.textures[0]->resident=false;eight.textures[0]->ready=false;
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer._texture_not_loaded && !layeredRenderer._sampler_error,"Pending texture confused capacity failure with asynchronous readiness");
  eight.textures[0]->ready=true;
  layeredRenderer._texture_not_loaded=false;
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady(),"Finished BLP did not restore sampler readiness");
  layeredRenderer.pruneUnusedSamplers(true);
  require(layeredRenderer.texturesReady(),"Throttled alpha-only update invalidated unchanged bindings");
  // The upload budget can defer chunk UBO/alpha refresh after pruning a
  // released binding. Its previous mapping must not render on any skipped
  // frame, even though all BLP payloads are resident by the next frame.
  eight.textures[0]=std::make_shared<Texture>(3400,30);
  eight.textures[0]->upload();
  layeredRenderer.pruneUnusedSamplers(true);
  require(!layeredRenderer.texturesReady() && layeredRenderer._requires_sampler_reset
    && !layeredRenderer._sampler_error,"Deferred replacement left an obsolete sampler mapping ready");
  for(int skippedFrame=0;skippedFrame<3;++skippedFrame)
  {
    layeredRenderer._texture_not_loaded=false;
    layeredRenderer.pruneUnusedSamplers(true);
    require(!layeredRenderer.texturesReady(),"Repeated skipped frame cleared invalid mapping readiness");
  }
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady(),"Full mapping refresh did not restore a deferred replacement");
  auto const replacementSampler=layeredRenderer._chunk_instance_data[17].ChunkTextureSamplers[0];
  require(layeredRenderer._draw_calls.front().samplers[replacementSampler]==3400,"Refreshed mapping retained the replaced diffuse name");
  eight.textures.pop_back();
  layeredRenderer.pruneUnusedSamplers(true);
  require(!layeredRenderer.texturesReady(),"Deferred layer removal left an obsolete mapping ready");
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady()
    && layeredRenderer._chunk_instance_data[17].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[2]==7,
    "Mapping refresh did not publish the reduced layer count");
  // All sixteen diffuse/height pairs use their actual page allocations. The
  // extended record must preserve both identities without consuming 32 samplers.
  for(auto& row:layered.mChunks)for(auto& chunk:row)
  {
    chunk->texture_set->textures.clear();chunk->texture_set->heightTextures.fill(nullptr);
    chunk->texture_set->flags.fill(0);chunk->texture_set->mapping.fill({});
  }
  auto& sixteen=eight;
  for(unsigned i=0;i<16;++i)
  {
    auto diffuse=TextureManager::get_tex_array(64,64,7,Noggit::map);
    auto height=TextureManager::get_tex_array(128,128,8,Noggit::map);
    auto texture=std::make_shared<Texture>(diffuse.array,diffuse.layer);
    texture->specular=i%2!=0;
    sixteen.textures.push_back(texture);
    sixteen.heightTextures[i]=std::make_shared<Texture>(height.array,height.layer);
    sixteen.mapping[i]={int(i%4+1),float(i+2),float(i)/10};
  }
  // Same nontrivial height material as the observed untouched leaf seam.
  sixteen.mapping[3]={1,10.f,1.f};
  sixteen.mapping[0]={2,8.f,1.f};
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady(),"Full sixteen-layer height mapping was not ready");
  auto const& fullBindings=layeredRenderer._draw_calls.front().samplers;
  require(std::count_if(fullBindings.begin(),fullBindings.end(),[](int id){return id>=0;})==2,"Compatible diffuse/height pages did not fit the terrain sampler budget");
  auto verifyLayer=[&](unsigned slot)
  {
    auto const& currentBindings=layeredRenderer._draw_calls.front().samplers;
    auto const& data=layeredRenderer._chunk_instance_data[17];
    auto const& ext=layeredRenderer._chunk_layer_ext_data[17*12+(slot<4?0:slot-4)];
    bool base=slot<4;
    int diffuseSampler=base?data.ChunkTextureSamplers[slot]:ext.params_packed&15;
    int diffuseIndex=base?data.ChunkTextureArrayIDs[slot]:ext.array_index;
    int heightSampler=base?data.ChunkHeightTextureSamplers[slot]:(ext.params_packed>>15)&15;
    int heightIndex=base?data.ChunkHeightTextureArrayIDs[slot]:(ext.params_packed>>19)&255;
    int uv=base?data.ChunkTextureUVScale[slot]:(ext.params_packed>>11)&15;
    float scale=base?data.ChunkTextureHeightScale[slot]:ext.height_scale;
    float offset=base?data.ChunkTextureHeightOffset[slot]:ext.height_offset;
    auto const& diffuse=*sixteen.textures[slot];auto const& height=*sixteen.heightTextures[slot];
    require(currentBindings[diffuseSampler]==diffuse.array && (diffuseIndex<0?-(diffuseIndex+1):diffuseIndex)==int(diffuse.layer),"Packed diffuse page/slice is wrong");
    require((diffuseIndex>=0)==diffuse.specular,"Diffuse slice zero lost its non-specular encoding");
    require(currentBindings[heightSampler]==height.array && heightIndex==int(height.layer),"Packed height page/slice is wrong");
    require(uv==sixteen.mapping[slot].uvScale && scale==sixteen.mapping[slot].heightScale && offset==sixteen.mapping[slot].heightOffset,"Packed extended height/UV parameters are wrong");
  };
  for(unsigned i=0;i<16;++i)verifyLayer(i);
  auto leafDiffuse=sixteen.textures[3];auto leafHeight=sixteen.heightTextures[3];
  std::swap(sixteen.textures[3],sixteen.textures[4]);
  std::swap(sixteen.heightTextures[3],sixteen.heightTextures[4]);
  std::swap(sixteen.mapping[3],sixteen.mapping[4]);
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady() && sixteen.textures[4]==leafDiffuse && sixteen.heightTexture(4)==leafHeight.get(),"Fourth-to-fifth leaf layer lost its height override");
  for(unsigned i=0;i<16;++i)verifyLayer(i);
  require(sixteen.heightTexture(15)==sixteen.heightTextures[15].get() && sixteen.heightTexture(16)==nullptr,"Height lookup still truncates extended layers or accepts an invalid slot");
  auto& packedLast=layeredRenderer._chunk_layer_ext_data[17*12+11];
  auto packedBefore=packedLast.params_packed;
  sixteen.flags[15]=(1<<6)|(5<<3)|3;
  layeredRenderer.updateTextureAnimation(layered.mChunks[1][1].get(),17);
  require((packedLast.params_packed&~(0x7F<<4))==(packedBefore&~(0x7F<<4)),"Animation refresh erased extended UV/height fields");
  // A surviving GLuint does not make an old slice mapping valid during an
  // alpha-budget skip: both diffuse and height replacements must invalidate it.
  auto const oldPage=sixteen.textures[4]->array;
  sixteen.textures[4]=std::make_shared<Texture>(oldPage,15);sixteen.textures[4]->upload();
  layeredRenderer.pruneUnusedSamplers(true);
  require(!layeredRenderer.texturesReady() && layeredRenderer._requires_sampler_reset,"Shared-page replacement retained an obsolete diffuse slice");
  for(int frame=0;frame<3;++frame){layeredRenderer.pruneUnusedSamplers(true);require(!layeredRenderer.texturesReady(),"Repeated skip restored a stale shared-page mapping");}
  layeredRenderer.rebuildDrawCalls();require(layeredRenderer.texturesReady(),"Shared-page diffuse replacement did not recover");
  sixteen.heightTextures[15]=std::make_shared<Texture>(sixteen.heightTextures[15]->array,1);sixteen.heightTextures[15]->upload();
  layeredRenderer.pruneUnusedSamplers(true);
  require(!layeredRenderer.texturesReady(),"Shared-page replacement retained an obsolete height slice");
  layeredRenderer.rebuildDrawCalls();require(layeredRenderer.texturesReady(),"Shared-page height replacement did not recover");
  sixteen.heightTextures[15]->resident=false;sixteen.heightTextures[15]->ready=false;
  layeredRenderer.rebuildDrawCalls();require(!layeredRenderer.texturesReady() && layeredRenderer._texture_not_loaded,"Unloaded sixteenth height map was published as ready");
  sixteen.heightTextures[15]->ready=true;layeredRenderer._texture_not_loaded=false;
  layeredRenderer.rebuildDrawCalls();require(layeredRenderer.texturesReady(),"Extended height map did not recover after loading");
  sixteen.heightTextures[15].reset();
  layeredRenderer.pruneUnusedSamplers(true);
  require(!layeredRenderer.texturesReady(),"Removed extended height texture retained a stale height contribution");
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady() && packedLast.height_scale==0.f && packedLast.height_offset==1.f,"Missing extended height map did not use the neutral fallback");
  // Mists heightmapping remains available on non-Retail clients, but only
  // Retail consumes the newly supported UV/height data beyond the fourth layer.
  sixteen.textures.clear();sixteen.heightTextures.fill(nullptr);sixteen.flags.fill(0);
  for(unsigned i=0;i<5;++i)
  {
    sixteen.textures.push_back(std::make_shared<Texture>(8000+i,i));
    sixteen.heightTextures[i]=std::make_shared<Texture>(9000+i,i);
    sixteen.mapping[i]={int(i+1),float(i+2),float(i)/10};
  }
  application->client_version=BlizzardArchive::ClientVersion::LEGACY;
  application->modern_features=true;
  sixteen.heightTextures[4]->ready=false;
  require(sixteen.heightTexture(3)==sixteen.heightTextures[3].get()
    && sixteen.heightTexture(4)==nullptr,"Non-Retail height lookup did not keep the four-layer boundary");
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady(),"Unused fifth height texture blocked non-Retail terrain");
  auto const& scopedInstance=layeredRenderer._chunk_instance_data[17];
  auto const& scopedExtra=layeredRenderer._chunk_layer_ext_data[17*12];
  auto scopedBindings=[&]() -> auto const& {return layeredRenderer._draw_calls.front().samplers;};
  for(unsigned i=0;i<4;++i)
    require(sixteen.heightTextures[i]->coldUploads==1
      && scopedInstance.ChunkTextureUVScale[i]==int(i+1)
      && scopedInstance.ChunkTextureHeightScale[i]==float(i+2)
      && scopedBindings()[scopedInstance.ChunkHeightTextureSamplers[i]]==int(9000+i),
      "Retail isolation disabled existing non-Retail Mists heightmapping");
  require(sixteen.heightTextures[4]->coldUploads==0 && (scopedExtra.params_packed>>11)==0
    && scopedExtra.height_scale==0.f && scopedExtra.height_offset==1.f,
    "Retail extended height/UV metadata leaked into non-Retail terrain");
  sixteen.mapping[4]={9,25.f,3.f};
  layeredRenderer.pruneUnusedSamplers(true);
  require(layeredRenderer.texturesReady(),"Unused non-Retail extended metadata invalidated a ready mapping");
  sixteen.heightTextures[0]->resident=false;sixteen.heightTextures[0]->ready=false;
  layeredRenderer.rebuildDrawCalls();
  require(!layeredRenderer.texturesReady() && layeredRenderer._texture_not_loaded,
    "Non-Retail Mists rendering ignored a pending base height texture");
  application->modern_features=false;layeredRenderer._texture_not_loaded=false;
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady(),"Disabled modern features still waited for height textures");
  require(std::count_if(scopedBindings().begin(),scopedBindings().end(),[](int id){return id>=0;})==5,
    "Legacy plain-alpha terrain bound unused height textures");
  for(unsigned i=0;i<4;++i)
    require(scopedInstance.ChunkTextureUVScale[i]==0 && scopedInstance.ChunkTextureHeightScale[i]==0.f
      && scopedInstance.ChunkTextureHeightOffset[i]==1.f,"Disabled modern features retained Mists metadata");
  layeredRenderer.pruneUnusedSamplers(true);
  require(layeredRenderer.texturesReady(),"Legacy sampler pruning consulted unused height metadata");
  application->modern_features=true;
  application->client_version=BlizzardArchive::ClientVersion::RETAIL;
  sixteen.heightTextures[0]->ready=true;
  layeredRenderer.rebuildDrawCalls();
  require(!layeredRenderer.texturesReady() && layeredRenderer._texture_not_loaded,
    "Retail rendering ignored a pending extended height texture");
  sixteen.heightTextures[4]->ready=true;layeredRenderer._texture_not_loaded=false;
  layeredRenderer.rebuildDrawCalls();
  require(layeredRenderer.texturesReady() && sixteen.heightTextures[4]->coldUploads==1
    && ((scopedExtra.params_packed>>11)&15)==9 && scopedExtra.height_scale==25.f
    && scopedExtra.height_offset==3.f
    && scopedBindings()[(scopedExtra.params_packed>>15)&15]==9004,
    "Retail height/UV metadata was not restored after the client boundary");
  // A failed mip upload must return its reserved slice without disturbing a
  // live sibling. Execute the production upload method for both storage paths.
  for(bool compressed:{false,true})
  {
    auto payload=[&]
    {
      UploadTexture texture;
      if(compressed)texture._compression_format=GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
      for(int mip=0,side=8;mip<4;++mip,side=std::max(side/2,1))
      {
        if(compressed)texture._compressed_data[mip].assign(((side+3)/4)*((side+3)/4)*16,static_cast<std::uint8_t>(mip+1));
        else texture._data[mip].assign(side*side,0x10203040u+unsigned(mip));
      }
      return texture;
    };
    auto sibling=payload();sibling.upload();
    require(sibling._uploaded && sibling._array_index==0,"Initial upload did not reserve the first shared slice");
    auto texture=payload();
    auto const rgba=texture._data;
    auto const dxt=texture._compressed_data;
    GLuint previousBinding=0;gl.genTextures(1,&previousBinding);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY,previousBinding);
    gl.failUploadCall=gl.uploadCalls+2;
    bool failed=false;
    try{texture.upload();}catch(std::runtime_error const& error){failed=std::string(error.what())=="Injected mip upload failure";}
    require(failed && !texture._uploaded && texture._texture_array==0 && texture._array_index==-1,"Partial upload retained a ready texture or reserved allocation");
    require(texture._data==rgba && texture._compressed_data==dxt,"Failed upload discarded CPU mip data needed for retry");
    require(gl.textures[{gl.unit,GL_TEXTURE_2D_ARRAY}]==previousBinding,"Failed upload did not restore the previous array binding");
    auto const& pending=TextureManager::_retired_arrays[Noggit::map];
    require(pending.size()==1 && pending.front().array==sibling._texture_array && pending.front().layer==1,"Failed upload did not queue exactly its reserved shared slice");
    require(gl.isTexture(sibling._texture_array),"Failed upload deleted live sibling storage");
    gl.failUploadCall=-1;
    texture.upload();
    require(texture._uploaded && texture._texture_array==sibling._texture_array && texture._array_index==1,"Retry leaked the failed reservation instead of reusing its slice");
    require(texture._data.empty() && texture._compressed_data.empty(),"Successful retry retained CPU mip payloads");
    require(TextureManager::_retired_arrays[Noggit::map].empty() && gl.isTexture(sibling._texture_array),"Retry did not drain the failed slice safely");
    require(gl.textures[{gl.unit,GL_TEXTURE_2D_ARRAY}]==previousBinding,"Successful retry did not restore the previous array binding");
    TextureManager::retire_texture(Noggit::map,texture._texture_array,texture._array_index,texture._texture_generation);
    TextureManager::retire_texture(Noggit::map,sibling._texture_array,sibling._array_index,sibling._texture_generation);
    TextureManager::release_retired_textures(Noggit::map);
    require(!gl.isTexture(sibling._texture_array),"Final upload slice release retained its page");
    gl.deleteTextures(1,&previousBinding);
  }
  std::cout<<"PASS: actual WMO upload/draw packing, cold/warm interleaving, sampler budget, context rehydration and failed reload\n";return 0;
 }catch(std::exception const& error){std::cerr<<error.what()<<'\n';return 1;}
}
