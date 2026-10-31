// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QMainWindow>

namespace Ui
{
  class SettingsPanel;
}

class QSettings;
class QComboBox;
class QCheckBox;
class QLabel;
class QShowEvent;

namespace Noggit
{
  namespace Ui
  {
    class settings : public QMainWindow
    {
      Q_OBJECT
      QSettings* _settings;
      ::Ui::SettingsPanel* ui;
      QComboBox* _retail_preview_quality;
      QComboBox* _retail_preview_animation;
      QComboBox* _retail_preview_lod;
      QComboBox* _retail_preview_decals;
      QCheckBox* _retail_generate_derived_on_save;
      QLabel* _retail_preview_description;
      void update_retail_preview_description();
    public:
      settings(QWidget* parent = nullptr);
      void discard_changes();
      void load_retail_preview_settings();
      void save_changes();

    protected:
      void showEvent(QShowEvent* event) override;

    signals:
      void saved();
    };
  }
}
