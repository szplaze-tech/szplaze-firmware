#!/bin/bash
# P0 bugfix build script (ASCII only; sketch dir = $PWD)
CLI="C:/Users/Administrator/AppData/Local/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
FQBN="esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=huge_app"
"$CLI" compile --fqbn "$FQBN" "$PWD" --build-path "$PWD/build_p0"
echo "EXIT=$?"
