%{!?mirage_version:%global mirage_version 1.0.7}

Name:           miragewallpaper
Version:        %{mirage_version}
Release:        1%{?dist}
Summary:        Wallpaper Engine-compatible desktop wallpaper manager
License:        GPL-3.0-or-later
URL:            https://github.com/laobamac/MirageWallpaper
Source0:        %{name}-%{mirage_version}.tar.gz

BuildRequires:  cmake
BuildRequires:  clang
BuildRequires:  desktop-file-utils
BuildRequires:  dotnet-sdk-10.0
BuildRequires:  gcc-c++
BuildRequires:  glslang
BuildRequires:  libX11-devel
BuildRequires:  libass-devel
BuildRequires:  alsa-lib-devel
BuildRequires:  dbus-devel
BuildRequires:  ffmpeg-free-devel
BuildRequires:  fontconfig-devel
BuildRequires:  freetype-devel
BuildRequires:  libdrm-devel
BuildRequires:  libglvnd-devel
BuildRequires:  lttng-ust
BuildRequires:  mpv-libs-devel
BuildRequires:  libplacebo-devel
BuildRequires:  pulseaudio-libs-devel
BuildRequires:  lz4-devel
BuildRequires:  mesa-libEGL-devel
BuildRequires:  mesa-libGLES-devel
BuildRequires:  ninja-build
BuildRequires:  pipewire-devel
BuildRequires:  pkgconf-pkg-config
BuildRequires:  qt6-qt5compat-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtshadertools-devel
BuildRequires:  qt6-qtwayland-devel
BuildRequires:  qt6-qtwebchannel-devel
BuildRequires:  qt6-qtwebengine-devel
BuildRequires:  vulkan-headers
BuildRequires:  vulkan-loader-devel
BuildRequires:  libxkbcommon-devel

Requires:       qt6-qtbase
Requires:       qt6-qtdeclarative
Requires:       qt6-qtquickcontrols2
Requires:       qt6-qt5compat
Requires:       qt6-qtwayland
Requires:       qt6-qtwebengine
Requires:       qt6-qtwebchannel
Requires:       vulkan-loader
Requires:       mpv-libs
Requires:       lttng-ust
Requires:       dotnet-runtime-10.0

%description
MirageWallpaper is a Qt desktop application that manages Wallpaper Engine
projects and drives the mirage-display desktop wallpaper protocol.

This package contains the Qt application, scene/video/web renderer hosts,
shared wallpaper assets, and SteamKit2 service integration using Fedora's
dotnet-runtime-10.0 package.

%prep
%autosetup -n MirageWallpaper-%{mirage_version}

%build
test -x "$CC"
test -x "$CXX"
# Fedora's RPM flags include GCC-only -specs files. Preserve the distribution
# hardening flags, but suppress Clang's driver-only diagnostic so the project's
# required -Werror still applies to diagnostics from source code. SceneRenderer
# uses C++ modules; Clang 21 cannot link imported fortified libc declarations,
# and Fedora's automatic LTO has the same module-linking limitation. Disable
# only those incompatible options for this renderer; the other RPM hardening
# options remain in effect.
cmake -S SceneRenderer -B %{_builddir}/mirage-scene -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_INIT='-Wno-unused-command-line-argument -fno-lto -Xclang -U_FORTIFY_SOURCE -Xclang -D_FORTIFY_SOURCE=0' \
    -DCMAKE_CXX_FLAGS_INIT='-Wno-unused-command-line-argument -fno-lto -Xclang -U_FORTIFY_SOURCE -Xclang -D_FORTIFY_SOURCE=0' \
    -DCMAKE_LINKER_TYPE=LLD \
    -DSCENERENDERER_BUILD_WALLPAPER_HOST=ON
cmake -S VideoRenderer -B %{_builddir}/mirage-video -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_INIT=-Wno-unused-command-line-argument \
    -DCMAKE_CXX_FLAGS_INIT=-Wno-unused-command-line-argument \
    -DCMAKE_LINKER_TYPE=LLD \
    -DVIDEORENDERER_BUILD_VIEWER=OFF
cmake -S WebRenderer -B %{_builddir}/mirage-web -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_INIT=-Wno-unused-command-line-argument \
    -DCMAKE_CXX_FLAGS_INIT=-Wno-unused-command-line-argument \
    -DCMAKE_LINKER_TYPE=LLD \
    -DWEBRENDERER_BUILD_VIEWER=OFF
cmake -S MirageQt -B %{_builddir}/mirage-qt -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_INIT=-Wno-unused-command-line-argument \
    -DCMAKE_CXX_FLAGS_INIT=-Wno-unused-command-line-argument \
    -DCMAKE_LINKER_TYPE=LLD \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DMIRAGEQT_RUNTIME_DIR=%{_libexecdir}/miragewallpaper \
    -DMIRAGEQT_DATA_DIR=%{_datadir}/miragewallpaper
cmake --build %{_builddir}/mirage-scene --parallel %{?_smp_build_ncpus}
cmake --build %{_builddir}/mirage-video --parallel %{?_smp_build_ncpus}
cmake --build %{_builddir}/mirage-web --parallel %{?_smp_build_ncpus}
cmake --build %{_builddir}/mirage-qt --parallel %{?_smp_build_ncpus}
bash scripts/build_steam_service_linux.sh %{_builddir}/mirage-steamservice

%install
# Use MirageQt's top-level install script so CMake replaces its build-time
# empty RUNPATH with the package RUNPATH. Local-only mode excludes vendored
# subprojects, whose development outputs are not part of this runtime package.
cmake -DCMAKE_INSTALL_PREFIX=%{buildroot}%{_prefix} \
    -DCMAKE_INSTALL_LOCAL_ONLY=1 \
    -P %{_builddir}/mirage-qt/cmake_install.cmake
install -Dm755 %{_builddir}/mirage-scene/Tools/SceneWallpaper/SceneWallpaper \
    %{buildroot}%{_libexecdir}/miragewallpaper/SceneWallpaper
install -Dm755 %{_builddir}/mirage-video/Tools/VideoWallpaper/VideoWallpaper \
    %{buildroot}%{_libexecdir}/miragewallpaper/VideoWallpaper
install -Dm755 %{_builddir}/mirage-web/Tools/WebWallpaper/WebWallpaper \
    %{buildroot}%{_libexecdir}/miragewallpaper/WebWallpaper
install -d %{buildroot}%{_datadir}/miragewallpaper
cp -a assets %{buildroot}%{_datadir}/miragewallpaper/assets
# SteamServiceManager resolves the FHS bundle under linux-x64, matching the
# repository build layout and preventing a packaged runtime path mismatch.
# Fedora owns the .NET host and runtime through dotnet-runtime-10.0; copying
# those ELF files would duplicate their Build-IDs and conflict with that RPM.
# Keep only the service application and licenses in the private bundle, then
# provide the expected runtime/dotnet entry point as a system-dotnet launcher.
install -d %{buildroot}%{_libexecdir}/miragewallpaper/SteamService/linux-x64
cp -a %{_builddir}/mirage-steamservice/app \
    %{buildroot}%{_libexecdir}/miragewallpaper/SteamService/linux-x64/
cp -a %{_builddir}/mirage-steamservice/Licenses \
    %{buildroot}%{_libexecdir}/miragewallpaper/SteamService/linux-x64/
install -d %{buildroot}%{_libexecdir}/miragewallpaper/SteamService/linux-x64/runtime
printf '%s\n' '#!/bin/sh' 'unset DOTNET_ROOT' 'exec %{_bindir}/dotnet "$@"' \
    > %{buildroot}%{_libexecdir}/miragewallpaper/SteamService/linux-x64/runtime/dotnet
chmod 0755 %{buildroot}%{_libexecdir}/miragewallpaper/SteamService/linux-x64/runtime/dotnet
install -Dm644 LICENSE %{buildroot}%{_docdir}/%{name}/LICENSE
install -Dm644 MirageQt/Vendor/FluentUI/License \
    %{buildroot}%{_docdir}/%{name}/FluentUI-MIT.txt
install -d %{buildroot}%{_datadir}/licenses/%{name}
find %{_builddir}/mirage-steamservice/Licenses -maxdepth 1 -type f \
    -exec install -m644 {} %{buildroot}%{_datadir}/licenses/%{name}/ \;
desktop-file-validate %{buildroot}%{_datadir}/applications/mirageqt.desktop

%files
%license %{_docdir}/%{name}/LICENSE
%license %{_docdir}/%{name}/FluentUI-MIT.txt
%license %{_datadir}/licenses/%{name}/*
%{_bindir}/MirageQt
%{_libexecdir}/miragewallpaper/
%{_datadir}/miragewallpaper/
%{_datadir}/applications/mirageqt.desktop
%{_datadir}/icons/hicolor/512x512/apps/mirageqt.png

%changelog
* Sun Aug 16 2026 elysia-best <a.elysia@proton.me> - 1.0.7-1
- Initial native Fedora package.
