// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/DerivedMapState.hpp>

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace
{
  void require(bool value, char const* message)
  {
    if (!value) throw std::runtime_error(message);
  }
  template<typename Function> void rejects(Function function)
  {
    try { function(); }
    catch (std::exception const&) { return; }
    throw std::runtime_error("Invalid or unwritable state was accepted.");
  }
}

int main()
{
  using State = Noggit::DerivedMapState;
  try
  {
    QTemporaryDir project;
    require(project.isValid(), "Cannot create test project.");
    auto const path = project.path().toStdString();
    auto const stateFile = project.path() + "/.noggit/derived-maps/azeroth.json";
    TileIndex const first(27, 29), second(28, 29);
    // Project scope is based on primary ADT overrides, not pending markers or
    // old derived outputs. Deduplicate split files and isolate map names.
    auto const mapDir = project.path() + "/world/maps/azeroth";
    require(QDir().mkpath(mapDir), "Cannot create project map fixture.");
    for (auto const* name : {"azeroth_27_29.adt", "azeroth_27_29_tex0.adt",
      "azeroth_28_29_obj0.adt", "azeroth_29_29_obj1.adt", "azeroth_30_29_tex0.adt",
      "azeroth_31_29_lod.adt", "azeroth_32_29.blp", "azeroth_64_29.adt",
      "other_33_29.adt"})
    {
      QFile file(mapDir + "/" + name);
      require(file.open(QIODevice::WriteOnly), "Cannot create scope fixture.");
    }
    auto const scope = State::projectTiles(path, "azeroth");
    require(scope == std::vector<TileIndex>({first, second, TileIndex(29,29), TileIndex(30,29)}),
      "Project scope included derived/unrelated files or lost a primary override.");
    require(State::projectTiles(path, "kalimdor").empty(), "Project scope crossed map boundaries.");
    // OBJ-only neighbours written by a UID repair stay outside the project;
    // a terrain file or a cleared marker makes the tile an edited tile again.
    State::mark(path, "azeroth", std::vector<TileIndex>{second, TileIndex(29,29), first},
      State::ObjectDependency);
    require(State::projectTiles(path, "azeroth") == std::vector<TileIndex>({first, TileIndex(30,29)}),
      "Neighbour OBJ dependency expanded the project scope or hid a terrain override.");
    require(State::pending(path, "azeroth").empty(), "Provenance was reported as pending work.");
    State::clear(path, "azeroth", second, State::ObjectDependency);
    require(State::projectTiles(path, "azeroth") == std::vector<TileIndex>({first, second, TileIndex(30,29)}),
      "Saving a dependency tile did not make it an edited tile.");
    State::clear(path, "azeroth", std::vector<TileIndex>{first, TileIndex(29,29)}, State::ObjectDependency);
    require(!QFile::exists(stateFile) || State::pending(path, "azeroth", State::Known).empty(),
      "Provenance cleanup left state behind.");
    State::mark(path, "wdl", first);
    State::clear(path, "wdl", first, State::Lod);
    require(State::pending(path, "wdl", State::Wdl) == std::vector<TileIndex>{first},
      "Completing terrain LOD cleared the pending WDL update.");
    State::clear(path, "wdl", first, State::Wdl);
    require(State::pending(path, "wdl", State::Wdl).empty()
      && State::pending(path, "wdl", State::Diffuse) == std::vector<TileIndex>{first},
      "WDL completion lost unrelated pending work.");
    State::mark(path, "batch", std::vector<TileIndex>{first, second, first}, State::Lod);
    require(State::pending(path, "batch", State::Lod).size() == 2, "Batch mark lost or duplicated tiles.");
    rejects([&] { State::mark(path, "batch", std::vector<TileIndex>{TileIndex(1,1), TileIndex(64,0)}); });
    require(State::pending(path, "batch").size() == 2, "Invalid batch partially modified state.");
    State::mark(path, "batch", first, State::Diffuse);
    State::clear(path, "batch", std::vector<TileIndex>{first, second}, State::Lod);
    require(State::pending(path, "batch", State::Lod).empty()
      && State::pending(path, "batch", State::Diffuse) == std::vector<TileIndex>{first},
      "Batch completion lost unrelated pending work.");
    require(State::pending(path, "azeroth").empty(), "New project is not empty.");
    State::mark(path, "azeroth", first);
    State::mark(path, "azeroth", second, State::Normal);
    require(State::pending(path, "azeroth").size() == 2, "Marks did not survive a fresh read.");
    State::clear(path, "azeroth", first, State::Diffuse);
    require(State::pending(path, "azeroth", State::Diffuse).empty(), "Diffuse was not cleared.");
    require(State::pending(path, "azeroth", State::Normal).size() == 2, "Clear lost unrelated work.");
    require(State::pending(path, "azeroth", State::Lod).size() == 1, "Clear lost LOD work.");
    State::clear(path, "azeroth", second, State::All);
    auto const pending = State::pending(path, "azeroth");
    require(pending.size() == 1 && pending[0] == first, "Clear lost another tile.");
    State::mark(path, "kalimdor", second);
    require(State::pending(path, "azeroth").size() == 1, "Map states interfered.");
    require(State::pending(path, "kalimdor")[0] == second, "Second map was not persisted.");

    int diffuseWrites = 0, normalWrites = 0;
    rejects([&] {
      State::writeTexturePair(path, "pairs", first,
        [&] { ++diffuseWrites; throw std::runtime_error("diffuse write failed"); },
        [&] { ++normalWrites; });
    });
    require(diffuseWrites == 1 && normalWrites == 0, "A failed diffuse write did not stop the pair.");
    require(State::pending(path, "pairs", State::Diffuse).size() == 1
         && State::pending(path, "pairs", State::Normal).size() == 1,
            "First write failure lost pending texture work.");
    rejects([&] {
      State::writeTexturePair(path, "pairs", first,
        [&] { ++diffuseWrites; },
        [&] { ++normalWrites; throw std::runtime_error("normal write failed"); });
    });
    require(diffuseWrites == 2 && normalWrites == 1, "Pair writers ran in an unexpected order.");
    require(State::pending(path, "pairs", State::Diffuse).size() == 1
         && State::pending(path, "pairs", State::Normal).size() == 1,
            "Second write failure accepted a half-written pair.");
    State::mark(path, "pairs", first, State::Lod);
    State::mark(path, "pairs", second, State::Diffuse | State::Normal);
    State::writeTexturePair(path, "pairs", first,
      [&] { ++diffuseWrites; }, [&] { ++normalWrites; });
    auto const pendingPairs = State::pending(path, "pairs", State::Diffuse | State::Normal);
    require(pendingPairs.size() == 1 && pendingPairs[0] == second,
            "Retry did not accept the complete pair or lost another tile.");
    require(State::pending(path, "pairs", State::Lod).size() == 1,
            "Texture completion cleared pending LOD work.");
    {
      QLockFile completionLock(project.path() + "/.noggit/derived-maps/pairs.json.lock");
      rejects([&] {
        State::writeTexturePair(path, "pairs", second, [] {},
          [&] { require(completionLock.tryLock(0), "Cannot lock pair completion."); });
      });
    }
    require(State::pending(path, "pairs", State::Diffuse).size() == 1
         && State::pending(path, "pairs", State::Normal).size() == 1,
            "Failed state publication lost unfinished texture work.");

    // Independent readers still see unfinished work after a failure.
    require(State::pending(path, "azeroth", State::Normal).size() == 1, "Pending generation vanished.");
    {
      QLockFile competingWriter(stateFile + ".lock");
      require(competingWriter.tryLock(0), "Cannot create test writer lock.");
      rejects([&] { State::clear(path, "azeroth", first, State::All); });
    }
    require(State::pending(path, "azeroth").size() == 1, "Locked update damaged state.");

    QFile corrupt(stateFile);
    require(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write corrupt fixture.");
    corrupt.write("{\"version\":1,\"tiles\":[{\"x\":64,\"z\":0,\"flags\":7}]}");
    corrupt.close();
    rejects([&] { State::pending(path, "azeroth"); });
    rejects([&] { State::mark(path, "azeroth", first); });
    require(corrupt.open(QIODevice::ReadOnly), "Cannot read corrupt fixture.");
    require(corrupt.readAll().contains("\"x\":64"), "Corrupt evidence was overwritten.");
    corrupt.close();
    rejects([&] { State::mark(path, "../azeroth", first); });
    rejects([&] { State::mark(path, "azeroth", TileIndex(64, 0)); });
    rejects([&] { State::pending(path, "kalimdor", 32); });

    QTemporaryDir unwritable;
    QFile obstruction(unwritable.path() + "/.noggit");
    require(obstruction.open(QIODevice::WriteOnly), "Cannot create path obstruction.");
    obstruction.close();
    rejects([&] { State::mark(unwritable.path().toStdString(), "azeroth", first); });
    diffuseWrites = normalWrites = 0;
    rejects([&] {
      State::writeTexturePair(unwritable.path().toStdString(), "azeroth", first,
        [&] { ++diffuseWrites; }, [&] { ++normalWrites; });
    });
    require(diffuseWrites == 0 && normalWrites == 0, "Texture writes started without persistent pending state.");
    std::cout << "PASS: derived map state, texture pair failure/retry, locks, corruption and write failure.\n";
    return 0;
  }
  catch (std::exception const& error)
  {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
