---
title: Community & Feedback
description: Report issues, contribute, sponsor the project, and learn about the license and acknowledgments.
---

Mirage is an open-source project, and feedback and participation are welcome. It's still in its early stages, so your bug reports are a big help in improving compatibility.

## Reporting Issues

When you run into a bug or unexpected behavior, please open an Issue on GitHub:

- [Open an Issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose)

To make it easier to pin down, please try to state clearly:

- Your system version and Mirage version;
- Steps to reproduce;
- The expected result and the actual behavior;
- Relevant logs (you can enable "Verbose logging" in [General Settings](/en/settings/general/), and for Workshop issues you can export a redacted support report).

You can also join the **QQ group 2160040437** to chat and share feedback.

## Contributing

Pull Requests are welcome. Before submitting, please confirm at least that:

1. The three renderers build independently;
2. `./Mirage/scripts/build.sh Release` produces a complete app bundle;
3. The app bundle contains the three renderers, the runtime dynamic libraries, the MoltenVK ICD, and `assets`;
4. You haven't committed API Keys, Steam login data, build directories, or user wallpapers.

For build instructions, see [Building from Source](/en/advanced/build/).

## Sponsoring

Mirage will continue to be developed freely and openly. If it brings value to your desktop, you're welcome to sponsor it as you see fit — every bit of support goes toward ongoing maintenance, compatibility improvements, and new features. **Sponsorship is entirely voluntary and does not affect access to any features.**

- Afdian (爱发电): <https://www.ifdian.net/a/laobamac>
- WeChat Pay / Alipay: see the QR codes in the project [README](https://github.com/laobamac/MirageWallpaper)
- Overseas users can sponsor with USDT (see the README)

## License

Mirage is released under [GPL-3.0](https://github.com/laobamac/MirageWallpaper/blob/main/LICENSE). Third-party code and resources in the repository remain under their respective licenses.

## Acknowledgments

Mirage's implementation references and uses the following projects:

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) — runtime Vulkan → Metal translation
- [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) — UI framework reference
- [open-wallpaper-engine](https://github.com/waywallen/open-wallpaper-engine) — particle system reference
- [laobamac/OpenMetalWallpaper](https://github.com/laobamac/OpenMetalWallpaper) — model parsing
- [rstd](https://github.com/hypengw/rstd)

## Affiliation Notice

Mirage is not affiliated with, nor endorsed by, Valve, Steam, or Wallpaper Engine. Workshop content belongs to its respective authors; please observe the applicable licenses and terms of use.
