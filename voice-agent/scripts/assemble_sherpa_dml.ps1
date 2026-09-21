$ErrorActionPreference = "Stop"

$root   = "C:\Users\ted\Desktop\talk2agent\voice-agent"
$build  = "$root\third_party\sherpa-onnx-dml-build"
$dest   = "$root\third_party\sherpa-onnx-dml"
$ort    = "$root\third_party\onnxruntime-win-x64-directml-1.24.4"

# 1) 头文件（与 DML 版编译对应）
$inc = "$dest\include\sherpa-onnx\c-api"
New-Item -ItemType Directory -Force -Path $inc | Out-Null
Copy-Item "$root\third_party\sherpa-onnx-src\sherpa-onnx\c-api\c-api.h"   $inc
Copy-Item "$root\third_party\sherpa-onnx-src\sherpa-onnx\c-api\cxx-api.h" $inc

# 2) 库与 DLL
New-Item -ItemType Directory -Force -Path "$dest\lib" | Out-Null
$capiLib = Get-ChildItem "$build" -Recurse -Filter "sherpa-onnx-c-api.lib" | Select-Object -First 1
if (-not $capiLib) { throw "sherpa-onnx-c-api.lib not found in $build" }
Copy-Item $capiLib.FullName "$dest\lib\sherpa-onnx-c-api.lib"

$capiDll = Get-ChildItem "$build" -Recurse -Filter "sherpa-onnx-c-api.dll" | Select-Object -First 1
if (-not $capiDll) { throw "sherpa-onnx-c-api.dll not found in $build" }
Copy-Item $capiDll.FullName "$dest\lib\sherpa-onnx-c-api.dll"

# 3) DirectML 运行时（ORT DML 版依赖）
foreach ($dll in @("onnxruntime.dll", "DirectML.dll", "onnxruntime_providers_shared.dll")) {
    $src = Get-ChildItem "$build" -Recurse -Filter $dll | Select-Object -First 1
    if ($src) { Copy-Item $src.FullName "$dest\lib\$dll" }
    elseif (Test-Path "$ort\runtimes\win-x64\native\$dll") {
        Copy-Item "$ort\runtimes\win-x64\native\$dll" "$dest\lib\$dll"
    } else {
        Write-Warning "DLL not found: $dll"
    }
}

Get-ChildItem "$dest\lib" | Select-Object Name, Length
Write-Host "sherpa-onnx-dml assembled at $dest"
