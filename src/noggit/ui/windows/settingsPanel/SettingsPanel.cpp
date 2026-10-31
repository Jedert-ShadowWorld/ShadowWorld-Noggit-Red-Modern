// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h>
#include <noggit/ui/FramelessWindow.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>
#include <noggit/database/SqlDatabaseManager.h>

#include <QDir>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QShowEvent>
#include <ClientData.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <QtCore/QSettings>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QPushButton>

#include <ui_SettingsPanel.h>

#include <sstream>
#include <QMessageBox>


namespace Noggit
{
  namespace Ui
  {
    settings::settings(QWidget *parent) : QMainWindow(parent, Qt::Window), _settings(new QSettings(this))
    {
      auto body = new QWidget(this);
      ui = new ::Ui::SettingsPanel;
      ui->setupUi(body);
      setCentralWidget(body);

      auto previewTab = new QWidget(ui->tabWidget);
      auto previewLayout = new QVBoxLayout(previewTab);
      auto scope = new QLabel(tr("Model quality for the Retail map viewport"), previewTab);
      previewLayout->addWidget(scope);
      auto previewForm = new QFormLayout;
      previewLayout->addLayout(previewForm);
      _retail_preview_quality = new QComboBox(previewTab);
      _retail_preview_quality->addItems({tr("Low — layout"), tr("Medium — editing"),
          tr("High — review"), tr("Ultra — full detail")});
      previewForm->addRow(tr("Model quality"), _retail_preview_quality);
      _retail_preview_lod = new QComboBox(previewTab);
      _retail_preview_lod->addItems({tr("From quality preset"), tr("Highest"),
          tr("Reduced"), tr("Lowest available")});
      _retail_preview_lod->setToolTip(tr(
          "Inspect an available M2 model LOD without changing placements. "
          "Models without a usable lower LOD retain their original mesh."));
      previewForm->addRow(tr("Model geometry (LOD)"), _retail_preview_lod);
      _retail_preview_animation = new QComboBox(previewTab);
      _retail_preview_animation->addItems({tr("Running"), tr("Paused — hold current pose"),
          tr("Off — static pose")});
      _retail_preview_animation->setToolTip(tr(
          "Paused holds the current animation time. Off uses a static pose. "
          "Neither option changes the saved model or its placement."));
      previewForm->addRow(tr("Animation playback"), _retail_preview_animation);
      _retail_preview_decals = new QComboBox(previewTab);
      _retail_preview_decals->addItems({tr("Full"), tr("Reduced texture detail"), tr("Hidden")});
      _retail_preview_decals->setToolTip(tr(
          "Controls identified M2 surface decals, such as stains, cracks and tracks. "
          "Recognition uses projected-texture flags or explicit decal asset names. "
          "Unrecognized details and marks baked into terrain or WMO textures remain visible. "
          "Hiding a decal never removes its placement."));
      previewForm->addRow(tr("Surface decals (identified)"), _retail_preview_decals);
      _retail_preview_description = new QLabel(previewTab);
      _retail_preview_description->setWordWrap(true);
      previewLayout->addWidget(_retail_preview_description);
      connect(_retail_preview_quality, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [this] { update_retail_preview_description(); });
      auto explanation = new QLabel(tr(
          "Use Low or Medium to arrange a scene, then High or Ultra to inspect it. "
          "Model geometry overrides the preset LOD when selected; other preset settings stay active.\n\n"
          "Applies after Save settings. View distance and the number of placed objects are unchanged. "
          "Selection, bounds, collision calculations, UIDs, tile references and saved map data use the original assets. "
          "Exports retain full detail."), previewTab);
      explanation->setWordWrap(true);
      previewLayout->addWidget(explanation);
      _retail_generate_derived_on_save = new QCheckBox(
          tr("Update LOD, map textures and heightmap after saving"), previewTab);
      _retail_generate_derived_on_save->setToolTip(tr(
          "Retail only. Source edits are saved first. When enabled, saved tiles are then processed "
          "by the terrain export queue while scene rendering is paused. When disabled, use "
          "Update project LOD + map textures + heightmap before testing or publishing the map. "
          "Existing generated files are kept until that update succeeds."));
      previewLayout->addWidget(_retail_generate_derived_on_save);
      previewLayout->addStretch();
      ui->tabWidget->addTab(previewTab, tr("Retail"));
      setWindowTitle("Settings");

      // dev QOL, selecting another tab when editing the UI file makes it the default
      ui->tabWidget->setCurrentIndex(0);

      ui->mysql_client_db_checkbox->hide();// Currently unfinished feature, need all DBCs to use the new database library
      ui->mysql_client_db_checkbox->setChecked(false);

      auto titlebar = new QWidget(this);
      setupFramelessWindow(titlebar, this, minimumSize(), maximumSize(), false);
      setMenuWidget(titlebar);

      setWindowFlags(windowFlags() | Qt::Tool | Qt::WindowStaysOnTopHint);

      connect(ui->importPathField, &QLineEdit::textChanged, [&](QString value)
              {
                _settings->setValue("project/import_file", value);
              }
      );


      connect(ui->importPathField_browse, &QPushButton::clicked, [=]
              {
                auto result(QFileDialog::getOpenFileName(
                    nullptr, "Import File Path", ui->importPathField->text()));

                if (!result.isNull())
                {
                  ui->importPathField->setText(result);
                }
              }
      );

      connect(ui->wmvLogPathField, &QLineEdit::textChanged, [&](QString value)
              {
                _settings->setValue("project/import_file", value);
              }
      );


      connect(ui->wmvLogPathField_browse, &QPushButton::clicked, [=]
              {
                auto result(QFileDialog::getOpenFileName(
                    nullptr, "WMV Log Path", ui->wmvLogPathField->text()));

                if (!result.isNull())
                {
                  ui->wmvLogPathField->setText(result);
                }
              }
      );


      ui->MySQL_box->setEnabled(true);
      // ui->MySQL_box->setCheckable(true);

      ui->_theme->addItem("System");

      QDir theme_dir = QDir("./themes/");
      if (theme_dir.exists())
      {
        for (auto dir : theme_dir.entryList(QDir::AllDirs | QDir::NoDotAndDotDot))
        {
          if (QDir(theme_dir.path() + "/" + dir).exists("theme.qss"))
          {
            ui->_theme->addItem(dir);
          }
        }
      }
      else
      {
        LogError
            << "Failed to load themes. The \"themes/\" folder does not exist in Noggit directory. Using system theme."
            << std::endl;
      }

      connect(ui->_theme, &QComboBox::currentTextChanged, [&](QString s)
              {
                if (s == "System")
                {
                  qApp->setStyleSheet("");
                  return;
                }

                auto sstream = std::stringstream();
                sstream << "./themes/" << s.toStdString() << "/theme.qss";

                QFile file(sstream.str().c_str());
                if (file.open(QFile::ReadOnly))
                {
                  QString style_sheet = QLatin1String(file.readAll());
                  QString style_sheet_fixed = style_sheet.replace("@rpath", QCoreApplication::applicationDirPath());

                  if (style_sheet_fixed.endsWith("/"))
                    style_sheet_fixed.chop(1);
                  else if (style_sheet_fixed.endsWith("\\"))
                    style_sheet_fixed.chop(2);

                  qApp->setStyleSheet(style_sheet_fixed);
                }
              }
      );

      connect(ui->_fps_limit_slider, &QSlider::valueChanged, [&](int value)
              {
                  ui->_fps_limit_current_label->setText(
                      QString(tr("FPS limitation, current : %1")).arg(value));
              });

      connect(ui->_adt_unload_dist, qOverload<int>(&QSpinBox::valueChanged)
          , [&](int v)
          {
              QSignalBlocker const blocker(ui->_adt_loading_radius);
              ui->_adt_loading_radius->setMaximum(v - 1);
          });

      connect(ui->_adt_loading_radius, qOverload<int>(&QSpinBox::valueChanged)
          , [&](int v)
          {
              QSignalBlocker const blocker(ui->_adt_unload_dist);
              ui->_adt_unload_dist->setMinimum(v + 1);
          });

      ui->_wireframe_color->setColor(Qt::white);

      connect(ui->saveButton, &QPushButton::clicked, [this]
              {
                hide();
                save_changes();
              }
      );

      connect(ui->discardButton, &QPushButton::clicked, [this]
              {
                hide();
                discard_changes();
              }
      );

      connect(ui->mysql_connect_test, &QPushButton::clicked, [this]
          {
              // test button for noggit db only !

              // save_changes(); // old method saved and only used qsetting

              auto& db_manager = Sql::SqlDatabaseManager::instance();

              if (!db_manager.isDriverAvailable())
              {
                qDebug() << "Available drivers:" << QSqlDatabase::drivers();

                // SPECIAL MISSING/currupted driver DLL POPUP
                QMessageBox prompt;
                prompt.setWindowFlag(Qt::WindowStaysOnTopHint);
                prompt.setIcon(QMessageBox::Critical);
                prompt.setText("mySQL driver could not be loaded.");
                prompt.setWindowTitle("Noggit Database Error");

                std::stringstream promptText;
                promptText << "Make sure <Noggit/sqldrivers/qsqlmysql.dll>  exists.\n";
                promptText << "If not, it can be downloaded at https://github.com/thecodemonkey86/qt_mysql_driver/releases \n";
                promptText << "Noggit Qt version :" << QT_VERSION_STR;

                prompt.setInformativeText("Make sure Noggit/sqldrivers/qsqlmysql.dll exists."
                "If not, it can be downloaded at ");
                prompt.exec();
                return;
              }

              QString host = ui->_mysql_server_field->text();
              QString user = ui->_mysql_user_field->text();
              QString pass = ui->_mysql_pwd_field->text();
              QString db_name = ui->_mysql_db_field->text();
              int port = ui->_mysql_port_field->value();

              // test temporary connection from text fields
              bool test1 = db_manager.initializeDbConnection(Sql::SQLDbType::Noggit, host, port, db_name, user, pass);
              bool test2 = db_manager.testConnectionWithPopup(Sql::SQLDbType::Noggit, false);

              // reconnect to saved db from settings
              QSettings settings;
              host = settings.value("project/mysql/server").toString();
              port = settings.value("project/mysql/port", "3306").toInt();
              db_name = settings.value("project/mysql/db").toString();  // schema name
              user = settings.value("project/mysql/user").toString();
              pass = settings.value("project/mysql/pwd").toString();

              bool test3 = db_manager.initializeDbConnection(Sql::SQLDbType::Noggit, host, port, db_name, user, pass);
              assert(db_manager.testConnection(Sql::SQLDbType::Noggit));

          }
      );

      // load the values in the fields
      discard_changes();

      // ui->_adt_loading_radius->setMaximum(ui->_adt_unload_dist->value() - 1);
      // ui->_adt_unload_dist->setMinimum(ui->_adt_loading_radius->value() + 1);
    }

    void settings::update_retail_preview_description()
    {
      switch (_retail_preview_quality->currentIndex())
      {
        case 0:
          _retail_preview_description->setText(tr(
              "Low: lowest available model LOD, coarser textures, animation updates up to 15 FPS; "
              "particles and ribbons are hidden."));
          break;
        case 1:
          _retail_preview_description->setText(tr(
              "Medium: reduced model LOD where available, moderately coarser textures, "
              "animation updates up to 30 FPS; particles and ribbons are shown."));
          break;
        case 2:
          _retail_preview_description->setText(tr(
              "High: highest model LOD and full texture detail, animation updates up to 60 FPS; "
              "particles and ribbons are shown."));
          break;
        default:
          _retail_preview_description->setText(tr(
              "Ultra: highest model LOD and full texture detail; animations update every frame. "
              "Particles and ribbons are shown."));
          break;
      }
    }

    void settings::showEvent(QShowEvent* event)
    {
      auto app = Noggit::Application::NoggitApplication::instance();
      // The launcher may configure future Retail projects before loading a client.
      bool const available = !app->hasClientData()
          || app->clientData()->version() == BlizzardArchive::ClientVersion::RETAIL;
      ui->tabWidget->setTabVisible(ui->tabWidget->indexOf(_retail_preview_quality->parentWidget()), available);
      // Viewport quick controls write the same keys; spontaneous show events keep unsaved edits.
      if (!event->spontaneous())
        load_retail_preview_settings();
      QMainWindow::showEvent(event);
    }

    void settings::load_retail_preview_settings()
    {
      _retail_preview_quality->setCurrentIndex(qBound(0, _settings->value("retail/model_preview_quality", 3).toInt(), 3));
      _retail_preview_animation->setCurrentIndex(qBound(0, _settings->value("retail/model_preview_animation", 0).toInt(), 2));
      _retail_preview_lod->setCurrentIndex(qBound(0, _settings->value("retail/model_preview_lod", 0).toInt(), 3));
      _retail_preview_decals->setCurrentIndex(qBound(0, _settings->value("retail/model_preview_decals", 0).toInt(), 2));
      _retail_generate_derived_on_save->setChecked(_settings->value("retail/generate_derived_on_save", false).toBool());
      update_retail_preview_description();
    }

    void settings::discard_changes()
    {
      load_retail_preview_settings();
      ui->importPathField->setText(_settings->value("project/import_file", "import.txt").toString());
      ui->wmvLogPathField->setText(_settings->value("project/wmv_log_file").toString());
      ui->_view_distance->setValue(_settings->value("view_distance", 2000.f).toFloat());
      ui->_fov->setValue(_settings->value("fov", 54.f).toFloat());
      ui->_undock_tool_properties->setChecked(
          _settings->value("undock_tool_properties/enabled", true).toBool());
      ui->_undock_small_texture_palette->setChecked(
          _settings->value("undock_small_texture_palette/enabled", true).toBool());
      ui->_vsync_cb->setChecked(_settings->value("vsync", false).toBool());
      ui->_anti_aliasing_cb->setChecked(_settings->value("anti_aliasing", false).toBool());
      ui->_fullscreen_cb->setChecked(_settings->value("fullscreen", false).toBool());
      ui->_background_fps_limit_cb->setChecked(_settings->value("background_fps_limit", true).toBool());
      ui->_directional_light_cb->setChecked(_settings->value("directional_lightning", true).toBool());
      ui->_local_lightning_cb->setChecked(_settings->value("local_lightning", true).toBool());
      ui->_adt_unload_dist->setValue(_settings->value("unload_dist", 5).toInt());
      ui->_adt_unload_check_interval->setValue(_settings->value("unload_interval", 30).toInt());
      ui->_adt_loading_radius->setValue(_settings->value("loading_radius", 2).toInt());
      ui->_uid_cb->setChecked(_settings->value("uid_startup_check", true).toBool());
      ui->_load_fav_cb->setChecked(_settings->value("auto_load_fav_project", true).toBool());
      ui->_systemWindowFrame->setChecked(_settings->value("systemWindowFrame", true).toBool());
      ui->_nativeMenubar->setChecked(_settings->value("nativeMenubar", true).toBool());
      ui->_classic_ui->setChecked(_settings->value("classicUI", false).toBool());
      ui->_additional_file_loading_log->setChecked(
          _settings->value("additional_file_loading_log", false).toBool());
      ui->_keyboard_locale->setCurrentText(_settings->value("keyboard_locale", "QWERTY").toString());
      ui->_use_mclq_liquids_export->setChecked(_settings->value("use_mclq_liquids_export", false).toBool());
      ui->_theme->setCurrentText(_settings->value("theme", "Dark").toString());
      ui->_modern_features->setChecked(_settings->value("modern_features", false).toBool());

      ui->assetBrowserBgCol->setColor(_settings->value("assetBrowser/background_color",
        QVariant::fromValue(QColor(127, 127, 127))).value<QColor>());
      ui->assetBrowserDiffuseLight->setColor(_settings->value("assetBrowser/diffuse_light",
        QVariant::fromValue(QColor::fromRgbF(1.0f, 0.532352924f, 0.0f))).value<QColor>());

      ui->assetBrowserAmbientLight->setColor(_settings->value("assetBrowser/ambient_light",
        QVariant::fromValue(QColor::fromRgbF(0.407770514f, 0.508424163f, 0.602650642f))).value<QColor>());

      ui->assetBrowserCopyToClipboard->setChecked(_settings->value("assetBrowser/copy_to_clipboard", true).toBool());
      ui->assetBrowserDefaultModel->setText(_settings->value("assetBrowser/default_model",
                                     "world/wmo/azeroth/human/buildings/human_farm/farm.wmo").toString());
      ui->assetBrowserMoveSensitivity->setValue(_settings->value("assetBrowser/move_sensitivity", 15.0f).toFloat());
      ui->assetBrowserRenderAssetPreview->setChecked(_settings->value("assetBrowser/render_asset_preview", false).toBool());

      ui->_m2_aabb_2->setChecked(_settings->value("render/m2_aabb", false).toBool());
      ui->_m2_coll_bb->setChecked(_settings->value("render/m2_coll_bb", false).toBool());
      ui->_wmo_aabb->setChecked(_settings->value("render/wmo_aabb", false).toBool());
      ui->_wmo_group_bounds->setChecked(_settings->value("render/wmo_groups_bounds", false).toBool());

      // ui->MySQL_box->setChecked (_settings->value ("project/mysql/enabled").toBool());
      ui->mysql_uid_checkbox->setChecked(_settings->value("project/mysql/enabled").toBool());
      ui->mysql_client_db_checkbox->setChecked(_settings->value("project/mysql/client_db_enabled").toBool());

      auto server_str = _settings->value("project/mysql/server", "127.0.0.1").toString();
      auto user_str = _settings->value("project/mysql/user", "root").toString();
      auto pwd_str = _settings->value("project/mysql/pwd", "root").toString();
      auto db_str = _settings->value("project/mysql/db", "noggit").toString();
      auto port_int = _settings->value("project/mysql/port", "3306").toInt();

      // set some default
      if (server_str.isEmpty())
          server_str = "127.0.0.1";
      if (user_str.isEmpty())
          user_str = "root";
      if (pwd_str.isEmpty())
          pwd_str = "root";
      if (db_str.isEmpty())
          db_str = "noggit";
      if (!port_int)
          port_int = 3306;

      ui->_mysql_server_field->setText (server_str);
      ui->_mysql_user_field->setText(user_str);
      ui->_mysql_pwd_field->setText (pwd_str);
      ui->_mysql_db_field->setText (db_str);
      ui->_mysql_port_field->setValue (port_int);

      bool wireframe_type = _settings->value("wireframe/type", false).toBool();

      if (wireframe_type)
      {
        ui->radio_wire_cursor->setChecked(true);
      }
      else
      {
        ui->radio_wire_full->setChecked(true);
      }

      ui->_wireframe_radius->setValue(_settings->value("wireframe/radius", 1.5f).toFloat());
      ui->_wireframe_width->setValue(_settings->value("wireframe/width", 1.f).toFloat());
      ui->_wireframe_color->setColor(_settings->value("wireframe/color").value<QColor>());
      ui->_fps_limit_slider->setValue(_settings->value("fps_limit", 60).toInt());
    }

    void settings::save_changes()
    {
      if (ui->tabWidget->isTabVisible(ui->tabWidget->indexOf(_retail_preview_quality->parentWidget())))
      {
        _settings->setValue("retail/model_preview_quality", _retail_preview_quality->currentIndex());
        _settings->setValue("retail/model_preview_animation", _retail_preview_animation->currentIndex());
        _settings->setValue("retail/model_preview_lod", _retail_preview_lod->currentIndex());
        _settings->setValue("retail/model_preview_decals", _retail_preview_decals->currentIndex());
        _settings->setValue("retail/generate_derived_on_save", _retail_generate_derived_on_save->isChecked());
      }
      _settings->setValue("project/import_file", ui->importPathField->text());
      _settings->setValue("project/wmv_log_file", ui->wmvLogPathField->text());
      _settings->setValue("view_distance", ui->_view_distance->value());
      _settings->setValue("fov", ui->_fov->value());
      _settings->setValue("undock_tool_properties/enabled", ui->_undock_tool_properties->isChecked());
      _settings->setValue("undock_small_texture_palette/enabled",
                          ui->_undock_small_texture_palette->isChecked());
      _settings->setValue("vsync", ui->_vsync_cb->isChecked());
      _settings->setValue("anti_aliasing", ui->_anti_aliasing_cb->isChecked());
      _settings->setValue("fullscreen", ui->_fullscreen_cb->isChecked());
      _settings->setValue("background_fps_limit", ui->_background_fps_limit_cb->isChecked());
      _settings->setValue("directional_lightning", ui->_directional_light_cb->isChecked());
      _settings->setValue("local_lightning", ui->_local_lightning_cb->isChecked());
      _settings->setValue("unload_dist", ui->_adt_unload_dist->value());
      _settings->setValue("unload_interval", ui->_adt_unload_check_interval->value());
      _settings->setValue("loading_radius", ui->_adt_loading_radius->value());
      _settings->setValue("uid_startup_check", ui->_uid_cb->isChecked());
      _settings->setValue("auto_load_fav_project", ui->_load_fav_cb->isChecked());
      _settings->setValue("additional_file_loading_log", ui->_additional_file_loading_log->isChecked());
      _settings->setValue("keyboard_locale", ui->_keyboard_locale->currentText());
      _settings->setValue("systemWindowFrame", ui->_systemWindowFrame->isChecked());
      _settings->setValue("nativeMenubar", ui->_nativeMenubar->isChecked());
      _settings->setValue("classicUI", ui->_classic_ui->isChecked());
      _settings->setValue("modern_features", ui->_modern_features->isChecked());
      _settings->setValue("use_mclq_liquids_export", ui->_use_mclq_liquids_export->isChecked());

      // _settings->setValue ("project/mysql/enabled", ui->MySQL_box->isChecked());
      _settings->setValue("project/mysql/enabled", ui->mysql_uid_checkbox->isChecked());
      _settings->setValue("project/mysql/client_db_enabled", ui->mysql_client_db_checkbox->isChecked());

      _settings->setValue ("project/mysql/server", ui->_mysql_server_field->text());
      _settings->setValue ("project/mysql/user", ui->_mysql_user_field->text());
      _settings->setValue ("project/mysql/pwd", ui->_mysql_pwd_field->text());
      _settings->setValue ("project/mysql/db", ui->_mysql_db_field->text());
      _settings->setValue ("project/mysql/port", ui->_mysql_port_field->text());

      _settings->setValue("wireframe/type", ui->radio_wire_cursor->isChecked());
      _settings->setValue("wireframe/radius", ui->_wireframe_radius->value());
      _settings->setValue("wireframe/width", ui->_wireframe_width->value());
      _settings->setValue("wireframe/color", ui->_wireframe_color->color());
      _settings->setValue("theme", ui->_theme->currentText());
      _settings->setValue("assetBrowser/background_color", ui->assetBrowserBgCol->color());
      _settings->setValue("assetBrowser/diffuse_light", ui->assetBrowserDiffuseLight->color());
      _settings->setValue("assetBrowser/ambient_light", ui->assetBrowserAmbientLight->color());
      _settings->setValue("assetBrowser/copy_to_clipboard", ui->assetBrowserCopyToClipboard->isChecked());
      _settings->setValue("assetBrowser/default_model", ui->assetBrowserDefaultModel->text());
      _settings->setValue("assetBrowser/move_sensitivity", ui->assetBrowserMoveSensitivity->value());
      _settings->setValue("assetBrowser/render_asset_preview", ui->assetBrowserRenderAssetPreview->isChecked());
      _settings->setValue("fps_limit", ui->_fps_limit_slider->value());

      _settings->setValue("render/m2_aabb", ui->_m2_aabb_2->isChecked());
      _settings->setValue("render/m2_coll_bb", ui->_m2_coll_bb->isChecked());
      _settings->setValue("render/wmo_aabb", ui->_wmo_aabb->isChecked());
      _settings->setValue("render/wmo_groups_bounds", ui->_wmo_group_bounds->isChecked());

      _settings->sync();

      // reinitialize db on save
      auto& db_manager = Sql::SqlDatabaseManager::instance();
      QString host = _settings->value("project/mysql/server").toString();
      int port = _settings->value("project/mysql/port", "3306").toInt();
      QString db_name = _settings->value("project/mysql/db").toString();
      QString user = _settings->value("project/mysql/user").toString();
      QString pass = _settings->value("project/mysql/pwd").toString();

      bool test1 = db_manager.initializeDbConnection(Sql::SQLDbType::Noggit, host, port, db_name, user, pass);
      bool test2 = db_manager.testConnectionWithPopup(Sql::SQLDbType::Noggit, true);


      // calls MapView::onSettingsSave()
      emit saved();
    }
  }
}
