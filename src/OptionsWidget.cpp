#include <QFileDialog>
#include <QTextStream>
#include <QTextEdit>

#include <coreplugin/variablechooser.h>

#include "OptionsWidget.h"
#include "ui_OptionsWidget.h"
#include "Settings.h"

using namespace QtcCppcheck::Internal;

namespace {
  const QLatin1String versionArg ("--version");
  const QLatin1String helpArg ("--help");
}

OptionsWidget::OptionsWidget (Settings *settings, QWidget *parent) :
  QWidget (parent),
  ui (new Ui::OptionsWidget), settings_ (settings) {
  Q_ASSERT (settings_ != NULL);

  ui->setupUi (this);
  ui->binFileEdit->setExpectedKind (Utils::PathChooser::ExistingCommand);
  ui->binFileEdit->setCommandVersionArguments ({ versionArg });

  auto chooser = new Core::VariableChooser (this);
  chooser->addSupportedWidget (ui->customParametersEdit);

  connect (ui->getHelpButton, &QAbstractButton::clicked, this, &OptionsWidget::getPossibleParams);
  connect (&process_, static_cast<void (QProcess::*)(int)>(&QProcess::finished),
           this, &OptionsWidget::finished);

  initInterface ();
}

OptionsWidget::~OptionsWidget () {
  delete ui;
  settings_ = NULL;
}

void OptionsWidget::getPossibleParams () {
  QString binary = ui->binFileEdit->path ();
  if (binary.isEmpty ()) {
    return;
  }
  processArguments_ = QStringList () << helpArg;
  process_.start (binary, processArguments_);
}

void OptionsWidget::finished () {
  QByteArray output = process_.readAllStandardOutput ();
  QString outputString = QString::fromUtf8 (output).trimmed ();
  if (processArguments_.contains (helpArg)) {
    int startIndex = outputString.indexOf (QLatin1String ("Options:"));
    int endIndex = outputString.indexOf (QLatin1String ("Example usage:"));
    if (startIndex >= endIndex) {
      return;
    }
    QString options = outputString.mid (startIndex, endIndex - startIndex);
    QTextEdit *editor = new QTextEdit;
    editor->setAttribute (Qt::WA_ShowModal);
    editor->setAttribute (Qt::WA_DeleteOnClose);
    editor->setReadOnly (false);
    editor->setText (options);
    editor->showMaximized ();
  }
}

void OptionsWidget::applySettings () {
  Q_ASSERT (settings_ != NULL);
  settings_->setBinaryFile (ui->binFileEdit->path ());
  settings_->setCheckOnBuild (ui->onBuildCheckBox->isChecked ());
  settings_->setCheckOnSave (ui->onSaveCheckBox->isChecked ());
  settings_->setCheckOnProjectChange (ui->onProjectChangeCheckBox->isChecked ());
  settings_->setCheckOnFileAdd (ui->onFileAddedCheckBox->isChecked ());
  settings_->setCheckUnused (ui->unusedCheckBox->isChecked ());
  settings_->setCheckInconclusive (ui->inconclusiveCheckBox->isChecked ());
  settings_->setCustomParameters (ui->customParametersEdit->text ());
  settings_->setIgnorePatterns (ui->ignoreEdit->text ().split (","));
  settings_->setIgnoreIncludePaths (ui->ignoreIncludePathsCheck->isChecked ());
  settings_->setShowBinaryOutput (ui->showOutputCheckBox->isChecked ());
  settings_->setShowId (ui->showIdCheckBox->isChecked ());
  settings_->setPopupOnError (ui->popupOnErrorCheckBox->isChecked ());
  settings_->setPopupOnWarning (ui->popupOnWarningCheckBox->isChecked ());
  settings_->save ();
}

void OptionsWidget::initInterface () {
  Q_ASSERT (settings_ != NULL);
  ui->binFileEdit->setPath (settings_->binaryFile ());
  ui->onBuildCheckBox->setChecked (settings_->checkOnBuild ());
  ui->onSaveCheckBox->setChecked (settings_->checkOnSave ());
  ui->onProjectChangeCheckBox->setChecked (settings_->checkOnProjectChange ());
  ui->onFileAddedCheckBox->setChecked (settings_->checkOnFileAdd ());
  ui->unusedCheckBox->setChecked (settings_->checkUnused ());
  ui->inconclusiveCheckBox->setChecked (settings_->checkInconclusive ());
  ui->customParametersEdit->setText (settings_->customParameters ());
  ui->ignoreEdit->setText (settings_->ignorePatterns ().join (","));
  ui->ignoreIncludePathsCheck->setChecked (settings_->ignoreIncludePaths ());
  ui->showOutputCheckBox->setChecked (settings_->showBinaryOutput ());
  ui->showIdCheckBox->setChecked (settings_->showId ());
  ui->popupOnErrorCheckBox->setChecked (settings_->popupOnError ());
  ui->popupOnWarningCheckBox->setChecked (settings_->popupOnWarning ());
}
