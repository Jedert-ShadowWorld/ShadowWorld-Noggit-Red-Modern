// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>

namespace
{
  std::osyncstream log_message(std::ostream& target, char const* file, int line, char const* level)
  {
    auto const* basename = strrchr(file, '/');
    if (!basename) basename = strrchr(file, '\\');
    std::osyncstream message(target);
    message << clock() * 1000 / CLOCKS_PER_SEC << " - (" << (basename ? basename + 1 : file)
            << ':' << line << "): " << level;
    return message;
  }
}

std::osyncstream _LogError(const char * pFile, int pLine)
{
  return log_message(std::cerr, pFile, pLine, "[Error] ");
}
std::osyncstream _LogDebug(const char * pFile, int pLine)
{
  return log_message(std::clog, pFile, pLine, "[Debug] ");
}
std::osyncstream _Log(const char * pFile, int pLine)
{
  return log_message(std::cout, pFile, pLine, "");
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
    std::osyncstream(std::cerr).write(text.constData(), text.size()) << std::endl;
  }
  gPreviousSession.clear();
  gPreviousSession.squeeze();
}
#endif
