#!/usr/bin/env bash
# PhoneMic: сборка Android APK (требуется JDK 17 + Android SDK, ANDROID_HOME задан).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/android-app"

if [ ! -d "$HOME/.gradle" ]; then echo "gradle не найден локально — CI сделает это за вас"; fi

# Если нет wrapper jar — используем установленный gradle или создаём wrapper:
if [ ! -f gradle/wrapper/gradle-wrapper.jar ]; then
  if command -v gradle >/dev/null; then
    gradle wrapper --gradle-version 8.9
  else
    echo "Нужен gradle (apt install gradle | sdkman) для генерации wrapper"; exit 1
  fi
fi

./gradlew :app:assembleRelease -PciSigning
mkdir -p "$ROOT/artifacts"
cp app/build/outputs/apk/release/app-release.apk "$ROOT/artifacts/PhoneMic-android.apk" 2>/dev/null || true
echo "APK: $ROOT/artifacts/PhoneMic-android.apk"
