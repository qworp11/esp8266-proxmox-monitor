$ErrorActionPreference = "Continue"
$cli = "E:\nodemcuv3\tools\arduino-cli.exe"

Write-Output "=== COMPILE ==="
& $cli compile --fqbn esp8266:esp8266:nodemcuv2 --libraries "E:\nodemcuv3\libs" --build-property "runtime.tools.ctags.path=E:\nodemcuv3\tools\ctags" --output-dir "E:\nodemcuv3\build" "E:\nodemcuv3\monitor_server"

if ($LASTEXITCODE -ne 0) {
    Write-Output "COMPILE_FAILED"
    exit 1
}
Write-Output "=== UPLOAD ==="
& $cli upload -p COM3 --fqbn esp8266:esp8266:nodemcuv2 --input-dir "E:\nodemcuv3\build" -t
if ($LASTEXITCODE -ne 0) {
    Write-Output "UPLOAD_FAILED"
    exit 1
}
Write-Output "=== DONE ==="
