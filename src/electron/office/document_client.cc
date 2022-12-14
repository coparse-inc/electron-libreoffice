// Copyright (c) 2022 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "office/document_client.h"
#include <sys/types.h>

#include <iterator>
#include <string_view>
#include <vector>
#include "base/bind.h"
#include "base/callback_forward.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/task/task_runner_util.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "gin/per_isolate_data.h"
#include "include/core/SkAlphaType.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkPaint.h"
#include "net/base/filename_util.h"
#include "office/event_bus.h"
#include "office/lok_callback.h"
#include "office/office_client.h"
#include "shell/common/gin_converters/gfx_converter.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/libreofficekit/LibreOfficeKitEnums.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/geometry/size_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "url/gurl.h"
#include "v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"

namespace electron::office {
gin::WrapperInfo DocumentClient::kWrapperInfo = {gin::kEmbedderNativeGin};

DocumentClient::DocumentClient(lok::Document* document,
                               std::string path,
                               EventBus* event_bus)
    : document_(document), path_(path), event_bus_(event_bus) {
  // assumes the document loaded succesfully from OfficeClient
  DCHECK(document_);

  event_bus->Handle(LOK_CALLBACK_DOCUMENT_SIZE_CHANGED,
                    base::BindRepeating(&DocumentClient::HandleDocSizeChanged,
                                        base::Unretained(this)));
  event_bus->Handle(LOK_CALLBACK_INVALIDATE_TILES,
                    base::BindRepeating(&DocumentClient::HandleInvalidate,
                                        base::Unretained(this)));
  event_bus->Handle(LOK_CALLBACK_STATE_CHANGED,
                    base::BindRepeating(&DocumentClient::HandleStateChange,
                                        base::Unretained(this)));
}

DocumentClient::~DocumentClient() {
  LOG(ERROR) << "DOC CLIENT DESTROYED";
}

// gin::Wrappable
gin::ObjectTemplateBuilder DocumentClient::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  gin::PerIsolateData* data = gin::PerIsolateData::From(isolate);
  v8::Local<v8::FunctionTemplate> constructor =
      data->GetFunctionTemplate(&kWrapperInfo);
  if (constructor.IsEmpty()) {
    constructor = v8::FunctionTemplate::New(isolate);
    constructor->SetClassName(gin::StringToV8(isolate, GetTypeName()));
    constructor->ReadOnlyPrototype();
    data->SetFunctionTemplate(&kWrapperInfo, constructor);
  }
  return gin::ObjectTemplateBuilder(isolate, GetTypeName(),
                                    constructor->InstanceTemplate())
      .SetMethod("on", &DocumentClient::On)
      .SetMethod("off", &DocumentClient::Off)
      .SetMethod("emit", &DocumentClient::Emit)
      .SetMethod("twipToPx", &DocumentClient::TwipToPx)
      .SetMethod("postUnoCommand", &DocumentClient::PostUnoCommand)
      .SetMethod("gotoOutline", &DocumentClient::GotoOutline)
      .SetMethod("getTextSelection", &DocumentClient::GetTextSelection)
      .SetMethod("setTextSelection", &DocumentClient::SetTextSelection)
      .SetMethod("sendDialogEvent", &DocumentClient::SendDialogEvent)
      .SetMethod("getPartName", &DocumentClient::GetPartName)
      .SetMethod("getPartHash", &DocumentClient::GetPartHash)
      .SetMethod("getSelectionTypeAndText",
                 &DocumentClient::GetSelectionTypeAndText)
      .SetMethod("getClipboard", &DocumentClient::GetClipboard)
      .SetMethod("setClipboard", &DocumentClient::SetClipboard)
      .SetMethod("paste", &DocumentClient::Paste)
      .SetMethod("setGraphicSelection", &DocumentClient::SetGraphicSelection)
      .SetMethod("resetSelection", &DocumentClient::ResetSelection)
      .SetMethod("getCommandValues", &DocumentClient::GetCommandValues)
      .SetMethod("setOutlineState", &DocumentClient::SetOutlineState)
      .SetMethod("setViewLanguage", &DocumentClient::SetViewLanguage)
      .SetMethod("selectPart", &DocumentClient::SelectPart)
      .SetMethod("moveSelectedParts", &DocumentClient::MoveSelectedParts)
      .SetMethod("removeTextContext", &DocumentClient::RemoveTextContext)
      .SetMethod("completeFunction", &DocumentClient::CompleteFunction)
      .SetMethod("sendFormFieldEvent", &DocumentClient::SendFormFieldEvent)
      .SetMethod("sendContentControlEvent",
                 &DocumentClient::SendContentControlEvent)
      .SetLazyDataProperty("pageRects", &DocumentClient::PageRects)
      .SetLazyDataProperty("size", &DocumentClient::Size)
      .SetLazyDataProperty("isReady", &DocumentClient::IsReady);
}

const char* DocumentClient::GetTypeName() {
  return "DocumentClient";
}

bool DocumentClient::IsReady() const {
  return is_ready_;
}

std::vector<gfx::Rect> DocumentClient::PageRects() const {
  std::vector<gfx::Rect> result;
  float zoom = zoom_;  // want CSS pixels, which are already scaled to the
                       // device, so don't use TotalScale
  std::transform(page_rects_.begin(), page_rects_.end(),
                 std::back_inserter(result), [zoom](const gfx::Rect& rect) {
                   float scale = zoom / lok_callback::kTwipPerPx;
                   return gfx::Rect(
                       gfx::ScaleToCeiledPoint(rect.origin(), scale),
                       gfx::ScaleToCeiledSize(rect.size(), scale));
                 });
  return result;
}

gfx::Size DocumentClient::Size() const {
  return gfx::ToCeiledSize(document_size_px_);
}

// actually TwipTo_CSS_Px, since the pixels are device-indpendent
float DocumentClient::TwipToPx(float in) const {
  return lok_callback::TwipToPixel(in, zoom_);
}

lok::Document* DocumentClient::GetDocument() {
  return document_;
}

gfx::Size DocumentClient::DocumentSizeTwips() {
  return gfx::Size(document_width_in_twips_, document_height_in_twips_);
}

gfx::SizeF DocumentClient::DocumentSizePx() {
  return document_size_px_;
}

namespace {
void SaveBackup(const std::string& path) {
  GURL url(path);
  base::FilePath file_path;
  if (!url.SchemeIsFile()) {
    LOG(ERROR) << "Unable to make backup. Expected a URL as a path, but got: "
               << path;
    return;
  }
  if (!net::FileURLToFilePath(url, &file_path)) {
    LOG(ERROR) << "Unable to make backup. Unable to make file URL into path: "
               << path;
    return;
  }

  auto now = base::Time::Now();
  base::Time::Exploded exploded;
  now.UTCExplode(&exploded);
  // ex: .bak.2022-11-07-154601
  auto backup_suffix =
      base::StringPrintf(FILE_PATH_LITERAL(".bak.%04d-%02d-%02d-%02d%02d%02d"),
                         exploded.year, exploded.month, exploded.day_of_month,
                         exploded.hour, exploded.minute, exploded.second);

  base::FilePath backup_path = file_path.InsertBeforeExtension(backup_suffix);

  base::CopyFile(file_path, backup_path);
}
}  // namespace

int DocumentClient::Mount(v8::Isolate* isolate) {
  if (view_id_ != -1) {
    return view_id_;
  }

  {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> wrapper;
    if (this->GetWrapper(isolate).ToLocal(&wrapper)) {
      event_bus_->SetContext(isolate, isolate->GetCurrentContext());
      // prevent garbage collection
      mounted_.Reset(isolate, wrapper);
    }
  }

  if (OfficeClient::GetInstance()->MarkMounted(document_)) {
    view_id_ = document_->getView();
  } else {
    view_id_ = document_->createView();
  }

  RefreshSize();

  tile_mode_ = static_cast<LibreOfficeKitTileMode>(document_->getTileMode());

  // emit the state change buffer when ready
  v8::Local<v8::Array> ready_value =
      v8::Array::New(isolate, state_change_buffer_.size());
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  if (!state_change_buffer_.empty()) {
    for (size_t i = 0; i < state_change_buffer_.size(); i++) {
      // best effort
      std::ignore = ready_value->Set(
          context, i,
          lok_callback::PayloadToLocalValue(isolate, LOK_CALLBACK_STATE_CHANGED,
                                            state_change_buffer_[i].c_str()));
    }

    state_change_buffer_.clear();
  }

  // save a backup before we continue
  SaveBackup(path_);

  event_bus_->Emit("ready", ready_value);

  return view_id_;
}

void DocumentClient::Unmount() {
  // not mounted
  if (view_id_ == -1)
    return;

  if (document_->getViewsCount()) {
    document_->destroyView(view_id_);
    view_id_ = -1;
  }

  // allow garbage collection
  mounted_.Reset();
}

// Plugin Engine {
void DocumentClient::BrowserZoomUpdated(double new_zoom_level) {
  view_zoom_ = new_zoom_level;
  RefreshSize();
}

int DocumentClient::GetNumberOfPages() const {
  return document_->getParts();
}
//}

// Editing State {
bool DocumentClient::CanCopy() {
  auto res = uno_state_.find(".uno:Copy");
  return res != uno_state_.end() && res->second == "enabled";
}

bool DocumentClient::CanUndo() {
  auto res = uno_state_.find(".uno:Undo");
  return res != uno_state_.end() && res->second == "enabled";
}
bool DocumentClient::CanRedo() {
  auto res = uno_state_.find(".uno:Redo");
  return res != uno_state_.end() && res->second == "enabled";
}

// TODO: read only mode?
bool DocumentClient::CanEditText() {
  return true;
}
bool DocumentClient::HasEditableText() {
  return true;
}
// }

base::WeakPtr<DocumentClient> DocumentClient::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void DocumentClient::HandleStateChange(std::string payload) {
  std::pair<std::string, std::string> pair =
      lok_callback::ParseStatusChange(payload);
  if (!pair.first.empty()) {
    uno_state_.insert(pair);
  }

  if (view_id_ == -1 || !is_ready_) {
    state_change_buffer_.emplace_back(payload);
  }
}

void DocumentClient::HandleDocSizeChanged(std::string payload) {
  RefreshSize();
}

void DocumentClient::HandleInvalidate(std::string payload) {
  is_ready_ = true;
}

void DocumentClient::RefreshSize() {
  // not mounted
  if (view_id_ == -1)
    return;

  document_->getDocumentSize(&document_width_in_twips_,
                             &document_height_in_twips_);

  float zoom = zoom_;
  document_size_px_ =
      gfx::SizeF(lok_callback::TwipToPixel(document_width_in_twips_, zoom),
                 lok_callback::TwipToPixel(document_height_in_twips_, zoom));

  std::string_view page_rect_sv(document_->getPartPageRectangles());
  std::string_view::const_iterator start = page_rect_sv.begin();
  int new_size = GetNumberOfPages();
  // TODO: is there a better way, like updating a page only when it changes?
  page_rects_ =
      lok_callback::ParseMultipleRects(start, page_rect_sv.end(), new_size);
}

std::string DocumentClient::Path() {
  return path_;
}

void DocumentClient::On(const std::string& event_name,
                        v8::Local<v8::Function> listener_callback) {
  event_bus_->On(event_name, listener_callback);
}

void DocumentClient::Off(const std::string& event_name,
                         v8::Local<v8::Function> listener_callback) {
  event_bus_->Off(event_name, listener_callback);
}

void DocumentClient::Emit(const std::string& event_name,
                          v8::Local<v8::Value> data) {
  event_bus_->Emit(event_name, data);
}

DocumentClient::DocumentClient() = default;

v8::Local<v8::Value> DocumentClient::GotoOutline(int idx, gin::Arguments* args) {
  char* result = document_->gotoOutline(idx);
  v8::Isolate* isolate = args->isolate();

  if (!result) {
    return v8::Undefined(isolate);
  }

  v8::MaybeLocal<v8::String> maybe_res_json_str =
      v8::String::NewFromUtf8(isolate, result);

  if (maybe_res_json_str.IsEmpty()) {
    return v8::Undefined(isolate);
  }

  v8::MaybeLocal<v8::Value> res = v8::JSON::Parse(
      args->GetHolderCreationContext(), maybe_res_json_str.ToLocalChecked());

  if (res.IsEmpty()) {
    return v8::Undefined(isolate);
  }

  return res.ToLocalChecked();
}

void DocumentClient::PostUnoCommand(const std::string& command,
                                    gin::Arguments* args) {
  v8::Local<v8::Value> arguments;
  v8::MaybeLocal<v8::String> maybe_args_json;
  char* json_buffer = nullptr;

  bool notifyWhenFinished;
  if (args->GetNext(&arguments)) {
    // Convert JSON object into stringifed JSON
    maybe_args_json =
        v8::JSON::Stringify(args->GetHolderCreationContext(), arguments);

    if (!maybe_args_json.IsEmpty()) {
      auto args_json = maybe_args_json.ToLocalChecked();
      uint32_t len = args_json->Utf8Length(args->isolate());
      json_buffer = new char[len];
      args_json->WriteUtf8(args->isolate(), json_buffer);
    }
  }

  args->GetNext(&notifyWhenFinished);

  document_->postUnoCommand(command.c_str(), json_buffer, notifyWhenFinished);

  if (json_buffer)
    delete[] json_buffer;
}

std::vector<std::string> DocumentClient::GetTextSelection(
    const std::string& mime_type,
    gin::Arguments* args) {
  char* used_mime_type;
  char* text_selection;
  text_selection =
      document_->getTextSelection(mime_type.c_str(), &used_mime_type);

  return {text_selection, used_mime_type};
}

void DocumentClient::SetTextSelection(int n_type, int n_x, int n_y) {
  document_->setTextSelection(n_type, n_x, n_y);
}

v8::Local<v8::Value> DocumentClient::GetPartName(int n_part,
                                                 gin::Arguments* args) {
  char* part_name = document_->getPartName(n_part);
  if (!part_name)
    return v8::Undefined(args->isolate());

  return gin::StringToV8(args->isolate(), part_name);
}

v8::Local<v8::Value> DocumentClient::GetPartHash(int n_part,
                                                 gin::Arguments* args) {
  char* part_hash = document_->getPartHash(n_part);
  if (!part_hash)
    return v8::Undefined(args->isolate());

  return gin::StringToV8(args->isolate(), part_hash);
}

// TODO: Investigate correct type of args here
void DocumentClient::SendDialogEvent(uint64_t n_window_id,
                                     gin::Arguments* args) {
  v8::Local<v8::Value> arguments;
  v8::MaybeLocal<v8::String> maybe_arguments_str;
  char* p_arguments = nullptr;

  if (args->GetNext(&arguments)) {
    maybe_arguments_str = arguments->ToString(args->GetHolderCreationContext());
    if (!maybe_arguments_str.IsEmpty()) {
      v8::String::Utf8Value p_arguments_utf8(
          args->isolate(), maybe_arguments_str.ToLocalChecked());
      p_arguments = *p_arguments_utf8;
    }
  }
  document_->sendDialogEvent(n_window_id, p_arguments);
}

v8::Local<v8::Value> DocumentClient::GetSelectionTypeAndText(
    const std::string& mime_type,
    gin::Arguments* args) {
  char* used_mime_type;
  char* p_text_char;

  int selection_type = document_->getSelectionTypeAndText(
      mime_type.c_str(), &p_text_char, &used_mime_type);

  v8::Isolate* isolate = args->isolate();
  if (!p_text_char || !used_mime_type)
    return v8::Undefined(isolate);

  v8::Local<v8::Name> names[3] = {gin::StringToV8(isolate, "selectionType"),
                                  gin::StringToV8(isolate, "text"),
                                  gin::StringToV8(isolate, "usedMimeType")};
  v8::Local<v8::Value> values[3] = {gin::StringToV8(isolate, p_text_char),
                                    gin::ConvertToV8(isolate, selection_type),
                                    gin::StringToV8(isolate, used_mime_type)};

  return v8::Object::New(isolate, v8::Null(isolate), names, values, 2);
}

v8::Local<v8::Value> DocumentClient::GetClipboard(gin::Arguments* args) {
  std::vector<std::string> mime_types;
  std::vector<const char*> mime_c_str;

  if (args->GetNext(&mime_types)) {
    for (const std::string& mime_type : mime_types) {
      // c_str() gaurantees that the string is null-terminated, data() does not
      mime_c_str.push_back(mime_type.c_str());
    }

    // add the nullptr terminator to the list of null-terminated strings
    mime_c_str.push_back(nullptr);
  }
  size_t out_count;

  // these are arrays of out_count size, variable size arrays in C are simply
  // pointers to the first element
  char** out_mime_types = nullptr;
  size_t* out_sizes = nullptr;
  char** out_streams = nullptr;

  bool success = document_->getClipboard(
      mime_types.size() ? mime_c_str.data() : nullptr, &out_count,
      &out_mime_types, &out_sizes, &out_streams);

  // we'll be refrencing this and the context frequently inside of the loop
  v8::Isolate* isolate = args->isolate();

  // return an empty array if we failed
  if (!success)
    return v8::Array::New(isolate, 0);

  // an array of n=out_count array buffers
  v8::Local<v8::Array> result = v8::Array::New(isolate, out_count);
  // we pull out the context once outside of the array
  v8::Local<v8::Context> context = args->GetHolderCreationContext();

  for (size_t i = 0; i < out_count; ++i) {
    size_t buffer_size = out_sizes[i];

    // allocate a new ArrayBuffer and copy the stream to the backing store
    v8::Local<v8::ArrayBuffer> buffer =
        v8::ArrayBuffer::New(isolate, buffer_size);
    std::memcpy(buffer->GetBackingStore()->Data(), out_streams[i], buffer_size);

    v8::Local<v8::Name> names[2] = {gin::StringToV8(isolate, "mimeType"),
                                    gin::StringToV8(isolate, "buffer")};
    v8::Local<v8::Value> values[2] = {
        gin::StringToV8(isolate, out_mime_types[i]),
        gin::ConvertToV8(isolate, buffer)};

    v8::Local<v8::Value> object =
        v8::Object::New(isolate, v8::Null(isolate), names, values, 2);
    std::ignore = result->Set(context, i, object);
  }

  return result;
}

bool DocumentClient::SetClipboard(
    std::vector<v8::Local<v8::Object>> clipboard_data,
    gin::Arguments* args) {
  // entries in clipboard_data
  const size_t entries = clipboard_data.size();

  // No entries in clipboard_data
  if (entries == 0) {
    return false;
  }

  std::vector<const char*> mime_c_str;
  size_t in_sizes[entries];
  const char* streams[entries];

  v8::Isolate* isolate = args->isolate();
  for (size_t i = 0; i < entries; ++i) {
    gin::Dictionary dictionary(isolate, clipboard_data[i]);

    std::string mime_type;
    dictionary.Get<std::string>("mimeType", &mime_type);

    v8::Local<v8::ArrayBuffer> buffer;
    dictionary.Get<v8::Local<v8::ArrayBuffer>>("buffer", &buffer);

    in_sizes[i] = buffer->ByteLength();
    mime_c_str.push_back(mime_type.c_str());
    streams[i] = static_cast<char*>(buffer->GetBackingStore()->Data());
  }

  // add the nullptr terminator to the list of null-terminated strings
  mime_c_str.push_back(nullptr);

  return document_->setClipboard(entries, mime_c_str.data(), in_sizes, streams);
}

bool DocumentClient::Paste(const std::string& mime_type,
                           const std::string& data,
                           gin::Arguments* args) {
  return document_->paste(mime_type.c_str(), data.c_str(), data.size());
}

void DocumentClient::SetGraphicSelection(int n_type, int n_x, int n_y) {
  document_->setGraphicSelection(n_type, n_x, n_y);
}

void DocumentClient::ResetSelection() {
  document_->resetSelection();
}

v8::Local<v8::Value> DocumentClient::GetCommandValues(
    const std::string& command,
    gin::Arguments* args) {
  char* result = document_->getCommandValues(command.c_str());

  v8::Isolate* isolate = args->isolate();

  if (!result) {
    return v8::Undefined(isolate);
  }

  v8::MaybeLocal<v8::String> maybe_res_json_str =
      v8::String::NewFromUtf8(isolate, result);

  if (maybe_res_json_str.IsEmpty()) {
    return v8::Undefined(isolate);
  }

  v8::MaybeLocal<v8::Value> res = v8::JSON::Parse(
      args->GetHolderCreationContext(), maybe_res_json_str.ToLocalChecked());

  if (res.IsEmpty()) {
    return v8::Undefined(isolate);
  }

  return res.ToLocalChecked();
}

void DocumentClient::SetOutlineState(bool column,
                                     int level,
                                     int index,
                                     bool hidden) {
  document_->setOutlineState(column, level, index, hidden);
}

void DocumentClient::SetViewLanguage(int id, const std::string& language) {
  document_->setViewLanguage(id, language.c_str());
}

void DocumentClient::SelectPart(int part, int select) {
  document_->selectPart(part, select);
}

void DocumentClient::MoveSelectedParts(int position, bool duplicate) {
  document_->moveSelectedParts(position, duplicate);
}

void DocumentClient::RemoveTextContext(unsigned window_id,
                                       int before,
                                       int after) {
  document_->removeTextContext(window_id, before, after);
}

void DocumentClient::CompleteFunction(const std::string& function_name) {
  document_->completeFunction(function_name.c_str());
}

void DocumentClient::SendFormFieldEvent(const std::string& arguments) {
  document_->sendFormFieldEvent(arguments.c_str());
}

bool DocumentClient::SendContentControlEvent(
    const v8::Local<v8::Object>& arguments,
    gin::Arguments* args) {

  v8::MaybeLocal<v8::String> maybe_str_object =
      v8::JSON::Stringify(args->GetHolderCreationContext(), arguments);

  if (maybe_str_object.IsEmpty()) {
    return false;
  }

  v8::Local<v8::String> str_object = maybe_str_object.ToLocalChecked();

  uint32_t len = str_object->Utf8Length(args->isolate());
  char* json_buffer = new char[len];
  str_object->WriteUtf8(args->isolate(), json_buffer);

  document_->sendContentControlEvent(json_buffer);

  delete[] json_buffer;

  return true;
}
}  // namespace electron::office
