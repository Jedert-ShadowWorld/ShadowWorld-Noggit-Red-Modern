// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_MODELRENDER_HPP
#define NOGGIT_MODELRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>
#include <noggit/ModelHeaders.h>
#include <noggit/tool_enums.hpp>
#include <opengl/scoped.hpp>

namespace math
{
  class frustum;
}

namespace OpenGL::Scoped
{
  struct use_program;
}

class Model;
class ModelInstance;

namespace Noggit::Rendering
{
  enum class M2Blend : uint16_t
  {
    Opaque,
    Alpha_Key,
    Alpha,
    No_Add_Alpha,
    Add,
    Mod,
    Mod2x,
    InvSrcAlphaAdd
  };

  enum class ModelPixelShader : uint16_t
  {
    Combiners_Opaque,
    Combiners_Decal,
    Combiners_Add,
    Combiners_Mod2x,
    Combiners_Fade,
    Combiners_Mod,
    Combiners_Opaque_Opaque,
    Combiners_Opaque_Add,
    Combiners_Opaque_Mod2x,
    Combiners_Opaque_Mod2xNA,
    Combiners_Opaque_AddNA,
    Combiners_Opaque_Mod,
    Combiners_Mod_Opaque,
    Combiners_Mod_Add,
    Combiners_Mod_Mod2x,
    Combiners_Mod_Mod2xNA,
    Combiners_Mod_AddNA,
    Combiners_Mod_Mod,
    Combiners_Add_Mod,
    Combiners_Mod2x_Mod2x,
    Combiners_Opaque_Mod2xNA_Alpha,
    Combiners_Opaque_AddAlpha,
    Combiners_Opaque_AddAlpha_Alpha,
  };

  enum class texture_unit_lookup : int
  {
    environment,
    t1,
    t2,
    none
  };


  struct ModelRenderPass : ModelTexUnit
  {
    ModelRenderPass() = delete;
    ModelRenderPass(ModelTexUnit const& tex_unit, Model* m);

    float ordering_thingy = 0.f;
    uint32_t index_start = 0, index_count = 0, vertex_start = 0, vertex_end = 0;
    uint16_t blend_mode = 0;
    texture_unit_lookup tu_lookups[3]{};
    uint16_t textures[3]{};
    uint16_t uv_animations[3]{};
    std::optional<ModelPixelShader> pixel_shader;


    bool surface_decal = false;
    bool layered_geometry = false;
    bool prepareDraw(OpenGL::Scoped::use_program& m2_shader, Model *m, OpenGL::M2RenderState& model_render_state, int preview_decals = 0);
    void afterDraw();
    void bindTexture(size_t index, Model* m, OpenGL::M2RenderState& model_render_state, OpenGL::Scoped::use_program& m2_shader);
    void initUVTypes(Model* m);

    bool operator< (const ModelRenderPass& m) const;
  };

  class ModelRender : public BaseRender
  {
    friend struct ModelRenderPass;

  public:
    ModelRender(Model* model);

    void upload() override;
    void unload() override;

    [[nodiscard]]
    bool uploaded() const { return _uploaded; }

    void draw(glm::mat4x4 const& model_view
        , ModelInstance& instance
        , OpenGL::Scoped::use_program& m2_shader
        , OpenGL::M2RenderState& model_render_state
        , math::frustum const& frustum
        , const float& cull_distance
        , const glm::vec3& camera
        , int animtime
        , display_mode display
        , bool no_cull
        , bool animate
    );

    void draw (glm::mat4x4 const& model_view
        , std::vector<glm::mat4x4> const& instances
        , OpenGL::Scoped::use_program& m2_shader
        , OpenGL::M2RenderState& model_render_state
        , math::frustum const& frustum
        , const float cull_distance
        , glm::vec3 const& camera
        , int animtime
        , bool all_boxes
        , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
        , display_mode display
        , bool no_cull
        , bool animate
        , bool draw_fake_geometry_box
        , bool draw_animation_box
        , int preview_lod = 0
        , bool preview_animation = false
        , int preview_decals = 0
    );

    void drawParticles(glm::mat4x4 const& model_view
        , OpenGL::Scoped::use_program& particles_shader
        , std::vector<ModelInstance*> const& instances
    );

    void drawRibbons(OpenGL::Scoped::use_program& ribbons_shader
        , std::vector<ModelInstance*> const& instances
    );

    void drawBox(OpenGL::Scoped::use_program& m2_box_shader, std::size_t box_count);

    [[nodiscard]]
    std::vector<ModelRenderPass> const& renderPasses() const;;

    void markBoneMatricesDirty() { _bone_matrices_dirty = true; }
    void updateBoneMatrices();

    void initRenderPasses(ModelView const* view, ModelTexUnit const* tex_unit, ModelGeoset const* model_geosets, std::size_t indexCount = 0);
    static void beginPreviewFrame() { _preview_skin_load_budget = 2; }
    void configurePreviewSkins(std::vector<std::uint32_t> const& fileIds, std::size_t viewCount);

  private:

    struct PreviewSkin
    {
      bool attempted = false;
      bool valid = false;
      std::vector<uint16_t> indices;
      std::vector<ModelRenderPass> passes;
    };
    void loadPreviewSkin(int choice);
    void classifyPreviewDecals(std::vector<ModelRenderPass>& passes);
    inline static int _preview_skin_load_budget = 0;
    std::array<std::uint32_t, 2> _preview_skin_ids{};
    std::vector<uint16_t> _primary_geoset_ids;
    std::vector<std::vector<std::size_t>> _shared_geosets;
    std::array<PreviewSkin, 2> _preview_skins;
    OpenGL::Scoped::deferred_upload_buffers<2> _preview_index_buffers;
    int _last_preview_animtime = -1;
    bool _bone_matrices_dirty = false;

    void setupVAO(OpenGL::Scoped::use_program& m2_shader);
    void fixShaderIdBlendOverride();
    void fixShaderIDLayer();
    void computePixelShaderIDs();


    Model* _model;

    // buffers
    OpenGL::Scoped::deferred_upload_buffers<6> _buffers;
    OpenGL::Scoped::deferred_upload_vertex_arrays<2> _vertex_arrays;

    std::vector<uint16_t> const _box_indices = {5, 7, 3, 2, 0, 1, 3, 1, 5, 4, 0, 4, 6, 2, 6, 7};

    GLuint const& _vao = _vertex_arrays[0];
    GLuint const& _transform_buffer = _buffers[0];
    GLuint const& _vertices_buffer = _buffers[1];
    GLuint const& _indices_buffer = _buffers[3];
    GLuint const& _box_indices_buffer = _buffers[4];
    GLuint const& _bone_matrices_buffer = _buffers[5];

    GLuint const& _box_vao = _vertex_arrays[1];
    GLuint const& _box_vbo = _buffers[2];

    GLuint _bone_matrices_buf_tex;
    std::array<glm::vec3, 8> _vertex_box_points;
    std::vector<ModelRenderPass> _render_passes;

    bool _uploaded = false;
    bool _vao_setup = false;
  };
}

#endif //NOGGIT_MODELRENDER_HPP
