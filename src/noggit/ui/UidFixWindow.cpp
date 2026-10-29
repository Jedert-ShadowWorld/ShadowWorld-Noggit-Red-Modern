// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/UidFixWindow.hpp>
#include <noggit/project/CurrentProject.hpp>

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QComboBox>


namespace Noggit
{
  namespace Ui
  {
    UidFixWindow::UidFixWindow (glm::vec3 pos
                                   , math::degrees camera_pitch
                                   , math::degrees camera_yaw
                                   )
      : QDialog (nullptr)
    {
      setWindowIcon (QIcon (":/icon"));
      auto layout (new QFormLayout (this));


      auto label1 = new QLabel ("<big><p>In order to avoid issues with duplicating/missing "
        "models and possibly with model culling/collision "
        "unique ID of all objects on a map, or to fix that and "
        "also fix all existing models on the map.</p>"
        "<b>This is only required once.</b></big><br><br>"
        "<h2>For unedited blizzard maps</h2>"
        "<p>This is the fastest method but it will only prevent "
        "new issues from occuring and not fix the current ones.</p>"
        , this
      );
      label1->setWordWrap(true);
      layout->addWidget(label1);
      
      auto get_max (new QPushButton("Get max UID", this));
      layout->addWidget(get_max);

      bool const retail = Noggit::Project::CurrentProject::get()->projectVersion
        == Noggit::Project::ProjectVersion::RETAIL;
      auto label2 = new QLabel(retail
        ? "<hr><h2>Review and repair an edited or converted map</h2>"
          "<p>Missing or invalid models are listed in a review window before any file is written. "
          "On confirmation, listed placements are removed; backups and a report are saved. "
          "Project tiles: existing UIDs are kept where possible and only required neighbour "
          "OBJ0/OBJ1 references are updated. Whole map: all UIDs and references are rebuilt.</p>"
        : "<hr><h2>Recommended for edited/custom maps</h2>"
          "<p>Takes more time than the max uid method but "
          "it will fix any model duplication, collision and culling issue "
          "while making sure no models have the same id. "
          "It will fail if any model is missing or could not be loaded "
          "to avoid any collision and culling issue.</p>", this);
      label2->setWordWrap(true);
      layout->addWidget(label2);
      QComboBox* scope = nullptr;
      if (retail)
      {
        scope = new QComboBox(this);
        scope->addItem("Project tiles only (edited areas)");
        scope->addItem("Whole map (fully custom map)");
        scope->setToolTip("Project tiles have ROOT, TEX0, OBJ0 or OBJ1 files in the project. "
          "Only required neighbour OBJ0/OBJ1 files may be added; ROOT/TEX files and unrelated placements are not changed.");
        layout->addRow("Repair scope:", scope);
      }
      auto fix_all = new QPushButton(retail ? "Review and fix UIDs..." : "Fix all UIDs", this);
      layout->addWidget(fix_all);

      if (!retail)
      {
        auto label3 = new QLabel("<hr><font color=red><h2>/!\\ NOT RECOMMENDED</h2>"
          "<h3>USE AT YOUR OWN RISKS</h3></font>"
          "Same as fix all but it will ignore any model loading error "
          "resulting in collision and culling issue. "
          "Any <b>missing or fuckported</b> model will lead to collision and culling issues. "
          "Don't come complaining if you encounter any of those issues afterward."
          , this
        );
        label3->setWordWrap(true);
        layout->addWidget(label3);

        auto fix_all_fuckporting_edition (new QPushButton("Fix all UIDs but potentially fuck up collisions and culling for models", this));
        layout->addWidget(fix_all_fuckporting_edition);
        connect ( fix_all_fuckporting_edition, &QPushButton::clicked
                , [=]
                  {
                    hide();
                    emit fix_uid(pos, camera_pitch, camera_yaw, uid_fix_mode::fix_all_fuckporting_edition);
                    deleteLater();
                  }
                );
      }

      connect ( get_max, &QPushButton::clicked
              , [=]
                {
                  hide();
                  emit fix_uid(pos, camera_pitch, camera_yaw, uid_fix_mode::max_uid);
                  deleteLater();
                }
              );

      connect ( fix_all, &QPushButton::clicked
              , [=]
                {
                  hide();
                  emit fix_uid(pos, camera_pitch, camera_yaw,
                    scope && scope->currentIndex() == 0 ? uid_fix_mode::fix_project_tiles
                      : uid_fix_mode::fix_all_fail_on_model_loading_error);
                  deleteLater();
                }
              );
    }
  }
}
