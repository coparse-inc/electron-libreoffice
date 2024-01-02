#include "web_plugin_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_plugin_container.h"
#include "third_party/blink/public/web/web_widget.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/display/screen_info.h"
#include "ui/events/blink/blink_event_util.h"

namespace electron::office {

namespace container {
bool Initialize(blink::WebPluginContainer* container) {
  // This prevents the wheel event hit test data from causing a crash, wheel
  // events are handled by the scroll container anyway
  container->SetWantsWheelEvents(false);

  // TODO: figure out what false means?
  return true;
}

float DeviceScale(blink::WebPluginContainer* container) {
  // get the document root frame's scale factor so that any widget scaling does
  // not affect the device scale
  blink::WebWidget* widget =
      container->GetDocument().GetFrame()->LocalRoot()->FrameWidget();

  return widget->GetOriginalScreenInfo().device_scale_factor;
}

std::string CSSCursor(blink::WebPluginContainer* container) {
  return container->GetElement().GetComputedValue("cursor").Ascii();
}

void Invalidate(blink::WebPluginContainer* container) {
  container->Invalidate();
}
}  // namespace container

namespace input {
gfx::PointF GetRelativeMousePosition(const blink::WebInputEvent& event,
                                gfx::Vector2dF delta) {
  std::unique_ptr<blink::WebInputEvent> transformed_event =
      ui::TranslateAndScaleWebInputEvent(event, delta, 1.0);

  const blink::WebInputEvent& event_to_handle =
      transformed_event ? *transformed_event : event;

  blink::WebMouseEvent mouse_event =
      static_cast<const blink::WebMouseEvent&>(event_to_handle);

  return mouse_event.PositionInWidget();
}

int GetClickCount(const blink::WebInputEvent& event) {
	return static_cast<const blink::WebMouseEvent&>(event).ClickCount();
}
}  // namespace input

namespace clipboard {
ui::Clipboard* GetCurrent() {
  return ui::Clipboard::GetForCurrentThread();
}

const std::vector<std::u16string> GetAvailableTypes(ui::Clipboard* clipboard) {
  return clipboard->ReadAvailableStandardAndCustomFormatNames(
      ui::ClipboardBuffer::kCopyPaste, nullptr);
}

std::string ReadTextUtf8(ui::Clipboard* clipboard) {
  std::u16string system_clipboard_data;
  clipboard->ReadText(ui::ClipboardBuffer::kCopyPaste, nullptr,
                      &system_clipboard_data);
  return base::UTF16ToUTF8(system_clipboard_data);
}

std::vector<uint8_t> ReadPng(ui::Clipboard* clipboard) {
  std::vector<uint8_t> image;
  clipboard->ReadPng(
      ui::ClipboardBuffer::kCopyPaste, nullptr,
      base::BindOnce(
          [](std::vector<uint8_t>* image, const std::vector<uint8_t>& result) {
            *image = std::move(result);
          },
          &image));

  return image;
}

}  // namespace clipboard
}  // namespace electron::office
