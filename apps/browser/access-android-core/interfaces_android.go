//go:build android

package main

/*
#include <android/api-level.h>
*/
import "C"

import "github.com/wlynxg/anet"

func init() {
	// Android 11 restricts RTM_GETLINK for apps. Select anet's address-based
	// enumeration before any core can use Pion's network interface adapter.
	if C.android_get_device_api_level() >= 30 {
		anet.SetAndroidVersion(11)
	}
}
