# Packaging and code signing

The release packages are built by `.github/workflows/build.yml` (see the comment at its top) with
`scripts/package.py`. This page covers how the Windows packages are signed.

## Windows code signing

The Windows packages (`SafeHouse-Launcher-Windows-<arch>-Setup.exe` and `-Portable.zip`, x86_64 and arm64) can
be Authenticode-signed in one of two ways. Without either, they are published unsigned, as before. Nothing has to be
changed in the workflow to switch: it depends only on which secrets and variables the repository has.

### 1. SignPath (preferred when configured)

[SignPath](https://signpath.io) signs on its own service: the certificate never leaves SignPath, and every signing
request of a release is approved by hand in SignPath. CI uses SignPath's official GitHub Action,
`signpath/github-action-submit-signing-request` (pinned by commit, v3.0).

Repository settings (Settings > Secrets and variables > Actions):

| Kind | Name | Value |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | API token of a SignPath CI user that may submit to the project |
| Variable | `SIGNPATH_ORGANIZATION_ID` | the organization ID (SignPath: Organization > Settings) |
| Variable | `SIGNPATH_PROJECT_SLUG` | the project's slug, for example `SafeHouse` |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | `release-signing` (or `test-signing` while trying it out) |
| Variable (optional) | `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG` | the artifact configuration below, when it is not the project's default |
| Variable (optional) | `SIGNPATH_WAIT_SECONDS` | how long each request waits for its approval (default 3600) |

SignPath is used only for releases (pushes to `main` of FridiNaTor1/SafeHouse), and only when the secret and the
three required variables are all set; pull requests and forks build as before. When SignPath is configured, the
certificate secrets below are ignored.

What a release does then:

1. The `windows` job (per architecture) builds and stops after the package folder
   (`scripts/package.py --windows-stage only`), and uploads it with its CMake cache (`windows-<arch>-stage`).
2. The `windows-sign` job collects every `.exe` of both package folders and submits them as one signing request
   (artifact `windows-unsigned-programs`). After the approval, the signed programs replace the unsigned ones.
3. It makes the portable zips and the setups from the signed folders (`scripts/package.py --windows-stage reuse`)
   and submits both setups as a second signing request (artifact `windows-unsigned-setups`).
4. The signed setups and the zips become the `windows-<arch>-release` artifacts. `windows-smoke` tests them and the
   release publishes them, as it does unsigned packages.

So every release needs two approvals in SignPath (programs, then setups). Until both are approved the run waits
(at most `SIGNPATH_WAIT_SECONDS` per request), and later pushes to `main` queue behind it. If a request is denied
or times out, the release is not published; run the workflow again (or push again) to retry.

Not signed: the uninstaller inside the setup (NSIS writes it while it compiles the setup, so it cannot be sent to
SignPath on its own; the setup installs for the current user, no administrator prompt) and the libraries from other
projects (SDL2, FFmpeg, the OpenXR loader, the LLVM/MinGW runtime, XAudio2), which are not ours to sign.

#### SignPath project setup

Artifact configuration (both requests use it: their zips hold the programs one or two folders deep):

```xml
<?xml version="1.0" encoding="utf-8"?>
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <parameters>
    <parameter name="version" />
  </parameters>
  <zip-file>
    <pe-file-set>
      <include path="**/*.exe" max-matches="unbounded" product-name="SafeHouse" product-version="${version}" />
      <for-each>
        <authenticode-sign />
      </for-each>
    </pe-file-set>
  </zip-file>
</artifact-configuration>
```

The workflow passes the release version (`x.y.z`) as the `version` parameter. The product name / version
restrictions check the file properties: the programs get them from `cmake/windows-version.rc.in`
(`cane_windows_resources` in `CMakeLists.txt`), the setup from `scripts/windows/setup.nsi`. Every program in the
package needs them, or the request is rejected: `Sly3-Release.exe` does not have them yet (add
`cane_windows_resources(Sly3-Release ...)` and check the Windows build before the first SignPath release).

Signing policy `release-signing`: approval required for every request, the GitHub trusted build system linked to the
FridiNaTor1/SafeHouse repository, and the source branch restricted to `main`. Try the whole flow first with a
`test-signing` policy (SignPath's test certificate): set `SIGNPATH_SIGNING_POLICY_SLUG` to `test-signing`, push,
approve, check the run, then switch the variable to `release-signing`.

### 2. A certificate of your own (PKCS#12)

Secrets `WINDOWS_SIGNING_CERT_BASE64` (the `.pfx` file, base64) and `WINDOWS_SIGNING_CERT_PASSWORD`. The `windows`
job decodes the certificate and `scripts/windows/sign.py` signs with osslsigncode while packaging: every program
of the package, the uninstaller (`!uninstfinalize`) and the setup (`!finalize`). The same works locally:

```sh
SAFEHOUSE_WINDOWS_SIGN_PFX=/path/to/cert.pfx SAFEHOUSE_WINDOWS_SIGN_PASSWORD=... \
  python3 scripts/package.py --build build/windows-cross --output dist --windows
```

## scripts/package.py, Windows options

- `--windows-package both|portable|setup`: which packages to make (default both).
- `--windows-stage full|only|reuse`: `full` (default) installs the build into the package folder and packs it;
  `only` stops after the package folder; `reuse` packs an existing package folder (for example after its programs
  were signed elsewhere) without installing again. `--build` must still point at a folder with the build's
  `CMakeCache.txt` (the setup's version).
