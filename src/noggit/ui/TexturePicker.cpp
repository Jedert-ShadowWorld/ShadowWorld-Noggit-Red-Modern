// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TexturePicker.h>

#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Selection.h>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/ui/tools/UiCommon/expanderwidget.h>

#include <QtWidgets/QGridLayout>
#include <QtWidgets/QPushButton>

#include <algorithm>
#include <cassert>
#include <numeric>

namespace Noggit
{
  namespace Ui
  {
    namespace
    {
      constexpr int texture_picker_columns = 4;
      constexpr int texture_picker_slots = MAX_TEXTURE_LAYERS;
    }

    texture_picker::texture_picker
        (current_texture* current_texture_window, QWidget* parent)
      : widget (parent, Qt::Window)
      , layout (new ::QGridLayout(this))
      , _chunk (nullptr)
      , _main_texture_window(current_texture_window)
    {
      setWindowTitle ("Texture Picker");
      setWindowFlags (Qt::Tool | Qt::WindowStaysOnTopHint);

      auto* AlphamapsBox = new ExpanderWidget(this);
      AlphamapsBox->setExpanderTitle("View Chunk Alphamaps");
      AlphamapsBox->setExpanded(false);
      auto AlphamapsBox_content = new QWidget(this);
      AlphamapsBox_content->setLayoutDirection(Qt::LeftToRight);
      auto Alphamaps_layout = new QGridLayout(AlphamapsBox_content);
      AlphamapsBox->setLayoutDirection(Qt::LeftToRight);
      AlphamapsBox->addPage(AlphamapsBox_content);

      connect(AlphamapsBox, &ExpanderWidget::expanderChanged, [&](bool flag)
          {
              adjustSize();
          });

      for (int i = 0; i < texture_picker_slots; i++)
      {
        current_texture* click_label = new current_texture(false, this);
        connect ( click_label, &ClickableLabel::leftClicked
                , [=]()
                  {
                    if (click_label->_is_swap_selected)
                        return;

                    setTexture(i, current_texture_window);
                  }
                );

        connect(click_label, &ClickableLabel::rightClicked, [=]()
                {
                    if (click_label->_is_selected)
                        return;

                    if (click_label->_is_swap_selected)
                    {
                        click_label->_is_swap_selected = false;
                        click_label->unselectSwap();
                        return;
                    }

                    for (unsigned long long i = 0; i < _labels.size(); ++i)
                    {
                        _labels[i]->unselectSwap();

                        if (_labels[i] == click_label && !_labels[i]->_is_selected)
                        {
                            _labels[i]->selectSwap();
                        }
                    }
                });

        if (click_label->filename() == current_texture_window->filename())
            click_label->select();

        layout->addWidget(click_label, i / texture_picker_columns, i % texture_picker_columns);
        _labels.push_back(click_label);

        QLabel* alphamap_label = new QLabel(this);
        alphamap_label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        alphamap_label->setMinimumSize(128, 128);
        // alphamap_label->hide();

        Alphamaps_layout->addWidget(alphamap_label, i / texture_picker_columns, i % texture_picker_columns);
        _alphamap_preview_labels.push_back(alphamap_label);
      }

      QPushButton* btn_left = new QPushButton (this);
      QPushButton* btn_right = new QPushButton (this);
      QPushButton* btn_add = new QPushButton (this);
      btn_left->setIcon(FontAwesomeIcon(FontAwesome::angledoubleleft));
      btn_right->setIcon(FontAwesomeIcon(FontAwesome::angledoubleright));
      btn_add->setIcon(FontAwesomeIcon(FontAwesome::plus));
      btn_add->setToolTip("Add current texture to chunk");

      // QPushButton* btn_hide_alphamaps = new QPushButton(this);
      // btn_hide_alphamaps->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::VISIBILITY_HIDDEN_MODELS));
      // btn_hide_alphamaps->setText("Display Chunk Alphamaps");

      btn_left->setMinimumHeight(16);
      btn_right->setMinimumHeight(16);
      btn_add->setMinimumHeight(16);
      // btn_hide_alphamaps->setMinimumHeight(16);

      auto btn_layout(new QGridLayout);
      btn_layout->addWidget (btn_left, 0, 0);
      btn_layout->addWidget (btn_add, 0, 1);
      btn_layout->addWidget (btn_right, 0, 2);

      // layout->addWidget(btn_hide_alphamaps, 2, 0, 1, 4, Qt::AlignHCenter | Qt::AlignBottom);

      int texture_rows = (texture_picker_slots + texture_picker_columns - 1) / texture_picker_columns;

      layout->addItem(btn_layout, texture_rows, 0, 1, texture_picker_columns, Qt::AlignHCenter | Qt::AlignBottom);
      layout->addWidget(AlphamapsBox, texture_rows + 2, 0, 1, texture_picker_columns, Qt::AlignLeft);

      connect ( btn_left, &QPushButton::clicked
              , [this]
                {
                  emit shift_left();
                }
              );

      connect ( btn_right, &QPushButton::clicked
              , [this]
                {
                  emit shift_right();
                }
              );

      connect ( btn_add, &QPushButton::clicked
              , [this]
                {
                  if (!_chunk || !_main_texture_window)
                    return;

                  scoped_blp_texture_reference texture(_main_texture_window->filename(), Noggit::NoggitRenderContext::MAP_VIEW);
                  int texture_index = _chunk->texture_set->get_texture_index_or_add(std::move(texture), 1.f);
                  if (texture_index < 0)
                    return;

                  _chunk->texture_set->markDirty();
                  _chunk->mt->changed = true;
                  update();
                }
              );

      adjustSize();
      setMinimumSize(sizeHint());

    }

    void texture_picker::updateSelection()
    {
        if (!_chunk)
            return;

        size_t visible_texture_count = std::min(_chunk->texture_set->num(), _labels.size());

        for (size_t index = 0; index < visible_texture_count; ++index)
        {
            _labels[index]->unselect();
            if (_main_texture_window->filename() == _labels[index]->filename())
                _labels[index]->select();
        }
    }

    void texture_picker::setMainTexture(current_texture *tex)
    {
        _main_texture_window = tex;
    }

    void texture_picker::getTextures(selection_type lSelection)
    {
      if (lSelection.index() == eEntry_MapChunk)
      {
        MapChunk* chunk = std::get<selected_chunk_type> (lSelection).chunk;
        _chunk = chunk;
        update(false);
      }
    }

    void texture_picker::refreshIfTexturesChanged()
    {
      if (!_chunk)
        return;

      size_t visible_texture_count = std::min(_chunk->texture_set->num(), _labels.size());
      bool changed = visible_texture_count != _textures.size();

      for (size_t index = 0; !changed && index < visible_texture_count; ++index)
      {
        changed = _chunk->texture_set->filename(index) != _textures[index]->file_key().filepath();
      }

      if (changed)
      {
        update(false);
      }
    }

    void texture_picker::setTexture
      (size_t id, current_texture* current_texture_window)
    {
      assert(id < _textures.size());

      scoped_blp_texture_reference texture = _textures[id];

      emit set_texture(texture);
      current_texture_window->set_texture(texture->file_key().filepath());
      updateSelection();
    }

    void texture_picker::shiftSelectedTextureLeft()
    {
      auto&& selectedTexture = selected_texture::get();

      if (!_chunk)
          return;

      auto ts = _chunk->texture_set.get();
      for (int i = 1; i < ts->num(); i++)
      {
        if (ts->texture(i) == selectedTexture)
        {
          ts->swap_layers(i - 1, i);
          update();
          return;
        }
      }
    }

    void texture_picker::shiftSelectedTextureRight()
    {
      auto&& selectedTexture = selected_texture::get();

      if (!_chunk)
          return;

      auto ts = _chunk->texture_set.get();
      for (int i = 0; i < ts->num() - 1; i++)
      {
        if (ts->texture(i) == selectedTexture)
        {
          ts->swap_layers(i, i + 1);
          update();
          return;
        }
      }
    }

    void texture_picker::update(bool set_changed)
    {

      if (set_changed)
      {
        _chunk->mt->changed = true;
      }

      _textures.clear();
      // uint8_t index = 0;

      size_t visible_texture_count = std::min(_chunk->texture_set->num(), _labels.size());
      std::vector<int> weights(visible_texture_count, 0);

      for (size_t index = 0; index < visible_texture_count; ++index)
      {
        _labels[index]->unselect();
        _textures.push_back(_chunk->texture_set->texture(index));
        _labels[index]->set_texture(_textures[index]->file_key().filepath());
        _labels[index]->show();

        if (_main_texture_window->filename() == _labels[index]->filename())
            _labels[index]->select();

        if (_display_alphamaps)
            _alphamap_preview_labels[index]->show();

        // alphamap_preview.fill(Qt::black);
        QImage image(64, 64, QImage::Format_RGBA8888);
        image.fill(Qt::black);
        
        _chunk->texture_set->apply_alpha_changes();
        auto alphamaps = _chunk->texture_set->getAlphamaps();
        
        for (int k = 0; k < 64; ++k)
        {
            for (int l = 0; l < 64; ++l)
            {
                if (index == 0)
                {
                    // WoW calculates layer 0 as 255 - sum(Layer[1]...Layer[N]).
                    int layers_sum = 0;
                    for (size_t alpha_index = 0; alpha_index < _chunk->texture_set->num() - 1; ++alpha_index)
                    {
                      if (alphamaps->at(alpha_index))
                        layers_sum += alphamaps->at(alpha_index)->getAlpha(64 * l + k);
                    }
        
                    int value = std::clamp((255 - layers_sum), 0, 255);
                    image.setPixelColor(k, l, QColor(value, value, value, 255));
                    weights[index] += value;

                }
                else
                {
                    auto& alpha_layer = *alphamaps->at(index - 1);
        
                    int value = alpha_layer.getAlpha(64 * l + k);
                    image.setPixelColor(k, l, QColor(value, value, value, 255));
                    weights[index] += value;
                }
            }
        }

        // QPixmap alphamap_preview(64, 64);

        QPixmap alphamap_preview = QPixmap::fromImage(image).scaled(128, 128);
        // alphamap_preview.fromImage(image);
        // _alphamap_preview_labels[index]->setPixmap(alphamap_preview);
        _alphamap_preview_labels[index]->setPixmap(alphamap_preview);
      }

      int sum = std::accumulate(weights.begin(), weights.end(), 0);

      for (size_t index = 0; index < visible_texture_count; ++index)
      {
          float alpha_weight = sum ? (static_cast<float>(weights[index]) / static_cast<float>(sum) * 100.f) : 0.f;
          std::string weight_tooltip = "Weight: " + std::to_string(alpha_weight);
        _alphamap_preview_labels[index]->setToolTip(QString::fromStdString(weight_tooltip));
      }

      for (size_t index = visible_texture_count; index < _labels.size(); ++index)
      {
        _labels[index]->hide();
        _alphamap_preview_labels[index]->hide();
      }
    }
  }
}
