# Golden Replay Fixtures

These fixtures are public-boundary regression tests for the command pipeline:

```text
raw input tape -> parser -> exchange -> order book -> event formatter -> expected output tape
```

Each scenario has a `<name>.txt` command tape and a matching `<name>.expected`
output tape. Tests compare exact output after normalizing only line endings.
