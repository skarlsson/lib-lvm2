#include "minimal_widget.h"

SignalStateWidget::SignalStateWidget(QWidget *parent, std::vector<minimal_dataplane::init_entry_t> signal_meta) : QWidget(parent) {
  layout_ = new QGridLayout(this);

  for (auto& entry : signal_meta) {
    signals_.push_back(entry.name); // Ensure signal names are added to signals_
  }

  for (int i = 0; i < signal_meta.size(); ++i) {
    QLabel* label = new QLabel(QString::fromStdString(signal_meta[i].name), this);
    QProgressBar* progressBar = new QProgressBar(this);
    QLabel* valueLabel = new QLabel("0", this); // Initialize the value label with 0 or any default value

    progressBar->setRange(signal_meta[i].min, signal_meta[i].max);
    progressBar->setTextVisible(false);

    signalLabels_.push_back(label);
    signalValues_.push_back(progressBar);
    valueLabels_.push_back(valueLabel); // Add the value label to the vector

    layout_->addWidget(label, i, 0); // Add label to grid layout
    layout_->addWidget(progressBar, i, 1); // Add progress bar next to label
    layout_->addWidget(valueLabel, i, 2); // Add value label next to the progress bar
  }
}

void SignalStateWidget::updateSignalValueSlot(const QString& name, int value) {
  std::string stdName = name.toStdString();
  updateSignalValue(stdName, value); // Use your existing method here
}

void SignalStateWidget::updateSignalValue(const std::string& name, int value) {
  auto it = std::find(signals_.begin(), signals_.end(), name);
  if (it != signals_.end()) {
    int index = std::distance(signals_.begin(), it);
    signalValues_[index]->setValue(value);
    valueLabels_[index]->setText(QString::number(value)); // Update the value label
  }
}