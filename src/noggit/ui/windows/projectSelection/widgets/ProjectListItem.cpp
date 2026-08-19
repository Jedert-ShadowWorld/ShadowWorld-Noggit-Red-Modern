#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/ProjectListItem.hpp>

#include <qgraphicseffect.h>
#include <QGridLayout>
#include <QLabel>
#include <QColor>

namespace Noggit::Ui::Widget
{
  ProjectListItem::ProjectListItem(const ProjectListItemData& data, QWidget* parent = nullptr) : QWidget(parent)
  {
    auto layout = QGridLayout();
    layout.setContentsMargins(4, 4, 4, 4);
    layout.setHorizontalSpacing(6);
    layout.setVerticalSpacing(0);

    QIcon icon;
    if (data.project_version == Project::ProjectVersion::WOTLK)
      icon = QIcon(":/icon-wrath");
    if (data.project_version == Project::ProjectVersion::SL)
      icon = QIcon(":/icon-shadow");

    _project_version_icon = new QLabel("", this);
    _project_version_icon->setPixmap(icon.pixmap(QSize(52, 52)));
    _project_version_icon->setAlignment(Qt::AlignCenter);
    _project_version_icon->setFixedSize(58, 58);

    auto max_width = parent->sizeHint().width();

    auto project_name = toCamelCase(QString(data.project_name));
    _project_name_label = new QLabel(project_name, this);
    _project_name_label->setObjectName("project-title-label");
    _project_name_label->setStyleSheet(
        "QLabel#project-title-label {"
        " color: #eadbfa;"
        " font-size: 15px;"
        " font-weight: 700;"
        " padding: 1px 0px;"
        " }");
    _project_name_label->setToolTip(project_name);

    _project_directory_label = new QLabel(data.project_directory, this);
    _project_directory_label->setObjectName("project-information");
    _project_directory_label->setStyleSheet(
        "QLabel#project-information {"
        " color: #9b8ca7;"
        " font-size: 9px;"
        " padding: 0px;"
        " }");
    _project_directory_label->setToolTip(data.project_directory);

    auto directory_effect = new QGraphicsOpacityEffect(this);
    directory_effect->setOpacity(0.82);
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
        " color: #bda4cf;"
        " font-size: 9px;"
        " font-weight: 600;"
        " padding: 0px;"
        " }");

    auto version_effect = new QGraphicsOpacityEffect(this);
    version_effect->setOpacity(0.9);
    _project_version_label->setGraphicsEffect(version_effect);

    _project_last_edited_label = new QLabel(data.project_last_edited, this);
    _project_last_edited_label->setObjectName("project-information");
    _project_last_edited_label->setStyleSheet(
        "QLabel#project-information {"
        " color: #897a98;"
        " font-size: 9px;"
        " }");
    _project_last_edited_label->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);

    auto last_edited_effect = new QGraphicsOpacityEffect(this);
    last_edited_effect->setOpacity(0.75);
    _project_last_edited_label->setGraphicsEffect(last_edited_effect);

    if (data.is_favorite)
    {
        _project_favorite_icon = new QLabel("", this);
        _project_favorite_icon->setPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(16, 16)));
        _project_favorite_icon->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        _project_favorite_icon->setToolTip("Favorite project — auto-load enabled");

        auto colour = new QGraphicsColorizeEffect(this);
        colour->setColor(QColor(221, 170, 255));
        colour->setStrength(0.85f);
        _project_favorite_icon->setGraphicsEffect(colour);
    }

    setContextMenuPolicy(Qt::CustomContextMenu);
    setMinimumHeight(66);
    setStyleSheet(
        "QWidget { background: transparent; }"
        "QLabel { background: transparent; }");

    layout.addWidget(_project_version_icon, 0, 0, 3, 1, Qt::AlignCenter);
    layout.addWidget(_project_name_label, 0, 1, 1, 2);
    layout.addWidget(_project_directory_label, 1, 1, 1, 2);
    layout.addWidget(_project_version_label, 2, 1, 1, 1);
    layout.addWidget(_project_last_edited_label, 2, 2, 1, 1);

    if (_project_favorite_icon)
      layout.addWidget(_project_favorite_icon, 0, 3, 1, 1, Qt::AlignRight | Qt::AlignTop);

    setLayout(layout);
  }

  QSize ProjectListItem::minimumSizeHint() const
  {
    return QSize(125, 66);
  }

  QString ProjectListItem::toCamelCase(const QString& s)
  {
    QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i)
      parts[i].replace(0, 1, parts[i][0].toUpper());

    return parts.join(" ");
  }
}
