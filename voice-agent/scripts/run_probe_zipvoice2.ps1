$P = 'C:\Users\ted\Desktop\talk2agent\voice-agent'
$mdl = Join-Path $P 'models\tts\sherpa-onnx-zipvoice-distill-int8-zh-en-emilia'
$voc = Join-Path $P 'models\tts\vocos_24khz.onnx'
$exe = Join-Path $P 'build-msvc\probe_zipvoice2.exe'
$case = Join-Path $P 'scripts\probe_case.txt'

# 三个内置音色依次测（case 文件里只放 UTF-8 文本，避免命令行编码问题）
foreach ($wav in @('news-female.wav', 'news-female-2.wav', 'leijun-1.wav')) {
  $c = Get-Content $case -Raw -Encoding UTF8
  $c = $c -replace '<ref>.*?</ref>', "<ref>$wav</ref>"
  $tmp = Join-Path $env:TEMP "probe_case_$wav.txt"
  [System.IO.File]::WriteAllText($tmp, $c, (New-Object System.Text.UTF8Encoding $false))
  Write-Output "===== $wav ====="
  & $exe $mdl $voc $tmp 2>&1 | Where-Object { $_ -notmatch 'Gb2312|SplitUtf8|CategoryInfo|FullyQualified|^\s*\+|^所在位置' }
  Write-Output ''
}