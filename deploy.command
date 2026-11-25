#!/bin/sh
# Build (Release) → test → self-contained "DB Manager.app" (same steps as .github/workflows/macos.yml)
# → replace /Applications/DB Manager.app → open it. Double-click in Finder, or run ./deploy.command
set -e
cd "$(dirname "$0")"
# two runs at once delete each other's files mid-bundle and install a broken app
mkdir build-release.lock 2>/dev/null || { echo "deploy is already running (or remove build-release.lock)"; exit 1; }
trap 'rmdir build-release.lock' EXIT
B=$(brew --prefix)

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$B/opt/qt;$B/opt/qtkeychain;$B/opt/libpq"
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure

APP="build-release/DB Manager.app"
rm -rf "$APP"
cp -R build-release/db-manager.app "$APP"
# the QtVirtualKeyboard "Cannot resolve rpath" errors are for a plugin this app doesn't use
"$B/opt/qt/bin/macdeployqt" "$APP" -libpath="$B/lib"
# Sign with a stable self-signed identity, not ad-hoc (-): the keychain ties saved passwords to the
# app's signature, and an ad-hoc one changes every build, so macOS asked for each password again.
# Created once in the login keychain; click "Always Allow" on the next prompt and it stays allowed.
SIGN_ID="DB Manager Dev"
if ! security find-certificate -c "$SIGN_ID" >/dev/null 2>&1; then
    T=$(mktemp -d)
    # /usr/bin/openssl (LibreSSL): its .p12 imports; Homebrew OpenSSL 3+ needs -legacy
    /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 36500 -subj "/CN=$SIGN_ID" \
        -keyout "$T/k.pem" -out "$T/c.pem" \
        -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=critical,codeSigning"
    /usr/bin/openssl pkcs12 -export -inkey "$T/k.pem" -in "$T/c.pem" -out "$T/id.p12" -passout pass:x
    security import "$T/id.p12" -P x -T /usr/bin/codesign
    rm -rf "$T"
fi
codesign --force --deep --sign "$SIGN_ID" "$APP"

# quit the running copy; it asks first if a grid has unsaved edits, and Cancel stops the install
osascript -e 'quit app "DB Manager"' 2>/dev/null || true
for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -qf "DB Manager.app/Contents/MacOS" || break; sleep 1; done
if pgrep -qf "DB Manager.app/Contents/MacOS"; then
    echo "DB Manager is still running (unsaved edits?): not replaced. Quit it and run again."
    exit 1
fi
rm -rf "/Applications/DB Manager.app"
cp -R "$APP" /Applications/
open "/Applications/DB Manager.app"
echo "Installed and opened /Applications/DB Manager.app"
