// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Tool.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/MinimapRenderSettings.hpp>
#include <QOpenGLWidget>
#include <map>

class QProgressBar;
class QPushButton;
class World;
struct MinimapRenderSettings;

namespace Noggit
{
    namespace Ui
    {
        class MinimapCreator;
    }

    class MinimapTool final : public Tool
    {
    public:
        MinimapTool(MapView* mapView);
        ~MinimapTool();

        [[nodiscard]]
        virtual char const* name() const override;

        [[nodiscard]]
        virtual editing_mode editingMode() const override;

        [[nodiscard]]
        virtual Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;
        void registerMenuItems(QMenu* menu) override;

        [[nodiscard]]
        ToolDrawParameters drawParameters() const override;

        void onSelected() override;

        void onTick(float deltaTime, TickParameters const& params) override;

        bool preRender() override;
        bool isExportActive() const { return saving_minimap; }

        void onMouseMove(MouseMoveParameters const& params) override;

        void saveSettings() override;

        // Schedule only the successfully saved set using the existing export queue.
        // Source saves are already complete; failures/cancellation remain pending.
        bool queueSavedTerrain(std::vector<TileIndex> const& savedTiles);

    private:
        Ui::MinimapCreator* _minimapTool = nullptr;

        struct ExportJob { TileIndex tile; MinimapGenMode mode; };
        std::vector<ExportJob> _export_jobs;
        std::vector<TileIndex> _heightmap_tiles;
        MinimapRenderSettings _export_settings;
        QProgressBar* _progress = nullptr;
        QPushButton* _cancel = nullptr;
        unsigned _failed = 0;
        unsigned _blocked = 0;
        std::map<TileIndex, std::string> _terrain_errors;
        bool beginSaving(std::vector<TileIndex> const* savedTiles = nullptr);
        void writeMapTexturePair(TileIndex const& tile);
        bool preRenderLegacy();
        void finishSavingLegacy(QProgressBar* progress, QPushButton* cancel_btn, World* world, MinimapRenderSettings* settings);
        unsigned _mmap_async_index = 0;
        unsigned _mmap_render_index = 0;
        std::optional<QImage> _mmap_combined_image;

        bool saving_minimap = false;
        QOpenGLWidget::UpdateBehavior _previous_update_behavior = QOpenGLWidget::NoPartialUpdate;

        void setExportActive(bool active);

        void finishSaving(QProgressBar* progress, QPushButton* cancel_btn, World* world, MinimapRenderSettings* settings);
    };
}
