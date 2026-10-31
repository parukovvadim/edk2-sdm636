[Defines]
  VENDOR_NAME                    = Xiaomi
  PLATFORM_NAME                  = whyred
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Qualcomm/sdm660/sdm660.fdf
  DEVICE_DXE_FV_COMPONENTS       = Platform/Xiaomi/sdm660/whyred.fdf.inc

!include Platform/Qualcomm/sdm660/sdm660.dsc

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS = -DENABLE_SIMPLE_INIT

[PcdsFixedAtBuild.common]
  gQcomTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gQcomTokenSpaceGuid.PcdMipiFrameBufferHeight|2160

  # Auto-boot directly into XnuLoader
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0x01, 0x5e, 0x37, 0x8a, 0x8f, 0x09, 0x78, 0x4b, 0xa6, 0xb1, 0x38, 0xa2, 0xd4, 0xf6, 0xe1, 0xc0 }
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|1

  # Simple Init
  gSimpleInitTokenSpaceGuid.PcdGuiDefaultDPI|350

  gRenegadePkgTokenSpaceGuid.PcdDeviceVendor|"Redmi"
  gRenegadePkgTokenSpaceGuid.PcdDeviceProduct|"Note5"
  gRenegadePkgTokenSpaceGuid.PcdDeviceCodeName|"whyred"

[Components.common]
  Platform/RenegadePkg/Application/XnuLoader/XnuLoader.inf
