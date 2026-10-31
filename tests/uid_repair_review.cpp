#include <noggit/ui/UidRepairReview.hpp>
#include <QApplication>
#include <QTimer>
#include <stdexcept>
#include <iostream>

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  try
  {
    QVector<Noggit::Ui::UidRepairIssue> issues{
      {"filedataid:42", "map_1_2_obj0.adt", "7", "Missing"},
      {"broken.m2", "map_2_2_obj0.adt", "8", "Invalid header"}};
    Noggit::Ui::UidRepairReview cancel("No changes until confirmation", issues, "Apply", true);
    auto table = cancel.findChild<QTableWidget*>();
    if (!table || table->rowCount()!=2 || table->item(1,3)->text()!="Invalid header")
      throw std::runtime_error("Problem list is incomplete");
    auto filter = cancel.findChild<QLineEdit*>();
    filter->setText("broken");
    if (!table->isRowHidden(0) || table->isRowHidden(1))
      throw std::runtime_error("Filtering failed");
    QTimer::singleShot(0, &cancel, &QDialog::reject);
    if (cancel.exec()!=QDialog::Rejected) throw std::runtime_error("Cancel accepted changes");
    Noggit::Ui::UidRepairReview apply("Ready", issues, "Apply", true);
    QTimer::singleShot(0, &apply, [&]() { apply.findChild<QPushButton*>("applyUidRepair")->click(); });
    if (apply.exec()!=QDialog::Accepted) throw std::runtime_error("Confirmation not returned");
    Noggit::Ui::UidRepairReview blocked("Invalid ADT", issues, "Apply", false);
    if (blocked.findChild<QPushButton*>("applyUidRepair")->isEnabled())
      throw std::runtime_error("Invalid input can be applied");
    QTimer::singleShot(0, &blocked, &QDialog::close);
    if (blocked.exec()!=QDialog::Rejected) throw std::runtime_error("Closing window accepted changes");
    std::cout << "PASS: review list, filter, confirm, cancel, close and blocked apply\n";
    return 0;
  }
  catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
