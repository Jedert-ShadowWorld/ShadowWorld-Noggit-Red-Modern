#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

void check(bool value) { if (!value) throw std::runtime_error("Tile removal regression"); }
enum class model_update { add, remove };
struct TileIndex
{
  std::size_t x = 0, z = 0;
  TileIndex(int index = 0) : x(index) {}
  TileIndex(std::size_t x, std::size_t z) : x(x), z(z) {}
  bool is_valid() const { return x < 64 && z < 64; }
};
struct SceneObject;
struct MapTile
{
  bool changed = false, failed = false;
  std::function<void(MapTile*)> attach;
  void remove_model(SceneObject*);
  void wait_until_loaded() {}
  bool loading_failed() const { return failed; }
  bool finishedLoading() { if (attach) attach(this); return !failed; }
};
struct Model
{
  int waits = 0;
  void wait_until_loaded() { ++waits; }
  bool loading_failed() const { return false; }
};
struct SceneObject
{
  std::uint32_t uid = 7;
  Model model;
  std::vector<MapTile*> tiles;
  std::array<int, 2> extents{};
  auto const& getTiles() const { return tiles; }
  Model* instance_model() { return &model; }
  auto& getExtents() { return extents; }
};
void MapTile::remove_model(SceneObject* instance)
{ instance->tiles.erase(std::find(instance->tiles.begin(), instance->tiles.end(), this)); }
using selected_object_type = SceneObject*;
using selection_type = std::variant<selected_object_type>;
struct SelectionGroup { bool contains_object(SceneObject*) { return false; } void remove_member(std::uint32_t) {} };
struct Action { void registerObjectRemoved(SceneObject*) {} };
Action* current_action = nullptr;
#define NOGGIT_CUR_ACTION current_action
#define LogError std::cerr
struct World
{
  struct Index
  {
    int calls = 0;
    std::array<MapTile, 2> tiles;
    std::mutex* storage_mutex = nullptr;
    bool hasTile(TileIndex const& tile) const { return tile.x < tiles.size() && tile.z == 0; }
    MapTile* loadTile(TileIndex const& tile)
    {
      check(storage_mutex->try_lock());
      storage_mutex->unlock();
      ++calls;
      return &tiles.at(tile.x);
    }
    void update_model_tile(TileIndex, model_update, SceneObject*) { ++calls; }
  } mapIndex;
  std::vector<SelectionGroup> _selection_groups;
  std::mutex* storage_mutex = nullptr;
  bool drained = false, removed = false;
  void wait_for_all_tile_updates()
  {
    check(storage_mutex->try_lock());
    storage_mutex->unlock();
    drained = true;
  }
  void updateTilesEntry(selection_type const&, model_update);
};
namespace Noggit
{
// ACTUAL_UPDATE
struct world_model_instances_storage
{
  World* _world;
  std::mutex _mutex;
  std::map<std::uint32_t, int> _instance_count_per_uid;
  std::map<std::uint32_t, SceneObject> _m2s, _wmos;
  std::optional<selection_type> get_instance(std::uint32_t uid, bool)
  {
    auto it = _m2s.find(uid);
    return it == _m2s.end() ? std::optional<selection_type>{} : selection_type{&it->second};
  }
  void delete_instance(std::uint32_t uid, bool action);
};
// ACTUAL_DELETION
}
void World::updateTilesEntry(selection_type const& selected, model_update type)
{
  check(drained);
  Noggit::instance_update update(std::get<selected_object_type>(selected), type);
  update.apply(this);
  removed = true;
}
int main()
{
  try
  {
    World world;
    SceneObject object;
    Noggit::instance_update remove(&object, model_update::remove);
    remove.apply(&world);
    check(world.mapIndex.calls == 0 && object.model.waits == 0);
    MapTile a, b;
    object.tiles = {&a, &b};
    remove.apply(&world);
    check(a.changed && b.changed && object.tiles.empty());
    check(world.mapIndex.calls == 0 && object.model.waits == 0);
    Noggit::world_model_instances_storage storage{&world};
    world.storage_mutex = &storage._mutex;
    world.mapIndex.storage_mutex = &storage._mutex;
    storage._m2s.emplace(7, SceneObject{});
    storage._m2s.at(7).extents = {0, 1};
    storage._instance_count_per_uid.emplace(7, 1);
    for (auto& tile : world.mapIndex.tiles)
      tile.attach = [&](MapTile* attached) { storage._m2s.at(7).tiles.push_back(attached); };
    storage.delete_instance(7, false);
    check(world.drained && world.removed && storage._m2s.empty() && storage._instance_count_per_uid.empty());
    check(world.mapIndex.calls == 2 && world.mapIndex.tiles[0].changed && world.mapIndex.tiles[1].changed);

    // A failed extent load must retain the instance rather than silently delete
    // storage while leaving its disk placement unvisited.
    storage._m2s.emplace(7, SceneObject{});
    storage._m2s.at(7).extents = {0, 1};
    storage._instance_count_per_uid.emplace(7, 1);
    world.mapIndex.tiles[0].failed = true;
    world.removed = false;
    storage.delete_instance(7, false);
    check(!world.removed && storage._m2s.size() == 1 && storage._instance_count_per_uid.size() == 1);
    std::cout << "PASS removal avoids tile loading and drains additions before storage lock\n";
    return 0;
  }
  catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
