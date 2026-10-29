// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/DerivedMapState.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>

#include <array>
#include <stdexcept>

namespace
{
  using State = std::array<unsigned, 64 * 64>;

  [[noreturn]] void fail(QString const& path, QString const& reason)
  {
    throw std::runtime_error(("Derived map state " + path + ": " + reason).toStdString());
  }

  QString statePath(std::string const& project, std::string const& map)
  {
    auto const name = QString::fromStdString(map).toLower();
    if (project.empty() || !QRegularExpression("^[a-z0-9_]+$").match(name).hasMatch())
      throw std::invalid_argument("Derived map state requires a project path and a valid map name.");
    return QDir(QString::fromStdString(project)).filePath(".noggit/derived-maps/" + name + ".json");
  }

  void validateFlags(unsigned flags)
  {
    if (!flags || (flags & ~Noggit::DerivedMapState::Known))
      throw std::invalid_argument("Invalid derived map state flags.");
  }

  State read(QString const& path)
  {
    State state{};
    QFile file(path);
    if (!file.exists())
      return state;
    if (!file.open(QIODevice::ReadOnly))
      fail(path, file.errorString());
    if (file.size() > 1024 * 1024)
      fail(path, "file exceeds the 1 MiB limit");
    auto const bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
      fail(path, file.errorString());
    QJsonParseError error;
    auto const document = QJsonDocument::fromJson(bytes, &error);
    auto const root = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || root.value("version").toInt(-1) != 1 || !root.value("tiles").isArray())
      fail(path, "invalid or unsupported state document (preserved without overwriting)");
    auto const tiles = root.value("tiles").toArray();
    if (tiles.size() > 4096)
      fail(path, "too many tile entries");
    for (auto const& entry : tiles)
    {
      auto const object = entry.toObject();
      auto const x = object.value("x").toInt(-1);
      auto const z = object.value("z").toInt(-1);
      auto const flags = object.value("flags").toInt(-1);
      if (!entry.isObject() || x < 0 || x >= 64 || z < 0 || z >= 64
          || flags <= 0 || (static_cast<unsigned>(flags) & ~Noggit::DerivedMapState::Known)
          || state[z * 64 + x] != 0)
        fail(path, "invalid or duplicate tile entry (preserved without overwriting)");
      state[z * 64 + x] = static_cast<unsigned>(flags);
    }
    return state;
  }

  void update(std::string const& project, std::string const& map,
              std::vector<TileIndex> const& targets, unsigned flags, bool mark)
  {
    validateFlags(flags);
    for (auto const& tile : targets)
      if (!tile.is_valid())
        throw std::invalid_argument("Invalid tile for derived map state.");
    auto const path = statePath(project, map);
    if (targets.empty()) return;
    auto const parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent))
      fail(path, "cannot create metadata directory");
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0))
      fail(path, "state is locked by another writer; retry after it finishes");
    auto state = read(path);
    bool changed = false;
    for (auto const& tile : targets)
    {
      auto& current = state[tile.index()];
      auto const next = mark ? current | flags : current & ~flags;
      changed |= current != next;
      current = next;
    }
    if (!changed) return;
    QJsonArray tiles;
    for (unsigned z = 0; z < 64; ++z)
      for (unsigned x = 0; x < 64; ++x)
        if (state[z * 64 + x])
          tiles.append(QJsonObject{{"x", static_cast<int>(x)}, {"z", static_cast<int>(z)},
                                   {"flags", static_cast<int>(state[z * 64 + x])}});
    auto const bytes = QJsonDocument(QJsonObject{{"version", 1}, {"tiles", tiles}}).toJson();
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
      fail(path, file.errorString());
  }
}

std::vector<TileIndex> Noggit::DerivedMapState::pending(
    std::string const& projectPath, std::string const& mapName, unsigned flags)
{
  validateFlags(flags);
  auto const state = read(statePath(projectPath, mapName));
  std::vector<TileIndex> result;
  for (unsigned z = 0; z < 64; ++z)
    for (unsigned x = 0; x < 64; ++x)
      if (state[z * 64 + x] & flags)
        result.emplace_back(x, z);
  return result;
}

void Noggit::DerivedMapState::mark(std::string const& projectPath, std::string const& mapName,
                                 TileIndex const& tile, unsigned flags)
{
  mark(projectPath, mapName, std::vector<TileIndex>{tile}, flags);
}

void Noggit::DerivedMapState::mark(std::string const& projectPath, std::string const& mapName,
                                 std::vector<TileIndex> const& tiles, unsigned flags)
{
  update(projectPath, mapName, tiles, flags, true);
}

void Noggit::DerivedMapState::clear(std::string const& projectPath, std::string const& mapName,
                                  TileIndex const& tile, unsigned flags)
{
  clear(projectPath, mapName, std::vector<TileIndex>{tile}, flags);
}

void Noggit::DerivedMapState::clear(std::string const& projectPath, std::string const& mapName,
                                  std::vector<TileIndex> const& tiles, unsigned flags)
{
  update(projectPath, mapName, tiles, flags, false);
}

std::vector<TileIndex> Noggit::DerivedMapState::projectTiles(
    std::string const& projectPath, std::string const& mapName)
{
  auto const state = read(statePath(projectPath, mapName));
  auto const map = QString::fromStdString(mapName).toLower();
  QDir directory(QDir(QString::fromStdString(projectPath)).filePath("world/maps/" + map));
  QRegularExpression pattern("^" + QRegularExpression::escape(map)
    + "_([0-9]+)_([0-9]+)(_tex0|_obj0|_obj1)?\\.adt$",
    QRegularExpression::CaseInsensitiveOption);
  enum : unsigned { Terrain = 1, Objects = 2 };
  State present{};
  for (auto const& name : directory.entryList(QDir::Files))
  {
    auto match = pattern.match(name);
    if (!match.hasMatch()) continue;
    bool xOk = false, zOk = false;
    auto x = match.captured(1).toUInt(&xOk), z = match.captured(2).toUInt(&zOk);
    if (xOk && zOk && x < 64 && z < 64)
      present[z * 64 + x] |= match.captured(3).startsWith("_obj", Qt::CaseInsensitive) ? Objects : Terrain;
  }
  std::vector<TileIndex> result;
  for (unsigned z = 0; z < 64; ++z)
    for (unsigned x = 0; x < 64; ++x)
    {
      auto const i = z * 64 + x;
      if ((present[i] & Terrain) || (present[i] && !(state[i] & ObjectDependency)))
        result.emplace_back(x, z);
    }
  return result;
}

void Noggit::DerivedMapState::writeTexturePair(
    std::string const& projectPath, std::string const& mapName, TileIndex const& tile,
    std::function<void()> const& writeDiffuse, std::function<void()> const& writeNormal)
{
  constexpr unsigned flags = Diffuse | Normal;
  mark(projectPath, mapName, tile, flags);
  writeDiffuse();
  writeNormal();
  clear(projectPath, mapName, tile, flags);
}
