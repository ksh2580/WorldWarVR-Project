# Third-party notices and research provenance

World War VR first-party code is licensed under GPL-3.0-only as stated in the
root `LICENSE`. The following
components and references are not governed by that first-party license.

## Khronos OpenXR SDK and loader

- Source: <https://github.com/KhronosGroup/OpenXR-SDK>
- Pinned revision: `64f2b37c8c6da3d83c9b4d11865ba1fb752cb8ec`
- SDK version: 1.1.60
- Upstream license expression: Apache-2.0 OR MIT for the relevant loader files
- Selected compliance path and packaged terms: Apache-2.0 in
  `LICENSE-OPENXR-SDK.txt`

The x86 OpenXR loader is statically linked into `WorldWarVR.dll`.

## JsonCpp

- Source: vendored by the pinned OpenXR SDK under
  `src/external/jsoncpp`
- OpenXR-SDK tree object:
  `ee75252fe4102ad31f0a0aecd9c4cfba6adc2dcf`
- Version: 1.9.6
- License: Public Domain and/or MIT, as described by the upstream authors
- Packaged terms: `LICENSE-JSONCPP.txt`

JsonCpp implementation objects are statically linked through the OpenXR
loader.

## Vulkan headers detected by the pre-cleanup build

The audited pre-cleanup compiler graph included Vulkan SDK 1.4.350 header
paths. Current World War VR configuration disables Vulkan package discovery
for deterministic D3D11-only builds. The upstream CMake may still locate a
standalone shader compiler, but the clean audited build contains no Vulkan SDK
path in a generated compile or link input.

## Microsoft toolchain and Windows libraries

Release binaries contain the Release `/MT` MSVC static runtime from toolset
14.44.35207 and link against the Windows SDK 10.0.26100.0 x86 UCRT static
library and UM import libraries. No `/MTd` debug runtime enters a release
artifact and no Microsoft `.lib` file is shipped separately. Microsoft permits
distribution of the resulting qualifying Visual Studio and Windows SDK
Distributable Code subject to the applicable product licenses and conditions.

The Build Tools license alone is not the redistribution grant for this
proprietary application. Publication therefore remains blocked until the
final candidate is built under a valid qualifying Visual Studio 2022
Community, Professional, or Enterprise license and that basis is recorded.
The Windows SDK conditions also apply to the UCRT and import-library link
results, including protective downstream terms, Windows-platform use, no
implied Microsoft endorsement, and the developer obligations recorded in the
first-party license.

- Build Tools terms:
  <https://visualstudio.microsoft.com/license-terms/vs2022-ga-diagnosticbuildtools/>
- Visual Studio 2022 redistribution list:
  <https://learn.microsoft.com/visualstudio/releases/2022/redistribution>
- Visual Studio Community 2022 terms:
  <https://visualstudio.microsoft.com/license-terms/vs2022-ga-community/>
- Windows SDK redistributable-code list:
  <https://learn.microsoft.com/en-us/legal/windows-sdk/redist>

The corresponding Windows, Direct3D 9, Direct3D 11, DXGI, D3DCompiler, and
other Win32 runtime DLL implementations remain system-provided and are not
packaged as part of World War VR.

## t4-rtx research reference

- Source: <https://github.com/xoxor4d/t4-rtx>
- Audited revision: `ade9b2fe9d4101c093169bcc4fee86e88bef7296`
- Repository license at the audited revision: none found
- Release role: none; factual reverse-engineering research only

No t4-rtx implementation, shader, asset, object, or binary is copied or
packaged.

## Call of Duty: World at War and optional PeZBOT

World War VR does not include the game executable, fastfiles, IWD archives,
maps, videos, or other proprietary game content. Users must supply a lawfully
obtained compatible copy. PeZBOT is optional and user-supplied; World War VR
does not distribute its archive or extracted files.

Call of Duty and related names and marks are property of their respective
owners. World War VR is not affiliated with or endorsed by Activision.
