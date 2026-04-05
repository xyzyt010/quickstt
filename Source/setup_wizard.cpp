#include "setup_wizard.h"
#include "startup_utils.h"
#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QLabel>
#include <QSettings>
#include <QStringList>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
QString canonicalWakeEngineLabel(const QString &value) {
  const QString trimmed = value.trimmed();
  if (trimmed.contains("Porcupine", Qt::CaseInsensitive))
    return "Porcupine (Access Key Required)";
  if (trimmed.contains("Vosk", Qt::CaseInsensitive))
    return "Vosk Keyword (Built-in)";
  return "OpenWakeWord (TFLite)";
}

class LicensePage : public QWizardPage {
public:
  LicensePage() {
    setTitle("Welcome to QuickSTT");
    setSubTitle(
        "This setup covers the basic placeholders and startup behavior. Legal "
        "copy can be replaced later without changing the flow.");

    QVBoxLayout *welcomeLayout = new QVBoxLayout(this);
    QFrame *legalCard = new QFrame(this);
    QVBoxLayout *legalLayout = new QVBoxLayout(legalCard);

    QLabel *legalIntro = new QLabel(
        "The agreement, license, and terms text are placeholders for now. "
        "The pages exist so the app has a real setup flow instead of ad-hoc "
        "message boxes.");
    legalIntro->setWordWrap(true);
    legalLayout->addWidget(legalIntro);

    QTextBrowser *legalText = new QTextBrowser(this);
    legalText->setMinimumHeight(220);
    legalText->setPlainText(
        "User Agreement\n\n[Placeholder - empty for now]\n\n"
        "License\n\n[Placeholder - empty for now]\n\n"
        "Terms & Conditions\n\n[Placeholder - empty for now]");
    legalLayout->addWidget(legalText);

    QCheckBox *acceptAgreement =
        new QCheckBox("I accept the current User Agreement placeholder");
    QCheckBox *acceptLicense =
        new QCheckBox("I accept the current License placeholder");
    QCheckBox *acceptTerms =
        new QCheckBox("I accept the current Terms & Conditions placeholder");
    registerField("acceptAgreement*", acceptAgreement);
    registerField("acceptLicense*", acceptLicense);
    registerField("acceptTerms*", acceptTerms);
    legalLayout->addWidget(acceptAgreement);
    legalLayout->addWidget(acceptLicense);
    legalLayout->addWidget(acceptTerms);

    welcomeLayout->addWidget(legalCard);
    welcomeLayout->addStretch();
  }
};

class PreferencesPage : public QWizardPage {
public:
  PreferencesPage() {
    setTitle("Startup Preferences");
    setSubTitle(
        "Choose the basic behavior QuickSTT should use after setup.");

    QSettings s("QuickSTT", "Config");

    QVBoxLayout *prefsLayout = new QVBoxLayout(this);
    QFrame *prefsCard = new QFrame(this);
    QVBoxLayout *prefsCardLayout = new QVBoxLayout(prefsCard);

    QCheckBox *runOnStartup =
        new QCheckBox("Run QuickSTT automatically when Windows starts");
    runOnStartup->setChecked(s.value("startupChecked", false).toBool());
    registerField("runOnStartup", runOnStartup);
    prefsCardLayout->addWidget(runOnStartup);

    QCheckBox *startupBackground =
        new QCheckBox("Start minimized to tray when launched automatically");
    startupBackground->setChecked(s.value("startupBackground", true).toBool());
    registerField("startupBackground", startupBackground);
    prefsCardLayout->addWidget(startupBackground);

    QObject::connect(runOnStartup, &QCheckBox::toggled, startupBackground,
                     &QCheckBox::setEnabled);
    startupBackground->setEnabled(runOnStartup->isChecked());

    QCheckBox *showWaveform =
        new QCheckBox("Show the live waveform while listening");
    showWaveform->setChecked(s.value("showWaveform", true).toBool());
    registerField("showWaveform", showWaveform);
    prefsCardLayout->addWidget(showWaveform);

    QCheckBox *useOpenWakeword =
        new QCheckBox("Use OpenWakeWord as the default wakeword engine");
    useOpenWakeword->setChecked(
        canonicalWakeEngineLabel(
            s.value("wakeEngine", "OpenWakeWord (TFLite)").toString())
            .contains("OpenWakeWord", Qt::CaseInsensitive));
    registerField("useOpenWakeword", useOpenWakeword);
    prefsCardLayout->addWidget(useOpenWakeword);

    QLabel *startupHint = new QLabel(
        "You can change these later from the dashboard. The startup option "
        "writes the current launcher command into the Windows Run registry "
        "key.");
    startupHint->setWordWrap(true);
    prefsCardLayout->addWidget(startupHint);

    prefsLayout->addWidget(prefsCard);
    prefsLayout->addStretch();
  }
};

class SummaryPage : public QWizardPage {
public:
  SummaryPage() {
    setTitle("Review Setup");
    setSubTitle("Confirm the basic startup choices before QuickSTT launches.");

    summaryLabel = new QLabel(this);
    summaryLabel->setWordWrap(true);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(summaryLabel);
    layout->addStretch();
  }

  void initializePage() override {
    QStringList lines;
    lines << "Agreement placeholders accepted: Yes";
    lines << QString("Run on Windows startup: %1")
                 .arg(field("runOnStartup").toBool() ? "Yes" : "No");
    lines << QString("Start minimized to tray on auto launch: %1")
                 .arg(field("startupBackground").toBool() ? "Yes" : "No");
    lines << QString("Show waveform while listening: %1")
                 .arg(field("showWaveform").toBool() ? "Yes" : "No");
    lines << QString("Wakeword engine: %1")
                 .arg(field("useOpenWakeword").toBool()
                          ? "OpenWakeWord (TFLite)"
                          : "Vosk Keyword (Built-in)");
    summaryLabel->setText(lines.join("\n"));
  }

private:
  QLabel *summaryLabel = nullptr;
};
} // namespace

SetupWizard::SetupWizard(QWidget *parent) : QWizard(parent) {
  setWindowTitle("QuickSTT Setup");
  setWizardStyle(QWizard::ModernStyle);
  setOption(QWizard::NoBackButtonOnStartPage);
  setOption(QWizard::HaveHelpButton, false);
  setButtonText(QWizard::FinishButton, "Save & Launch");
  resize(680, 520);

  setStyleSheet(
      "QWizard { background: #121212; color: #E6E6E6; }"
      "QLabel { color: #E6E6E6; font-size: 13px; }"
      "QWizard QFrame { background: #1B1B1B; border: 1px solid #2E2E2E; "
      "border-radius: 10px; }"
      "QTextBrowser { background: #171717; border: 1px solid #2D2D2D; "
      "border-radius: 8px; color: #E0E0E0; padding: 10px; }"
      "QCheckBox { spacing: 8px; }"
      "QCheckBox::indicator { width: 18px; height: 18px; }"
      "QPushButton { background: #1D1D1D; color: #F0F0F0; border: 1px solid "
      "#333333; border-radius: 6px; padding: 6px 14px; }"
      "QPushButton:hover { background: #2A2A2A; }"
      "QPushButton:disabled { color: #777777; }");

  addPage(new LicensePage());
  addPage(new PreferencesPage());
  addPage(new SummaryPage());
}

void SetupWizard::applySettings() {
  QSettings s("QuickSTT", "Config");
  s.setValue("firstLaunch", false);
  s.setValue("setupCompleted", true);
  s.setValue("acceptedUserAgreement", field("acceptAgreement").toBool());
  s.setValue("acceptedLicense", field("acceptLicense").toBool());
  s.setValue("acceptedTerms", field("acceptTerms").toBool());
  s.setValue("agreementsAcceptedAt", QDateTime::currentDateTime());
  s.setValue("startupChecked", field("runOnStartup").toBool());
  s.setValue("startupBackground", field("startupBackground").toBool());
  s.setValue("showWaveform", field("showWaveform").toBool());
  s.setValue("wakeEngine", field("useOpenWakeword").toBool()
                               ? "OpenWakeWord (TFLite)"
                               : "Vosk Keyword (Built-in)");

  applyStartupSetting(field("runOnStartup").toBool());
}
