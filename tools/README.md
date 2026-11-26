# v71 APK patch and signing tools

Run the local pipeline after downloading the v71 ARM64 library artifact:

```bash
bash tools/build_patch_sign.sh BASE.apk libshotmeter-v71.so OUTPUT_SIGNED.apk
```

The base APK must already contain the v70 automatic-startup dependency patch and
`lib/arm64-v8a/libshotmeter.so`. The patcher replaces only that library and
removes stale APK v1 signature metadata. The pipeline then zipaligns, signs,
and verifies the result with Android build-tools.

## Signing identity

On first run, the script creates a persistent local key at
`~/.config/shotmeter/signing/release.jks` and a private password file in the
same directory. Back up both securely. Every later release must reuse that
same key to install as an in-place update.

If the installed v70 APK was signed with a different ZArchiver key, Android
will reject an update signed with this newly generated key. In that case,
either supply the original keystore and password through
`SHOTMETER_KEYSTORE`, `SHOTMETER_KEYSTORE_PASSWORD`, and optionally
`SHOTMETER_KEY_ALIAS`, or uninstall the old app before installing the new
signing identity. Do not delete or lose the release keystore after adopting it.

The keystore and password are local-only and are excluded from Git.
