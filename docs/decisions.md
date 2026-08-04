# Design decisions

Format: one entry per non-obvious choice, with the reasoning.

## 2026-08-04 — UDP over TCP
Chose UDP deliberately so the reliability layer must be implemented
by hand rather than provided by the kernel.
