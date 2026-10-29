// Executes the production upload/unload methods against a recording resource cache.
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <glm/glm.hpp>
constexpr unsigned GL_TEXTURE_BUFFER=1,GL_ARRAY_BUFFER=2,GL_ELEMENT_ARRAY_BUFFER=3,
 GL_STREAM_DRAW=4,GL_STATIC_DRAW=5,GL_RGBA32F=6;
struct Device {
 unsigned next=1;
 void genTextures(int n,unsigned* out){while(n--)*out++=next++;}
 void deleteTextures(int,unsigned const*){}
 void bindTexture(unsigned,unsigned){}
 void bufferData(unsigned,std::size_t,void const*,unsigned){}
 void texBuffer(unsigned,unsigned,unsigned){}
} gl;
namespace OpenGL::Scoped {template<unsigned target>struct buffer_binder{explicit buffer_binder(unsigned){}};}
namespace BlizzardArchive::Listfile {
 struct FileKey {
  std::string path;std::uint32_t id=0;
  FileKey(std::string path,std::uint32_t id=0):path(std::move(path)),id(id){}
 };
}
struct Texture {
 BlizzardArchive::Listfile::FileKey key;
 Texture(BlizzardArchive::Listfile::FileKey key,int):key(std::move(key)){}
 Texture(std::string path,int):key(std::move(path)){}
};
struct Resource{void unload(){}void upload(){}};
struct ModelVertex{float data[12]{};};
namespace misc {glm::vec3 transform_model_box_coords(glm::vec3 x){return x;}}
namespace math {std::array<glm::vec3,8> box_points(glm::vec3,glm::vec3){return {};}}
bool retailProject = true;
bool isRetailProject(){return retailProject;}
struct Model {
 glm::vec3 bounding_box_min{},bounding_box_max{};
 std::vector<std::string> _textureFilenames;
 std::vector<std::uint32_t> _textureFileDataIds;
 std::vector<Texture> _textures;
 std::vector<int> _specialTextures;
 void beginTextureLoad(unsigned count){struct{unsigned nTextures;}header{count};
 // ACTUAL_KEY_RESET
 }
 int _context=0;bool animBones=false,anim_calculated=false;
 std::vector<glm::mat4> bone_matrices;
 std::vector<ModelVertex> _vertices;
 std::vector<std::uint16_t> _indices;
 std::vector<Resource> _particles,_ribbons;
};
struct ModelRender {
 struct PreviewSkin{};
 Model* _model;
 std::array<glm::vec3,8> _vertex_box_points{};
 Resource _buffers,_vertex_arrays,_preview_index_buffers;
 std::array<PreviewSkin,2> _preview_skins{};
 unsigned _bone_matrices_buf_tex=0,_bone_matrices_buffer=1,_vertices_buffer=2,_box_vbo=3,_indices_buffer=4,_box_indices_buffer=5;
 std::vector<std::uint16_t> _box_indices;
 std::vector<int> _render_passes;
 bool _bone_matrices_dirty=false,_uploaded=false,_vao_setup=false;
 int _last_preview_animtime=-1;
 unsigned decalClassifications=0;
 void classifyPreviewDecals(std::vector<int> const&){++decalClassifications;}
 void upload();void unload();
};
// ACTUAL_LIFECYCLE
void require(bool ok,char const* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
 Model model;model._textureFilenames={"world/ordinary.blp","world/resolved.blp",""};
 model._textureFileDataIds={0,123456,654321};
 auto const names=model._textureFilenames;auto const ids=model._textureFileDataIds;
 ModelRender renderer{&model};
 for(bool retail : {false,true}) {
 retailProject=retail;
 renderer.decalClassifications=0;
 for(unsigned cycle=0;cycle<3;++cycle){
  renderer.upload();
  require(model._textures.size()==3,"Reupload lost or duplicated M2 texture resources");
  for(unsigned i=0;i<3;++i)require(model._textures[i].key.path==names[i]&&model._textures[i].key.id==ids[i],"M2 reupload changed exact resource key");
  renderer.unload();
  require(model._textures.empty()&&!renderer._uploaded,"Unload retained texture resource handles");
 }
 require(renderer.decalClassifications==(retail?3u:0u),"Retail decal classification leaked into legacy rendering");
 }
 require(model._textureFilenames==names&&model._textureFileDataIds==ids,"M2 key descriptors lost across context cycles");
 model.beginTextureLoad(2);
 require(model._textureFilenames==std::vector<std::string>(2)&&model._textureFileDataIds.empty(),"New model read retained stale path/FDID keys");
 std::cout<<"PASS: production M2 upload/unload preserves exact path/FDID keys across three cycles without retaining texture handles\n";return 0;
}catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}}
