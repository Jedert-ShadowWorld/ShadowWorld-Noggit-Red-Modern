#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/project/ApplicationProjectReader.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/projectCreation/NoggitProjectCreationDialog.h>
#include <noggit/ui/windows/projectSelection/components/CreateProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/LoadProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/RecentProjectsComponent.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>


#include <QFile>
#include <QFileDialog>
#include <QSettings>
#include <QString>
#include <QMessageBox>
#include <QGraphicsDropShadowEffect>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QPainter>
#include <QRadialGradient>
#include <QTimer>
#include <QMouseEvent>
#include <QEvent>

#include "ui_NoggitProjectSelectionWindow.h"

#include <filesystem>
#include <array>
#include <cmath>


using namespace Noggit::Ui::Windows;

namespace
{
class LauncherSparkles final : public QWidget
{
public:
  explicit LauncherSparkles(QWidget* parent) : QWidget(parent)
  {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    _clock.start();
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
    timer->start(40);
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto const t = _clock.elapsed() / 1000.0;
    constexpr std::array<QPointF, 8> stars = {{
        {0.11, 0.39}, {0.89, 0.13}, {0.86, 0.70}, {0.17, 0.86},
        {0.52, 0.16}, {0.48, 0.72}, {0.08, 0.69}, {0.93, 0.46}}};
    for (size_t i = 0; i < stars.size(); ++i)
    {
      auto const phase = t * (1.5 + i * 0.13) + i * 1.7;
      auto const glow = 0.5 + 0.5 * std::sin(phase);
      auto const x = stars[i].x() * width() + 5.0 * std::sin(t * 0.3 + i);
      auto const y = stars[i].y() * height() - 9.0 * std::sin(t * 0.5 + i);
      auto const radius = 7.0 + 8.0 * glow;
      QRadialGradient halo(QPointF(x, y), radius);
      halo.setColorAt(0, QColor(238, 225, 255, static_cast<int>(140 * glow)));
      halo.setColorAt(0.3, QColor(165, 118, 255, static_cast<int>(85 * glow)));
      halo.setColorAt(1, QColor(80, 90, 220, 0));
      painter.setPen(Qt::NoPen);
      painter.setBrush(halo);
      painter.drawEllipse(QPointF(x, y), radius, radius);
      painter.setPen(QPen(QColor(225, 213, 255, static_cast<int>(210 * glow)), 1.0));
      painter.drawLine(QPointF(x - 3.0 - 2.0 * glow, y), QPointF(x + 3.0 + 2.0 * glow, y));
      painter.drawLine(QPointF(x, y - 3.0 - 2.0 * glow), QPointF(x, y + 3.0 + 2.0 * glow));
    }
  }

private:
  QElapsedTimer _clock;
};
}

NoggitProjectSelectionWindow::NoggitProjectSelectionWindow(Noggit::Application::NoggitApplication* noggit_app,
                                                           QWidget* parent)
  : QMainWindow(parent)
  , _ui(new ::Ui::NoggitProjectSelectionWindow)
  , _noggit_application(noggit_app)
{
  _ui->setupUi(this);
  _ui->recentHint->hide();
  _ui->headerLayout->removeWidget(_ui->titleCrest);
  _ui->titleCrest->setParent(_ui->centralwidget);
  _ui->titleCrest->show();
  _ui->centralwidget->installEventFilter(this);
  _ui->topBar->installEventFilter(this);
  _ui->titleCrest->installEventFilter(this);
  _ui->shadowWorldTitle->installEventFilter(this);
  _ui->shadowWorldSubtitle->installEventFilter(this);
  _ui->titleCrest->raise();
  auto* title_glow = new QGraphicsDropShadowEffect(_ui->shadowWorldTitle);
  title_glow->setColor(QColor(71, 151, 255, 220));
  title_glow->setOffset(0, 0);
  _ui->shadowWorldTitle->setGraphicsEffect(title_glow);
  auto* breathing = new QPropertyAnimation(title_glow, "blurRadius", title_glow);
  breathing->setDuration(3600);
  breathing->setStartValue(7.0);
  breathing->setKeyValueAt(0.5, 28.0);
  breathing->setEndValue(7.0);
  breathing->setEasingCurve(QEasingCurve::InOutSine);
  breathing->setLoopCount(-1);
  breathing->start();
  auto* sparkles = new LauncherSparkles(_ui->heroArtwork);
  auto position_sparkles = [artwork = _ui->heroArtwork, sparkles]
  {
    sparkles->setGeometry((artwork->width() - 470) / 2,
                          (artwork->height() - 470) / 2, 470, 470);
    sparkles->raise();
  };
  QTimer::singleShot(0, sparkles, position_sparkles);
  auto* sparkle_layout_timer = new QTimer(sparkles);
  QObject::connect(sparkle_layout_timer, &QTimer::timeout, sparkles, position_sparkles);
  sparkle_layout_timer->start(250);
  _load_project_component = std::make_unique<Component::LoadProjectComponent>();

  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setFixedSize(size());
  QTimer::singleShot(0, this, [this]
  {
    _ui->titleCrest->move((_ui->centralwidget->width() - _ui->titleCrest->width()) / 2, 82);
    _ui->titleCrest->raise();
  });
  bool const force_project_selector = _noggit_application->GetCommand(2);

  ////////////////////////////
  // direct config project loading
  auto const configured_project_path =
      std::filesystem::path(_noggit_application->getConfiguration()->ApplicationProjectPath);

  if (!force_project_selector
      && !noggit_app->hasClientData()
      && std::filesystem::exists(configured_project_path)
      && std::filesystem::is_directory(configured_project_path))
  {
    Log << "Auto loading configured project path : "
        << configured_project_path.string() << std::endl;

    auto selected_project = _load_project_component->loadProject(
        this, QString(configured_project_path.string().c_str()));

    if (selected_project)
    {
      Noggit::Project::CurrentProject::initialize(selected_project.get());

      _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
          _noggit_application->getConfiguration(),
          selected_project);
      _project_selection_page->showMaximized();

      close();
      return;
    }
  }

  ////////////////////////////
  // auto load favorite project
  QSettings settings;
  int favorite_proj_idx = settings.value("favorite_project", -1).toInt();

  bool load_favorite = !force_project_selector
      && settings.value("auto_load_fav_project", true).toBool();

  // if it has client data, it means it already loaded before and we exited through the menu, skip autoloading favorite
  if (noggit_app->hasClientData())
      load_favorite = false;

  if (load_favorite && favorite_proj_idx != -1)
  {
    Log << "Auto loading favorite project index : " << favorite_proj_idx << std::endl;

    int size = settings.beginReadArray("recent_projects");

    QString project_final_path;

    if (size > favorite_proj_idx)
    {
      settings.setArrayIndex(favorite_proj_idx);
      std::filesystem::path project_path = settings.value("project_path").toString().toStdString().c_str();

      if (std::filesystem::exists(project_path) && std::filesystem::is_directory(project_path))
      {
        auto project_reader = Noggit::Project::ApplicationProjectReader();
        auto project = project_reader.readProject(project_path);

        if (project.has_value())
          project_final_path = QString(project_path.string().c_str());
      }
    }
    settings.endArray();

    if (!project_final_path.isEmpty())
    {
      auto selected_project = _load_project_component->loadProject(this, project_final_path);

      if (!selected_project)
      {
        LogError << "Selected Project is null, favorite loading failed." << std::endl;
      }
      else
      {
        Noggit::Project::CurrentProject::initialize(selected_project.get());

        _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
            _noggit_application->getConfiguration(),
            selected_project);
        _project_selection_page->showMaximized();

        close();
        return;
      }
    }
  }
  ///////////////////////////

  // Keep the panel title object names and styling defined by the ShadowWorld .ui file.
  // The old launcher code overwrote both labels with a generic 18px style at runtime,
  // which made the redesigned UI look like the stock Qt screen.

  _settings = new Noggit::Ui::settings(this);

  _ui->settings_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::cog));
  _ui->settings_button->setIconSize(QSize(24,24));

  Component::RecentProjectsComponent::buildRecentProjectsList(this);

  QObject::connect(_ui->settings_button, &QToolButton::clicked, [&]
      {
          _settings->show();
      }
  );

  // ShadowWorld launcher close button. This intentionally closes only the launcher;
  // the existing project actions below retain their original behavior.
  QObject::connect(_ui->button_exit_project, &QPushButton::clicked, this, &QWidget::close);

  QObject::connect(_ui->button_create_new_project, &QPushButton::clicked, [=, this]
                   {
                     ProjectInformation project_reference;
                     NoggitProjectCreationDialog project_creation_dialog(project_reference, this);

                     QObject::connect(&project_creation_dialog,  &QDialog::finished, [&project_reference, this](int result)
                     {
                       if (result != QDialog::Accepted)
                         return;

                       Component::CreateProjectComponent::createProject(this, project_reference);
                       resetFavoriteProject();
                       Component::RecentProjectsComponent::buildRecentProjectsList(this);
                     });

                     project_creation_dialog.exec();
                     project_creation_dialog.setFixedSize(project_creation_dialog.size());

                   }
  );

  QObject::connect(_ui->button_open_existing_project, &QPushButton::clicked, [=]
                   {
                     QString recent_project_path = _ui->listView->currentIndex().data(Qt::UserRole).toString();

                     if (!recent_project_path.isEmpty())
                     {
                       auto selected_project = _load_project_component->loadProject(this, recent_project_path);

                       if (!selected_project)
                       {
                         LogError << "Selected Project is null, loading failed." << std::endl;
                         QMessageBox::critical(this, "Error", "Failed to load selected recent project. Check the client path and project file.");
                         return;
                       }

                       Noggit::Project::CurrentProject::initialize(selected_project.get());

                       _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                           _noggit_application->getConfiguration(),
                           selected_project);
                       _project_selection_page->showMaximized();

                       close();
                       return;
                     }

                     auto project_reader = Noggit::Project::ApplicationProjectReader();

                     QString proj_file = QFileDialog::getOpenFileName(this, "Open File",
                                                                     "/",
                                                                     "*.noggitproj");

                     if (proj_file.isEmpty())
                     {
                       QMessageBox::critical(this, "Error", "No project file selected.");
                       return;
                     }

                     std::filesystem::path filepath(proj_file.toStdString());

                     auto project = project_reader.readProjectFile(filepath);

                     if (!project.has_value())
                     {
                       QMessageBox::critical(this, "Error", "Failed to read project");
                       return;
                     }

                     Component::RecentProjectsComponent::registerProjectChange(filepath.parent_path().string());

                     auto application_configuration = _noggit_application->getConfiguration();
                     auto application_projects_folder_path = std::filesystem::path(application_configuration->ApplicationProjectPath);
                     auto application_project_service = Noggit::Project::ApplicationProject(application_configuration);

                     auto project_to_launch = application_project_service.loadProject(filepath.parent_path());

                     if (!project_to_launch)
                     {
                        QMessageBox::critical(this, "Error", "Failed to load selected project. Check the client path and project file.");
                        return;
                     }

                     Noggit::Application::NoggitApplication::instance()->setClientData(project_to_launch->ClientData);

                     Noggit::Project::CurrentProject::initialize(project_to_launch.get());

                     _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                         _noggit_application->getConfiguration(),
                         project_to_launch);
                     _project_selection_page->showMaximized();

                     close();
                   }
  );

  QObject::connect(_ui->listView, &QListView::doubleClicked, [=]
                   {
                     auto selected_project = _load_project_component->loadProject(this);

                     if (!selected_project)
                     {
                       LogError << "Selected Project is null, loading failed." << std::endl;
                       QMessageBox::critical(this, "Error", "Failed to load selected recent project. Check the client path and project file.");
                       return;
                     }

                     Noggit::Project::CurrentProject::initialize(selected_project.get());

                     _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                         _noggit_application->getConfiguration(),
                         selected_project);
                     _project_selection_page->showMaximized();

                     close();
                   }
  );

  show();
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemDelete(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Delete Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Deleting a project will remove all saved data. Do you want to continue?");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
    {
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      QFile folder(project_path.c_str());
      folder.moveToTrash();
      break;
    }
    case QMessageBox::DestructiveRole:
    default:
      break;
  }
  resetFavoriteProject();

  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemForget(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Forget Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Data on the disk will not be removed, this action will only hide the project. Continue?.");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      break;
    case QMessageBox::DestructiveRole:
    default:
      break;
  }

  resetFavoriteProject();
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::handleContextMenuProjectListItemFavorite(int index)
{
  QSettings settings;
  settings.sync();
  settings.setValue("favorite_project", index);
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::resetFavoriteProject()
{
    QSettings settings;
    settings.sync();
    settings.setValue("favorite_project", -1);
}

NoggitProjectSelectionWindow::~NoggitProjectSelectionWindow()
{
  _ui->centralwidget->removeEventFilter(this);
  _ui->topBar->removeEventFilter(this);
  _ui->titleCrest->removeEventFilter(this);
  _ui->shadowWorldTitle->removeEventFilter(this);
  _ui->shadowWorldSubtitle->removeEventFilter(this);
  delete _ui;
}

bool NoggitProjectSelectionWindow::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == _ui->centralwidget && event->type() == QEvent::Resize)
    _ui->titleCrest->move((_ui->centralwidget->width() - _ui->titleCrest->width()) / 2, 82);

  if (watched == _ui->topBar || watched == _ui->titleCrest
      || watched == _ui->shadowWorldTitle || watched == _ui->shadowWorldSubtitle)
  {
    if (event->type() == QEvent::MouseButtonPress)
    {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (mouse->button() == Qt::LeftButton)
      {
        _drag_offset = mouse->globalPos() - frameGeometry().topLeft();
        _dragging = true;
        return true;
      }
    }
    if (event->type() == QEvent::MouseMove && _dragging)
    {
      move(static_cast<QMouseEvent*>(event)->globalPos() - _drag_offset);
      return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && _dragging)
    {
      _dragging = false;
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}
