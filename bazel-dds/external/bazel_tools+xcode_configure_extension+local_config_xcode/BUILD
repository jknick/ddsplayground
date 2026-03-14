package(default_visibility = ['//visibility:public'])


load("@apple_support//xcode:xcode_config.bzl", "xcode_config")
load("@apple_support//xcode:available_xcodes.bzl", "available_xcodes")
load("@apple_support//xcode:xcode_version.bzl", "xcode_version")
xcode_version(
  name = 'version26_3_0_17C529',
  version = '26.3.0.17C529',
  aliases = ['17C529', '26.3', '26.3.0', '26.3.0.17C529', '26'],
  default_ios_sdk_version = '26.2',
  default_tvos_sdk_version = '26.2',
  default_macos_sdk_version = '26.2',
  default_visionos_sdk_version = '26.2',
  default_watchos_sdk_version = '26.2',
)
xcode_config(
  name = 'host_xcodes',
  versions = [':version26_3_0_17C529'],
  default = ':version26_3_0_17C529',
)
available_xcodes(
  name = 'host_available_xcodes',
  versions = [':version26_3_0_17C529'],
  default = ':version26_3_0_17C529',
)
