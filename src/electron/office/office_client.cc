// Copyright (c) 2022 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "office/office_client.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

#include "LibreOfficeKit/LibreOfficeKit.hxx"
#include "base/bind.h"
#include "base/callback_forward.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/native_library.h"
#include "base/notreached.h"
#include "base/stl_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/token.h"
#include "gin/converter.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "gin/per_isolate_data.h"
#include "office/document_client.h"
#include "office/document_holder.h"
#include "office/lok_callback.h"
#include "office/office_instance.h"
#include "office/promise.h"
#include "unov8.hxx"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace electron::office {

namespace {
static base::NoDestructor<base::ThreadLocalOwnedPointer<OfficeClient>> lazy_tls;
}  // namespace

gin::WrapperInfo OfficeClient::kWrapperInfo = {gin::kEmbedderNativeGin};

void OfficeClient::OnLoaded(lok::Office* client) {
  office_ = client;
  ::UnoV8Instance::Set(client->getUnoV8());
  loaded_.Signal();
}

v8::Local<v8::Value> OfficeClient::GetHandle(v8::Isolate* isolate) {
  return self_.Get(isolate);
}

// instance
namespace {
static void GetOfficeHandle(v8::Local<v8::Name> name,
                            const v8::PropertyCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  if (!isolate) {
    NOTREACHED();
    return;
  }
  v8::Local<v8::Context> context;
  if (!info.This()->GetCreationContext().ToLocal(&context)) {
    NOTREACHED();
    return;
  }
  v8::Context::Scope context_scope(context);
  v8::HandleScope handle_scope(isolate);
  v8::MicrotasksScope microtasks_scope(
      isolate, v8::MicrotasksScope::kDoNotRunMicrotasks);

  DCHECK(lazy_tls->Get());
  info.GetReturnValue().Set(lazy_tls->Get()->GetHandle(isolate));
}
}  // namespace

// static
void OfficeClient::InstallToContext(v8::Local<v8::Context> context) {
  v8::Context::Scope context_scope(context);
  v8::Isolate* isolate = context->GetIsolate();
  v8::MicrotasksScope microtasks_scope(
      isolate, v8::MicrotasksScope::kDoNotRunMicrotasks);

  auto client = std::make_unique<OfficeClient>();
  v8::Local<v8::Object> wrapper;
  if (!client->GetWrapper(isolate).ToLocal(&wrapper) || wrapper.IsEmpty()) {
    NOTREACHED();
  }
  client->self_.Reset(isolate, wrapper);
  lazy_tls->Set(std::move(client));

  context->Global()
      ->SetAccessor(
          context, gin::StringToV8(isolate, office::OfficeClient::kGlobalEntry),
          GetOfficeHandle, nullptr, v8::MaybeLocal<v8::Value>(),
          v8::AccessControl::ALL_CAN_READ, v8::PropertyAttribute::ReadOnly)
      .Check();
}

void OfficeClient::Unset() {
  context_.Reset();
  self_.Reset();
}

// static
void OfficeClient::RemoveFromContext(v8::Local<v8::Context> /*context*/) {
  if (lazy_tls->Get())
    lazy_tls->Get()->Unset();
  lazy_tls->Set(nullptr);
}

OfficeClient::OfficeClient()
    : task_runner_(base::SequencedTaskRunnerHandle::Get()) {
  auto* inst = OfficeInstance::Get();
  if (inst)
    inst->AddLoadObserver(this);
}

OfficeClient::~OfficeClient() {
  auto* inst = OfficeInstance::Get();
  if (inst)
    inst->RemoveLoadObserver(this);
};

gin::ObjectTemplateBuilder OfficeClient::GetObjectTemplateBuilder(
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
      .SetMethod("setDocumentPassword", &OfficeClient::SetDocumentPasswordAsync)
      .SetMethod("loadDocument", &OfficeClient::LoadDocumentAsync)
      .SetMethod("loadDocumentFromArrayBuffer",
                 &OfficeClient::LoadDocumentFromArrayBuffer)
      .SetMethod("__handleBeforeUnload", &OfficeClient::HandleBeforeUnload);
}

const char* OfficeClient::GetTypeName() {
  return "OfficeClient";
}

lok::Office* OfficeClient::GetOffice() const {
  return office_;
}

std::string OfficeClient::GetLastError() {
  char* err = GetOffice()->getError();
  if (err == nullptr) {
    return std::string();
  }
  std::string result(err);
  GetOffice()->freeError(err);
  return result;
}

namespace {
void ResolveLoadWithDocumentClient(const base::WeakPtr<OfficeClient>& client,
                                   Promise<DocumentClient> promise,
                                   const std::string& path,
                                   lok::Document* doc) {
  if (!client.MaybeValid())
    return;  // don't resolve the promise, the v8 context probably doesn't exist
  if (!doc) {
    promise.Resolve();
    return;
  }

  auto* doc_client = new DocumentClient(DocumentHolderWithView(doc, path));

  promise.Resolve(doc_client);
}

// high priority IO, don't block on renderer thread sequence
void PostBlockingAsync(base::OnceClosure closure,
                       const base::Location& from_here = FROM_HERE) {
  base::ThreadPool::PostTask(
      from_here, {base::TaskPriority::USER_VISIBLE, base::MayBlock()},
      std::move(closure));
}

}  // namespace

v8::Local<v8::Promise> OfficeClient::LoadDocumentAsync(
    v8::Isolate* isolate,
    const std::string& path) {
  Promise<DocumentClient> promise(isolate);
  auto promise_handle = promise.GetHandle();

  auto load_ = base::BindOnce(
      [](OfficeClient* client, const std::string& path) {
        if (client->GetOffice()) {
          return client->GetOffice()->documentLoad(path.c_str(),
                                                   "Language=en-US,Batch=true");
        } else {
          return static_cast<lok::Document*>(nullptr);
        }
      },
      base::Unretained(this), std::string(path));
  auto complete_ =
      base::BindOnce(&ResolveLoadWithDocumentClient, weak_factory_.GetWeakPtr(),
                     std::move(promise), std::string(path));
  auto async_ = std::move(load_).Then(
      base::BindPostTask(task_runner_, std::move(complete_)));

  if (loaded_.is_signaled()) {
    PostBlockingAsync(std::move(async_));
  } else {
    auto deferred_ = base::BindOnce(
        [](decltype(async_) async) { PostBlockingAsync(std::move(async)); },
        std::move(async_));
    loaded_.Post(FROM_HERE, std::move(deferred_));
  }

  return promise_handle;
}

v8::Local<v8::Promise> OfficeClient::LoadDocumentFromArrayBuffer(
    v8::Isolate* isolate,
    v8::Local<v8::ArrayBuffer> array_buffer) {
  Promise<DocumentClient> promise(isolate);
  auto promise_handle = promise.GetHandle();

  if (array_buffer->ByteLength() == 0) {
    LOG(ERROR) << "Empty array buffer provided";
    promise.Resolve();
    return promise_handle;
  }
  std::shared_ptr<v8::BackingStore> backing_store =
      array_buffer->GetBackingStore();
  const std::string path = "memory://" + base::Token::CreateRandom().ToString();

  auto load_ = base::BindOnce(
      [](const base::WeakPtr<OfficeClient>& office_client, v8::Isolate* isolate,
         std::shared_ptr<v8::BackingStore> backing_store) {
        return office_client->GetOffice()->loadFromMemory(
            static_cast<char*>(backing_store->Data()),
            backing_store->ByteLength());
      },
      weak_factory_.GetWeakPtr(), isolate, backing_store);

  auto complete_ =
      base::BindOnce(&ResolveLoadWithDocumentClient, weak_factory_.GetWeakPtr(),
                     std::move(promise), path);
  auto async_ = std::move(load_).Then(
      base::BindPostTask(task_runner_, std::move(complete_)));

  if (loaded_.is_signaled()) {
    // high priority IO, don't block on renderer thread sequence
    base::ThreadPool::PostTask(
        FROM_HERE, {base::TaskPriority::USER_VISIBLE, base::MayBlock()},
        std::move(async_));
  } else {
    auto deferred_ = base::BindOnce(
        [](decltype(async_) async) {
          // high priority IO, don't block on renderer thread sequence
          base::ThreadPool::PostTask(
              FROM_HERE, {base::TaskPriority::USER_VISIBLE, base::MayBlock()},
              std::move(async));
        },
        std::move(async_));

    loaded_.Post(FROM_HERE, std::move(deferred_));
  }

  return promise_handle;
}

v8::Local<v8::Promise> OfficeClient::SetDocumentPasswordAsync(
    v8::Isolate* isolate,
    const std::string& url,
    const std::string& password) {
  Promise<void> promise(isolate);
  auto handle = promise.GetHandle();

  if (loaded_.is_signaled()) {
    GetOffice()->setDocumentPassword(url.c_str(), password.c_str());
    promise.Resolve();
  } else {
    auto async = base::BindOnce(
        [](const base::WeakPtr<OfficeClient>& client, Promise<void> promise,
           const std::string& url, const std::string& password) {
          if (client.MaybeValid()) {
            client->GetOffice()->setDocumentPassword(url.c_str(),
                                                     password.c_str());
          }
          promise.Resolve();
        },
        weak_factory_.GetWeakPtr(), std::move(promise), std::move(url),
        std::move(password));

    loaded_.Post(FROM_HERE, base::BindPostTask(task_runner_, std::move(async)));
  }

  return handle;
}

base::WeakPtr<OfficeClient> OfficeClient::GetWeakPtr() {
  if (!lazy_tls->Get())
    return {};
  return lazy_tls->Get()->weak_factory_.GetWeakPtr();
}

// This is the only place where the OfficeInstance should be used directly
void OfficeClient::HandleBeforeUnload() {
  auto* inst = OfficeInstance::Get();
  if (inst)
    inst->HandleClientDestroyed();
}

}  // namespace electron::office
