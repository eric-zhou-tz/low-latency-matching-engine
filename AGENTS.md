# AGENTS.md

## Matching Engine Guidelines

This project is a low-latency C++ matching engine focused on systems design, determinism, and exchange-style order matching.

### Engineering Principles
- Prefer simple, deterministic logic over premature optimization
- Keep price-time priority behavior correct and explicit
- Use integer-based pricing/quantity types
- Write clear, production-style code with comments where useful
- Avoid unnecessary abstractions early in development

### Architecture Goals
- Event-driven matching flow
- Price-level queues with FIFO time priority
- Balanced-tree order book structure
- Clear separation between parser, exchange, and order book logic
- Extensible design for future persistence/networking/concurrency

### Development Notes
- Update `CHANGELOG.md` for every major milestone or release (Only with user permission)
- Use semantic versioning (`v0.x.y`)
- Keep benchmarks and latency measurements reproducible
- Prefer small, incremental commits over large rewrites
- Preserve deterministic replayability where possible