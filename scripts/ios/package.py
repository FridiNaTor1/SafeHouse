#!/usr/bin/env python3
"""Assembles SafeHouse.app from an iOS build (cmake/ios.cmake) and packs it as an .ipa (.github/workflows/ios.yml).

The app: the SafeHouse executable, the frameworks (SafeHouseCore, SafeHouseLauncher, CaneExtractor, Sly2Release and
its copy Sly2Tools, which runs Sly 2's install tools so the game's own image starts clean), Info.plist, the icon
(packaging/ios/AppIcon-1024.png through actool) and the licences. No game data: the player installs from their own
disc image inside the app.

Signing: ad hoc ('-', the default: the simulator, and sideloading tools such as AltStore / SideStore sign it again),
or with --identity (a keychain identity) and --profile (a .mobileprovision) for a device build signed by its team.

  package.py --build build/ios-device --sdk iphoneos --out dist/ios [--version 0.2.1] [--ipa]
"""
import argparse
import os
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

PRODUCT = "SafeHouse"
FRAMEWORKS = ("SafeHouseCore", "SafeHouseLauncher", "CaneExtractor", "Sly2Release")
MINIMUM_OS = "15.0"


def run(*args, **kwargs):
    print("+", " ".join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def project_version(source):
    match = re.search(r"project\(ProjectCane VERSION ([0-9.]+)", (source / "CMakeLists.txt").read_text())
    return match.group(1) if match else "0.0.0"


def platform_name(sdk):
    return "iPhoneSimulator" if sdk == "iphonesimulator" else "iPhoneOS"


def framework_plist(name, identifier, version, sdk):
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": name,
        "CFBundleIdentifier": identifier,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name,
        "CFBundlePackageType": "FMWK",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "CFBundleSupportedPlatforms": [platform_name(sdk)],
        "MinimumOSVersion": MINIMUM_OS,
        "UIDeviceFamily": [1, 2],
    }


def app_plist(bundle_id, version, build, sdk):
    # (landscape first: the launch orientation; the app itself turns only to landscape on a phone-shaped screen,
    # SDL_HINT_ORIENTATIONS from platform/ios/ios_app.mm orientations())
    every = ["UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight",
             "UIInterfaceOrientationPortrait", "UIInterfaceOrientationPortraitUpsideDown"]
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": PRODUCT,
        "CFBundleExecutable": PRODUCT,
        "CFBundleIdentifier": bundle_id,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": PRODUCT,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": build,
        "CFBundleSupportedPlatforms": [platform_name(sdk)],
        "DTPlatformName": sdk,
        "LSRequiresIPhoneOS": True,
        "MinimumOSVersion": MINIMUM_OS,
        "UIDeviceFamily": [1, 2],
        "UIRequiredDeviceCapabilities": ["arm64", "metal"],
        # (a launch screen: without one iOS runs the app letterboxed at an old phone's size)
        "UILaunchScreen": {},
        # Every orientation on iPhone and iPad: an app that declares only some runs in a compatibility frame
        # (letterboxed, zoomable) on screens that are not phone-shaped: iPad windows (Split View, Slide Over, Stage
        # Manager) and a foldable iPhone's near-square inner screen. At run time a phone-shaped screen stays landscape.
        "UISupportedInterfaceOrientations": every,
        "UISupportedInterfaceOrientations~ipad": every,
        # (any window size: the launcher and the game lay out for every size and aspect, and follow live resizes)
        "UIRequiresFullScreen": False,
        # The UIScene life cycle (platform/ios/ios_app.mm SHSceneDelegate; SDL's windows are put into the scene): what
        # the iOS 27 SDK requires to start, and what new screens (a foldable's inner screen) need for their native size.
        "UIApplicationSceneManifest": {
            "UIApplicationSupportsMultipleScenes": False,
            "UISceneConfigurations": {
                "UIWindowSceneSessionRoleApplication": [
                    {"UISceneConfigurationName": "Default Configuration", "UISceneDelegateClassName": "SHSceneDelegate"}
                ]
            },
        },
        # ProMotion (120 Hz) iPhones: without this key iOS caps every app's Core Animation frames (the game's
        # CAMetalLayer, the launcher's GL view, SDL's display link) at 60 Hz. iPads allow 120 Hz without it.
        "CADisableMinimumFrameDurationOnPhone": True,
        "UIStatusBarHidden": True,
        "UIViewControllerBasedStatusBarAppearance": False,
        "UIApplicationSupportsIndirectInputEvents": True,
        # the profile (installations, saves, settings, logs, mods) is the app's Documents folder, shown in Files
        "UIFileSharingEnabled": True,
        "LSSupportsOpeningDocumentsInPlace": True,
        # controllers (Xbox, PlayStation, MFi, Switch Pro) and iOS 18's Game Mode
        "GCSupportsControllerUserInteraction": True,
        "GCSupportedGameControllers": [{"ProfileName": "ExtendedGamepad"}],
        "GCSupportsGameMode": True,
        "NSBluetoothAlwaysUsageDescription": "Bluetooth game controllers",
        "NSMicrophoneUsageDescription": "Sly 2's USB headset feature can use a microphone.",
        "ITSAppUsesNonExemptEncryption": False,
    }


def sdk_keys(sdk):
    """The DT* keys Xcode writes (the SDK and Xcode the app was built with), from xcrun; none without it."""
    keys = {}
    try:
        def ask(*args):
            return subprocess.run(["xcrun", *args], check=True, capture_output=True, text=True).stdout.strip()
        sdk_version = ask("--sdk", sdk, "--show-sdk-version")
        keys["DTSDKName"] = f"{sdk}{sdk_version}"
        keys["DTPlatformVersion"] = sdk_version
        keys["DTSDKBuild"] = ask("--sdk", sdk, "--show-sdk-build-version")
        xcode = subprocess.run(["xcodebuild", "-version"], check=True, capture_output=True, text=True).stdout.split()
        # ("Xcode 26.6 Build version 17F113": DTXcode 2660, DTXcodeBuild 17F113)
        parts = (xcode[1].split(".") + ["0", "0"])[:3]
        keys["DTXcode"] = f"{int(parts[0]):02d}{int(parts[1])}{int(parts[2])}"
        keys["DTXcodeBuild"] = xcode[-1]
    except (subprocess.CalledProcessError, FileNotFoundError, IndexError, ValueError) as failure:
        print(f"no SDK keys ({failure})", flush=True)
    print(f"SDK keys: {keys}", flush=True)
    return keys


def compile_icon(source, app, sdk, work):
    """The asset catalog (Assets.car) with the 1024 px icon; returns the Info.plist keys actool adds."""
    catalog = work / "Assets.xcassets"
    icon_set = catalog / "AppIcon.appiconset"
    icon_set.mkdir(parents=True)
    (catalog / "Contents.json").write_text('{"info":{"author":"xcode","version":1}}\n')
    shutil.copy2(source / "packaging/ios/AppIcon-1024.png", icon_set / "AppIcon-1024.png")
    (icon_set / "Contents.json").write_text(
        '{"images":[{"filename":"AppIcon-1024.png","idiom":"universal","platform":"ios","size":"1024x1024"}],'
        '"info":{"author":"xcode","version":1}}\n')
    partial = work / "icon.plist"
    try:
        run("xcrun", "actool", catalog, "--compile", app, "--platform", sdk, "--minimum-deployment-target", MINIMUM_OS,
            "--app-icon", "AppIcon", "--output-partial-info-plist", partial, "--target-device", "iphone",
            "--target-device", "ipad", "--output-format", "human-readable-text", "--notices", "--warnings")
        return plistlib.loads(partial.read_bytes())
    except (subprocess.CalledProcessError, FileNotFoundError) as failure:
        # (without actool: plain PNG icons, which iOS still reads)
        print(f"actool failed ({failure}); using plain PNG icons", flush=True)
        files = []
        for name, size in (("AppIcon60x60@2x.png", 120), ("AppIcon60x60@3x.png", 180), ("AppIcon76x76@2x~ipad.png", 152),
                           ("AppIcon83.5x83.5@2x~ipad.png", 167)):
            run("sips", "-z", size, size, source / "packaging/ios/AppIcon-1024.png", "--out", app / name)
            files.append(name.split("@")[0])
        return {"CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": sorted(set(files))}}}


def sign(path, identity, entitlements=None):
    args = ["codesign", "--force", "--sign", identity, "--timestamp=none"]
    if entitlements:
        args += ["--entitlements", entitlements]
    run(*args, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--sdk", choices=("iphoneos", "iphonesimulator"), default="iphoneos")
    parser.add_argument("--bundle-id", default=os.environ.get("SAFEHOUSE_IOS_BUNDLE_ID") or "org.safehouse.ios")
    parser.add_argument("--version", default=os.environ.get("SAFEHOUSE_VERSION") or "")
    parser.add_argument("--build-number", default=os.environ.get("GITHUB_RUN_NUMBER") or "1")
    parser.add_argument("--identity", default="-", help="codesign identity ('-': ad hoc)")
    parser.add_argument("--profile", type=pathlib.Path, help="a .mobileprovision to embed (with --identity)")
    parser.add_argument("--ipa", action="store_true", help="also pack SafeHouse-iOS.ipa")
    parser.add_argument("--ffmpeg", type=pathlib.Path, help="the FFmpeg prefix (its licence goes into the app)")
    options = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parent.parent.parent
    version = options.version or project_version(source)
    build = options.build.resolve()
    out = options.out.resolve()
    app = out / f"{PRODUCT}.app"
    shutil.rmtree(app, ignore_errors=True)
    (app / "Frameworks").mkdir(parents=True)

    shutil.copy2(build / "bin" / PRODUCT, app / PRODUCT)
    for name in FRAMEWORKS:
        built = build / "frameworks" / f"{name}.framework"
        if not built.is_dir():
            sys.exit(f"missing {built}")
        shutil.copytree(built, app / "Frameworks" / f"{name}.framework", symlinks=True)
    # Sly2Tools: Sly 2's image again under its own name (ios_app.mm runs the install tools there)
    tools = app / "Frameworks" / "Sly2Tools.framework"
    shutil.copytree(app / "Frameworks" / "Sly2Release.framework", tools, symlinks=True)
    (tools / "Sly2Release").rename(tools / "Sly2Tools")
    run("install_name_tool", "-id", "@rpath/Sly2Tools.framework/Sly2Tools", tools / "Sly2Tools")
    for framework in sorted((app / "Frameworks").glob("*.framework")):
        name = framework.stem
        for stale in ("_CodeSignature", "Headers", "Modules"):
            shutil.rmtree(framework / stale, ignore_errors=True)
        identifier = f"{options.bundle_id}.{name.lower()}"
        (framework / "Info.plist").write_bytes(plistlib.dumps(framework_plist(name, identifier, version, options.sdk)))

    info = app_plist(options.bundle_id, version, options.build_number, options.sdk)
    info.update(sdk_keys(options.sdk))
    with tempfile.TemporaryDirectory() as temporary:
        info.update(compile_icon(source, app, options.sdk, pathlib.Path(temporary)))
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    (app / "PkgInfo").write_text("APPL????")

    licenses = app / "licenses"
    shutil.copytree(source / "licenses", licenses)
    shutil.copy2(source / "THIRD_PARTY_NOTICES.md", app / "THIRD_PARTY_NOTICES.md")
    for name, path in (("sdl2", build / "_deps/sdl2-src/LICENSE.txt"), ("faudio", build / "_deps/faudio-src/LICENSE")):
        if path.is_file():
            (licenses / name).mkdir(exist_ok=True)
            shutil.copy2(path, licenses / name / path.name)
    if options.ffmpeg and (options.ffmpeg / "COPYING.LGPLv2.1").is_file():
        (licenses / "ffmpeg").mkdir(exist_ok=True)
        shutil.copy2(options.ffmpeg / "COPYING.LGPLv2.1", licenses / "ffmpeg" / "COPYING.LGPLv2.1")

    entitlements = None
    if options.profile:
        shutil.copy2(options.profile, app / "embedded.mobileprovision")
        decoded = subprocess.run(["security", "cms", "-D", "-i", str(options.profile)], check=True, capture_output=True).stdout
        profile = plistlib.loads(decoded)
        entitlements = out / "entitlements.plist"
        entitlements.write_bytes(plistlib.dumps(profile.get("Entitlements", {})))
        print(f"profile: {profile.get('Name')} ({profile.get('TeamName')}), expires {profile.get('ExpirationDate')}", flush=True)
    for framework in sorted((app / "Frameworks").glob("*.framework")):
        sign(framework, options.identity)
    sign(app, options.identity, entitlements)
    run("codesign", "--verify", "--deep", "--strict", "--verbose=2", app)

    total = sum(f.stat().st_size for f in app.rglob("*") if f.is_file())
    print(f"{app}: {total / 1e6:.1f} MB", flush=True)
    for f in sorted(app.rglob("*")):
        if f.is_file() and f.stat().st_size > 1_000_000:
            print(f"  {f.relative_to(app)}  {f.stat().st_size / 1e6:.1f} MB")
    if options.ipa:
        stage = out / "ipa"
        shutil.rmtree(stage, ignore_errors=True)
        (stage / "Payload").mkdir(parents=True)
        run("ditto", app, stage / "Payload" / app.name)
        ipa = out / f"{PRODUCT}-iOS.ipa"
        ipa.unlink(missing_ok=True)
        run("zip", "-qry", ipa, "Payload", cwd=stage)
        shutil.rmtree(stage)
        print(f"{ipa}: {ipa.stat().st_size / 1e6:.1f} MB", flush=True)


if __name__ == "__main__":
    main()
