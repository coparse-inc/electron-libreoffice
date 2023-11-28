// Copyright (c) 2023 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#pragma once

#include "base/observer_list_types.h"

namespace electron::office {
class DestroyedObserver : public base::CheckedObserver {
 public:
  virtual void OnDestroyed() = 0;
};
}  // namespace electron::office

