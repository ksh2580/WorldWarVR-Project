# Manual installer release

The private source repository exposes **Actions → Publish installer → Run
workflow**. This workflow does not run for ordinary commits or pushes. Make and
push as many commits as needed, then trigger it manually from `main` when a new
prerelease should be built.

## One-time GitHub setup

1. Create a fine-grained personal access token owned by `RyanCraighead`.
2. Give it access only to `RyanCraighead/WorldWarVR-Releases`.
3. Give it the repository permission **Contents: Read and write**. No Actions,
   Administration, Issues, or Workflows permission is needed.
4. In the already-created `release` environment in `RyanCraighead/WorldWarVR`,
   add an environment secret named `RELEASES_REPO_TOKEN` containing that token.

The current GitHub plan does not expose deployment-branch rules for this
private environment. The workflow therefore enforces the boundary itself: it
refuses to build or publish unless it was manually dispatched from `main` in
the private `RyanCraighead/WorldWarVR` repository by `RyanCraighead`.

The source workflow's normal `GITHUB_TOKEN` cannot publish a release in a
different repository. The narrowly scoped secret is exposed only to the clean
publish job after the build and test job succeeds.

## Publishing a build

1. Commit and push all desired source changes to `main`.
2. Open the source repository's **Actions** tab.
3. Select **Publish installer** and choose **Run workflow**.
4. Enter a new version such as `0.4.0-alpha.2`.
5. Enable the confirmation checkbox and run it from `main`.

The current native compatibility line is `0.4.x`, so the workflow accepts only
`0.4.x` release versions until that native version is deliberately advanced.

The workflow checks out that exact commit, initializes the OpenXR submodule,
runs the full native and managed test suites, builds the standalone launcher,
restores only the dependency graph recorded in the committed NuGet lock files,
validates the installer payload, and uses the pinned Inno Setup compiler. It
then creates a new published **prerelease** in `WorldWarVR-Releases` containing
only `WorldWarVR-Setup.exe`.

Versions must be new. If that tag already exists, the workflow stops instead of
overwriting the existing release. It records the exact private source commit
and installer SHA-256 in the release notes. The installer is currently
unsigned, so Windows reputation warnings are expected for prerelease builds.

The private source repository must remain private. The releases repository may
be private now and made public later; the workflow accepts either visibility
but always verifies its exact repository identity. Making that repository
public also makes its already-published prereleases visible to everyone. A
failed run that has already created a draft is deliberately not overwritten.
Delete that exact failed draft manually or choose another version before
retrying.
