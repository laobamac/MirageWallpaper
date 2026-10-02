<!-- Copyright © 2026 王孝慈. All rights reserved. -->
```sh
sudo apt update
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
  qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-templates \
  qml6-module-qtquick-shapes qml6-module-qtqml-workerscript qt6-image-formats-plugins
cmake -S LinuxUI -B LinuxUI/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build LinuxUI/build --parallel
./LinuxUI/build/Mirage
```
