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

#include "ui_NoggitProjectSelectionWindow.h"

#include <filesystem>


using namespace Noggit::Ui::Windows;

NoggitProjectSelectionWindow::NoggitProjectSelectionWindow(Noggit::Application::NoggitApplication* noggit_app,
                                                           QWidget* parent)
  : QMainWindow(parent)
  , _ui(new ::Ui::NoggitProjectSelectionWindow)
  , _noggit_application(noggit_app)
{
  _ui->setupUi(this);
  _load_project_component = std::make_unique<Component::LoadProjectComponent>();

  setWindowFlags(Qt::Window | Qt::MSWindowsFixedSizeDialogHint);
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
  delete _ui;
}
