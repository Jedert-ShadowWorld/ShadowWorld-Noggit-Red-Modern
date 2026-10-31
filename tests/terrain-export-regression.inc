// Execute the production frame/finish methods with a recording renderer. Qt's
// update behavior is real; this fixture does not claim to exercise a GPU context.
#include <noggit/MinimapRenderSettings.hpp>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <array>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#define LogError std::cerr
#define Log std::cout
void require(bool ok, char const* message) { if (!ok) throw std::runtime_error(message); }
bool retail = true;
bool isRetail() { return retail; }
MinimapRenderSettings mapTextureSettings(MinimapGenMode mode)
{ MinimapRenderSettings settings; settings.export_mode = mode; return settings; }
struct TileIndex
{
    unsigned x = 0, z = 0;
    TileIndex() = default;
    TileIndex(unsigned tx, unsigned tz) : x(tx), z(tz) {}
    bool operator<(TileIndex const& b) const { return x < b.x || (x == b.x && z < b.z); }
};
struct MapTile
{
    bool failed = false;
    void wait_until_loaded() {}
    bool loading_failed() const { return failed; }
};
struct MapIndex
{
    MapTile tile;
    unsigned unloads = 0;
    bool unsaved = false;
    bool hasTile(TileIndex const& index) const { return index.x < 64 && index.z < 64; }
    bool has_unsaved_changes(TileIndex const&) const { return unsaved; }
    bool tileLoaded(TileIndex const&) const { return false; }
    bool tileAwaitingLoading(TileIndex const&) const { return false; }
    MapTile* loadTile(TileIndex const&, bool, bool, bool) { return &tile; }
    void unloadTile(TileIndex const&) { ++unloads; }
    void saveMinimapMD5translate() {}
};
struct Renderer
{
    unsigned renders = 0;
    bool fail = false;
    bool saveMinimap(TileIndex const&, MinimapRenderSettings*, std::optional<QImage>&)
    { ++renders; if (fail) throw std::runtime_error("render failure"); return true; }
};
struct World
{
    std::string basename = "test";
    MapIndex mapIndex;
    Renderer render;
    Renderer* renderer() { return &render; }
    struct Horizon
    {
        unsigned updates = 0, writes = 0;
        void update_horizon_tile(MapTile*) { ++updates; }
        void save_wdl(World*, bool, std::vector<TileIndex>*) { ++writes; }
    } horizon;
};
struct MapView : QOpenGLWidget
{
    World world;
    QMainWindow window;
    unsigned makeCurrentCalls = 0;
    struct Camera { TileIndex position; } camera;
    Camera* getCamera() { return &camera; }
    World* getWorld() { return &world; }
    QMainWindow* mainWindow() { return &window; }
    void setCameraDirty() {}
    void makeCurrent() { ++makeCurrentCalls; }
};
struct RecordingGL
{
    GLint framebuffer = 42;
    std::array<GLint, 4> dimensions{5, 6, 700, 500};
    void getIntegerv(GLenum name, GLint* result)
    {
        if (name == GL_FRAMEBUFFER_BINDING) *result = framebuffer;
        else std::copy(dimensions.begin(), dimensions.end(), result);
    }
    void bindFramebuffer(GLenum, GLuint value) { framebuffer = value; }
    void viewport(GLint x, GLint y, GLint w, GLint h) { dimensions = {x, y, w, h}; }
} gl;
namespace OpenGL::context
{
    struct scoped_setter { scoped_setter(RecordingGL&, QOpenGLContext*) {} };
}
struct Tool
{
    unsigned frames = 0;
    virtual ~Tool() = default;
    virtual bool preRender() { ++frames; return true; }
};
namespace Noggit
{
    namespace Project
    {
        struct CurrentProject
        {
            std::string ProjectPath;
            static CurrentProject* get() { static CurrentProject project; return &project; }
        };
    }
    namespace DerivedMapState
    {
        constexpr unsigned Lod = 1, Wdl = 2, Diffuse = 4, Normal = 8, All = 15;
        unsigned scopeQueries = 0;
        std::vector<TileIndex> marked;
        std::vector<TileIndex> projectTiles(std::string const&, std::string const&)
        { ++scopeQueries; return {{20, 20}}; }
        std::vector<TileIndex> pending(std::string const&, std::string const&, unsigned)
        { ++scopeQueries; return {{21, 21}}; }
        void mark(std::string const&, std::string const&, std::vector<TileIndex> const& tiles, unsigned)
        { marked = tiles; }
        void clear(std::string const&, std::string const&, std::vector<TileIndex> const&, unsigned) {}
        void clear(std::string const&, std::string const&, TileIndex const&, unsigned) {}
    }
    namespace Formats::ADT
    {
        struct ShadowlandsADTCodec
        {
            static inline bool fail = false;
            static void rebuildDerived(std::string const&)
            { if (fail) throw std::runtime_error("LOD failure"); }
        };
    }
    class MinimapTool : public Tool
    {
    public:
        MapView view;
        struct Panel : QWidget
        {
            MinimapRenderSettings settings;
            MinimapRenderSettings* getMinimapRenderSettings() { return &settings; }
        } panel;
        Panel* _minimapTool = &panel;
        struct ExportJob { TileIndex tile; MinimapGenMode mode; };
        std::vector<ExportJob> _export_jobs;
        std::vector<TileIndex> _heightmap_tiles;
        MinimapRenderSettings _export_settings;
        QProgressBar* _progress = nullptr;
        QPushButton* _cancel = nullptr;
        unsigned _failed = 0, _blocked = 0, _mmap_render_index = 0, pairs = 0, legacyFrames = 0;
        std::map<TileIndex, std::string> _terrain_errors;
        std::optional<QImage> _mmap_combined_image;
        bool saving_minimap = false;
        bool isExportActive() const { return saving_minimap; }
        QOpenGLWidget::UpdateBehavior _previous_update_behavior = QOpenGLWidget::NoPartialUpdate;
        MapView* mapView() { return &view; }
        bool preRenderLegacy() { ++legacyFrames; return true; }
        void writeMapTexturePair(TileIndex const&) { ++pairs; }
        void setExportActive(bool);
        bool queueSavedTerrain(std::vector<TileIndex> const&);
        bool beginSaving(std::vector<TileIndex> const* = nullptr);
        bool preRender();
        void finishSaving(QProgressBar*, QPushButton*, World*, MinimapRenderSettings*);
        void start(std::initializer_list<MinimapGenMode> modes)
        {
            _export_settings.export_mode = TERRAIN_DERIVED;
            _export_jobs.clear(); _terrain_errors.clear();
            _failed = _blocked = _mmap_render_index = 0;
            for (auto mode : modes) _export_jobs.push_back({{1, 2}, mode});
            _progress = new QProgressBar(&view);
            _progress->setRange(0, static_cast<int>(modes.size()));
            _cancel = new QPushButton(&view);
            panel.setEnabled(false);
            setExportActive(true);
        }
        void cancel() { finishSaving(_progress, _cancel, &view.world, &_export_settings); }
    };
// ACTUAL_EXPORT_START
// ACTUAL_EXPORT_FRAME
// ACTUAL_EXPORT_FINISH
}

enum class editing_mode { ground, minimap };
struct FrameDispatch
{
    // Non-owning slots supply the same get() interface as MapView's tool list.
    struct Slot { Tool* tool; Tool* get() { return tool; } };
    std::array<Slot, 2> _tools;
    unsigned active = 0;
    Slot& activeTool() { return _tools[active]; }
    bool usesRetailMapWorkflow() { return retail; }
    bool frame()
    {
// ACTUAL_FRAME_DISPATCH
        return frameTool->preRender();
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try
    {
        using Noggit::MinimapTool;
        using Codec = Noggit::Formats::ADT::ShadowlandsADTCodec;
        QTemporaryDir project;
        require(project.isValid(), "temporary project");
        Noggit::Project::CurrentProject::get()->ProjectPath = project.path().toStdString();
        MinimapTool tool;
        require(tool.preRender(), "idle scene renders");
        tool.start({TERRAIN_DERIVED, LOD_MAPTEXTURES_BOTH, WDL_HEIGHTMAP});
        require(tool.view.updateBehavior() == QOpenGLWidget::PartialUpdate, "retain previous scene");
        tool.setExportActive(true); // A repeated start must not replace the restore state.
        require(!tool.preRender(), "terrain frame pauses normal scene");
        require(tool._progress->value() == 1 && tool.view.world.horizon.updates == 1, "terrain job advances");
        require(!tool.preRender() && tool.pairs == 1, "offscreen texture job advances while scene paused");
        require(tool.preRender(), "last job resumes scene");
        require(tool.view.world.horizon.writes == 1 && !tool.saving_minimap && tool.panel.isEnabled(), "success finishes export");
        require(tool.view.updateBehavior() == QOpenGLWidget::NoPartialUpdate, "success restores update behavior");
        require(gl.framebuffer == 42 && gl.dimensions == std::array<GLint,4>{5,6,700,500}, "restore editor framebuffer and viewport");

        Codec::fail = true;
        auto pairs = tool.pairs;
        tool.start({TERRAIN_DERIVED, LOD_MAPTEXTURES_BOTH, WDL_HEIGHTMAP});
        require(!tool.preRender() && tool._failed == 1, "failed terrain keeps queue alive");
        require(!tool.preRender() && tool._blocked == 1 && tool.pairs == pairs, "failed LOD blocks its texture pair");
        require(tool.preRender() && !tool.saving_minimap, "failed export still resumes scene");
        require(tool.view.updateBehavior() == QOpenGLWidget::NoPartialUpdate, "failure restores update behavior");
        Codec::fail = false;

        tool.view.world.render.fail = true;
        tool.start({LOD_MAPTEXTURES});
        require(tool.preRender() && tool._failed == 1 && !tool.saving_minimap, "render exception resumes scene");
        tool.view.world.render.fail = false;

        tool.view.setUpdateBehavior(QOpenGLWidget::PartialUpdate);
        tool.start({TERRAIN_DERIVED, LOD_MAPTEXTURES_BOTH});
        require(!tool.preRender(), "cancel fixture starts active");
        pairs = tool.pairs;
        tool.cancel();
        require(tool.preRender() && tool.pairs == pairs && tool._export_jobs.empty(), "cancel resumes without running remaining jobs");
        require(tool.view.updateBehavior() == QOpenGLWidget::PartialUpdate, "cancel restores original partial update setting");
        tool.view.setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
        tool.start({TERRAIN_DERIVED});
        tool.cancel();
        require(tool.view.updateBehavior() == QOpenGLWidget::NoPartialUpdate, "cancel restores original nonpartial setting");
        Tool otherTool;
        FrameDispatch dispatch{{FrameDispatch::Slot{&otherTool}, FrameDispatch::Slot{&tool}}};
        tool.start({TERRAIN_DERIVED, LOD_MAPTEXTURES_BOTH});
        require(!dispatch.frame() && otherTool.frames == 0, "tool switch does not stop the active export");
        require(dispatch.frame() && !tool.saving_minimap, "export finishes while another tool is selected");
        require(dispatch.frame() && otherTool.frames == 1, "selected tool resumes after export");
        tool.panel.settings.export_mode = LOD_MAPTEXTURES;
        tool.panel.settings.map_texture_scope = MapTextureScope::Project;
        require(!tool.queueSavedTerrain({}), "empty save does not start an export");
        require(tool.queueSavedTerrain({{3, 4}, {8, 9}}), "saved tiles start queued generation");
        require(tool._export_jobs.size() == 5, "two saved tiles produce two pairs and one WDL job");
        require(tool._export_jobs[0].tile.x == 3 && tool._export_jobs[0].tile.z == 4
            && tool._export_jobs[2].tile.x == 8 && tool._export_jobs[2].tile.z == 9,
            "automatic export uses the exact saved tile set");
        require(Noggit::DerivedMapState::scopeQueries == 0
            && Noggit::DerivedMapState::marked.size() == 2, "automatic export never expands to project or pending tiles");
        require(tool.panel.settings.export_mode == LOD_MAPTEXTURES
            && tool.panel.settings.map_texture_scope == MapTextureScope::Project,
            "automatic export preserves manual panel settings");
        require(!tool.queueSavedTerrain({{10, 11}}) && tool._export_jobs.size() == 5,
            "an active export cannot be replaced by another save");
        tool.cancel();
        retail = false;
        require(!tool.queueSavedTerrain({{3, 4}}), "non-Retail save never starts the Retail queue");
        tool.start({TERRAIN_DERIVED});
        require(dispatch.frame() && otherTool.frames == 2 && tool._mmap_render_index == 0, "legacy dispatch does not redirect to minimap");
        tool.cancel();
        require(tool.preRender() && tool.legacyFrames == 1, "legacy path is unchanged");
        std::cout << "Terrain export lifecycle checks passed.\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
