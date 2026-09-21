$ErrorActionPreference = "Stop"

Import-Module "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\2022\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

$root = "C:\Users\ted\Desktop\talk2agent\voice-agent"
$env:SHERPA_ONNXRUNTIME_INCLUDE_DIR = "$root\third_party\onnxruntime-win-x64-directml-1.24.4\build\native\include"
$env:SHERPA_ONNXRUNTIME_LIB_DIR     = "$root\third_party\onnxruntime-win-x64-directml-1.24.4\runtimes\win-x64\native"

cmake -S "$root\third_party\sherpa-onnx-src" -B "$root\third_party\sherpa-onnx-dml-build" -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DSHERPA_ONNX_ENABLE_DIRECTML=ON `
    -DBUILD_SHARED_LIBS=ON `
    -DSHERPA_ONNX_ENABLE_TESTS=OFF `
    -DSHERPA_ONNX_ENABLE_PYTHON=OFF `
    -DSHERPA_ONNX_ENABLE_BINARY=OFF `
    -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF `
    -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF `
    -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF `
    -DSHERPA_ONNX_ENABLE_JNI=OFF `
    -DSHERPA_ONNX_BUILD_C_API_EXAMPLES=OFF

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build "$root\third_party\sherpa-onnx-dml-build" -j
exit $LASTEXITCODE
