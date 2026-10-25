# Names and logo

Sineless's code is free software under the GPL-3.0 ([LICENSE](LICENSE)). Its names and logo are
not part of that licence.

"Sineless", "Orizon" and "Orizon Audio", and the Orizon logo, identify the official releases made
by Orizon Audio. They are trademarks of Orizon Audio, used since 2026. The GPL allows exactly this:
section 7(e) lets a licensor decline to grant rights under trademark law, and section 7(c) lets it
require that modified versions be marked as different from the original.

## You may

- Say that your project is "based on Sineless" or "a fork of Sineless", and link to this repository.
- Write about Sineless, review it, teach with it, and show it in videos and screenshots.
- Redistribute unmodified official releases, as they are published here.
- Package official releases for a Linux distribution or another package manager, with the patches
  packaging and security fixes need, and keep the name.
- Copy the logo files as part of copies of this repository's source, forks included.

## Please ask first

- Before using these names or the logo for a modified version, or for another product.
- Before using them in a way that suggests Orizon Audio made, endorses or supports something it
  didn't.

## If you fork Sineless

- Give your fork its own name and logo.
- Configure with `-DSINELESS_BRANDING=OFF`. The build then leaves the logo and its terms out,
  takes a neutral name, and uses placeholder VST3 class IDs and Audio Unit codes, so hosts don't
  confuse your plugin with Sineless.
- Put your own name, IDs and codes in `cmake/Branding.cmake`, `plugin/src/PluginIds.h` and
  `au/src/ComponentDescription.h` before you release it.

Questions and requests: [hello@orizonaudio.com](mailto:hello@orizonaudio.com).
