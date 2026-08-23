# Private release repository checklist

Target repository: `RyanCraighead/WorldWarVR-Releases`

Keep the repository **private** and any GitHub release marked as a **pre-release** until every required test and media item below is complete. This checklist is internal and should not be copied into the public-facing repository.

## Repository content

- [ ] Clone or fetch the private release repository into a separate checkout.
- [ ] Confirm the repository is still private before pushing.
- [ ] Keep only `README.md`, `REQUIREMENTS.md`, `CONTROLS.md`, and
      `KNOWN-ISSUES.md` in the repository tree.
- [ ] Keep the product and dependency licenses inside the installer rather than
      duplicating the product license in the releases repository.
- [ ] Keep source-provenance records and third-party notices out of the
      repository tree. Required vendor terms belong inside the installer.

## Build and installer

- [ ] Produce a clean self-contained launcher payload containing the required UI/runtime files and genuine dependency terms under `licenses\`.
- [ ] Confirm the payload contains no unapproved research source, t4-rtx, GPL-provenance,
      corresponding-source, or publication-blocker claims outside verbatim
      third-party license and notice files.
- [ ] Confirm the payload contains no Call of Duty executable or asset and no third-party bot archive/files.
- [ ] Build with pinned Inno Setup 6.7.3 using `installer/build-installer.ps1`.
- [ ] Resolve the compiler's **Non-commercial use only** restriction before enabling Nexus Donation Points, paid distribution, sponsorship, or any other monetization; use a suitably licensed compiler or alternative if needed.
- [ ] Confirm the only custom product asset is exactly `WorldWarVR-Setup.exe`.
- [ ] Record the final installer SHA-256 in the draft release notes.
- [ ] Authenticode-sign the installer, or explicitly label the pre-release unsigned and expect Windows reputation warnings.
- [ ] Verify the signature and hash after downloading the draft asset back from GitHub.

## Clean-machine acceptance

- [ ] Install as a standard Windows user without administrator elevation.
- [ ] Confirm the Start Menu has one World War VR application entry plus uninstall support.
- [ ] Confirm automatic Steam detection and manual Browse selection.
- [ ] Confirm invalid game folders are rejected with a useful message.
- [ ] Confirm settings persist across launcher restarts.
- [ ] Confirm all three VR resolution presets reach the expected backend value.
- [ ] Confirm the bots checkbox disables and enables bot setup as selected.
- [ ] Test Zombies launch, menu panel, gameplay VR, death/respawn, pause, and exit.
- [ ] Test local/offline multiplayer launch, team/class selection, one known-good map/mode, bots off, and bots on.
- [ ] Smoke-test Campaign without claiming completion beyond what was actually tested.
- [ ] Confirm online multiplayer is neither advertised nor used.
- [ ] Uninstall and confirm the game installation remains untouched.

## GitHub pre-release

- [ ] Publish it as a **pre-release**, not a stable release.
- [ ] Attach only `WorldWarVR-Setup.exe` as the custom product asset.
- [ ] Verify the mode-status language matches README and Known Issues.
- [ ] Keep the repository private until Ryan explicitly chooses to make it public.

## Nexus package

- [ ] Upload the same verified `WorldWarVR-Setup.exe`; do not create a different installer build.
- [ ] Reuse the truthful support table and known-issues language.
- [ ] Include the same SHA-256 and version number.
- [ ] Link bug reports to the GitHub Issues form after the repository is public.
