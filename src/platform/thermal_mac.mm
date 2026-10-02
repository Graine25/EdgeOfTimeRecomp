#include "platform/thermal.h"

#import <Foundation/Foundation.h>

namespace eot::platform {

int ThermalState() {
  @autoreleasepool {
    return static_cast<int>([NSProcessInfo processInfo].thermalState);
  }
}

}
