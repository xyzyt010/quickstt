#ifndef SETUP_WIZARD_H
#define SETUP_WIZARD_H

#include <QWizard>

class SetupWizard : public QWizard {
public:
  explicit SetupWizard(QWidget *parent = nullptr);
  void applySettings();
};

#endif // SETUP_WIZARD_H
