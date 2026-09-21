param([string]$Version = "1.13.0")

$ErrorActionPreference = "Stop"
$root = "C:\Users\ted\Desktop\talk2agent\voice-agent"
$tmp  = "$env:TEMP\directml-$Version"

New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$nupkg = "$tmp\directml.nupkg"
if (-not (Test-Path $nupkg)) {
    Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/$Version" -OutFile $nupkg -UseBasicParsing
}
$zip = "$tmp\directml.zip"
Copy-Item $nupkg $zip -Force
Expand-Archive -Path $zip -DestinationPath "$tmp\extract" -Force

$dll = "$tmp\extract\bin\x64-win\DirectML.dll"
if (-not (Test-Path $dll)) { throw "DirectML.dll not found in package" }

$dllInfo = (Get-Item $dll).VersionInfo
"Version: $($dllInfo.FileVersion) ($(Split-Path -Leaf $dll)) size=$([math]::Round((Get-Item $dll).Length/1MB,1))MB"

# 部署到 exe 目录 + sherpa-onnx-dml/lib（供后续构建自动拷贝）
Copy-Item $dll "$root\build-msvc\DirectML.dll" -Force
Copy-Item $dll "$root\third_party\sherpa-onnx-dml\lib\DirectML.dll" -Force
"Deployed DirectML.dll $Version to build-msvc and sherpa-onnx-dml/lib"
