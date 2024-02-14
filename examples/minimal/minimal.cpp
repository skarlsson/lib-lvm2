#include <QApplication>
#include <thread>
#include <glog/logging.h>
#include <lvm2/executor.h>
#include "minimal-dataplane.h"
#include "minimal_widget.h"

using namespace std::chrono_literals;

static bool exit_ = false;

int main(int argc, char **argv) {
  QApplication app(argc, argv);

  std::vector<minimal_dataplane::init_entry_t> default_data = {
      {"vehicle.Cabin.Door.Row1.Left.IsOpen", 0,  0,  1},
      {"vehicle.Cabin.Lights.IsDomeOn", 0,  0,  1},
      {"vehicle.Cabin.Seat.Row1.Pos1.Height", 0,  0,  100},
      {"passenger.approaching", 0,  0,  1},
      {"env.safe.to.open", 1,  0,  1},
  };

  auto db = minimal_dataplane::make_unique();
  db->initialize(default_data);

  auto signal_names = db->get_signal_names();

  SignalStateWidget *widget = new SignalStateWidget(nullptr, default_data);
  QObject::connect(widget, &SignalStateWidget::signalValueUpdated, widget, &SignalStateWidget::updateSignalValueSlot,
                   Qt::QueuedConnection);

  auto executor = lua_vm::executor::make_unique([&](auto L) {
    minimal_dataplane::bind_lua(L, db.get());
  });
  executor->load_scripts("../../../examples/minimal/scripts");
  // Use a separate thread to run the executor loop if it should be independent of the GUI
  std::thread executorThread([&] {
    while (!exit_) {
      executor->run_loop();
      std::this_thread::sleep_for(100ms);
    }
  });

  std::thread visuliserThread([&] {
    while (!exit_) {
      for (auto i: signal_names) {
        auto value = db->get(i);
        QMetaObject::invokeMethod(widget, "signalValueUpdated", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromStdString(i)), Q_ARG(int, value));
      }
      std::this_thread::sleep_for(100ms);
    }
  });

  widget->show(); // This makes the widget visible

  // Start the Qt event loop
  int result = app.exec();
  exit_ = true;
  // Make sure to join your threads before exiting
  executorThread.join();
  visuliserThread.join();

  return result;

}