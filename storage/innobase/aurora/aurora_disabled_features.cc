/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Disabled Features Implementation

*****************************************************************************/

#include "aurora_disabled_features.h"
#include "aurora_sysvars.h"

namespace aurora {

DisabledFeature get_disabled_features() {
  // In Aurora mode, disable specific features
  if (srv_aurora_mode) {
    // Use preset based on instance mode
    return DisabledFeature::WRITER_PRESET;
  }
  
  return DisabledFeature::NONE;
}

}  // namespace aurora
