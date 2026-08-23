# VR Mod Launcher handoff

WorldAtWarVR's exact proposed launcher identity is `world-at-war-vr`, displayed
as **Call of Duty: World at War VR**, with proposed managed-storage key `c`.
The launcher must keep the product quarantined until every blocker in
`distribution/world-at-war-vr/product-handoff.json` is resolved.

The authoritative machine-readable inputs are:

- `distribution/world-at-war-vr/product-handoff.json`
- `distribution/world-at-war-vr/runtime-contract.json`
- `distribution/world-at-war-vr/game-compatibility.json`
- `distribution/world-at-war-vr/payload-policy.json`
- `distribution/world-at-war-vr/licenses/components.json`

## Current unsigned candidate

Two clean builds using pinned CMake `3.31.6-msvc6`, MSVC `19.44.35216.0`,
linker `14.44.35207`, and Windows SDK `10.0.26100.0` passed all 22 tests and
produced byte-identical payload binaries. The standalone packages and
deterministic unsigned `.vrmod` archives are also byte-identical between
passes.

The exact current binary, package, archive, and inventory sizes and SHA-256
values are recorded only in `product-handoff.json` so this narrative cannot
silently retain superseded release hashes.

The payload contains only the wrapper, injected DLL, exact first-party PeZBOT
importer, license texts, notices, component inventory, and the explicit source
provenance record. It contains no game, Plutonium, PeZBOT, proxy DLL, developer
tool, test, PDB, or log.

## Initial launch contract

The initial platform variant is only the stock frontend. The trusted launcher
main process resolves all paths and starts the x86 wrapper from
`<stage>/g/wawvr`; renderer input supplies no path or argument. SP and MP
runtime/home directories live under `<stage>/x` and `<stage>/h`, never under
the read-only managed game root `<stage>/g`.

The wrapper receives the exact SP and MP executable paths. During SP prepare it
atomically writes identity-bound V2 content to the historical
`WaWVR-Multiplayer-Handoff.v1` filename inside the SP runtime. The
stock menu's no-argument `CoDWaWmp.exe` shim requires the exact sibling
`CoDWaW.exe` parent, waits for it to exit, and reconstructs MP only from that
canonical config. It performs no fallback discovery for game/source/runtime/
home paths and disables PeZBOT archive discovery/import; it only inspects the
already prepared MP home.

The trusted platform has a separate fixed prepare plan in
`runtime-contract.json`: `--prepare` plus the same resolution, game, SP source,
MP source, SP runtime, SP home, and DLL paths as frontend launch, with
`--launch`, `--menu`, and `--wait` omitted. Install/update/verify may run only
that non-launching plan. The adapter must enforce that every resolved path is
under the intended stage, record the prepared config identity, and revalidate
it before explicit launch. Those same-stage and receipt-integrity checks stay
platform responsibilities because standalone WaWVR deliberately supports an
E-drive game with C-drive isolated runtime/home paths. RCVR integration
remains blocked until the adapter implements them.

This is a 32-bit OpenXR product. The launcher needs a new strict x86 preflight
using `/reg:32`, `oculus_openxr_32.json`, an x86 signed Meta runtime library,
current-session `OVRServer_x64` and `OculusDash`, and the six-process SteamVR
reject set specified in `runtime-contract.json`. Existing x64 product variants
must not be weakened or optionalized.

## Readiness split

A standalone Nexus or GitHub public alpha does not depend on RCVR catalog,
commerce, adapter, runtime-guard, or signing work. It remains blocked on the
general release items recorded in `product-handoff.json`: first-party
ownership confirmation, a final build under a valid qualifying Visual Studio
2022 license, verification against the claimed English Steam target, final
headset/package smoke testing, and a reviewed commit and tag.

RCVR managed-launcher integration has additional platform-only blockers: the
product adapter, x86 OpenXR preflight, catalog and entitlement policy, release
metadata, runtime-guard support, signing, and publication authorization.

Do not upload, sign, sell, publish, or treat this internal candidate as a live
launcher release.
