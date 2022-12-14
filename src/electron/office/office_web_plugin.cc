// Copyright (c) 2022 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "office/office_web_plugin.h"

#include <chrono>
#include <memory>

#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread_task_runner_handle.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_image.h"
#include "cc/paint/paint_image_builder.h"
#include "components/plugins/renderer/plugin_placeholder.h"
#include "content/public/renderer/render_frame.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "gin/handle.h"
#include "include/core/SkColor.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkTextBlob.h"
#include "office/document_client.h"
#include "office/event_bus.h"
#include "office/lok_callback.h"
#include "office/lok_tilebuffer.h"
#include "office/office_client.h"
#include "office/office_keys.h"
#include "shell/common/gin_converters/gfx_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_pointer_properties.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_input_event_result.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_plugin_params.h"
#include "third_party/blink/public/web/web_widget.h"
#include "third_party/libreofficekit/LibreOfficeKitEnums.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/cursor/platform_cursor.h"
#include "ui/events/blink/blink_event_util.h"
#include "ui/gfx/geometry/point3_f.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/geometry/size_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/native_widget_types.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"

namespace electron {

namespace office {
extern const char kInternalPluginMimeType[] = "application/x-libreoffice";

blink::WebPlugin* CreateInternalPlugin(blink::WebPluginParams params,
                                       content::RenderFrame* render_frame) {
  return new OfficeWebPlugin(params, render_frame);
}

}  // namespace office

using Modifiers = blink::WebInputEvent::Modifiers;

OfficeWebPlugin::OfficeWebPlugin(blink::WebPluginParams params,
                                 content::RenderFrame* render_frame)
    : render_frame_(render_frame),
      task_runner_(render_frame->GetTaskRunner(
          blink::TaskType::kInternalMediaRealTime)) {
  office_client_ = office::OfficeClient::GetInstance();
  office_ = office_client_->GetOffice();

  // auto* frame = render_frame->GetWebFrame();
  // v8::Local<v8::Context> main_context = frame->MainWorldScriptContext();
  // v8::Isolate* isolate = main_context->GetIsolate();
};

// blink::WebPlugin {
bool OfficeWebPlugin::Initialize(blink::WebPluginContainer* container) {
  container_ = container;

  // TODO: figure out what false means?
  return true;
}

void OfficeWebPlugin::Destroy() {
  if (container_) {
    // TODO: release client container value
  }
  if (document_client_) {
    document_client_->Unmount();
  }
  delete this;
}

v8::Local<v8::Object> OfficeWebPlugin::V8ScriptableObject(
    v8::Isolate* isolate) {
  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  dict.SetMethod("renderDocument",
                 base::BindRepeating(&OfficeWebPlugin::RenderDocument,
                                     base::Unretained(this)));
  dict.SetMethod("updateScroll",
                 base::BindRepeating(&OfficeWebPlugin::UpdateScrollInTask,
                                     base::Unretained(this)));
  return dict.GetHandle();
}

blink::WebPluginContainer* OfficeWebPlugin::Container() const {
  return container_;
}

bool OfficeWebPlugin::SupportsKeyboardFocus() const {
  return true;
}

void OfficeWebPlugin::UpdateAllLifecyclePhases(
    blink::DocumentUpdateReason reason) {}

void OfficeWebPlugin::Paint(cc::PaintCanvas* canvas, const gfx::Rect& rect) {
  SkRect invalidate_rect =
      gfx::RectToSkRect(gfx::IntersectRects(css_plugin_rect_, rect));
  cc::PaintCanvasAutoRestore auto_restore(canvas, true);
  canvas->clipRect(invalidate_rect);

  // nothing drawn yet
  if (snapshot_.GetSkImageInfo().isEmpty()) {
    cc::PaintFlags flags;
    flags.setBlendMode(SkBlendMode::kSrc);
    flags.setColor(SK_ColorTRANSPARENT);

    canvas->drawRect(invalidate_rect, flags);
  }

  if (!total_translate_.IsZero())
    canvas->translate(total_translate_.x(), total_translate_.y());

  if (device_to_css_scale_ != 1.0f)
    canvas->scale(device_to_css_scale_, device_to_css_scale_);

  if (!plugin_rect_.origin().IsOrigin())
    canvas->translate(plugin_rect_.x(), plugin_rect_.y());

  if (snapshot_scale_ != 1.0f)
    canvas->scale(snapshot_scale_, snapshot_scale_);

  canvas->drawImage(snapshot_, 0, 0);

  // Ensure the background parts are cleared after scrolling
  // TODO: Handle during scrolling,
  // TODO: Clear top and bottom gap consistently, or just crop to page size and
  // pad in HTML
  cc::PaintFlags flags;
  flags.setBlendMode(SkBlendMode::kSrc);
  flags.setColor(SK_ColorTRANSPARENT);

  for (const gfx::Rect& background_part : background_parts_) {
    gfx::Rect offset_rect = gfx::Rect(available_area_);
    offset_rect.Offset(scroll_position_.x() - available_area_.x(),
                       scroll_position_.y());

    if (offset_rect.InclusiveIntersect(background_part)) {
      auto clear_rect = gfx::Rect(background_part);
      clear_rect.Offset(-scroll_position_.x() - available_area_.x(),
                        -scroll_position_.y());
      canvas->drawRect(gfx::RectToSkRect(clear_rect), flags);
    }
  }
}

void OfficeWebPlugin::UpdateGeometry(const gfx::Rect& window_rect,
                                     const gfx::Rect& clip_rect,
                                     const gfx::Rect& unobscured_rect,
                                     bool is_visible) {
  // nothing to render inside of
  if (window_rect.IsEmpty())
    return;

  // get the document root frame's scale factor so that any widget scaling does
  // not affect the device scale
  blink::WebWidget* widget =
      container_->GetDocument().GetFrame()->LocalRoot()->FrameWidget();
  OnViewportChanged(window_rect,
                    widget->GetOriginalScreenInfo().device_scale_factor);
}

void OfficeWebPlugin::UpdateFocus(bool focused,
                                  blink::mojom::FocusType focus_type) {
  if (has_focus_ != focused) {
    // TODO: update focus, input state, and selection bounds
  }

  has_focus_ = focused;
}

void OfficeWebPlugin::UpdateVisibility(bool visibility) {}

blink::WebInputEventResult OfficeWebPlugin::HandleInputEvent(
    const blink::WebCoalescedInputEvent& event,
    ui::Cursor* cursor) {
  blink::WebInputEvent::Type event_type = event.Event().GetType();

  // TODO: handle gestures
  if (blink::WebInputEvent::IsGestureEventType(event_type))
    return blink::WebInputEventResult::kNotHandled;

  if (container_ && container_->WasTargetForLastMouseEvent()) {
    *cursor = cursor_type_;
  }

  if (blink::WebInputEvent::IsKeyboardEventType(event_type))
    return HandleKeyEvent(
        std::move(static_cast<const blink::WebKeyboardEvent&>(event.Event())),
        cursor);

  std::unique_ptr<blink::WebInputEvent> scaled_event =
      ui::ScaleWebInputEvent(event.Event(), viewport_to_dip_scale_);
  std::unique_ptr<blink::WebInputEvent> transformed_event =
      ui::TranslateAndScaleWebInputEvent(
          scaled_event ? *scaled_event : event.Event(),
          gfx::Vector2dF(-available_area_.x() / device_scale_, 0),
          device_scale_);

  const blink::WebInputEvent& event_to_handle =
      transformed_event ? *transformed_event : event.Event();

  switch (event_type) {
    case blink::WebInputEvent::Type::kMouseDown:
    case blink::WebInputEvent::Type::kMouseUp:
    case blink::WebInputEvent::Type::kMouseMove:
      break;
    default:
      return blink::WebInputEventResult::kNotHandled;
  }

  blink::WebMouseEvent mouse_event =
      static_cast<const blink::WebMouseEvent&>(event_to_handle);

  int modifiers = mouse_event.GetModifiers();
  return HandleMouseEvent(event_type, mouse_event.PositionInWidget(), modifiers,
                          mouse_event.ClickCount(), cursor)
             ? blink::WebInputEventResult::kHandledApplication
             : blink::WebInputEventResult::kNotHandled;
}

blink::WebInputEventResult OfficeWebPlugin::HandleKeyEvent(
    const blink::WebKeyboardEvent event,
    ui::Cursor* cursor) {
  if (!document_ || view_id_ == -1)
    return blink::WebInputEventResult::kNotHandled;

  blink::WebInputEvent::Type type = event.GetType();

  // supress scroll event for any containers when pressing space
  if (type == blink::WebInputEvent::Type::kChar &&
      event.dom_code == office::DomCode::SPACE) {
    return blink::WebInputEventResult::kHandledApplication;
  }

  // only handle provided key events
  switch (type) {
    case blink::WebInputEvent::Type::kRawKeyDown:
    case blink::WebInputEvent::Type::kKeyUp:
      break;
    default:
      return blink::WebInputEventResult::kNotHandled;
  }

  // intercept some special key events on Ctr/Command
  if (event.GetModifiers() & (event.kControlKey | event.kMetaKey)) {
    switch (event.dom_code) {
      // don't close the internal LO window
      case office::DomCode::US_W:
        return blink::WebInputEventResult::kNotHandled;
    }
  }

  int lok_key_code =
      office::DOMKeyCodeToLOKKeyCode(event.dom_code, event.GetModifiers());

  task_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&lok::Document::setView,
                                        base::Unretained(document_), view_id_));
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&lok::Document::postKeyEvent, base::Unretained(document_),
                     type == blink::WebInputEvent::Type::kKeyUp
                         ? LOK_KEYEVENT_KEYUP
                         : LOK_KEYEVENT_KEYINPUT,
                     event.text[0], lok_key_code));
  needs_reraster_ = true;

  return blink::WebInputEventResult::kHandledApplication;
}

bool OfficeWebPlugin::HandleMouseEvent(blink::WebInputEvent::Type type,
                                       gfx::PointF position,
                                       int modifiers,
                                       int clickCount,
                                       ui::Cursor* cursor) {
  if (!document_ || view_id_ == -1)
    return false;

  LibreOfficeKitMouseEventType event_type;
  switch (type) {
    case blink::WebInputEvent::Type::kMouseDown:
      event_type = LOK_MOUSEEVENT_MOUSEBUTTONDOWN;
      break;
    case blink::WebInputEvent::Type::kMouseUp:
      event_type = LOK_MOUSEEVENT_MOUSEBUTTONUP;
      break;
    case blink::WebInputEvent::Type::kMouseMove:
      event_type = LOK_MOUSEEVENT_MOUSEMOVE;
      break;
    default:
      return false;
  }

  // allow focus even if not in area
  if (!available_area_.Contains(gfx::ToCeiledPoint(position))) {
    return event_type == LOK_MOUSEEVENT_MOUSEBUTTONDOWN;
  }

  // offset by the scroll position
  position.Offset(scroll_position_.x(), scroll_position_.y());

  // TODO: handle offsets
  gfx::Point pos = gfx::ToRoundedPoint(gfx::ScalePoint(
      position,
      office::lok_callback::kTwipPerPx / document_client_->TotalScale()));

  int buttons = 0;
  if (modifiers & blink::WebInputEvent::kLeftButtonDown)
    buttons |= 1;
  if (modifiers & blink::WebInputEvent::kMiddleButtonDown)
    buttons |= 2;
  if (modifiers & blink::WebInputEvent::kRightButtonDown)
    buttons |= 4;

  if (buttons > 0) {
    document_->postMouseEvent(event_type, pos.x(), pos.y(), clickCount, buttons,
                              office::EventModifiersToLOKModifiers(modifiers));
    return true;
  }

  return false;
}

void OfficeWebPlugin::DidReceiveResponse(
    const blink::WebURLResponse& response) {}

// no-op
void OfficeWebPlugin::DidReceiveData(const char* data, size_t data_length) {}
void OfficeWebPlugin::DidFinishLoading() {}
void OfficeWebPlugin::DidFailLoading(const blink::WebURLError& error) {}

bool OfficeWebPlugin::CanEditText() const {
  return true;
}

bool OfficeWebPlugin::HasEditableText() const {
  return true;
}

bool OfficeWebPlugin::CanUndo() const {
  return document_client_ && document_client_->CanUndo();
}

bool OfficeWebPlugin::CanRedo() const {
  return document_client_ && document_client_->CanRedo();
}

content::RenderFrame* OfficeWebPlugin::render_frame() const {
  return render_frame_;
}

OfficeWebPlugin::~OfficeWebPlugin() = default;

void OfficeWebPlugin::InvalidatePluginContainer() {
  if (container_)
    container_->Invalidate();
}

void OfficeWebPlugin::OnPaint(const std::vector<gfx::Rect>& paint_rects,
                              std::vector<chrome_pdf::PaintReadyRect>& ready,
                              std::vector<gfx::Rect>& pending) {
  base::AutoReset<bool> auto_reset_in_paint(&in_paint_, true);
  DoPaint(paint_rects, ready, pending);
}

void OfficeWebPlugin::DoPaint(const std::vector<gfx::Rect>& paint_rects,
                              std::vector<chrome_pdf::PaintReadyRect>& ready,
                              std::vector<gfx::Rect>& pending) {
  // not mounted
  if (!document_client_) {
    return;
  }

  if (image_data_.drawsNothing()) {
    DCHECK(plugin_rect_.IsEmpty());
    return;
  }

  PrepareForFirstPaint(ready);

  if (!needs_reraster_)
    return;

  DCHECK(document_);

  // start measuring render time
  auto start = std::chrono::steady_clock::now();
  document_->setView(view_id_);

  std::vector<gfx::Rect> ready_rects;
  SkCanvas canvas(image_data_);
  // LOK paint tiles are drawn in an absolute grid, whereas the canvas is drawn
  // in a grid relative to the available area
  canvas.translate(-scroll_position_.x(), -scroll_position_.y());

  for (const gfx::Rect& paint_rect : paint_rects) {
    // Intersect with plugin area since there could be pending invalidates from
    // when the plugin area was larger.
    gfx::Rect rect =
        gfx::IntersectRects(paint_rect, gfx::Rect(plugin_rect_.size()));
    if (rect.IsEmpty())
      continue;

    // Paint the rendering of the document.
    gfx::Rect dirty_rect = gfx::IntersectRects(rect, available_area_);
    if (!dirty_rect.IsEmpty()) {
      std::vector<gfx::Rect> callback_ready;
      std::vector<gfx::Rect> callback_pending;
      dirty_rect.Offset(-available_area_.x(), 0);

      // paint the absolute rect to the tile buffer
      dirty_rect.Offset(scroll_position_.x(), scroll_position_.y());
      // TODO: handle current part for non-text documents
      part_tile_buffer_.at(0).PaintInvalidTiles(
          canvas, dirty_rect, start, callback_ready, callback_pending);

      for (gfx::Rect& ready_rect : callback_ready) {
        ready_rect.Offset(available_area_.OffsetFromOrigin());
        ready_rect.Offset(-scroll_position_.x(), -scroll_position_.y());

        ready_rects.push_back(ready_rect);
      }
      for (gfx::Rect& pending_rect : callback_pending) {
        pending_rect.Offset(available_area_.OffsetFromOrigin());
        pending_rect.Offset(-scroll_position_.x(), -scroll_position_.y());
        pending.push_back(pending_rect);
      }
    }
  }

  // TODO(crbug.com/1263614): Write pixels directly to the `SkSurface` in
  // `PaintManager`, rather than using an intermediate `SkBitmap` and `SkImage`.
  sk_sp<SkImage> painted_image = image_data_.asImage();
  for (const gfx::Rect& ready_rect : ready_rects)
    ready.emplace_back(ready_rect, painted_image);

  InvalidateAfterPaintDone();
}

void OfficeWebPlugin::PrepareForFirstPaint(
    std::vector<chrome_pdf::PaintReadyRect>& ready) {
  if (!first_paint_)
    return;

  // Fill the image data buffer with the background color.
  first_paint_ = false;
  image_data_.eraseColor(background_color_);
  ready.emplace_back(gfx::SkIRectToRect(image_data_.bounds()),
                     image_data_.asImage(), /*flush_now=*/true);
}

void OfficeWebPlugin::OnGeometryChanged(double old_zoom,
                                        float old_device_scale) {
  RecalculateAreas(old_zoom, old_device_scale);
}

namespace {
void ResetTileBuffers(std::vector<office::TileBuffer>& buffers,
                      lok::Document* document,
                      int parts,
                      float scale) {
  // TODO: handle case where buffer exceeds the number of parts
  int missing = parts - buffers.size();
  if (missing < 0)
    missing = 0;

  for (int part = 0; part < missing; ++part) {
    buffers.emplace_back(document, scale, part);
  }

  parts -= missing;
  for (int part = 0; part < parts; ++part) {
    buffers[part] = std::move(office::TileBuffer(document, scale, part));
    LOG(ERROR) << "BUFFER RESET SCALE" << scale;
  }
}
}  // namespace

void OfficeWebPlugin::RecalculateAreas(double old_zoom,
                                       float old_device_scale) {
  if (!document_client_)
    return;

  if (zoom_ != old_zoom || device_scale_ != old_device_scale) {
    document_client_->BrowserZoomUpdated(zoom_ * device_scale_);
    ResetTileBuffers(part_tile_buffer_, document_,
                     // there is only one tile buffer for text documents
                     document_->getDocumentType() == LOK_DOCTYPE_TEXT
                         ? 1
                         : document_->getParts(),
                     document_client_->TotalScale());
  }

  available_area_ = gfx::Rect(plugin_rect_.size());
  gfx::Size doc_size = GetDocumentPixelSize();
  if (doc_size.width() < available_area_.width()) {
    available_area_.set_width(doc_size.width());
  }

  // The distance between top of the plugin and the bottom of the document in
  // pixels.
  int bottom_of_document = doc_size.height();
  if (bottom_of_document < plugin_rect_.height())
    available_area_.set_height(bottom_of_document);

  available_area_twips_ = gfx::ScaleToEnclosingRect(
      available_area_, office::lok_callback::kTwipPerPx);

  CalculateBackgroundParts();
}

void OfficeWebPlugin::CalculateBackgroundParts() {
  background_parts_.clear();

  // Add the gaps
  auto part = gfx::Rect();
  gfx::Rect empty_rect = gfx::Rect();
  gfx::Rect& previous_part_rect = empty_rect;
  std::vector<gfx::Rect> page_rects = document_client_->PageRects();
  for (gfx::Rect& page_rect : page_rects) {
    part.set_width(page_rect.width());
    part.set_x(page_rect.x());
    part.SetVerticalBounds(previous_part_rect.bottom(), page_rect.y());
    if (!part.IsEmpty())
      background_parts_.emplace_back(
          gfx::ScaleToEnclosingRect(part, device_scale_ * zoom_));
    previous_part_rect = page_rect;

    // Add the bottom gap for last rect
    if (page_rect == page_rects.back()) {
      part.SetVerticalBounds(page_rect.bottom(), document_client_->DocumentSizePx().height());
      background_parts_.emplace_back(
          gfx::ScaleToEnclosingRect(part, device_scale_ * zoom_));
    }
  }
}

gfx::Size OfficeWebPlugin::GetDocumentPixelSize() const {
  return gfx::ToCeiledSize(gfx::ScaleSize(document_client_->DocumentSizePx(),
                                          zoom_ * device_scale_));
}

void OfficeWebPlugin::InvalidateAfterPaintDone() {
  if (deferred_invalidates_.empty())
    return;

  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&OfficeWebPlugin::ClearDeferredInvalidates,
                                weak_factory_.GetWeakPtr()));
}

void OfficeWebPlugin::Invalidate(const gfx::Rect& rect) {
  if (in_paint_) {
    deferred_invalidates_.push_back(rect);
    return;
  }

  gfx::Rect offset_rect = rect + available_area_.OffsetFromOrigin();

  // paint manager aborts outside of a valid task runner
  task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&chrome_pdf::PaintManager::InvalidateRect,
                                base::Unretained(&paint_manager_),
                                std::move(offset_rect)));
}

void OfficeWebPlugin::ClearDeferredInvalidates() {
  DCHECK(!in_paint_);
  for (const gfx::Rect& rect : deferred_invalidates_)
    Invalidate(rect);
  deferred_invalidates_.clear();
}

void OfficeWebPlugin::UpdateSnapshot(sk_sp<SkImage> snapshot) {
  snapshot_ =
      cc::PaintImageBuilder::WithDefault()
          .set_image(std::move(snapshot), cc::PaintImage::GetNextContentId())
          .set_id(cc::PaintImage::GetNextId())
          .TakePaintImage();
  if (!plugin_rect_.IsEmpty())
    InvalidatePluginContainer();
}

void OfficeWebPlugin::UpdateScaledValues() {
  total_translate_ = snapshot_translate_;

  if (viewport_to_dip_scale_ != 1.0f)
    total_translate_.Scale(1.0f / viewport_to_dip_scale_);
}

void OfficeWebPlugin::UpdateScale(float scale) {
  if (scale <= 0.0f) {
    NOTREACHED();
    return;
  }

  viewport_to_dip_scale_ = scale;
  device_to_css_scale_ = 1.0f;
  UpdateScaledValues();
}

void OfficeWebPlugin::UpdateLayerTransform(float scale,
                                           const gfx::Vector2dF& translate) {
  snapshot_translate_ = translate;
  snapshot_scale_ = scale;
  UpdateScaledValues();
}

void OfficeWebPlugin::OnViewportChanged(
    const gfx::Rect& plugin_rect_in_css_pixel,
    float new_device_scale) {
  DCHECK_GT(new_device_scale, 0.0f);

  css_plugin_rect_ = plugin_rect_in_css_pixel;

  if (new_device_scale == device_scale_ &&
      plugin_rect_in_css_pixel == plugin_rect_) {
    return;
  }

  const float old_device_scale = device_scale_;
  device_scale_ = new_device_scale;
  plugin_rect_ = plugin_rect_in_css_pixel;

  // TODO(crbug.com/1250173): We should try to avoid the downscaling in this
  // calculation, perhaps by migrating off `plugin_dip_size_`.
  plugin_dip_size_ =
      gfx::ScaleToEnclosingRect(plugin_rect_in_css_pixel, 1.0f).size();

  paint_manager_.SetSize(plugin_rect_.size(), device_scale_);

  // Initialize the image data buffer if the context size changes.
  const gfx::Size old_image_size = gfx::SkISizeToSize(image_data_.dimensions());
  const gfx::Size new_image_size = chrome_pdf::PaintManager::GetNewContextSize(
      old_image_size, plugin_rect_.size());
  if (new_image_size != old_image_size) {
    image_data_.allocPixels(
        SkImageInfo::MakeN32Premul(gfx::SizeToSkISize(new_image_size)));
    first_paint_ = true;
  }

  // Skip updating the geometry if the new image data buffer is empty.
  if (image_data_.drawsNothing())
    return;

  OnGeometryChanged(zoom_, old_device_scale);
}

void OfficeWebPlugin::HandleInvalidateTiles(std::string payload) {
  // not mounted
  if (view_id_ == -1)
    return;

  std::string_view payload_sv(payload);
  std::string_view::const_iterator start = payload_sv.begin();
  gfx::Rect dirty_rect =
      office::lok_callback::ParseRect(start, payload_sv.end());

  // TODO: handle non-text document types for parts
  if (payload_sv == "EMPTY") {
    part_tile_buffer_.at(0).InvalidateAllTiles();
    TriggerFullRerender();
  } else if (!dirty_rect.IsEmpty()) {
    part_tile_buffer_.at(0).InvalidateTilesInTwipRect(dirty_rect);
    auto scaled_rect = gfx::ScaleToEnclosingRect(
        dirty_rect,
        document_client_->TotalScale() / office::lok_callback::kTwipPerPx);
    scaled_rect.Offset(-scroll_position_.x(), -scroll_position_.y());
    Invalidate(scaled_rect);
  }
}

void OfficeWebPlugin::HandleDocumentSizeChanged(std::string payload) {
  ResetTileBuffers(part_tile_buffer_, document_,
                   // there is only one tile buffer for text documents
                   document_->getDocumentType() == LOK_DOCTYPE_TEXT
                       ? 1
                       : document_->getParts(),
                   document_client_->TotalScale());
}

void OfficeWebPlugin::UpdateScrollInTask(const gfx::PointF& scroll_position) {
  if (task_runner_)
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&OfficeWebPlugin::UpdateScroll,
                                  base::Unretained(this), scroll_position));
}

void OfficeWebPlugin::UpdateScroll(const gfx::PointF& scroll_position) {
  if (!document_client_ || stop_scrolling_)
    return;

  // TODO: fix plugin_dip_size, this isn't correct
  float max_x = std::max(document_client_->DocumentSizePx().width() -
                             plugin_dip_size_.width() / device_scale_,
                         0.0f);
  float max_y = std::max(document_client_->DocumentSizePx().height() -
                             plugin_dip_size_.height() / device_scale_,
                         0.0f);

  gfx::PointF scaled_scroll_position(
      base::clamp(scroll_position.x(), 0.0f, max_x),
      base::clamp(scroll_position.y(), 0.0f, max_y));
  scaled_scroll_position.Scale(device_scale_);

  // needs_reraster_ = true;
  // paint manager requires that the x and y axis are updated separately
  gfx::Vector2d diff_x(scroll_position_.x() - scaled_scroll_position.x(), 0);
  gfx::Vector2d diff_y(0, scroll_position_.y() - scaled_scroll_position.y());

  paint_manager_.ScrollRect(available_area_, diff_y);

  scroll_position_ = scaled_scroll_position;
}

bool OfficeWebPlugin::RenderDocument(
    v8::Isolate* isolate,
    gin::Handle<office::DocumentClient> client) {
  if (client.IsEmpty()) {
    LOG(ERROR) << "invalid document client";
    return false;
  }
  office::OfficeClient* office = office::OfficeClient::GetInstance();

  // TODO: honestly, this is terrible, need to do this properly
  // already mounted
  bool needs_reset = view_id_ != -1;
  if (needs_reset) {
    part_tile_buffer_.clear();
    office->CloseDocument(document_client_->Path());
    document_client_->Unmount();
    delete document_;
  }

  document_ = client->GetDocument();
  document_client_ = client.get();
  view_id_ = client->Mount(isolate);
  document_client_->BrowserZoomUpdated(zoom_ * device_scale_);
  part_tile_buffer_.emplace_back(document_, document_client_->TotalScale());

  if (needs_reset) {
    // this is an awful hack
    auto device_scale = device_scale_;
    device_scale_ = 0;
    OnViewportChanged(css_plugin_rect_, device_scale);
  }

  document_->setViewLanguage(view_id_, "en-US");
  document_->setView(view_id_);
  document_->resetSelection();

  office->HandleDocumentEvent(
      document_, LOK_CALLBACK_INVALIDATE_TILES,
      base::BindRepeating(&OfficeWebPlugin::HandleInvalidateTiles,
                          base::Unretained(this)));
  office->HandleDocumentEvent(
      document_, LOK_CALLBACK_DOCUMENT_SIZE_CHANGED,
      base::BindRepeating(&OfficeWebPlugin::HandleDocumentSizeChanged,
                          base::Unretained(this)));

  TriggerFullRerender();
  return true;
}

void OfficeWebPlugin::TriggerFullRerender() {
  needs_reraster_ = true;
  OnGeometryChanged(zoom_, device_scale_);
  if (document_client_ && !document_client_->DocumentSizePx().IsEmpty()) {
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            &chrome_pdf::PaintManager::InvalidateRect,
            base::Unretained(&paint_manager_),
            gfx::Rect(gfx::ToCeiledSize(document_client_->DocumentSizePx()))));
  }
}

base::WeakPtr<OfficeWebPlugin> OfficeWebPlugin::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

// } blink::WebPlugin

}  // namespace electron
