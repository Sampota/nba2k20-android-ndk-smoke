#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 3 ]]; then
  echo "Usage: bash tools/build_patch_sign.sh BASE.apk libshotmeter.so OUTPUT_SIGNED.apk" >&2
  exit 2
fi
BASE_APK="$1"; SHOTMETER_SO="$2"; OUTPUT_APK="$3"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [[ -z "$SDK" || ! -d "$SDK/build-tools" ]]; then
  echo "Set ANDROID_SDK_ROOT or ANDROID_HOME to an Android SDK installation." >&2
  exit 1
fi
BT="$(for d in "$SDK"/build-tools/*; do [[ -d "$d" ]] && basename "$d"; done | sort -V | tail -n1)"
ZIPALIGN="$SDK/build-tools/$BT/zipalign"
APKSIGNER="$SDK/build-tools/$BT/apksigner"
for tool in "$ZIPALIGN" "$APKSIGNER" keytool python3; do
  command -v "$tool" >/dev/null 2>&1 || [[ -x "$tool" ]] || { echo "Missing tool: $tool" >&2; exit 1; }
done
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 "$ROOT/tools/patch_apk.py" "$BASE_APK" "$SHOTMETER_SO" "$TMP/patched.apk"
"$ZIPALIGN" -f -p 4 "$TMP/patched.apk" "$TMP/aligned.apk"

# A local key is generated once and reused. Back up this private directory.
KEY_DIR="${SHOTMETER_SIGNING_DIR:-$HOME/.config/shotmeter/signing}"
mkdir -p "$KEY_DIR"; chmod 700 "$KEY_DIR"
KEYSTORE="${SHOTMETER_KEYSTORE:-$KEY_DIR/release.jks}"
ALIAS="${SHOTMETER_KEY_ALIAS:-shotmeter-release}"
PASS_FILE="$KEY_DIR/keystore.pass"
if [[ ! -f "$KEYSTORE" ]]; then
  if [[ ! -f "$PASS_FILE" ]]; then
    python3 -c 'import secrets; print(secrets.token_urlsafe(36))' > "$PASS_FILE"
    chmod 600 "$PASS_FILE"
  fi
  PASS="$(cat "$PASS_FILE")"
  keytool -genkeypair -noprompt -keystore "$KEYSTORE" -storepass "$PASS" -keypass "$PASS" \
    -alias "$ALIAS" -dname "CN=Shotmeter Release, OU=Android, O=Local Build, C=PH" \
    -keyalg RSA -keysize 3072 -validity 10000
  chmod 600 "$KEYSTORE"
fi
if [[ -n "${SHOTMETER_KEYSTORE:-}" ]]; then
  : "${SHOTMETER_KEYSTORE_PASSWORD:?Set SHOTMETER_KEYSTORE_PASSWORD for your existing signing key}"
  PASS="$SHOTMETER_KEYSTORE_PASSWORD"
else
  [[ -f "$PASS_FILE" ]] || { echo "Signing password file missing: $PASS_FILE" >&2; exit 1; }
  PASS="$(cat "$PASS_FILE")"
fi
mkdir -p "$(dirname "$OUTPUT_APK")"
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-key-alias "$ALIAS" \
  --ks-pass "pass:$PASS" --key-pass "pass:$PASS" --out "$OUTPUT_APK" "$TMP/aligned.apk"
"$APKSIGNER" verify --verbose --print-certs "$OUTPUT_APK"
echo "Signed APK ready: $OUTPUT_APK"
echo "Signing key: $KEYSTORE (keep it safe; future updates must reuse it)"
