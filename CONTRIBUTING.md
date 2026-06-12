# Contributing

## Development Checks

Run the local validation script before opening a pull request:

```bash
python3 scripts/validate_release.py
```

The add-on should remain read-only unless a future release explicitly designs,
documents, and reviews control-command safety.

## Pull Requests

- Keep private captures and credentials out of commits.
- Include logs with secrets redacted.
- Update `weber_connect_ble/CHANGELOG.md` for user-visible changes.
- Prefer small, focused patches.
