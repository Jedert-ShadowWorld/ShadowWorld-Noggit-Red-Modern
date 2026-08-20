#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/ProjectListItem.hpp>

#include <qgraphicseffect.h>
#include <QGridLayout>
#include <QLabel>
#include <QColor>
#include <QSizePolicy>

namespace Noggit::Ui::Widget
{
  ProjectListItem::ProjectListItem(const ProjectListItemData& data, QWidget* parent = nullptr) : QWidget(parent)
  {
    auto layout = new QGridLayout(this);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(1);
    layout->setColumnStretch(0, 0);
    layout->setColumnStretch(1, 1);
    layout->setColumnStretch(2, 0);
    layout->setColumnStretch(3, 0);

    QIcon icon;
    if (data.project_version == Project::ProjectVersion::WOTLK)
      icon = QIcon(":/icon-wrath");
    if (data.project_version == Project::ProjectVersion::SL)
      icon = QIcon(":/icon-shadow");

    _project_version_icon = new QLabel("", this);
    _project_version_icon->setPixmap(icon.pixmap(QSize(66, 66)));
    _project_version_icon->setAlignment(Qt::AlignCenter);
    _project_version_icon->setFixedSize(72, 72);

    auto project_name = toCamelCase(QString(data.project_name));
    _project_name_label = new QLabel(project_name, this);
    _project_name_label->setObjectName("project-title-label");
    _project_name_label->setStyleSheet(
        "QLabel#project-title-label {"
        " color: #f0e7f5;"
        " font-size: 17px;"
        " font-weight: 700;"
        " padding: 0px;"
        " }");
    _project_name_label->setToolTip(project_name);
    _project_name_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    _project_directory_label = new QLabel(data.project_directory, this);
    _project_directory_label->setObjectName("project-directory-label");
    _project_directory_label->setStyleSheet(
        "QLabel#project-directory-label {"
        " color: #aa9daf;"
        " font-size: 10px;"
        " padding: 0px;"
        " }");
    _project_directory_label->setToolTip(data.project_directory);
    _project_directory_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto directory_effect = new QGraphicsOpacityEffect(this);
    directory_effect->setOpacity(0.9);
    _project_directory_label->setGraphicsEffect(directory_effect);

    QString version;
    if (data.project_version == Project::ProjectVersion::WOTLK)
      version = "Wrath Of The Lich King";
    if (data.project_version == Project::ProjectVersion::SL)
      version = "Shadowlands";

    _project_version_label = new QLabel(version, this);
    _project_version_label->setObjectName("project-version-label");
    _project_version_label->setStyleSheet(
        "QLabel#project-version-label {"
        " color: #c6b6cf;"
        " font-size: 10px;"
        " font-weight: 600;"
        " padding: 0px;"
        " }");

    _project_last_edited_label = new QLabel(data.project_last_edited, this);
    _project_last_edited_label->setObjectName("project-date-label");
    _project_last_edited_label->setStyleSheet(
        "QLabel#project-date-label {"
        " color: #87788e;"
        " font-size: 9px;"
        " padding: 0px;"
        " }");
    _project_last_edited_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    if (data.is_favorite)
    {
      _project_favorite_icon = new QLabel("", this);
      _project_favorite_icon->setPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(16, 16)));
      _project_favorite_icon->setAlignment(Qt::AlignRight | Qt::AlignTop);
      _project_favorite_icon->setToolTip("Favorite project — auto-load enabled");

      auto colour = new QGraphicsColorizeEffect(this);
      colour->setColor(QColor(221, 170, 255));
      colour->setStrength(0.85f);
      _project_favorite_icon->setGraphicsEffect(colour);
    }

    setContextMenuPolicy(Qt::CustomContextMenu);
    setMinimumHeight(88);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setStyleSheet(
        "QWidget { background: transparent; }"
        "QLabel { background: transparent; }");

    layout->addWidget(_project_version_icon, 0, 0, 3, 1, Qt::AlignVCenter);
    layout->addWidget(_project_name_label, 0, 1, 1, 2);
    layout->addWidget(_project_directory_label, 1, 1, 1, 2);
    layout->addWidget(_project_version_label, 2, 1, 1, 1);
    layout->addWidget(_project_last_edited_label, 2, 2, 1, 1, Qt::AlignRight);

    if (_project_favorite_icon)
      layout->addWidget(_project_favorite_icon, 0, 3, 1, 1, Qt::AlignRight | Qt::AlignTop);
  }

  QSize ProjectListItem::minimumSizeHint() const
  {
    return QSize(360, 88);
  }

  QString ProjectListItem::toCamelCase(const QString& s)
  {
    QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i)
      parts[i].replace(0, 1, parts[i][0].toUpper());

    return parts.join(" ");
  }
}
