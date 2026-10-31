#include <noggit/Log.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
void check(bool value) { if(!value) throw std::runtime_error("Log archival regression"); }
QByteArray read(QString const& path)
{ QFile f(path); check(f.open(QIODevice::ReadOnly)); return f.readAll(); }
void write(QString const& path,QByteArray const& value)
{ QFile f(path); check(f.open(QIODevice::WriteOnly)); check(f.write(value)==value.size()); }
int main(int argc,char** argv)
{
  QCoreApplication app(argc,argv);
  if(argc>1)
  {
    InitLogging();
    auto const mode = QString(argv[1]);
    ConfigureProjectLogging(mode == "retail");
    // The first non-Retail selection must release the snapshot, not defer it
    // until a later Retail project is opened in the same process.
    if(mode == "legacy-then-retail") ConfigureProjectLogging(true);
    std::cout<<"new-session-marker"<<std::endl;
    return 0;
  }
  try
  {
    for(auto const& mode : {"retail", "legacy", "legacy-then-retail"}) for(bool blockArchive : {false,true})
    {
      bool const retail = QString(mode) == "retail";
      QTemporaryDir scratch;check(scratch.isValid());
      write(scratch.filePath("log.txt"),"previous-session-marker\n");
      if(blockArchive) write(scratch.filePath("logs"),"This file prevents directory creation");
      QProcess child;child.setWorkingDirectory(scratch.path());
      child.start(QCoreApplication::applicationFilePath(),{mode});
      check(child.waitForFinished(10000) && child.exitStatus()==QProcess::NormalExit && child.exitCode()==0);
      auto current=read(scratch.filePath("log.txt"));
      check(current.contains("new-session-marker"));
      if(!retail)
      {
        check(!current.contains("previous-session-marker"));
        check(QDir(scratch.filePath("logs")).entryList({"session-*.log"},QDir::Files).isEmpty());
      }
      else if(blockArchive) check(current.contains("previous-session-marker"));
      else
      {
        auto archived=QDir(scratch.filePath("logs")).entryList({"session-*.log"},QDir::Files);
        check(archived.size()==1);
        check(read(scratch.filePath("logs/"+archived[0]))=="previous-session-marker\n");
        check(!current.contains("previous-session-marker"));
      }
    }
    std::cout<<"PASS Retail log preservation and fallback; non-Retail snapshot release\n";
    return 0;
  }
  catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
