#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace Noggit::Ui
{
  struct UidRepairIssue
  {
    QString asset;
    QString source;
    QString uid;
    QString reason;
  };

  // Esc, the window close button and Cancel reject the dialog.
  class UidRepairReview : public QDialog
  {
  public:
    UidRepairReview(QString const& summary, QVector<UidRepairIssue> const& issues,
                    QString const& action, bool canApply, QWidget* parent = nullptr)
      : QDialog(parent)
    {
      setWindowTitle("Fix UID — review");
      resize(1050, 600);
      auto layout = new QVBoxLayout(this);
      auto label = new QLabel(summary, this);
      label->setTextFormat(Qt::PlainText);
      label->setWordWrap(true);
      layout->addWidget(label);
      auto filter = new QLineEdit(this);
      filter->setPlaceholderText("Filter by asset, tile, UID or error");
      layout->addWidget(filter);
      auto table = new QTableWidget(issues.size(), 4, this);
      table->setHorizontalHeaderLabels({"Asset / FileDataID", "Source tile", "Old UID", "Problem / planned change"});
      table->setEditTriggers(QAbstractItemView::NoEditTriggers);
      table->setSelectionBehavior(QAbstractItemView::SelectRows);
      table->setAlternatingRowColors(true);
      table->verticalHeader()->hide();
      table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
      table->horizontalHeader()->setStretchLastSection(true);
      table->setColumnWidth(0, 300);
      table->setColumnWidth(1, 270);
      table->setColumnWidth(2, 100);
      for (int row = 0; row < issues.size(); ++row)
      {
        auto const& issue = issues[row];
        QStringList values{issue.asset, issue.source, issue.uid, issue.reason};
        for (int col = 0; col < values.size(); ++col)
        {
          auto item = new QTableWidgetItem(values[col]);
          item->setToolTip(values[col]);
          table->setItem(row, col, item);
        }
      }
      table->setSortingEnabled(true);
      layout->addWidget(table);
      connect(filter, &QLineEdit::textChanged, this, [table](QString const& text)
      {
        for (int row = 0; row < table->rowCount(); ++row)
        {
          bool match = text.isEmpty();
          for (int col = 0; col < table->columnCount(); ++col)
            match |= table->item(row, col)->text().contains(text, Qt::CaseInsensitive);
          table->setRowHidden(row, !match);
        }
      });
      auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
      auto apply = buttons->addButton(action, QDialogButtonBox::AcceptRole);
      apply->setObjectName("applyUidRepair");
      apply->setEnabled(canApply);
      apply->setAutoDefault(false);
      auto cancel = buttons->button(QDialogButtonBox::Cancel);
      cancel->setDefault(true);
      cancel->setFocus();
      connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
      layout->addWidget(buttons);
    }
  };
}
