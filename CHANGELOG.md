# Changelog

All notable changes to this project will be documented in this file.

## v0.2.0 - Limit Order Matching

- Added limit order matching with price-time priority.
- Added bid and ask price levels backed by FIFO order queues.
- Emitted `TradeEvent` records for executed matches.
- Rested partially filled incoming limit orders with remaining quantity.
- Removed fully filled resting orders from the book and cancel index.
- Expanded order book smoke tests for matching, partial fills, and FIFO behavior.

## v0.1.0 - Basic Order Model + CLI Parser

- Added the initial C++20/CMake project scaffold.
- Defined basic action, order, event, parser, order book, and exchange types.
- Added a stdin-driven CLI loop: `stdin -> parser -> exchange -> events`.
- Added basic command parsing for `SUBMIT`, `CANCEL`, and `PRINT`.
- Added minimal smoke tests and example order input.
