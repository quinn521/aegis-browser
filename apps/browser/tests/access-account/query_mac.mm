// Copyright 2026 GCSA
#import "chrome/browser/aegis/access/access_account_mac.h"

#include <cstdio>

// Exercise the production transport and Profile-bound credential reader.
int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 2) return 1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    AegisQueryAccessAccount([NSString stringWithUTF8String:argv[1]], ^(NSDictionary* result) {
      NSData* json = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
      fwrite(json.bytes, 1, json.length, stdout);
      dispatch_semaphore_signal(done);
    });
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC)) != 0;
  }
}
