// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "MinimapTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/DerivedMapState.hpp>
#include <noggit/formats/adt/ShadowlandsADTCodec.hpp>
#include <noggit/MapTile.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/MinimapCreator.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/World.h>

#include <QDir>
#include <QFileDialog>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QMessageBox>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace
{
    bool isRetail()
    {
        return Noggit::Application::NoggitApplication::instance()->clientData()->version()
            == BlizzardArchive::ClientVersion::RETAIL;
    }

    MinimapRenderSettings mapTextureSettings(MinimapGenMode mode)
    {
        MinimapRenderSettings settings;
        settings.export_mode = mode;
        settings.draw_m2 = settings.draw_wmo = settings.draw_water = false;
        settings.draw_adt_grid = settings.draw_elevation = settings.draw_shadows = false;
        settings.use_filters = settings.combined_minimap = false;
        settings.generate_mipmaps = true;
        settings.draw_only_normals = mode == LOD_MAPTEXTURES_N;
        settings.point_normals_up = !settings.draw_only_normals;
        settings.resolution = settings.draw_only_normals ? 256 : 512;
        settings.file_format = settings.draw_only_normals ? ".blp (DXT1)" : ".blp (DXT5)";
        return settings;
    }
}

namespace Noggit
{
    MinimapTool::MinimapTool(MapView* mapView)
        : Tool{ mapView }
    {
    }

    MinimapTool::~MinimapTool()
    {
        if (saving_minimap)
        {
            delete _progress;
            delete _cancel;
        }
        delete _minimapTool;
    }

    char const* MinimapTool::name() const
    {
        return "Minimap Editor";
    }

    editing_mode MinimapTool::editingMode() const
    {
        return editing_mode::minimap;
    }

    Ui::FontNoggit::Icons MinimapTool::icon() const
    {
        return Ui::FontNoggit::TOOL_MINIMAP_EDITOR;
    }

    void MinimapTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        auto mv = mapView();
        _minimapTool = new Noggit::Ui::MinimapCreator(mv, mv->getWorld(), mv);
        toolPanel->registerTool(this, _minimapTool);

        QObject::connect(_minimapTool, &Ui::MinimapCreator::onSave, [=] {
            beginSaving();
            });
    }

    void MinimapTool::registerMenuItems(QMenu* menu)
    {
        if (!isRetail()) return;
        auto* action = menu->addAction("Update project LOD + map textures + heightmap...");
        action->setToolTip("Rebuild derived data for all saved project tiles of the open map.");
        QObject::connect(action, &QAction::triggered, _minimapTool, [this]() {
            if (saving_minimap) return;
            mapView()->set_editing_mode(editing_mode::minimap);
            auto* settings = _minimapTool->getMinimapRenderSettings();
            settings->export_mode = TERRAIN_DERIVED;
            settings->map_texture_scope = MapTextureScope::Project;
            beginSaving();
        });
        auto* reference = menu->addAction("Choose LOD reference folder...");
        reference->setToolTip("Choose original ROOT and LOD files for the open map. They are read only.");
        QObject::connect(reference, &QAction::triggered, _minimapTool, [this]() {
            if (saving_minimap) return;
            auto const& map = mapView()->getWorld()->basename;
            auto const root = "world/maps/" + map + "/" + map + "_0_0.adt";
            try
            {
                using Codec = Formats::ADT::ShadowlandsADTCodec;
                QString initialDirectory;
                try
                {
                    initialDirectory = QString::fromStdString(Codec::lodReferenceDirectory(root));
                }
                catch (std::exception const& error)
                {
                    // A bad saved setting must not prevent choosing its replacement.
                    LogError << "[LOD reference] Could not read the saved folder: " << error.what() << std::endl;
                }
                auto const directory = QFileDialog::getExistingDirectory(mapView(), "Original ROOT and LOD files",
                    initialDirectory);
                if (directory.isEmpty()) return;
                Codec::setLodReferenceDirectory(root, directory.toStdString());
                mapView()->mainWindow()->statusBar()->showMessage("LOD reference folder saved for this map.", 5000);
            }
            catch (std::exception const& error)
            {
                QMessageBox::warning(mapView(), "LOD reference folder", error.what());
            }
        });
    }

    ToolDrawParameters MinimapTool::drawParameters() const
    {
        return
        {
            .radius = _minimapTool->brushRadius(),
            .minimapRenderSettings = *_minimapTool->getMinimapRenderSettings(),
        };
    }

    void MinimapTool::onSelected()
    {
        mapView()->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_selection_overlay = true;
        mapView()->getMinimapWidget()->use_selection(_minimapTool->getSelectedTiles());
    }

    void MinimapTool::onTick(float deltaTime, TickParameters const& params)
    {
        if (!mapView()->getWorld()->has_selection() || !params.left_mouse)
        {
            return;
        }
    }

    void MinimapTool::beginSaving()
    {
        if (saving_minimap)
            return;
        if (!isRetail())
        {
            saving_minimap = true;
            return;
        }
        auto world = mapView()->getWorld();
        _export_settings = *_minimapTool->getMinimapRenderSettings();
        _export_settings.draw_only_normals = false;
        _export_settings.point_normals_up = false;
        _export_jobs.clear();
        _heightmap_tiles.clear();
        _mmap_render_index = 0;
        _failed = 0;
        _blocked = 0;
        _terrain_errors.clear();
        const auto mode = _export_settings.export_mode;
        const bool derived = mode == TERRAIN_DERIVED;
        const bool paired = mode == LOD_MAPTEXTURES_BOTH || derived;
        const bool textures = paired || mode == LOD_MAPTEXTURES || mode == LOD_MAPTEXTURES_N;
        std::vector<TileIndex> tiles;
        try
        {
            if (paired && _export_settings.map_texture_scope == MapTextureScope::Project)
                tiles = DerivedMapState::projectTiles(Project::CurrentProject::get()->ProjectPath, world->basename);
            else if (paired && _export_settings.map_texture_scope == MapTextureScope::Stale)
            {
                for (unsigned x = 0; x < 64; ++x)
                    for (unsigned z = 0; z < 64; ++z)
                        if (world->mapIndex.has_unsaved_changes(TileIndex(x, z)))
                        {
                            QMessageBox::information(mapView(), "Map textures",
                                "Save changed ADTs first.");
                            return;
                        }
                tiles = DerivedMapState::pending(Project::CurrentProject::get()->ProjectPath, world->basename,
                    derived ? DerivedMapState::All : DerivedMapState::Diffuse | DerivedMapState::Normal);
            }
            else if (mode == CURRENT_ADT)
                tiles.emplace_back(mapView()->getCamera()->position);
            else
                for (unsigned x = 0; x < 64; ++x)
                    for (unsigned z = 0; z < 64; ++z)
                        if ((mode != SELECTED_ADTS && !(paired && _export_settings.map_texture_scope == MapTextureScope::Selected))
                            || _export_settings.selected_tiles.at(64 * x + z))
                            tiles.emplace_back(x, z);

            for (const auto& tile : tiles)
            {
                if (!world->mapIndex.hasTile(tile))
                    continue;
                if (textures && world->mapIndex.has_unsaved_changes(tile))
                {
                    QMessageBox::information(mapView(), "Map textures", "Save the affected ADTs before generating map textures.");
                    return;
                }
                if (paired)
                {
                    if (derived) _export_jobs.push_back({tile, TERRAIN_DERIVED});
                    _export_jobs.push_back({tile, LOD_MAPTEXTURES_BOTH});
                }
                else
                    _export_jobs.push_back({tile, mode});
            }
            if (derived && !_export_jobs.empty()
                && QMessageBox::question(mapView(), "Update terrain results",
                    QString("Rebuild LOD, map textures and WDL heights for %1 saved tiles of %2?\n"
                            "ROOT/TEX0/OBJ0 are not rewritten; OBJ1 water heights may be updated.")
                        .arg(_export_jobs.size() / 2).arg(QString::fromStdString(world->basename)),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            if (textures)
            {
                std::vector<TileIndex> targets;
                for (auto const& tile : tiles)
                    if (world->mapIndex.hasTile(tile)) targets.push_back(tile);
                unsigned flags = derived ? DerivedMapState::All : paired
                    ? DerivedMapState::Diffuse | DerivedMapState::Normal
                    : mode == LOD_MAPTEXTURES_N ? DerivedMapState::Normal : DerivedMapState::Diffuse;
                DerivedMapState::mark(Project::CurrentProject::get()->ProjectPath, world->basename, targets, flags);
            }
            if (derived && !_export_jobs.empty())
                _export_jobs.push_back({_export_jobs.front().tile, WDL_HEIGHTMAP});
        }
        catch (const std::exception& error)
        {
            QMessageBox::warning(mapView(), "Map textures", error.what());
            return;
        }
        if (_export_jobs.empty())
        {
            mapView()->mainWindow()->statusBar()->showMessage("No tiles to export.", 5000);
            return;
        }
        if (textures)
            _export_settings.combined_minimap = false;
        if (_export_settings.combined_minimap)
        {
            _mmap_combined_image.emplace(8192, 8192, QImage::Format_RGBA8888);
            _mmap_combined_image->fill(Qt::black);
        }
        _progress = new QProgressBar();
        _progress->setRange(0, static_cast<int>(_export_jobs.size()));
        _cancel = new QPushButton("Cancel");
        mapView()->mainWindow()->statusBar()->addPermanentWidget(_progress);
        mapView()->mainWindow()->statusBar()->addPermanentWidget(_cancel);
        QObject::connect(_cancel, &QPushButton::clicked, _cancel, [this]() {
            _export_settings.combined_minimap = false;
            _mmap_combined_image.reset();
            finishSaving(_progress, _cancel, mapView()->getWorld(), &_export_settings);
            mapView()->mainWindow()->statusBar()->showMessage("Export cancelled. Unfinished terrain results remain pending.", 10000);
        });
        _minimapTool->setEnabled(false);
        setExportActive(true);
    }

    void MinimapTool::setExportActive(bool active)
    {
        if (saving_minimap == active)
            return;
        if (active)
        {
            _previous_update_behavior = mapView()->updateBehavior();
            // Keep the editor image while only the offscreen export renders.
            mapView()->setUpdateBehavior(QOpenGLWidget::PartialUpdate);
        }
        else
        {
            mapView()->setUpdateBehavior(_previous_update_behavior);
            mapView()->update();
        }
        saving_minimap = active;
    }

    bool MinimapTool::preRender()
    {
        if (!isRetail())
            return preRenderLegacy();
        if (!saving_minimap)
            return true;
        auto mv = mapView();
        auto world = mv->getWorld();
        mv->setCameraDirty();
        OpenGL::context::scoped_setter const context(::gl, mv->context());
        mv->makeCurrent();
        GLint framebuffer = 0;
        std::array<GLint, 4> viewport{};
        gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        gl.getIntegerv(GL_VIEWPORT, viewport.data());
        const auto job = _export_jobs.at(_mmap_render_index);
        auto settings = _export_settings;
        settings.export_mode = job.mode;
        settings.draw_only_normals = false;
        settings.point_normals_up = false;
        const bool pair = job.mode == LOD_MAPTEXTURES_BOTH;
        const bool textures = pair || job.mode == LOD_MAPTEXTURES || job.mode == LOD_MAPTEXTURES_N;
        if (textures)
        {
            settings = mapTextureSettings(job.mode);
        }
        auto const terrain_error = _terrain_errors.find(job.tile);
        bool const blocked = pair && terrain_error != _terrain_errors.end();
        if (blocked)
        {
            ++_blocked;
            LogError << "Map textures skipped for " << job.tile.x << "_" << job.tile.z
                << ": LOD rebuild failed: " << terrain_error->second
                << "; existing map textures were kept." << std::endl;
        }
        else try
        {
            if ((textures || job.mode == TERRAIN_DERIVED) && world->mapIndex.has_unsaved_changes(job.tile))
                throw std::runtime_error("Tile changed during export; save and regenerate.");
            if (job.mode == TERRAIN_DERIVED)
            {
                auto const root = "world/maps/" + world->basename + "/" + world->basename + "_"
                    + std::to_string(job.tile.x) + "_" + std::to_string(job.tile.z) + ".adt";
                Formats::ADT::ShadowlandsADTCodec::rebuildDerived(root);
                bool const unload = !world->mapIndex.tileLoaded(job.tile) && !world->mapIndex.tileAwaitingLoading(job.tile);
                auto* tile = world->mapIndex.loadTile(job.tile, false, false, false);
                if (!tile) throw std::runtime_error("Cannot load saved terrain heights.");
                try
                {
                    tile->wait_until_loaded();
                    if (tile->loading_failed())
                        throw std::runtime_error("Saved terrain failed to load; heights were not updated.");
                    world->horizon.update_horizon_tile(tile);
                    _heightmap_tiles.push_back(job.tile);
                }
                catch (...)
                {
                    if (unload) world->mapIndex.unloadTile(job.tile);
                    throw;
                }
                if (unload) world->mapIndex.unloadTile(job.tile);
            }
            else if (job.mode == WDL_HEIGHTMAP)
            {
                for (auto const& tile : _heightmap_tiles)
                    if (world->mapIndex.has_unsaved_changes(tile))
                        throw std::runtime_error("Tile changed during heightmap export; save and regenerate.");
                if (!_heightmap_tiles.empty()) world->horizon.save_wdl(world, false, &_heightmap_tiles);
                DerivedMapState::clear(Project::CurrentProject::get()->ProjectPath, world->basename,
                    _heightmap_tiles, DerivedMapState::Lod | DerivedMapState::Wdl);
            }
            else if (pair)
                writeMapTexturePair(job.tile);
            else if (!world->renderer()->saveMinimap(job.tile, &settings, _mmap_combined_image))
                throw std::runtime_error("Rendering or writing the texture failed.");
            if (textures && !pair)
                DerivedMapState::clear(Project::CurrentProject::get()->ProjectPath, world->basename, job.tile,
                    job.mode == LOD_MAPTEXTURES_N ? DerivedMapState::Normal : DerivedMapState::Diffuse);
        }
        catch (const std::exception& error)
        {
            ++_failed;
            if (job.mode == TERRAIN_DERIVED)
                _terrain_errors[job.tile] = error.what();
            LogError << "Terrain export " << job.tile.x << "_" << job.tile.z << ": " << error.what() << std::endl;
        }
        _progress->setValue(++_mmap_render_index);
        if (_mmap_render_index == _export_jobs.size())
        {
            const auto message = QString("Terrain export finished: %1 completed, %2 failed, %3 skipped after LOD failure. Unfinished results remain pending; see log.txt for tile details.")
                .arg(_mmap_render_index - _failed - _blocked).arg(_failed).arg(_blocked);
            finishSaving(_progress, _cancel, world, &_export_settings);
            mv->mainWindow()->statusBar()->showMessage(message, 15000);
        }
        // Status widget updates can switch the current GL context; the editor
        // framebuffer is not FBO 0.
        mv->makeCurrent();
        gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        gl.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        // Returning false skips the normal scene, but paintGL is still called
        // by the frame timer to advance the next export job.
        return !saving_minimap;
    }

    void MinimapTool::writeMapTexturePair(TileIndex const& tile)
    {
        auto* world = mapView()->getWorld();
        std::optional<QImage> unusedAtlas;
        auto write = [&](MinimapGenMode mode, std::string const& outputPath) {
            auto settings = mapTextureSettings(mode);
            if (!world->renderer()->saveMinimap(tile, &settings, unusedAtlas, QString::fromStdString(outputPath)))
                throw std::runtime_error(mode == LOD_MAPTEXTURES
                    ? "Failed to render or write the diffuse map texture."
                    : "Failed to render or write the normal map texture.");
        };
        DerivedMapState::writeTexturePair(Project::CurrentProject::get()->ProjectPath,
            world->basename, tile,
            [&](std::string const& outputPath) { write(LOD_MAPTEXTURES, outputPath); },
            [&](std::string const& outputPath) { write(LOD_MAPTEXTURES_N, outputPath); });
    }

    QStringList MinimapTool::saveRetailMapTextures(std::vector<TileIndex> const& savedTiles)
    {
        QStringList errors;
        if (!isRetail())
            return errors;
        auto mv = mapView();
        auto world = mv->getWorld();
        auto const& projectPath = Project::CurrentProject::get()->ProjectPath;
        constexpr unsigned flags = DerivedMapState::Diffuse | DerivedMapState::Normal;
        std::vector<TileIndex> tiles;
        try
        {
            std::vector<TileIndex> targets;
            for (auto const& tile : savedTiles)
                if (world->mapIndex.hasTile(tile)) targets.push_back(tile);
            DerivedMapState::mark(projectPath, world->basename, targets, flags);
            if (saving_minimap)
                throw std::runtime_error("An export is already running; map textures remain pending.");
            // Only the saved tiles; other pending tiles are processed by the Minimap Editor.
            auto const pendingLod = DerivedMapState::pending(projectPath, world->basename, DerivedMapState::Lod);
            for (auto const& tile : targets)
            {
                if (std::find(pendingLod.begin(), pendingLod.end(), tile) != pendingLod.end())
                {
                    auto const message = QString("LOD remains pending for tile %1_%2; existing map textures were retained. Rebuild LOD before retrying the save.")
                        .arg(tile.x).arg(tile.z);
                    errors.append(message);
                    LogError << "Map texture save " << message.toStdString() << std::endl;
                }
                else tiles.push_back(tile);
            }
        }
        catch (std::exception const& error)
        {
            errors.append(QString::fromUtf8(error.what()));
            return errors;
        }
        if (tiles.empty())
            return errors;

        mv->makeCurrent();
        OpenGL::context::scoped_setter const context(::gl, mv->context());
        std::array<GLint, 4> viewport{};
        gl.getIntegerv(GL_VIEWPORT, viewport.data());
        GLint framebuffer = 0;
        gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        for (auto const& tile : tiles)
        {
            try
            {
                if (!world->mapIndex.hasTile(tile))
                    throw std::runtime_error("The pending tile is absent from the map.");
                if (world->mapIndex.has_unsaved_changes(tile))
                    throw std::runtime_error("The tile has unsaved ADT changes.");
                writeMapTexturePair(tile);
            }
            catch (std::exception const& error)
            {
                auto const message = QString("%1_%2: %3").arg(tile.x).arg(tile.z).arg(QString::fromUtf8(error.what()));
                errors.append(message);
                LogError << "Map texture save " << message.toStdString() << std::endl;
            }
        }
        gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        gl.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        mv->setCameraDirty();
        return errors;
    }

    bool MinimapTool::preRenderLegacy()
    {
        if (!saving_minimap)
        {
            return true;
        }

        auto mv = mapView();
        auto world = mv->getWorld();
        auto settings = _minimapTool->getMinimapRenderSettings();

        mv->setCameraDirty();

        OpenGL::context::scoped_setter const _(::gl, mv->context());
        mv->makeCurrent();

        bool mmap_render_success = false;

        static QProgressBar* progress;
        static QPushButton* cancel_btn;

        auto init = [=](int max) {
            progress = new QProgressBar(nullptr);
            progress->setMinimum(0);
            progress->setMaximum(max);
            mv->mainWindow()->statusBar()->addPermanentWidget(progress);

            cancel_btn = new QPushButton(nullptr);
            cancel_btn->setText("Cancel");

            QObject::connect(cancel_btn, &QPushButton::clicked,
                [=]
                {
                    _mmap_async_index = 0;
                    _mmap_render_index = 0;
                    saving_minimap = false;
                    progress->deleteLater();
                    cancel_btn->deleteLater();
                    _mmap_combined_image.reset();
                });

            mv->mainWindow()->statusBar()->addPermanentWidget(cancel_btn);

            QObject::connect(mv, &MapView::updateProgress,
                [=](int value)
                {
                    // This weirdness is required due to a bug on Linux when QT repaint crashes due to too many events
                    // being passed through. TODO: this potentially only masks the issue, which may reappear on faster
                    // hardware.
                    if (progress->value() != value)
                        progress->setValue(value);
                });

            // setup combined image if necessary
            if (settings->combined_minimap)
            {
                _mmap_combined_image.emplace(8192, 8192, QImage::Format_RGBA8888);
                _mmap_combined_image->fill(Qt::black);
            }
            };

        auto save = [=, &mmap_render_success]()
            {
                while (!world->mapIndex.hasTile({ _mmap_async_index / 64, _mmap_async_index % 64 }))
                {
                    ++_mmap_async_index;
                }

                TileIndex tile = TileIndex(_mmap_async_index / 64, _mmap_async_index % 64);

                if (world->mapIndex.hasTile(tile))
                {
                    OpenGL::context::scoped_setter const _(::gl, mv->context());
                    mv->makeCurrent();
                    mmap_render_success = world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);

                    _mmap_render_index++;
                    emit mv->updateProgress(_mmap_render_index);

                    if (!mmap_render_success)
                    {
                        LogError << "Minimap rendered incorrectly for tile: " << tile.x << "_" << tile.z << std::endl;
                    }
                }
            };


        switch (settings->export_mode)
        {
        case MinimapGenMode::CURRENT_ADT:
        {
            TileIndex tile = TileIndex(mv->getCamera()->position);

            if (world->mapIndex.hasTile(tile))
            {
                mmap_render_success = world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);
            }

            if (mmap_render_success)
            {
                world->mapIndex.saveMinimapMD5translate();
            }

            saving_minimap = false;

            break;
        }
        case MinimapGenMode::MAP:
        case MinimapGenMode::LOD_MAPTEXTURES:
        case MinimapGenMode::LOD_MAPTEXTURES_N:
        {
            // init progress
            if (!_mmap_async_index)
            {
                init(world->mapIndex.getNumExistingTiles());
            }

            if (!saving_minimap)
                return false;

            bool modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;
            if (modern_features && (settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES || settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES_N))
            {
                settings->draw_m2 = false;
                settings->draw_wmo = false;
                settings->draw_water = false;
                settings->resolution = 512;
                settings->file_format = ".blp (DXT5)";

                if (settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES_N)
                {
                    settings->draw_only_normals = true;
                    settings->resolution = 256;
                }
                else if (settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES) {
                    // Point normals upwards for diffuse maptexture baking
                    settings->point_normals_up = true;
                }
            }

            if (_mmap_async_index < 4096 && static_cast<int>(_mmap_render_index) < progress->maximum())
            {
                save();
                _mmap_async_index++;
            }
            else
            {
                finishSavingLegacy(progress, cancel_btn, world, settings);
            }

            //_main_window->statusBar()->showMessage("Minimap rendering done.", 2000);
            break;
        }
        case MinimapGenMode::SELECTED_ADTS:
        {
            auto selected_tiles = _minimapTool->getSelectedTiles();

            // init progress
            if (!_mmap_async_index)
            {
                int n_selected_tiles = 0;

                for (int i = 0; i < 4096; ++i)
                {
                    if (selected_tiles->at(i))
                        n_selected_tiles++;
                }

                init(n_selected_tiles);
            }

            if (!saving_minimap)
                return false;

            if (_mmap_async_index < 4096 && static_cast<int>(_mmap_render_index) < progress->maximum())
            {
                if (selected_tiles->at(_mmap_async_index))
                {
                    save();
                }
                _mmap_async_index++;
            }
            else
            {
                finishSavingLegacy(progress, cancel_btn, world, settings);
            }

            break;
        }
        }

        //minimapTool->progressUpdate(0);
        return !saving_minimap;
    }

    void MinimapTool::finishSavingLegacy(QProgressBar* progress, QPushButton* cancel_btn, World* world, MinimapRenderSettings* settings)
    {
        _mmap_async_index = 0;
        _mmap_render_index = 0;
        saving_minimap = false;
        progress->deleteLater();
        cancel_btn->deleteLater();
        world->mapIndex.saveMinimapMD5translate();

        // save combined minimap
        if (settings->combined_minimap)
        {
            QString image_path = QString(std::string(world->basename + "_combined_minimap.png").c_str());
            QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
            if (!(str.endsWith('\\') || str.endsWith('/')))
            {
                str += "/";
            }

            QDir dir(str + "/textures/minimap/");
            if (!dir.exists())
                dir.mkpath(".");

            _mmap_combined_image->save(dir.filePath(image_path));
            _mmap_combined_image.reset();
        }
    }

    void MinimapTool::onMouseMove(MouseMoveParameters const& params)
    {
        if (params.left_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                _minimapTool->changeRadius(params.relative_movement.dx() / XSENS);
            }

            if (params.mod_shift_down || params.mod_ctrl_down)
            {
                mapView()->doSelection(false, true);
            }
        }
    }

    void MinimapTool::finishSaving(QProgressBar* progress, QPushButton* cancel_btn, World* world, MinimapRenderSettings* settings)
    {
        _export_jobs.clear();
        _heightmap_tiles.clear();
        _minimapTool->setEnabled(true);
        _mmap_render_index = 0;
        setExportActive(false);
        progress->deleteLater();
        cancel_btn->deleteLater();
        _progress = nullptr;
        _cancel = nullptr;
        if (settings->export_mode != LOD_MAPTEXTURES && settings->export_mode != LOD_MAPTEXTURES_N
            && settings->export_mode != LOD_MAPTEXTURES_BOTH && settings->export_mode != TERRAIN_DERIVED)
            world->mapIndex.saveMinimapMD5translate();

        // save combined minimap
        if (settings->combined_minimap)
        {
            QString image_path = QString(std::string(world->basename + "_combined_minimap.png").c_str());
            QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
            if (!(str.endsWith('\\') || str.endsWith('/')))
            {
                str += "/";
            }

            QDir dir(str + "/textures/minimap/");
            if (!dir.exists())
                dir.mkpath(".");

            _mmap_combined_image->save(dir.filePath(image_path));
            _mmap_combined_image.reset();
        }
    }

    void MinimapTool::saveSettings()
    {
        // Save minimap creator model filters
        _minimapTool->saveFiltersToJSON();
    }
}
