# ADR 0006 — Vite + Tailwind v4 + Alpine CSP build, palette enforced at build time

Status: accepted (2026-11-18)

## Decision
- Static bundle built on the developer machine/CI with pnpm (lockfile `web/pnpm-lock.yaml`;
  AGENTS.md mentions npm/package-lock — the owner's tooling policy mandates pnpm). Pinned:
  vite 8.3.8, tailwindcss/@tailwindcss/vite 4.3.5, @alpinejs/csp 3.17.5,
  @fontsource-variable/inter 5.3.0 (Inter, SIL OFL 1.1, Latin + Latin-Extended variable woff2).
- Strict CSP from the daemon (`script-src 'self'`, no `unsafe-eval`/`unsafe-inline`); no inline
  scripts/styles/handlers; dynamic widths use Alpine's object-style bindings (CSSOM).
- Tailwind's `@theme` resets `--color-*` and re-adds only black, white, neutral grays and blue:
  other color utilities cannot compile. `scripts/design-check.mjs` (static) and `tests/e2e.mjs`
  (computed styles in a browser) fail on gradients, non-palette colors, tooltips, remote loads.
- Hash routing (`#/item/<id>`), polling (1 s active / 5 s idle / paused while hidden / backoff to
  30 s), daemon is the source of truth so Safari reloads and tab suspension are harmless.
- Dark appearance only (no theme toggle).
