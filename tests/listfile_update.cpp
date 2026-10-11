#include <noggit/project/ListfileUpdate.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

void require(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
void write(QString const& path, QByteArray const& bytes)
{
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Fixture write failed");
}
QByteArray read(QString const& path) { QFile file(path); require(file.open(QIODevice::ReadOnly), "Fixture read failed"); return file.readAll(); }
int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  auto log = [](QString const& text) { std::cout << text.toStdString() << '\n'; };
  QString error;
  if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--live")
    return Noggit::Project::refreshListfile(QString::fromLocal8Bit(argv[2]), error, log) ? 0 : 1;
  try
  {
    QTemporaryDir dir;
    auto const path = dir.path() + "/listfile.csv";
    QByteArray const old = "100;old.blp\n", updated = "101;new.blp\n";
    write(path, old);
    int downloads = 0;
    int asset_id = 1;
    QByteArray body = old;
    bool offline = false, not_modified = false, truncated = false, wrong_digest = false;
    auto transport = [&](QUrl const&, QByteArray const& etag, Noggit::Project::ListfileSink const& sink, bool download) {
      if (offline) return Noggit::Project::ListfileHttpResult{};
      if (!download)
      {
        if (not_modified) {
          require(etag == "release-etag", "Conditional ETag not sent");
          return Noggit::Project::ListfileHttpResult{304, {}, true};
        }
        auto const digest = wrong_digest ? QByteArray(64, '0') : QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex();
        QJsonObject asset{{"name", "community-listfile.csv"}, {"id", asset_id}, {"size", body.size()},
          {"browser_download_url", "https://github.com/wowdev/wow-listfile/releases/download/test/community-listfile.csv"},
          {"digest", "sha256:" + QString::fromLatin1(digest)}};
        sink(QJsonDocument(QJsonObject{{"assets", QJsonArray{asset}}}).toJson());
        return Noggit::Project::ListfileHttpResult{200, "release-etag", true};
      }
      ++downloads;
      sink(truncated ? body.left(3) : body);
      return Noggit::Project::ListfileHttpResult{200, {}, true};
    };
    auto refresh = [&] { return Noggit::Project::refreshListfile(dir.path(), error, log, transport); };
    require(refresh() && downloads == 0 && read(path) == old, "Matching digest downloaded CSV");
    not_modified = true;
    require(refresh() && downloads == 0, "304 downloaded CSV");
    not_modified = false;
    require(refresh() && downloads == 0, "Same asset downloaded CSV");
    body = updated; ++asset_id;
    require(refresh() && downloads == 1 && read(path) == updated, "New asset not installed");
    body = "102;next.blp\n"; ++asset_id; truncated = true;
    require(refresh() && read(path) == updated, "Truncated download damaged local file");
    truncated = false; wrong_digest = true;
    require(refresh() && read(path) == updated, "Wrong hash damaged local file");
    wrong_digest = false; body = "<html>failure</html>"; ++asset_id;
    require(refresh() && read(path) == updated, "Invalid CSV damaged local file");
    offline = true;
    require(refresh() && error.isEmpty() && read(path) == updated, "Offline fallback failed");
    require(QFile::remove(path), "Could not remove fixture");
    require(!refresh() && !error.isEmpty(), "Missing local file accepted offline");
    offline = false; body = old; ++asset_id;
    require(refresh() && read(path) == old, "First-time download failed");
    std::cout << "PASS digest/ETag/no-update/new-update/truncation/hash/CSV/offline/missing/first-download\n";
  }
  catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
