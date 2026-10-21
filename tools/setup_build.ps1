$ErrorActionPreference = "Stop"
$cli = "E:\nodemcuv3\tools\arduino-cli.exe"
$ProgressPreference = "SilentlyContinue"

if (-not (Test-Path $cli)) {
    Invoke-WebRequest -Uri "https://github.com/arduino/arduino-cli/releases/download/v1.5.1/arduino-cli_1.5.1_Windows_64bit.zip" -OutFile "$env:TEMP\arduino-cli.zip" -UserAgent "ps"
    Expand-Archive "$env:TEMP\arduino-cli.zip" -DestinationPath "E:\nodemcuv3\tools" -Force
}

& $cli config init --overwrite
& $cli config add board_manager.additional_urls http://arduino.esp8266.com/stable/package_esp8266com_index.json
& $cli core update-index
& $cli core install esp8266:esp8266
& $cli lib install "Adafruit SSD1306"
& $cli lib install "Adafruit GFX Library"
& $cli lib install "ESP8266Ping"
Write-Output "SETUP_DONE"
