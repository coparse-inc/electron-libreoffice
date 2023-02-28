// Copyright (c) 2022 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "office/lok_callback.h"

#include <codecvt>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "gin/converter.h"
#include "third_party/libreofficekit/LibreOfficeKitEnums.h"
#include "ui/gfx/geometry/rect.h"
#include "v8-primitive.h"
#include "v8/include/v8-exception.h"

namespace electron::office::lok_callback {

void SkipWhitespace(std::string_view::const_iterator& target,
                    std::string_view::const_iterator end) {
  while (target < end && std::iswspace(*target))
    ++target;
}

// simple, fast parse for an unsigned long
// target comes from a stored iterator from a string_view,
// end is the end iterator from a string_view,
// value is the optional initial value
uint64_t ParseLong(std::string_view::const_iterator& target,
                   std::string_view::const_iterator end,
                   uint64_t value = 0) {
  for (char c; target < end && (c = *target ^ '0') <= 9; ++target) {
    value = value * 10 + c;
  }
  return value;
}

// simple, fast parse for a ,-separated list of longs, optionally terminated
// with a ;
// target comes from a stored iterator from a string_view
// end is the end iterator from a string_view
std::vector<uint64_t> ParseCSV(std::string_view::const_iterator& target,
                               std::string_view::const_iterator end) {
  std::vector<uint64_t> result;
  while (target < end) {
    if (*target == ';') {
      ++target;
      break;
    }
    if (*target == ',')
      ++target;
    SkipWhitespace(target, end);

    // no number follows, finish
    if ((*target ^ '0') > 9) {
      return result;
    }

    result.emplace_back(ParseLong(target, end));
  }

  return result;
}

// simple, fast parse for a ;-separated list of ,-separated lists of longs,
// optionally terminated with a ;
// target comes from a stored iterator from a string_view
// end is the end iterator from a string_view
std::vector<std::vector<uint64_t>> ParseMultipleCSV(
    std::string_view::const_iterator& target,
    std::string_view::const_iterator end) {
  std::vector<std::vector<uint64_t>> result;
  while (target < end) {
    result.emplace_back(ParseCSV(target, end));
  }

  return result;
}

void SkipNonNumeric(std::string_view::const_iterator& target,
                    std::string_view::const_iterator end) {
  while ((*target ^ '0') > 9) {
    ++target;
  }
}

// simple, fast parse for a ,-separated list of longs, optionally terminated
// with a ;
// target comes from a stored iterator from a string_view
// end is the end iterator from a string_view
gfx::Rect ParseRect(std::string_view::const_iterator& target,
                    std::string_view::const_iterator end) {
  SkipNonNumeric(target, end);
  if (target == end)
    return gfx::Rect();

  long x = ParseLong(target, end);
  SkipNonNumeric(target, end);
  long y = ParseLong(target, end);
  SkipNonNumeric(target, end);
  long w = ParseLong(target, end);
  SkipNonNumeric(target, end);
  long h = ParseLong(target, end);

  return gfx::Rect(x, y, w, h);
}

// simple, fast parse for a ;-separated list of ,-separated lists of longs,
// optionally terminated with a ;
// target comes from a stored iterator from a string_view
// end is the end iterator from a string_view
std::vector<gfx::Rect> ParseMultipleRects(
    std::string_view::const_iterator& target,
    std::string_view::const_iterator end,
    size_t size) {
  std::vector<gfx::Rect> result;
  result.reserve(size);

  while (target < end) {
    result.emplace_back(ParseRect(target, end));
  }

  return result;
}

std::string TypeToEventString(int type) {
  switch (static_cast<LibreOfficeKitCallbackType>(type)) {
    case LOK_CALLBACK_INVALIDATE_TILES:
      return "invalidate_tiles";
    case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR:
      return "invalidate_visible_cursor";
    case LOK_CALLBACK_TEXT_SELECTION:
      return "text_selection";
    case LOK_CALLBACK_TEXT_SELECTION_START:
      return "text_selection_start";
    case LOK_CALLBACK_TEXT_SELECTION_END:
      return "text_selection_end";
    case LOK_CALLBACK_CURSOR_VISIBLE:
      return "cursor_visible";
    case LOK_CALLBACK_VIEW_CURSOR_VISIBLE:
      return "view_cursor_visible";
    case LOK_CALLBACK_GRAPHIC_SELECTION:
      return "graphic_selection";
    case LOK_CALLBACK_GRAPHIC_VIEW_SELECTION:
      return "graphic_view_selection";
    case LOK_CALLBACK_CELL_CURSOR:
      return "cell_cursor";
    case LOK_CALLBACK_HYPERLINK_CLICKED:
      return "hyperlink_clicked";
    case LOK_CALLBACK_MOUSE_POINTER:
      return "mouse_pointer";
    case LOK_CALLBACK_STATE_CHANGED:
      return "state_changed";
    case LOK_CALLBACK_STATUS_INDICATOR_START:
      return "status_indicator_start";
    case LOK_CALLBACK_STATUS_INDICATOR_SET_VALUE:
      return "status_indicator_set_value";
    case LOK_CALLBACK_STATUS_INDICATOR_FINISH:
      return "status_indicator_finish";
    case LOK_CALLBACK_SEARCH_NOT_FOUND:
      return "search_not_found";
    case LOK_CALLBACK_DOCUMENT_SIZE_CHANGED:
      return "document_size_changed";
    case LOK_CALLBACK_SET_PART:
      return "set_part";
    case LOK_CALLBACK_SEARCH_RESULT_SELECTION:
      return "search_result_selection";
    case LOK_CALLBACK_DOCUMENT_PASSWORD:
      return "document_password";
    case LOK_CALLBACK_DOCUMENT_PASSWORD_TO_MODIFY:
      return "document_password_to_modify";
    case LOK_CALLBACK_CONTEXT_MENU:
      return "context_menu";
    case LOK_CALLBACK_INVALIDATE_VIEW_CURSOR:
      return "invalidate_view_cursor";
    case LOK_CALLBACK_TEXT_VIEW_SELECTION:
      return "text_view_selection";
    case LOK_CALLBACK_CELL_VIEW_CURSOR:
      return "cell_view_cursor";
    case LOK_CALLBACK_CELL_ADDRESS:
      return "cell_address";
    case LOK_CALLBACK_CELL_FORMULA:
      return "cell_formula";
    case LOK_CALLBACK_UNO_COMMAND_RESULT:
      return "uno_command_result";
    case LOK_CALLBACK_ERROR:
      return "error";
    case LOK_CALLBACK_VIEW_LOCK:
      return "view_lock";
    case LOK_CALLBACK_REDLINE_TABLE_SIZE_CHANGED:
      return "redline_table_size_changed";
    case LOK_CALLBACK_REDLINE_TABLE_ENTRY_MODIFIED:
      return "redline_table_entry_modified";
    case LOK_CALLBACK_INVALIDATE_HEADER:
      return "invalidate_header";
    case LOK_CALLBACK_COMMENT:
      return "comment";
    case LOK_CALLBACK_RULER_UPDATE:
      return "ruler_update";
    case LOK_CALLBACK_WINDOW:
      return "window";
    case LOK_CALLBACK_VALIDITY_LIST_BUTTON:
      return "validity_list_button";
    case LOK_CALLBACK_VALIDITY_INPUT_HELP:
      return "validity_input_help";
    case LOK_CALLBACK_CLIPBOARD_CHANGED:
      return "clipboard_changed";
    case LOK_CALLBACK_CONTEXT_CHANGED:
      return "context_changed";
    case LOK_CALLBACK_SIGNATURE_STATUS:
      return "signature_status";
    case LOK_CALLBACK_PROFILE_FRAME:
      return "profile_frame";
    case LOK_CALLBACK_CELL_SELECTION_AREA:
      return "cell_selection_area";
    case LOK_CALLBACK_CELL_AUTO_FILL_AREA:
      return "cell_auto_fill_area";
    case LOK_CALLBACK_TABLE_SELECTED:
      return "table_selected";
    case LOK_CALLBACK_REFERENCE_MARKS:
      return "reference_marks";
    case LOK_CALLBACK_JSDIALOG:
      return "jsdialog";
    case LOK_CALLBACK_CALC_FUNCTION_LIST:
      return "calc_function_list";
    case LOK_CALLBACK_TAB_STOP_LIST:
      return "tab_stop_list";
    case LOK_CALLBACK_FORM_FIELD_BUTTON:
      return "form_field_button";
    case LOK_CALLBACK_INVALIDATE_SHEET_GEOMETRY:
      return "invalidate_sheet_geometry";
    case LOK_CALLBACK_DOCUMENT_BACKGROUND_COLOR:
      return "document_background_color";
    case LOK_COMMAND_BLOCKED:
      return "lok_command_blocked";
    case LOK_CALLBACK_SC_FOLLOW_JUMP:
      return "sc_follow_jump";
    case LOK_CALLBACK_CONTENT_CONTROL:
      return "content_control";
    case LOK_CALLBACK_PRINT_RANGES:
      return "print_ranges";
    case LOK_CALLBACK_FONTS_MISSING:
      return "fonts_missing";
    default:
      return "unknown_event";
  }
}

bool IsTypeJSON(int type) {
  switch (static_cast<LibreOfficeKitCallbackType>(type)) {
    case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR:  // INVALIDATE_VISIBLE_CURSOR
                                                  // may also be CSV
    case LOK_CALLBACK_CURSOR_VISIBLE:
    case LOK_CALLBACK_VIEW_CURSOR_VISIBLE:
    case LOK_CALLBACK_GRAPHIC_SELECTION:
    case LOK_CALLBACK_GRAPHIC_VIEW_SELECTION:
    case LOK_CALLBACK_SET_PART:
    case LOK_CALLBACK_SEARCH_RESULT_SELECTION:
    case LOK_CALLBACK_CONTEXT_MENU:
    case LOK_CALLBACK_INVALIDATE_VIEW_CURSOR:
    case LOK_CALLBACK_TEXT_VIEW_SELECTION:
    case LOK_CALLBACK_CELL_VIEW_CURSOR:
    case LOK_CALLBACK_UNO_COMMAND_RESULT:
    case LOK_CALLBACK_ERROR:
    case LOK_CALLBACK_VIEW_LOCK:
    case LOK_CALLBACK_REDLINE_TABLE_SIZE_CHANGED:
    case LOK_CALLBACK_REDLINE_TABLE_ENTRY_MODIFIED:
    case LOK_CALLBACK_COMMENT:
    case LOK_CALLBACK_RULER_UPDATE:
    case LOK_CALLBACK_WINDOW:
    case LOK_CALLBACK_VALIDITY_INPUT_HELP:
    case LOK_CALLBACK_CLIPBOARD_CHANGED:
    case LOK_CALLBACK_REFERENCE_MARKS:
    case LOK_CALLBACK_JSDIALOG:
      // TODO: Not Certain, limited documentation {
    case LOK_CALLBACK_CALC_FUNCTION_LIST:
    case LOK_CALLBACK_TAB_STOP_LIST:
    case LOK_COMMAND_BLOCKED:
    case LOK_CALLBACK_HYPERLINK_CLICKED:
    case LOK_CALLBACK_TABLE_SELECTED:
      // }

    case LOK_CALLBACK_FORM_FIELD_BUTTON:
    case LOK_CALLBACK_CONTENT_CONTROL:
    case LOK_CALLBACK_PRINT_RANGES:
    case LOK_CALLBACK_STATUS_INDICATOR_SET_VALUE:
      return true;
    default:
      return false;
  }
}

/* Is comma-separated number values. A semi-colon indicates a new array */
bool IsTypeCSV(int type) {
  switch (static_cast<LibreOfficeKitCallbackType>(type)) {
    case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR:  // INVALIDATE_VISIBLE_CURSOR
                                                  // may also be JSON
    case LOK_CALLBACK_INVALIDATE_TILES:
    case LOK_CALLBACK_TEXT_SELECTION_START:
    case LOK_CALLBACK_TEXT_SELECTION_END:
    case LOK_CALLBACK_CELL_CURSOR:
    case LOK_CALLBACK_DOCUMENT_SIZE_CHANGED:
    case LOK_CALLBACK_VALIDITY_LIST_BUTTON:
    case LOK_CALLBACK_CELL_SELECTION_AREA:
    case LOK_CALLBACK_CELL_AUTO_FILL_AREA:
    case LOK_CALLBACK_SC_FOLLOW_JUMP:
      return true;
    default:
      return false;
  }
}

/* Is comma-separated number values. A semi-colon indicates a new array */
bool IsTypeMultipleCSV(int type) {
  switch (static_cast<LibreOfficeKitCallbackType>(type)) {
    case LOK_CALLBACK_TEXT_SELECTION:
      return true;
    default:
      return false;
  }
}

std::pair<std::string, std::string> ParseStatusChange(std::string payload) {
  std::string_view sv(payload);
  std::string_view::const_iterator target = sv.begin();
  std::string_view::const_iterator end = sv.end();
  if (target == end || *target == '{')
    return std::make_pair(std::string(), std::string());

  while (target < end && *target != '=')
    ++target;

  return std::make_pair(std::string(sv.begin(), target),
                        std::string(target + 1, end));
}

std::pair<std::string, bool> ParseUnoCommandResult(std::string payload) {
  std::pair<std::string, bool> result;
  // Used to correctly grab the value of commandValues
  std::string commandPreface = "{ \"commandName\": \"";
  result.first =
      payload.substr(commandPreface.length(),
                     payload.find("\", \"success\"") - commandPreface.length());
  // Used to correctly grab the value of success
  std::string successPreface =
      commandPreface + result.first + "\", \"success\": ";
  result.second = payload.substr(successPreface.length(),
                                 payload.find(", \"result\"") -
                                     successPreface.length()) == "true";
  return result;
}

v8::Local<v8::Value> ParseJSON(v8::Isolate* isolate,
                               v8::Local<v8::String> json) {
  if (json->Length() == 0)
    return v8::Null(isolate);

  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  auto maybe_value = v8::JSON::Parse(context, json);

  if (maybe_value.IsEmpty() || try_catch.HasCaught()) {
    v8::Local<v8::Message> message(try_catch.Message());
    if (!message.IsEmpty()) {
      v8::String::Utf8Value utf8(isolate, message->Get());
      v8::String::Utf8Value utf8Json(isolate, json);
      LOG(ERROR) << "Unable to parse callback JSON:"
                 << std::string(*utf8, utf8.length());
      LOG(ERROR) << std::string(*utf8Json, utf8Json.length());
    }

    return v8::Null(isolate);
  }

  return maybe_value.ToLocalChecked();
}

// The weirdest of the types:
// A pair of the ([x,y,width,height,angle], JSON string)
// See docs of LOK_CALLBACK_GRAPHIC_SELECTION enum type for more details
v8::Local<v8::Value> GraphicSelectionPayloadToLocalValue(v8::Isolate* isolate,
                                                         const char* payload) {
  std::string_view payload_sv(payload);
  std::string_view::const_iterator start = payload_sv.begin();
  std::string_view::const_iterator end = payload_sv.end();
  std::vector<uint64_t> numbers = ParseCSV(start, end);
  auto numbers_v8 =
      gin::Converter<std::vector<uint64_t>>::ToV8(isolate, numbers);

  v8::Local<v8::Context> context = v8::Context::New(isolate);
  auto result_array = v8::Array::New(isolate, 2);
  std::ignore = result_array->Set(context, 0, numbers_v8);

  if (start == end) {
    std::ignore = result_array->Set(context, 1, v8::Null(isolate));
    return result_array;
  }

  v8::MaybeLocal<v8::String> maybe_string = v8::String::NewFromUtf8(
      isolate, start, v8::NewStringType::kNormal, end - start);
  if (maybe_string.IsEmpty()) {
    std::ignore = result_array->Set(context, 1, v8::Null(isolate));
    return result_array;
  }
  v8::Local<v8::String> string = maybe_string.ToLocalChecked();
  std::ignore = result_array->Set(context, 1, ParseJSON(isolate, string));
  return result_array;
}

v8::Local<v8::Value> PayloadToLocalValue(v8::Isolate* isolate,
                                         int type,
                                         const char* payload) {
  if (payload == nullptr) {
    return v8::Null(isolate);
  }

  LibreOfficeKitCallbackType cast_type =
      static_cast<LibreOfficeKitCallbackType>(type);
  if (cast_type == LOK_CALLBACK_GRAPHIC_SELECTION) {
    return GraphicSelectionPayloadToLocalValue(isolate, payload);
  }

  // INVALIDATE_VISIBLE_CURSOR may also be JSON, so check if the payload starts
  // with '{'
  if (IsTypeCSV(type) && payload[0] != '{') {
    std::string_view payload_sv(payload);
    std::string_view::const_iterator start = payload_sv.begin();
    std::vector<uint64_t> result = ParseCSV(start, payload_sv.end());
    return gin::Converter<std::vector<uint64_t>>::ToV8(isolate, result);
  }

  if (IsTypeMultipleCSV(type)) {
    std::string_view payload_sv(payload);
    std::string_view::const_iterator start = payload_sv.begin();
    std::vector<std::vector<uint64_t>> result =
        ParseMultipleCSV(start, payload_sv.end());
    return gin::Converter<std::vector<std::vector<uint64_t>>>::ToV8(isolate,
                                                                    result);
  }

  v8::MaybeLocal<v8::String> maybe_string =
      v8::String::NewFromUtf8(isolate, payload);
  if (maybe_string.IsEmpty()) {
    return v8::Null(isolate);
  }

  v8::Local<v8::String> string = maybe_string.ToLocalChecked();
  if (!IsTypeJSON(type) &&
      !(cast_type == LOK_CALLBACK_STATE_CHANGED && payload[0] == '{')) {
    return string;
  }

  return ParseJSON(isolate, string);
}

/* Remaining odd/string types:
    case LOK_CALLBACK_MOUSE_POINTER:
    case LOK_CALLBACK_STATUS_INDICATOR_START:
    case LOK_CALLBACK_STATUS_INDICATOR_FINISH:
    case LOK_CALLBACK_SEARCH_NOT_FOUND:
    case LOK_CALLBACK_DOCUMENT_PASSWORD:
    case LOK_CALLBACK_DOCUMENT_PASSWORD_TO_MODIFY:
    case LOK_CALLBACK_CELL_ADDRESS:
    case LOK_CALLBACK_CELL_FORMULA:
    case LOK_CALLBACK_INVALIDATE_HEADER:
    case LOK_CALLBACK_CONTEXT_CHANGED:
    case LOK_CALLBACK_SIGNATURE_STATUS:
    case LOK_CALLBACK_PROFILE_FRAME:
    case LOK_CALLBACK_INVALIDATE_SHEET_GEOMETRY:
    case LOK_CALLBACK_DOCUMENT_BACKGROUND_COLOR:
*/

}  // namespace electron::office::lok_callback
