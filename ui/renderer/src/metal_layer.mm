// Validates the CAMetalLayer handed over by the Swift bridge.
#import <QuartzCore/CAMetalLayer.h>

extern "C" bool tsg_is_metal_layer(void* layer) {
  return layer != nullptr && [(__bridge id)layer isKindOfClass:[CAMetalLayer class]];
}
