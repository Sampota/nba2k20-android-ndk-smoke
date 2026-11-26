# Patch APK Tool (Android)

Build channel: standalone Android utility app.

A standalone Android app for replacing a native shared library inside an APK and signing the result.

## App features

- Pick a base APK from any document-provider location.
- Pick a replacement `.so` file independently; future versions of the library can be selected without rebuilding the app.
- Choose the destination ABI: `arm64-v8a`, `armeabi-v7a`, `x86_64`, or `x86`.
- Choose the library entry name inside the APK. It defaults to `libshotmeter.so`, so an input such as `libshotmeter-v72.so` is written to `lib/<ABI>/libshotmeter.so`.
- Choose an output folder using Android's folder picker. The original file is never overwritten and a unique output name is chosen.
- Native libraries are stored uncompressed and aligned; stale V1 signature metadata is removed before signing.
- Output APKs are signed with V1, V2, and V3 and checked with apksig before they are saved.

## Build

From this folder, use Android Gradle Plugin 8.9.1, Gradle 8.11.1, JDK 17, and Android SDK platform 35:

```sh
gradle --no-daemon clean assembleDebug
```

The debug APK is created at:

```
app/build/outputs/apk/debug/app-debug.apk
```

GitHub Actions builds this APK and publishes it as the `patch-apk-tool-debug-apk` artifact.

## Important signing note

The patcher generates its own RSA signing identity the first time it signs an APK and stores it in the app's private data directory. Keep the app installed and do not clear its data if you need later patched APKs to have the same signing certificate. Android will not install a patched game APK as an in-place update when its signing certificate changes. Use the in-app Back Up Signing Identity and Restore Signing Identity controls before reinstalling or clearing the app's data. The backup ZIP contains an unencrypted private key: keep it private and never share it.

The patcher's signing identity is separate from the certificate used to sign/install the patcher app itself. This build is a debug-distribution build, not a Play Store release.

## Compatibility

The selected library must be a valid little-endian ELF shared object matching the selected ABI. The APK must be a regular installable APK, not a split-APK set or an APK bundle. This first version does not rewrite Android manifests or native-library references; the selected entry name must match the library name expected by the app being patched.

A successful patch/sign operation validates the archive, verifies that the inserted library matches the selected bytes, and verifies the resulting APK signature. It cannot guarantee that a replacement library is compatible with the target application's native code or will work at runtime.
