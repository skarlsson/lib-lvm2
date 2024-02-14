#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QGridLayout>
#include <vector>
#include <string>
#include "minimal-dataplane.h"
#pragma once

class SignalStateWidget : public QWidget {
Q_OBJECT
public:
  explicit SignalStateWidget(QWidget *parent = nullptr, std::vector<minimal_dataplane::init_entry_t> tables= { {"dummy", 0, 0, 10}});
  void updateSignalValue(const std::string& name, int value);

signals:
  void signalValueUpdated(const QString& name, int value);

public slots:
  void updateSignalValueSlot(const QString& name, int value);

private:
  std::vector<QLabel*> signalLabels_;
  std::vector<QProgressBar*> signalValues_;
  std::vector<QLabel*> valueLabels_; // Vector to hold value labels
  QGridLayout* layout_;
  std::vector<std::string> signals_; // To hold signal names
};
