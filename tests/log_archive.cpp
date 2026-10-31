#include <noggit/Log.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
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
    if(mode == "concurrent")
    {
      std::vector<std::thread> writers;
      for(int thread = 0; thread < 6; ++thread)
        writers.emplace_back([thread]
        {
          for(int row = 0; row < 200; ++row)
          {
            // Exercise all streams sharing the same underlying log file.
            auto message = thread % 3 == 0 ? _Log(__FILE__, __LINE__)
              : thread % 3 == 1 ? _LogDebug(__FILE__, __LINE__) : _LogError(__FILE__, __LINE__);
            message << "atomic-message " << thread << ':' << row << ' '
                    << std::string(128, static_cast<char>('a' + thread)) << '\n';
          }
        });
      for(auto& writer : writers) writer.join();
    }
    std::cout<<"new-session-marker"<<std::endl;
    return 0;
  }
  try
  {
    for(auto const& mode : {"retail", "legacy", "legacy-then-retail", "concurrent"}) for(bool blockArchive : {false,true})
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
      if(QString(mode) == "concurrent")
      {
        // std::ofstream uses native newlines; compare complete logical records.
        auto records = current;
        records.replace("\r\n", "\n");
        check(records.count("atomic-message ") == 1200);
        for(int thread = 0; thread < 6; ++thread)
          for(int row = 0; row < 200; ++row)
          {
            auto expected = QByteArray("atomic-message ") + QByteArray::number(thread) + ':'
              + QByteArray::number(row) + ' ' + QByteArray(128, static_cast<char>('a' + thread)) + '\n';
            check(records.count(expected) == 1);
          }
      }
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
    std::cout<<"PASS Retail log preservation and fallback; non-Retail snapshot release; concurrent message integrity\n";
    return 0;
  }
  catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
