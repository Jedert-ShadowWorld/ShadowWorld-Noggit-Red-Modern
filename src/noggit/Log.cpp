// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>

std::ostream& _LogError(const char * pFile, int pLine)
{
  return std::cerr << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): [Error] ";
}
std::ostream& _LogDebug(const char * pFile, int pLine)
{
  return std::clog << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): [Debug] ";
}
std::ostream& _Log(const char * pFile, int pLine)
{
  return std::cout << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): ";
}

#if DEBUG__LOGGINGTOCONSOLE
void InitLogging()
{
  LogDebug << "Logging to console window." << std::endl;
}
void ConfigureProjectLogging(bool) {}
#else
namespace
{
  std::ofstream gLogStream;
  QByteArray gPreviousSession;
}
void InitLogging()
{
  // The client version is unknown until a project is selected;
  // ConfigureProjectLogging() archives or discards this snapshot.
  QFile previous("log.txt");
  if (previous.open(QIODevice::ReadOnly)) gPreviousSession = previous.readAll();
  previous.close();
  // Set up log.
  gLogStream.open("log.txt", std::ios_base::out | std::ios_base::trunc);
  if (gLogStream)
  {
    std::cout.rdbuf(gLogStream.rdbuf());
    std::clog.rdbuf(gLogStream.rdbuf());
    std::cerr.rdbuf(gLogStream.rdbuf());
  }
}
void ConfigureProjectLogging(bool retail)
{
  if (!retail)
  {
    gPreviousSession.clear();
    gPreviousSession.squeeze();
    return;
  }
  if (gPreviousSession.isEmpty()) return;
  auto name = "logs/session-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-")
    + QString::number(QCoreApplication::applicationPid()) + ".log";
  QFile archive(name);
  if (!QDir().mkpath("logs") || !archive.open(QIODevice::WriteOnly)
      || archive.write(gPreviousSession) != gPreviousSession.size() || !archive.flush())
  {
    LogError << "Cannot archive preceding session to " << name.toStdString() << std::endl;
    auto text = gPreviousSession;
    text.replace("\r\n", "\n");
    std::cerr.write(text.constData(), text.size());
    std::cerr << std::endl;
  }
  gPreviousSession.clear();
  gPreviousSession.squeeze();
}
#endif
