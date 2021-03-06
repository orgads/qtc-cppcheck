#include <QDebug>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QThread>

#include <coreplugin/messagemanager.h>
#include <coreplugin/progressmanager/progressmanager.h>
#include <coreplugin/progressmanager/futureprogress.h>
#include <utils/macroexpander.h>

#include "CppcheckRunner.h"
#include "Constants.h"
#include "Settings.h"

using namespace QtcCppcheck::Internal;

namespace {
  enum ErrorField {
    ErrorFieldFile = 0, ErrorFieldLine, ErrorFieldSeverity, ErrorFieldId,
    ErrorFieldMessage
  };
}

CppcheckRunner::CppcheckRunner (Settings *settings, QObject *parent) :
  QObject (parent), settings_ (settings), showOutput_ (false), showId_ (false), futureInterface_ (NULL),
  maxArgumentsLength_ (0) {
#ifdef __linux__
  QProcess getConf;
  getConf.start (QLatin1String ("getconf ARG_MAX"));
  getConf.waitForFinished (2000);
  QByteArray argMax = getConf.readAllStandardOutput ().replace ("\n", "");
  maxArgumentsLength_ = std::max (argMax.toInt (), 32000);
#else
  maxArgumentsLength_ = 32767;
#endif
  Q_ASSERT (settings_ != NULL);

  connect (&process_, &QProcess::readyReadStandardOutput,
           this, &CppcheckRunner::readOutput);
  connect (&process_, &QProcess::readyReadStandardError,
           this, &CppcheckRunner::readError);
  connect (&process_, &QProcess::started,
           this, &CppcheckRunner::started);
  connect (&process_, &QProcess::errorOccurred,
           this, &CppcheckRunner::error);
  connect (&process_, static_cast<void (QProcess::*)(int)>(&QProcess::finished),
           this, &CppcheckRunner::finished);

  // Restart checking if got queue.
  connect (&process_, static_cast<void (QProcess::*)(int)>(&QProcess::finished),
           this, &CppcheckRunner::checkQueuedFiles);
}

CppcheckRunner::~CppcheckRunner () {
  if (process_.isOpen ()) {
    process_.kill ();
  }
  queueTimer_.stop ();
  settings_ = NULL;
  delete futureInterface_;
}

void CppcheckRunner::updateSettings () {
  Q_ASSERT (settings_ != NULL);
  showOutput_ = settings_->showBinaryOutput ();
  showId_ = settings_->showId ();
  runArguments_.clear ();
  QString enabled = QLatin1String ("--enable=warning,style,performance,"
                                   "portability,information,missingInclude");
  // Overwrite enable with user parameters if present
  for (int i = runArguments_.size () - 1; i >= 0; --i) {
    if (runArguments_.at (i).startsWith (QLatin1String ("--enable"))) {
      enabled = runArguments_.takeAt (i);
      break;
    }
  }
  if (settings_->checkUnused ()) {
    enabled += QLatin1String (",unusedFunction");
  }
  else{ //TODO always check with threads but rescan for unused after finish?
    runArguments_ << (QLatin1String ("-j ") +
                      QString::number (QThread::idealThreadCount ()));
  }
  runArguments_ << enabled;
  if (settings_->checkInconclusive ()) {
    runArguments_ << QLatin1String ("--inconclusive");
  }
  runArguments_ << QLatin1String ("--template={file},{line},{severity},{id},{message}");
}

void CppcheckRunner::setIncludePaths (const QStringList &paths) {
  includePaths_.clear ();
  includePaths_.reserve (paths.size ());
  for (const auto &i: paths) {
    includePaths_.append (QLatin1String ("-I") + i);
  }
}

void CppcheckRunner::checkFiles (const QStringList &fileNames) {
  Q_ASSERT (!fileNames.isEmpty ());
  fileCheckQueue_ += fileNames;
  fileCheckQueue_.removeDuplicates ();
  fileCheckQueue_.sort ();
  if (process_.isOpen ()) {
    if (fileCheckQueue_ == currentlyCheckingFiles_) {
      process_.kill ();
      // Rechecking will be restarted on finish signal.
    }
    return;
  }
  // Delay helps to avoid double checking same file on editor change.
  const int checkDelayInMs = 200;
  if (!queueTimer_.isActive ()) {
    queueTimer_.singleShot (checkDelayInMs, this, &CppcheckRunner::checkQueuedFiles);
  }
}

void CppcheckRunner::stopChecking () {
  fileCheckQueue_.clear ();
  if (process_.isOpen ()) {
    process_.kill ();
  }
}

void CppcheckRunner::checkQueuedFiles () {
  if (fileCheckQueue_.isEmpty ()) {
    return;
  }
  QString binary = settings_->binaryFile ();
  if (binary.isEmpty ()) {
    return;
  }
  // Pass custom params BEFORE most of runner's to shadow if some repeat.
  auto expander = Utils::globalMacroExpander ();
  auto expanded = expander->expand (settings_->customParameters ());
  QStringList arguments (expanded.split (QLatin1Char (' '), QString::SkipEmptyParts));
  arguments += runArguments_;

  auto includes = !settings_->ignoreIncludePaths () ? includePaths_ : QStringList {};
  currentlyCheckingFiles_ = fileCheckQueue_;
  fileCheckQueue_.clear ();

  int argumentLength = arguments.join (QLatin1Literal (" ")).length ();
  int filesLength = currentlyCheckingFiles_.join (QLatin1Literal (" ")).length ();
  int includesLength = includes.join (QLatin1Literal (" ")).length ();
  if (argumentLength + includesLength + filesLength >= maxArgumentsLength_) {
    if (fileListFileContents_ != currentlyCheckingFiles_) {
      fileListFileContents_ = currentlyCheckingFiles_;
      fileListFile_.resize (0);
      includeListFile_.resize (0);

      if (fileListFile_.open () && includeListFile_.open ()) {
        QByteArray filesArg = fileListFileContents_.join (QLatin1String ("\n")).toLocal8Bit ();
        fileListFile_.write (filesArg);
        fileListFile_.close ();

        for (auto &i: includes) {
          i = i.mid (2);
        }
        QByteArray includesArg = includes.join (QLatin1String ("\n")).toLocal8Bit ();
        includeListFile_.write (includesArg);
        includeListFile_.close ();
      }
      else{
        Core::MessageManager::write (tr ("Failed to write cppcheck's argument files"),
                                     Core::MessageManager::Silent);
        return;
      }
    }
    arguments << QString (QLatin1String ("--file-list=%1")).arg (fileListFile_.fileName ());
    arguments << QString (QLatin1String ("--includes-file=%1")).arg (includeListFile_.fileName ());
  }
  else{
    arguments += currentlyCheckingFiles_;
    arguments += includes;
  }
  emit startedChecking (currentlyCheckingFiles_);
  if (showOutput_) {
    Core::MessageManager::write (QString ("Starting CppChecker with:%1, %2")
                                 .arg (binary,arguments.join (" ")), Core::MessageManager::WithFocus);
  }
  process_.start (binary, arguments);
}

void CppcheckRunner::readOutput () {
  if (!showOutput_) {
    return;
  }
  process_.setReadChannel (QProcess::StandardOutput);

  while (!process_.atEnd () && process_.canReadLine ()) {
    QByteArray rawLine = process_.readLine ();
    QString line = QString::fromUtf8 (rawLine).trimmed ();
    if (line.isEmpty ()) {
      continue;
    }
    const QString progressSample = QLatin1String ("% done");
    // check futureInterface because read can be triggered before started..
    if (line.endsWith (progressSample) && futureInterface_ != NULL) {
      int percentEndIndex = line.length () - progressSample.length ();
      int percentStartIndex = line.lastIndexOf (QLatin1String (" "), percentEndIndex);
      int done = line.mid (percentStartIndex, percentEndIndex - percentStartIndex).toInt ();
      futureInterface_->setProgressValue (done);
    }
    Core::MessageManager::write (line, Core::MessageManager::Silent);
  }
}

void CppcheckRunner::readError () {
  process_.setReadChannel (QProcess::StandardError);

  while (!process_.atEnd () && process_.canReadLine ()) {
    QByteArray rawLine = process_.readLine ();
    QString line = QString::fromUtf8 (rawLine).trimmed ();
    if (line.isEmpty ()) {
      continue;
    }
    if (showOutput_) {
      Core::MessageManager::write (line, Core::MessageManager::Silent);
    }
    QStringList details = line.split (QLatin1Char (','));
    if (details.size () <= ErrorFieldMessage) {
      continue;
    }
    QString file = QDir::fromNativeSeparators (details.at (ErrorFieldFile));
    int lineNumber = details.at (ErrorFieldLine).toInt ();
    char type = details.at (ErrorFieldSeverity).at (0).toLatin1 ();
    QString id = "";
    if (showId_) {
      id = details.at (ErrorFieldId);
    }
    QString description = line.mid (line.indexOf (details.at (ErrorFieldMessage)));
    emit newTask (type, id, description, file, lineNumber);
  }
}

void CppcheckRunner::started () {
  if (showOutput_) {
    Core::MessageManager::write (tr ("Cppcheck started"), Core::MessageManager::Silent);
  }

  using namespace Core;
  delete futureInterface_;
  futureInterface_ = new QFutureInterface<void>;
  FutureProgress *progress = ProgressManager::addTask (futureInterface_->future (),
                                                       tr ("Cppcheck"), Constants::TASK_CHECKING);
  connect (progress, &Core::FutureProgress::canceled, this, &CppcheckRunner::stopChecking);
  futureInterface_->setProgressRange (0, 100); // %
  futureInterface_->reportStarted ();
}

void CppcheckRunner::error (QProcess::ProcessError error) {
  Q_UNUSED (error);
  if (showOutput_) {
    Core::MessageManager::write (tr ("Cppcheck error occured"), Core::MessageManager::Silent);
  }
  if (error == QProcess::FailedToStart) {
    finished (-1);
  }
}

void CppcheckRunner::finished (int exitCode) {
  Q_UNUSED (exitCode);
  if (futureInterface_ != NULL) {
    futureInterface_->reportFinished ();
  }
  process_.close ();
  if (showOutput_) {
    Core::MessageManager::write (tr ("Cppcheck finished"), Core::MessageManager::Silent);
  }
}
