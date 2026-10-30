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
#include <QTemporaryDir>

#include <array>
#include <memory>
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
    std::function<void(std::string const&)> const& writeDiffuse,
    std::function<void(std::string const&)> const& writeNormal)
{
  constexpr unsigned flags = Diffuse | Normal;
  mark(projectPath, mapName, tile, flags);
  auto const map = QString::fromStdString(mapName).toLower();
  QDir directory(QDir(QString::fromStdString(projectPath)).filePath("world/maptextures/" + map));
  if (!directory.mkpath("."))
    fail(directory.path(), "cannot create map texture directory");
  // Keep interrupted staging outside the paths consumed by map patch packers.
  QDir stagingDirectory(QDir(QString::fromStdString(projectPath)).filePath(".noggit/derived-staging/" + map));
  if (!stagingDirectory.mkpath(".")) fail(stagingDirectory.path(), "cannot create staging directory");
  QTemporaryDir staging(stagingDirectory.filePath("pair-XXXXXX"));
  if (!staging.isValid())
    fail(directory.path(), "cannot stage map textures");
  std::array<QString, 2> const staged{staging.filePath("diffuse.blp"), staging.filePath("normal.blp")};
  writeDiffuse(staged[0].toStdString());
  writeNormal(staged[1].toStdString());

  struct Output
  {
    QString path;
    bool existed = false;
    QByteArray previous;
    std::unique_ptr<QSaveFile> file;
  };
  auto const basename = QString("%1_%2_%3").arg(map).arg(tile.x).arg(tile.z);
  std::array<Output, 2> outputs;
  for (std::size_t i = 0; i < outputs.size(); ++i)
  {
    auto& output = outputs[i];
    output.path = directory.filePath(basename + (i == 0 ? ".blp" : "_n.blp"));
    QFile previous(output.path);
    output.existed = previous.exists();
    if (output.existed)
    {
      if (!previous.open(QIODevice::ReadOnly)) fail(output.path, previous.errorString());
      output.previous = previous.readAll();
      if (previous.error() != QFileDevice::NoError) fail(output.path, previous.errorString());
    }
    QFile prepared(staged[i]);
    if (!prepared.open(QIODevice::ReadOnly)) fail(staged[i], prepared.errorString());
    auto const bytes = prepared.readAll();
    if (prepared.error() != QFileDevice::NoError) fail(staged[i], prepared.errorString());
    if (bytes.isEmpty()) fail(staged[i], "map texture writer produced no data");
    output.file = std::make_unique<QSaveFile>(output.path);
    output.file->setDirectWriteFallback(false);
    if (!output.file->open(QIODevice::WriteOnly) || output.file->write(bytes) != bytes.size())
      fail(output.path, output.file->errorString());
  }
  for (std::size_t committed = 0; committed < outputs.size(); ++committed)
  {
    if (outputs[committed].file->commit()) continue;
    auto message = "Cannot commit map texture " + outputs[committed].path + ": "
      + outputs[committed].file->errorString();
    for (std::size_t i = 0; i < committed; ++i)
    {
      auto const& output = outputs[i];
      bool restored;
      if (output.existed)
      {
        QSaveFile restore(output.path);
        restore.setDirectWriteFallback(false);
        restored = restore.open(QIODevice::WriteOnly)
          && restore.write(output.previous) == output.previous.size() && restore.commit();
      }
      else restored = QFile::remove(output.path);
      if (!restored) message += "; rollback failed for " + output.path;
    }
    throw std::runtime_error(message.toStdString());
  }
  clear(projectPath, mapName, tile, flags);
}
