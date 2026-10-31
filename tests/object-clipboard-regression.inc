// Compiles the production copy function against lightweight editor stubs without GPU loading.
#include <noggit/formats/adt/RetailObjectTransfer.hpp>
#include <glm/vec3.hpp>
#include <iostream>
#include <string>
#include <sstream>
#include <variant>

using Noggit::Formats::ADT::RetailObjectTransfer::WmoState;
using Noggit::Formats::ADT::RetailObjectTransfer::WmoSource;
struct Key
{
  std::string path;
  unsigned id=0;
  Key(std::string value, unsigned fileId=0):path(std::move(value)),id(fileId){}
  std::string const& filepath() const
  { if(path.empty())throw std::runtime_error("FileDataID-only asset has no path");return path; }
  std::string stringRepr()const{return path.empty()?std::to_string(id):path;}
};
struct Asset
{
  Key key;
  Key const& file_key() const {return key;}
  Asset* operator->(){return this;}
  Asset const* operator->()const{return this;}
};
enum {eMODEL,eWMO};
struct SceneObject
{
  glm::vec3 pos{},dir{};
  float scale=1;
  virtual int which()const=0;
  virtual Asset* instance_model()=0;
  virtual ~SceneObject()=default;
};
struct ModelInstance:SceneObject
{
  Asset model;
  ModelInstance(Key key,int):model{std::move(key)}{}
  int which()const override{return eMODEL;}
  Asset* instance_model()override{return &model;}
};
struct WMOInstance:SceneObject
{
  Asset wmo;
  std::optional<WmoState> appearance;
  WMOInstance(Key key,int):wmo{std::move(key)}{}
  int which()const override{return eWMO;}
  Asset* instance_model()override{return &wmo;}
  auto retail_state()const{return appearance;}
  void apply_retail_state(WmoState const& value){appearance=value;}
};
using selected_object_type=SceneObject*;
constexpr unsigned eEntry_Object=1;
struct World
{
  std::vector<std::variant<int,selected_object_type>> selection;
  std::optional<glm::vec3> pivot;
  auto const& current_selection()const{return selection;}
  auto const& multi_select_pivot()const{return pivot;}
};
struct MapView {int getRenderContext()const{return 7;}};
struct Label
{
  unsigned updates=0;
  std::string text,tooltip;
  void setPlainText(char const* value){text=value;++updates;}
  void setToolTip(char const* value){tooltip=value;}
};
#define LogError std::cerr
struct object_editor
{
  MapView view;
  MapView* _map_view=&view;
  std::vector<std::variant<int,selected_object_type>> _model_instance_created;
  Label label;
  Label* _filename=&label;
  unsigned& updates=label.updates;
  ~object_editor(){for(auto const& object:_model_instance_created)delete std::get<selected_object_type>(object);}
  void update_clipboard();
  void copy_current_selection(World*);
};
// ACTUAL_CLIPBOARD_FUNCTION
void check(bool ok,char const* message){if(!ok)throw std::runtime_error(message);}
int main()
{
  try
  {
    WMOInstance original(Key("world/wmo/ship.wmo",4191560),7);
    original.pos={10,20,30};original.dir={17,23,41};original.scale=1.75f;
    WmoState state;state.flags=0x8c;state.nameSet=11;state.doodadSet=7;
    state.source=WmoSource{};state.source->primary.sets={7,12};state.source->primary.byte=93;
    state.source->lod=state.source->primary;state.source->lodKnown=true;
    original.apply_retail_state(state);
    WMOInstance unnamed(Key("",6033633),7);unnamed.apply_retail_state(state);
    ModelInstance model(Key("",12345),7);
    World world;world.selection={42,selected_object_type(&original),selected_object_type(&unnamed),selected_object_type(&model)};
    world.pivot=glm::vec3(2,3,4);
    object_editor editor;editor.copy_current_selection(&world);
    check(editor.updates==1 && editor._model_instance_created.size()==3,"Selected objects were not copied exactly once");
    auto* copy=dynamic_cast<WMOInstance*>(std::get<selected_object_type>(editor._model_instance_created[0]));
    check(copy && copy->wmo.key.path==original.wmo.key.path && copy->wmo.key.id==4191560,"Clipboard changed WMO resource identity");
    check(copy->pos==glm::vec3(8,17,26) && copy->dir==original.dir && copy->scale==original.scale,"Clipboard changed transform/pivot behavior");
    check(copy->appearance && copy->appearance->flags==0x8c && copy->appearance->nameSet==11 && copy->appearance->doodadSet==7,"Clipboard discarded WMO appearance");
    auto const& source=copy->appearance->source;
    check(source && source->primary.sets==std::vector<std::uint16_t>({7,12}) && source->primary.byte==93 && source->lodKnown && source->lod,"Clipboard lost Retail provenance/LOD data");
    original.appearance->source->primary.sets.clear();
    check(source->primary.sets.size()==2,"Clipboard aliases mutable placement data");
    check(dynamic_cast<WMOInstance*>(std::get<selected_object_type>(editor._model_instance_created[1]))->wmo.key.id==6033633,"Clipboard lost unnamed WMO FileDataID");
    check(dynamic_cast<ModelInstance*>(std::get<selected_object_type>(editor._model_instance_created[2]))->model.key.id==12345,"Clipboard lost unnamed M2 FileDataID");
    World empty;editor.copy_current_selection(&empty);
    check(editor.updates==1 && editor._model_instance_created.size()==3,"Empty selection changed clipboard");
    WMOInstance legacy(Key("world/wmo/legacy.wmo"),7);World legacyWorld;legacyWorld.selection={selected_object_type(&legacy)};
    object_editor legacyEditor;legacyEditor.copy_current_selection(&legacyWorld);
    auto* legacyCopy=dynamic_cast<WMOInstance*>(std::get<selected_object_type>(legacyEditor._model_instance_created.front()));
    check(!legacyCopy->appearance && legacyCopy->pos==glm::vec3(),"Legacy clipboard gained Retail state or incorrect origin");
    World unnamedWorld;unnamedWorld.selection={selected_object_type(&unnamed)};
    object_editor unnamedEditor;unnamedEditor.copy_current_selection(&unnamedWorld);
    check(unnamedEditor.label.text=="Model: 6033633","Clipboard label failed for unnamed FileDataID asset");
    check(unnamedEditor.label.tooltip==unnamedEditor.label.text,"FileDataID fallback is missing from the tooltip");
    std::string const longPath="world/wmo/expansion11/bloodelf/12be_bloodelf_silvermoon_building02.wmo";
    WMOInstance modern(Key(longPath,5947475),7);World modernWorld;modernWorld.selection={selected_object_type(&modern)};
    object_editor modernEditor;modernEditor.copy_current_selection(&modernWorld);
    check(modernEditor.label.text=="Model: "+longPath,"Long modern model name was truncated in clipboard data");
    check(modernEditor.label.tooltip==modernEditor.label.text,"Full model identity is missing from the tooltip");
    std::cout<<"PASS: actual clipboard copy preserves FileDataID, appearance, provenance, transforms and legacy defaults\n";
    return 0;
  }
  catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
