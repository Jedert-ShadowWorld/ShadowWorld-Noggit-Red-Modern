#pragma once

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUrl>
#include <functional>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace Noggit::Project
{
  struct ListfileHttpResult
  {
    int status = 0;
    QByteArray etag;
    bool complete = false;
  };
  using ListfileSink = std::function<bool(QByteArray const&)>;
  using ListfileTransport = std::function<ListfileHttpResult(QUrl const&, QByteArray const&, ListfileSink const&, bool)>;

  inline ListfileHttpResult requestListfile(QUrl const& url, QByteArray const& etag,
                                           ListfileSink const& sink, bool download)
  {
    ListfileHttpResult result;
#ifdef _WIN32
    if (url.scheme() != "https") return result;
    struct CloseInternet { void operator()(void* h) const { if (h) WinHttpCloseHandle(h); } };
    using Handle = std::unique_ptr<void, CloseInternet>;
    Handle session(WinHttpOpen(L"ShadowWorld-Noggit", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return result;
    WinHttpSetTimeouts(session.get(), 2000, 2000, 2000, 2000);
    auto const host = url.host().toStdWString();
    Handle connection(WinHttpConnect(session.get(), host.c_str(), static_cast<INTERNET_PORT>(url.port(443)), 0));
    if (!connection) return result;
    auto const resource = (url.path(QUrl::FullyEncoded) + (url.hasQuery() ? "?" + url.query(QUrl::FullyEncoded) : "")).toStdWString();
    Handle request(WinHttpOpenRequest(connection.get(), L"GET", resource.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) return result;
    auto headers = QString("Accept: application/vnd.github+json\r\n");
    if (!etag.isEmpty()) headers += "If-None-Match: " + QString::fromLatin1(etag) + "\r\n";
    auto const wide_headers = headers.toStdWString();
    if (!WinHttpSendRequest(request.get(), wide_headers.c_str(), static_cast<DWORD>(wide_headers.size()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) return result;
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return result;
    result.status = static_cast<int>(status);
    wchar_t etag_buffer[512]{};
    length = sizeof(etag_buffer);
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CUSTOM, L"ETag", etag_buffer, &length, WINHTTP_NO_HEADER_INDEX))
      result.etag = QString::fromWCharArray(etag_buffer).toLatin1();
    if (status == 304) { result.complete = true; return result; }
    if (status != 200) return result;
    auto const deadline = GetTickCount64() + (download ? 180000 : 5000);
    quint64 total = 0;
    QByteArray buffer(65536, '\0');
    while (GetTickCount64() < deadline)
    {
      DWORD read = 0;
      if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) return result;
      if (!read) { result.complete = true; return result; }
      total += read;
      if (total > (download ? 512ULL * 1024 * 1024 : 2ULL * 1024 * 1024)
          || !sink(QByteArray(buffer.constData(), static_cast<int>(read)))) return result;
    }
#else
    Q_UNUSED(url); Q_UNUSED(etag); Q_UNUSED(sink); Q_UNUSED(download);
#endif
    return result;
  }

  inline bool validListfile(QString const& path)
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() == 0) return false;
    for (int i = 0; i < 25 && !file.atEnd(); ++i)
    {
      auto const line = file.readLine(8192).trimmed();
      auto const separator = line.indexOf(';');
      bool numeric = false;
      auto const id = line.left(separator).toUInt(&numeric);
      if (separator > 0 && numeric && id > 0 && separator + 1 < line.size()) return true;
    }
    return false;
  }

  inline QByteArray listfileHash(QString const& path)
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result().toHex() : QByteArray{};
  }

  inline bool refreshListfile(QString const& directory, QString& error,
                              std::function<void(QString const&)> const& log,
                              ListfileTransport const& transport = requestListfile)
  {
    error.clear();
    auto const path = QDir(directory).filePath("listfile.csv");
    auto const metadata_path = path + ".update.json";
    bool const local = validListfile(path);
    auto fallback = [&](QString const& why) {
      log(why + (local ? "; using existing listfile." : "; no usable local listfile."));
      if (!local) error = why + " No usable local listfile.csv is available.";
      return local;
    };
    if (!QDir().mkpath(directory)) return fallback("Could not create listfile directory");
    QJsonObject metadata;
    QFile metadata_file(metadata_path);
    if (metadata_file.open(QIODevice::ReadOnly)) metadata = QJsonDocument::fromJson(metadata_file.readAll()).object();
    metadata_file.close();
    QFileInfo const local_info(path);
    bool const cache_matches = local && metadata["size"].toDouble() == local_info.size()
      && metadata["modified"].toString() == QString::number(local_info.lastModified().toMSecsSinceEpoch());
    QByteArray json;
    log("Checking for a newer wowdev listfile...");
    auto const response = transport(QUrl("https://api.github.com/repos/wowdev/wow-listfile/releases/latest"),
      cache_matches ? metadata["etag"].toString().toLatin1() : QByteArray{},
      [&](QByteArray const& bytes) { json += bytes; return json.size() <= 2 * 1024 * 1024; }, false);
    if (response.complete && response.status == 304 && cache_matches)
    {
      log("Listfile is up to date; using existing file.");
      return true;
    }
    if (!response.complete || response.status != 200) return fallback("Listfile update check failed or timed out");
    QJsonObject asset;
    for (auto const& value : QJsonDocument::fromJson(json).object()["assets"].toArray())
      if (value.toObject()["name"].toString() == "community-listfile.csv") { asset = value.toObject(); break; }
    qint64 const expected_size = static_cast<qint64>(asset["size"].toDouble());
    auto const asset_id = QString::number(asset["id"].toDouble(), 'f', 0);
    QUrl const download_url(asset["browser_download_url"].toString());
    if (asset.isEmpty() || expected_size <= 0 || expected_size > 512LL * 1024 * 1024
        || download_url.scheme() != "https" || download_url.host() != "github.com")
      return fallback("Invalid listfile release metadata");
    auto digest = asset["digest"].toString().toLatin1();
    if (digest.startsWith("sha256:")) digest = digest.mid(7).toLower(); else digest.clear();
    auto remember = [&] {
      QFileInfo const info(path);
      QJsonObject state{{"asset", asset_id}, {"etag", QString::fromLatin1(response.etag)},
                        {"size", static_cast<double>(info.size())},
                        {"modified", QString::number(info.lastModified().toMSecsSinceEpoch())}};
      QSaveFile file(metadata_path);
      auto const bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        log("Could not cache listfile update metadata.");
    };
    if ((cache_matches && metadata["asset"].toString() == asset_id && local_info.size() == expected_size)
        || (local && local_info.size() == expected_size && !digest.isEmpty() && listfileHash(path) == digest))
    {
      remember();
      log("Listfile is up to date; no CSV download needed.");
      return true;
    }
    log("New listfile available; downloading " + QString::number(expected_size / 1024 / 1024) + " MB...");
    QTemporaryFile temporary(QDir(directory).filePath("listfile-download-XXXXXX"));
    if (!temporary.open()) return fallback("Could not create temporary listfile");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    auto const download = transport(download_url, {}, [&](QByteArray const& bytes) {
      if (temporary.size() + bytes.size() > expected_size || temporary.write(bytes) != bytes.size()) return false;
      hash.addData(bytes);
      return true;
    }, true);
    temporary.flush();
    if (!download.complete || download.status != 200 || temporary.size() != expected_size
        || (!digest.isEmpty() && hash.result().toHex() != digest) || !validListfile(temporary.fileName()))
      return fallback("Listfile download failed validation or timed out");
    if (!temporary.seek(0)) return fallback("Could not read downloaded listfile");
    QSaveFile destination(path);
    if (!destination.open(QIODevice::WriteOnly)) return fallback("Could not stage updated listfile");
    while (!temporary.atEnd())
    {
      auto const bytes = temporary.read(65536);
      if (bytes.isEmpty() || destination.write(bytes) != bytes.size()) return fallback("Could not stage updated listfile");
    }
    if (!destination.commit()) return fallback("Could not replace listfile safely");
    remember();
    log("Updated listfile installed successfully.");
    return true;
  }
}
