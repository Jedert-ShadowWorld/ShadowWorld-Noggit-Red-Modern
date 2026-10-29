// Execute production upload/unload/setupVao/draw against a recording GL device.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <glm/glm.hpp>
#include <noggit/formats/WmoMaterial.hpp>
using GLuint=unsigned;
using GLenum=unsigned;
using GLint=int;
constexpr unsigned GL_ARRAY_BUFFER=1,GL_ELEMENT_ARRAY_BUFFER=2,GL_TEXTURE_BUFFER=3,
 GL_TEXTURE_2D_ARRAY=4,GL_STATIC_DRAW=5,GL_RGBA32UI=6,GL_FLOAT=7,GL_FALSE=0,
 GL_UNSIGNED_INT=8,GL_TEXTURE0=100,GL_CULL_FACE=9,GL_TRIANGLES=10,GL_UNSIGNED_SHORT=11,
 GL_RGBA8=12,GL_RGBA=13,GL_UNSIGNED_BYTE=14,GL_TEXTURE_MAX_LEVEL=15,GL_TEXTURE_MIN_FILTER=16,
 GL_TEXTURE_MAG_FILTER=17,GL_LINEAR_MIPMAP_LINEAR=18,GL_LINEAR=19,
 GL_BLEND=20,GL_TRUE=1,GL_SRC_ALPHA=21,GL_ONE_MINUS_SRC_ALPHA=22,GL_ONE=23,GL_ZERO=24,
 GL_DST_COLOR=25,GL_SRC_COLOR=26,GL_DST_ALPHA=27,GL_CONSTANT_ALPHA=28,GL_ONE_MINUS_CONSTANT_ALPHA=29,GL_ONE_MINUS_SRC_COLOR=30;
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
 void genTextures(int n,unsigned* ids){while(n--)*ids++=next++;}
 void deleteTextures(int n,unsigned const* ids){while(n--)tbo.erase(*ids++);}
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
namespace BlizzardArchive::Listfile{struct FileKey{std::string stringRepr()const{return "fixture.blp";}};}
struct blp_texture{unsigned unloads=0;void unload(){++unloads;}};
struct Pool
{
 std::array<blp_texture,2> textures;
 template<class F>void context_aware_apply(F function,Noggit::NoggitRenderContext context)
 {function(BlizzardArchive::Listfile::FileKey{},textures[context]);}
};
struct TexArrayParams{std::vector<unsigned> arrays;int n_used=0;};
constexpr unsigned N_ARRAY_TEX=1;
struct TextureManager
{
 static inline Pool _;
 static inline std::array<std::map<std::tuple<int,int,int,int>,TexArrayParams>,2> _tex_arrays;
 static void unload_all(Noggit::NoggitRenderContext);
 static TexArrayParams& get_tex_array(int,int,int,Noggit::NoggitRenderContext);
};
// ACTUAL_TEXTURE_LIFECYCLE
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
 unsigned array,layer,coldUploads=0;bool resident=false,ready=true,failed=false;
 Texture(unsigned a,unsigned l):array(a),layer(l){}
 void wait_until_loaded(){}
 bool finishedLoading()const{return ready;}
 bool loading_failed()const{return failed;}
 void upload(){if(ready&&!resident){++coldUploads;resident=true;gl.bindTexture(GL_TEXTURE_2D_ARRAY,array);}}
 unsigned texture_array()const{return array;}unsigned array_index()const{return layer;}
};
struct Material{unsigned shader=0;struct{bool unculled=false;}flags;unsigned blend_mode=0;unsigned texture1=0,texture2=0;};
using WMOMaterial=Material;
struct Root
{
 bool modern=true;
 std::vector<Material> materials;
 std::vector<std::shared_ptr<Texture>> textures;
 std::vector<std::array<unsigned,9>> material_textures;
 bool uses_file_data_ids()const{return modern;}
};
struct Batch{unsigned texture=0,flags=0;std::array<unsigned,6> unused{};unsigned index_start=0,index_count=3;};
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
 void draw(OpenGL::Scoped::use_program&,math::frustum const&,float const&,glm::vec3 const&,bool,bool);
};
struct Group
{
 Root* wmo;WMOGroupRender renderer{this};unsigned loads=0;bool failLoad=false;
 struct{struct{bool has_two_motv=true,has_vertex_color=true,use_mocv2_for_texture_blending=true;}flags;}header;
 std::vector<Batch> _batches;
 std::vector<glm::vec3> _vertices,_normals;
 std::vector<glm::vec2> _texcoords,_texcoords_2,_texcoords_3,_texcoords_4;
 std::vector<glm::vec4> _vertex_colors,_material_weights;
 std::vector<float> _material_blend_alpha;
 std::vector<std::uint16_t> _indices;
 explicit Group(Root* root):wmo(root){}
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
  auto& array=TextureManager::get_tex_array(64,64,7,Noggit::map);auto oldArray=array.arrays[0];array.n_used=1;
  auto& otherContext=TextureManager::get_tex_array(32,32,6,Noggit::preview);auto otherArray=otherContext.arrays[0];otherContext.n_used=1;
  TextureManager::unload_all(Noggit::map);
  require(TextureManager::_tex_arrays[Noggit::map].empty(),"Deleted texture arrays remain in allocator cache");
  require(TextureManager::_tex_arrays[Noggit::preview].begin()->second.arrays[0]==otherArray,"Cleanup changed another context");
  auto& recreated=TextureManager::get_tex_array(64,64,7,Noggit::map);
  require(recreated.n_used==0 && recreated.arrays.size()==1 && recreated.arrays[0]!=oldArray,"Texture allocator reused deleted array/layer position");
  require(TextureManager::_.textures[Noggit::map].unloads==1 && TextureManager::_.textures[Noggit::preview].unloads==0,"Texture cleanup crossed context boundary");
  std::cout<<"PASS: actual WMO upload/draw packing, cold/warm interleaving, sampler budget, context rehydration and failed reload\n";return 0;
 }catch(std::exception const& error){std::cerr<<error.what()<<'\n';return 1;}
}
