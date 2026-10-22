# Releasing

The website and the firmware are built by GitHub Actions (`.github/workflows/publish.yml`).

## One-time setup

1. **Settings → Pages → Build and deployment → Source: GitHub Actions.**
2. Check that `project.json` has the right `repo` (`owner/name`) and `ref`.
   - While testing, `ref` can be `main`. Users who take control then follow `main`.
   - For production, set `"ref": "auto"`. The configs then point to the tag `v<version>`, so users only receive a release when they choose to update.
3. Update the website address in `README.md` if the repository name changed.

## Publishing a new version

1. Change the code in `src/` or `components/`.
2. Bump `version` in `project.json`.
3. Run `python tools/build_yaml.py` and commit the generated `esp-controller-*.yaml` files.
4. If you renamed or removed a Home Assistant entity on purpose, run `python tools/check_entities.py --update` and mention it in the release notes.
5. Push to `main`. The website now shows the new firmware.
6. Create the tag: `git tag v1.0.1 && git push origin v1.0.1` (the tag must match `version`). This also creates a GitHub release with the firmware files.

## Before the first public release

- Build both variants and test them on a real controller: install via the website, take control in ESPHome Builder, check the entities in Home Assistant, upload a fallback file.
- Check the size of the compiled firmware: the app partition is 1.75 MB.
