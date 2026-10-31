// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// Run the real reference resolver with isolated project/archive gateways. Reuse
// the terrain codec fixtures instead of keeping another binary format builder.
#define main terrain_lod_fixture_main
#include "terrain_lod_updater.cpp"
#undef main
#include <noggit/formats/adt/ShadowlandsADTCodec.hpp>
#include <noggit/formats/adt/RetailLiquidLod.hpp>
#include <noggit/formats/RetailWorldTables.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <optional>
#include <exception>
#include <sstream>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace BlizzardArchive
{
  enum class ClientVersion { WOTLK, RETAIL };
  namespace Listfile
  {
    struct FileKey
    {
      std::string path;
      explicit FileKey(std::string const& value) : path(value) {}
    };
  }
}

struct TestClient
{
  QString project;
  std::map<std::string, Bytes> archive;
  unsigned archiveReads = 0;
  BlizzardArchive::ClientVersion target = BlizzardArchive::ClientVersion::RETAIL;
  auto version() const { return target; }
  std::string getDiskPath(BlizzardArchive::Listfile::FileKey const& key) const
  {
    return QDir(project).filePath(QString::fromStdString(key.path)).toStdString();
  }
  bool readFile(BlizzardArchive::Listfile::FileKey const& key, std::vector<char>& bytes)
  {
    ++archiveReads;
    auto found = archive.find(key.path);
    if (found == archive.end()) return false;
    bytes.assign(found->second.begin(), found->second.end());
    return true;
  }
};
namespace Noggit::Application
{
  struct NoggitApplication
  {
    TestClient client;
    static NoggitApplication* instance() { static NoggitApplication app; return &app; }
    TestClient* clientData() { return &client; }
  };
}

namespace Noggit::Formats::ADT
{
  namespace
  {
    std::ostringstream Log, LogError;
// ACTUAL_PART_INDEX
// ACTUAL_REFERENCE_HELPERS
// ACTUAL_UPDATE_LOD
  }
// ACTUAL_BACKING_HAS
// ACTUAL_BACKING_PART
// ACTUAL_REFERENCE_CONFIGURATION
}

namespace
{
  using Codec = Noggit::Formats::ADT::ShadowlandsADTCodec;
  using Backing = Noggit::Formats::ADT::ShadowlandsADTBackingStore;
  using Part = Noggit::Formats::ADT::ShadowlandsADTPart;
  void write_file(QString const& path, QByteArray const& bytes)
  {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create fixture directory.");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
      && file.write(bytes) == bytes.size(), "Cannot write reference fixture.");
  }
  void write_file(QString const& path, Bytes const& bytes)
  {
    write_file(path, QByteArray(reinterpret_cast<char const*>(bytes.data()), static_cast<int>(bytes.size())));
  }
  QByteArray read_file(QString const& path)
  {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read reference fixture.");
    return file.readAll();
  }
  template<class F> std::string rejection(F&& run)
  {
    try { run(); }
    catch (std::exception const& error) { return error.what(); }
    throw std::runtime_error("Invalid reference unexpectedly accepted.");
  }
  struct Fixture
  {
    QTemporaryDir project;
    QTemporaryDir originals;
    Backing backing;
    Bytes originalRoot = root(false), originalLod = mesh_lod({});
    std::string rootPath = "world/maps/azeroth/azeroth_27_29.adt";
    std::string lodPath = "world/maps/azeroth/azeroth_27_29_lod.adt";
    TestClient& client = Noggit::Application::NoggitApplication::instance()->client;
    Fixture()
    {
      require(project.isValid() && originals.isValid(), "Cannot create isolated reference fixture.");
      client = {};
      client.project = project.path();
      backing.source.root_path = rootPath;
      backing.source.paths[static_cast<std::size_t>(Part::Root)] = rootPath;
      backing.source.paths[static_cast<std::size_t>(Part::Lod)] = lodPath;
      backing.parts[static_cast<std::size_t>(Part::Root)] = {true, originalRoot};
      backing.parts[static_cast<std::size_t>(Part::Lod)] = {true, originalLod};
    }
    QString local(std::string const& relative) const
    {
      return QDir(project.path()).filePath(QString::fromStdString(relative));
    }
    QString retained(char const* extension) const
    {
      return local(".noggit/lod-reference/" + rootPath + extension);
    }
    QString config() const { return local(".noggit/lod-reference/azeroth.json"); }
    void configure(Bytes const& sourceRoot, Bytes const& sourceLod)
    {
      write_file(QDir(originals.path()).filePath("azeroth_27_29.adt"), sourceRoot);
      write_file(QDir(originals.path()).filePath("azeroth_27_29_lod.adt"), sourceLod);
      Codec::setLodReferenceDirectory(rootPath, originals.path().toStdString());
    }
    auto resolve() const
    {
      return Noggit::Formats::ADT::resolve_lod_reference(backing, originalLod);
    }
    void retain()
    {
      std::vector<std::pair<std::string, Bytes>> parts;
      Noggit::Formats::ADT::retain_lod_reference(rootPath, originalRoot, originalLod, "fixture", parts);
      require(parts.size() == 3, "Reference did not stage ROOT, LOD and metadata together.");
      for (auto const& [path, bytes] : parts) write_file(local(path), bytes);
    }
  };
  Bytes changed_legacy_liquid(Bytes terrain)
  {
    auto const first = body_offset(terrain, "MCNK");
    auto const size = get(terrain, first - 4);
    Bytes liquid; append(liquid, "MCLQ", {1, 2, 3, 4});
    terrain.insert(terrain.begin() + first + size, liquid.begin(), liquid.end());
    put(terrain, first - 4, size + static_cast<std::uint32_t>(liquid.size()));
    return terrain;
  }
#ifdef _WIN32
  void codec_low_stack_regression()
  {
    Fixture fixture;
    fixture.originalLod = mesh_lod({}, 0, true);
    auto height = [](Bytes const& b, std::size_t p) {
      auto bits=get(b,p); float value; std::memcpy(&value,&bits,4); return value;
    };
    // A gently curved surface remains below the refinement bound until a local
    // edit introduces a peak. Complete skirts exercise the manual cut path.
    for (unsigned c=0;c<256;++c) for (unsigned v=0;v<145;++v)
    {
      auto p=92+c*724+136+v*4;
      putf(fixture.originalRoot,p,height(fixture.originalRoot,p)/256);
    }
    auto hp=body_offset(fixture.originalLod,"MLVH"), sp=body_offset(fixture.originalLod,"MLSI");
    for (unsigned v=0;v<33025;++v) putf(fixture.originalLod,hp+v*4,height(fixture.originalLod,hp+v*4)/256);
    for (unsigned i=0;i<2;++i)
    {
      auto base=unsigned(fixture.originalLod[sp+i*2])|(unsigned(fixture.originalLod[sp+i*2+1])<<8);
      putf(fixture.originalLod,hp+(33025+i)*4,height(fixture.originalLod,hp+base*4)-32);
    }
    auto mh=body_offset(fixture.originalLod,"MLHD");
    putf(fixture.originalLod,mh+8,height(fixture.originalLod,mh+8)/256);
    fixture.backing.part(Part::Root).bytes=fixture.originalRoot;
    fixture.backing.part(Part::Lod).bytes=fixture.originalLod;
    fixture.configure(fixture.originalRoot, fixture.originalLod);
    struct Work
    {
      Fixture& fixture;
      Bytes unsupportedRoot, changedRoot, expectedLod, objects, cutRoot, expectedCutLod;
      std::exception_ptr failure;
    } work{fixture, fixture.originalRoot, fixture.originalRoot, {}, {}, fixture.originalRoot, {}, {}};
    for (unsigned c=0;c<256;++c) putf(work.changedRoot,92+c*724+112,3);
    work.expectedLod = TerrainLodUpdater::update(fixture.originalRoot,
      fixture.originalLod, work.changedRoot);
    work.unsupportedRoot = changed_legacy_liquid(work.unsupportedRoot);
    put(work.cutRoot,92,0x10000); put(work.cutRoot,92+20,1u<<27);
    auto peak=92+136+99*4;
    putf(work.cutRoot,peak,height(work.cutRoot,peak)+10);
    work.expectedCutLod=TerrainLodUpdater::update(fixture.originalRoot,
      fixture.originalLod,work.cutRoot,nullptr,true);
    append(work.objects, "TEST", {9, 8, 7});
    auto run = [](void* parameter) -> DWORD
    {
      auto& w = *static_cast<Work*>(parameter);
      auto& f = w.fixture;
      try
      {
        using Noggit::Formats::ADT::update_lod;
        Bytes obj = w.objects, result;
        std::vector<std::pair<std::string, Bytes>> parts{{"discarded", {1}}};
        std::string error;
        require(!update_lod(f.backing, w.unsupportedRoot, obj, result, parts, &error, true),
          "Unsupported legacy-liquid rebuild unexpectedly succeeded");
        require(error.find("MCLQ changed") != std::string::npos,
          "Legacy-liquid rebuild did not reach the production failure handler");
        require(result == f.originalLod && obj == w.objects,
          "Failed rebuild changed LOD or OBJ1");
        auto const base = ".noggit/lod-reference/" + f.rootPath;
        require(parts.size() == 3 && parts[0].first == base + ".root"
          && parts[0].second == f.originalRoot && parts[1].first == base + ".lod"
          && parts[1].second == f.originalLod && parts[2].first == base + ".json",
          "Failed rebuild did not retain only the verified baseline");
        auto metadata = QJsonDocument::fromJson(QByteArray(
          reinterpret_cast<char const*>(parts[2].second.data()),
          static_cast<int>(parts[2].second.size()))).object();
        require(metadata.value("rootSha256").toString()
            == Noggit::Formats::ADT::reference_hash(f.originalRoot)
          && metadata.value("lodSha256").toString()
            == Noggit::Formats::ADT::reference_hash(f.originalLod),
          "Failure-path metadata does not identify its retained files");
        require(!QFile::exists(f.retained(".root")), "Failure handler wrote unstaged project data");

        parts.clear();
        error.clear();
        require(update_lod(f.backing, w.changedRoot, obj, result, parts, &error),
          "Low-stack production LOD update failed");
        require(result == w.expectedLod && obj == w.objects && parts.size() == 3,
          "Low-stack production update changed expected outputs");
        for (auto const& [path, bytes] : parts) write_file(f.local(path), bytes);
        f.backing.parts[static_cast<std::size_t>(Part::Root)].bytes = w.changedRoot;
        f.backing.parts[static_cast<std::size_t>(Part::Lod)].bytes = result;
        parts.clear();
        require(update_lod(f.backing, w.changedRoot, obj, result, parts, &error),
          "Low-stack repeat production update failed");
        require(result == w.expectedLod && obj == w.objects,
          "Repeated production update is not byte-idempotent");
        parts.clear();
        require(update_lod(f.backing, w.cutRoot, obj, result, parts, &error, true),
          "Low-stack manual hole cutting/refinement failed");
        require(result == w.expectedCutLod && obj == w.objects && parts.size() == 3
            && parts[0].second == w.cutRoot && parts[1].second == result,
          "Low-stack manual cut changed expected bytes or retained an incoherent pair");
      }
      catch (...) { w.failure = std::current_exception(); }
      return 0;
    };
    auto thread = CreateThread(nullptr, 256*1024, run, &work,
      STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    require(thread != nullptr, "Cannot create low-stack codec regression thread");
    auto const waited = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    require(waited == WAIT_OBJECT_0, "Cannot wait for low-stack codec regression");
    if (work.failure) std::rethrow_exception(work.failure);
  }
#endif
}

int main()
{
  try
  {
#ifdef _WIN32
    codec_low_stack_regression();
#endif
    {
      // A fully custom map has no archive reference. A deferred source save
      // must retain the old coherent pair before the ROOT is replaced.
      Fixture f;
      auto changedRoot = root(false, 7);
      Bytes objects; append(objects, "TEST", {9, 8, 7});
      auto const oldObjects = objects;
      Bytes generated = f.originalLod;
      std::vector<std::pair<std::string, Bytes>> parts;
      require(!Noggit::Formats::ADT::prepare_lod_for_save(f.backing, changedRoot,
          objects, generated, parts, derived_save_policy::defer),
        "Deferred source save unexpectedly generated LOD");
      require(objects == oldObjects && generated == f.originalLod && parts.size() == 3,
        "Deferred source save changed generated bytes or lost its reference");
      require(parts[0].second == f.originalRoot && parts[1].second == f.originalLod,
        "Deferred source save retained the modified ROOT with stale LOD");
      for (auto const& [path, bytes] : parts)
      {
        require(path.starts_with(".noggit/lod-reference/"),
          "Deferred source save staged a generated output");
        write_file(f.local(path), bytes);
      }
      // Model the source commit, then execute the real manual rebuild path.
      f.backing.part(Part::Root).bytes = changedRoot;
      parts.clear();
      require(!Noggit::Formats::ADT::prepare_lod_for_save(f.backing, changedRoot,
          objects, generated, parts, derived_save_policy::defer) && parts.empty(),
        "Repeated deferred save overwrote the retained pre-edit baseline");
      auto const expected = TerrainLodUpdater::update(f.originalRoot, f.originalLod, changedRoot);
      std::string failure;
      require(Noggit::Formats::ADT::update_lod(f.backing, changedRoot,
          objects, generated, parts, &failure),
        "Custom map could not rebuild after a deferred source save");
      require(generated == expected && objects == oldObjects && f.client.archiveReads == 0,
        "Postponed rebuild did not reuse the preserved custom-map reference");
    }
    {
      Fixture f;
      write_file(f.retained(".root"), Bytes{1, 2, 3});
      Bytes objects, generated;
      std::vector<std::pair<std::string, Bytes>> parts;
      Noggit::Formats::ADT::prepare_lod_for_save(f.backing, root(false, 7),
        objects, generated, parts, derived_save_policy::defer);
      require(parts.empty() && read_file(f.retained(".root")) == QByteArray("\1\2\3", 3),
        "Deferred save overwrote an incomplete reference");
      f.client.target = BlizzardArchive::ClientVersion::WOTLK;
      require(!Noggit::Formats::ADT::prepare_lod_for_save(f.backing, root(false, 7),
          objects, generated, parts, derived_save_policy::defer)
          && generated == f.originalLod && parts.empty(),
        "Deferred policy changed the non-Retail save path");
    }
    {
      Fixture f;
      write_file(f.retained(".root"), f.originalRoot);
      write_file(f.retained(".lod"), f.originalLod);
      auto result = f.resolve();
      require(result.origin == "retained" && result.root == f.originalRoot,
        "Valid legacy reference without metadata was not accepted.");
      require(f.client.archiveReads == 0, "Valid retained reference unexpectedly read archives.");
      f.retain();
      auto meta = QJsonDocument::fromJson(read_file(f.retained(".json"))).object();
      auto hash = [&](char const* extension) {
        return QString::fromLatin1(QCryptographicHash::hash(read_file(f.retained(extension)), QCryptographicHash::Sha256).toHex());
      };
      require(meta.value("version").toInt() == 1
        && meta.value("rootPath").toString() == QString::fromStdString(f.rootPath)
        && meta.value("rootSha256").toString() == hash(".root")
        && meta.value("lodSha256").toString() == hash(".lod")
        && meta.value("origin").toString() == "fixture",
        "Generated reference metadata does not identify its actual files.");
      require(f.resolve().origin == "retained", "Generated metadata did not round-trip.");
      meta.insert("rootSha256", "incorrect");
      auto const corrupt = QJsonDocument(meta).toJson();
      write_file(f.retained(".json"), corrupt);
      auto configuredRoot = f.originalRoot;
      append(configuredRoot, "TEST", {9, 8, 7});
      f.configure(configuredRoot, f.originalLod);
      result = f.resolve();
      require(result.origin == "configured folder" && result.root == configuredRoot,
        "Hash mismatch did not fall back to the validated configured pair.");
      require(read_file(f.retained(".json")) == corrupt, "Resolution overwrote corrupt reference evidence.");
    }
    for (bool hasRoot : {true, false})
    {
      Fixture f;
      write_file(f.retained(hasRoot ? ".root" : ".lod"), hasRoot ? f.originalRoot : f.originalLod);
      f.configure(f.originalRoot, f.originalLod);
      require(f.resolve().origin == "configured folder", "Half-missing retained pair prevented fallback.");
    }
    {
      Fixture f;
      auto differentLod = f.originalLod;
      differentLod[body_offset(differentLod, "TEST")] ^= 1;
      require(TerrainLodUpdater::inspectReference(f.originalRoot, differentLod).usable,
        "Opaque-byte fixture must still have a high geometry score.");
      f.configure(f.originalRoot, differentLod);
      auto result = f.resolve();
      require(result.origin == "configured folder" && result.calibrated_lod
          && *result.calibrated_lod == f.originalLod,
        "Calibration rejected identical indexing or replaced current opaque bytes.");
    }
    {
      Fixture f;
      auto differentLod = f.originalLod;
      differentLod[body_offset(differentLod, "MLSI")] = 1;
      require(TerrainLodUpdater::inspectReference(f.originalRoot, differentLod).usable,
        "Different-index fixture must still have a high geometry score.");
      f.configure(f.originalRoot, differentLod);
      auto error = rejection([&] { (void)f.resolve(); });
      require(error.find("different LOD indexing: MLSI") != std::string::npos,
        "Calibration accepted incompatible skirt indexing.");
    }
    {
      Fixture f;
      auto currentRoot = root(false, 7), currentLod = f.originalLod;
      auto modified = bodies(lod(false, 0, 4));
      for (auto const* name : {"MLVH", "MLHD"})
        std::copy(modified.at(name).begin(), modified.at(name).end(),
          currentLod.begin() + body_offset(currentLod, name));
      currentLod[body_offset(currentLod, "TEST")] ^= 8;
      f.backing.parts[static_cast<std::size_t>(Part::Root)].bytes = currentRoot;
      f.backing.parts[static_cast<std::size_t>(Part::Lod)].bytes = currentLod;
      f.client.archive.emplace(f.rootPath, f.originalRoot);
      f.client.archive.emplace(f.lodPath, f.originalLod);
      auto reference = Noggit::Formats::ADT::resolve_lod_reference(f.backing, currentLod);
      require(reference.origin == "client archives" && reference.root == f.originalRoot
          && reference.calibrated_lod,
        "Modified LOD heights did not recover from the original archive pair.");
      require(bodies(*reference.calibrated_lod).at("MLVH") == bodies(f.originalLod).at("MLVH")
          && bodies(*reference.calibrated_lod).at("TEST") == bodies(currentLod).at("TEST"),
        "Calibration did not use reference heights and current opaque bytes.");

      Bytes obj; append(obj, "TEST", {9, 8, 7});
      auto const oldObj = obj;
      Bytes generated;
      auto unsupportedRoot = changed_legacy_liquid(currentRoot);
      std::vector<std::pair<std::string, Bytes>> parts{{"discarded", {1}}};
      std::string error;
      require(!Noggit::Formats::ADT::update_lod(f.backing, unsupportedRoot, obj, generated, parts, &error),
        "Calibrated rebuild accepted unsupported legacy-liquid changes.");
      require(error.find("MCLQ changed") != std::string::npos
          && generated == currentLod && obj == oldObj && parts.empty(),
        "Failed calibration rebuild changed outputs or persisted an incoherent baseline.");
      require(!QFile::exists(f.retained(".root")) && !QFile::exists(f.retained(".lod")),
        "Failed calibration wrote reference files.");

      error.clear();
      require(Noggit::Formats::ADT::update_lod(f.backing, currentRoot, obj, generated, parts, &error),
        "Archive-calibrated production update failed.");
      require(bodies(generated).at("MLVH") == bodies(lod(false, 0, 7)).at("MLVH")
          && bodies(generated).at("TEST") == bodies(currentLod).at("TEST") && obj == oldObj,
        "Archive-calibrated update lost current heights, opaque data or OBJ1.");
      check_bounds(generated);
      require(parts.size() == 3 && parts[0].second == currentRoot && parts[1].second == generated,
        "Successful calibration did not retain the newly generated coherent pair.");
    }
    {
      Fixture f;
      write_file(f.local(f.rootPath), root(true));
      write_file(f.local(f.lodPath), Bytes{1, 2, 3});
      f.backing.parts[static_cast<std::size_t>(Part::Root)].bytes = root(true);
      f.client.archive.emplace(f.rootPath, f.originalRoot);
      f.client.archive.emplace(f.lodPath, f.originalLod);
      auto result = f.resolve();
      require(result.origin == "client archives" && result.root == f.originalRoot && f.client.archiveReads == 2,
        "Archive fallback used a project override or the already-loaded ROOT.");
      require(read_file(f.local(f.lodPath)) == QByteArray("\1\2\3", 3), "Reference lookup wrote project data.");
    }
    {
      Fixture f;
      write_file(f.local(f.rootPath), f.originalRoot);
      write_file(f.local(f.lodPath), f.originalLod);
      auto error = rejection([&] { (void)f.resolve(); });
      require(error.find("No coherent ROOT/LOD reference") != std::string::npos,
        "Loaded/project ROOT became an unproven implicit reference.");
    }
    {
      Fixture f;
      auto const broken = QByteArray("{broken");
      write_file(f.config(), broken);
      rejection([&] { (void)Codec::lodReferenceDirectory(f.rootPath); });
      require(read_file(f.config()) == broken, "Reading bad configuration overwrote it.");
      f.configure(f.originalRoot, f.originalLod);
      require(QString::fromStdString(Codec::lodReferenceDirectory(f.rootPath))
          == QFileInfo(f.originals.path()).canonicalFilePath(), "Explicit selection did not repair configuration.");
      require(f.resolve().origin == "configured folder", "Repaired configuration is not usable.");
      auto const accepted = read_file(f.config());
      f.client.target = BlizzardArchive::ClientVersion::WOTLK;
      rejection([&] { Codec::setLodReferenceDirectory(f.rootPath, f.originals.path().toStdString()); });
      require(read_file(f.config()) == accepted, "Legacy client changed Retail reference configuration.");
    }
    std::cout << "PASS: real LOD reference resolver, production failure/success paths, bounded stack, provenance, hashes, fallback order, archive isolation and configuration recovery.\n";
    return 0;
  }
  catch (std::exception const& error)
  {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
