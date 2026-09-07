# Pilot Readiness To-Do

**Audience:** Internal engineering/founder tracking -- not client-facing.
**Purpose:** Punch list of what's left before AnimusCore_v1 is ready to hand
to a prospective client for the paid Institutional Pilot Program
(`docs/PILOT_PROGRAM.md`). Originated from a repo-wide readiness audit;
update this file directly as items close rather than re-deriving the list
from scratch each time.

---

## Blocking

- [ ] **PoP fee amount unset** -- `docs/PILOT_CONTRACT.md` §2.1 still has
      `[USD $______]`. Can't send a contract to a client without a price.
- [ ] **Counsel review not done** -- `docs/PILOT_CONTRACT.md`,
      `docs/PILOT_AGREEMENT.md`, and `LEGAL_EULA.md` are all explicitly
      self-flagged as AI-drafted and not fit to send or execute. Governing
      law (India), arbitration forum (SIAC/Singapore), and cross-border
      enforceability are founder-drafted, not vetted. Tracked in GitHub
      issue #7.
- [ ] **CIN not issued** -- Animus Technologies Private Limited's
      incorporation is decided but not filed; every contract has a
      `[CIN -- pending]` placeholder and no registered notice address.
- [ ] **Client-side contract fields are all placeholders** -- expected
      per-deal (`[CLIENT LEGAL ENTITY NAME]`, `[EFFECTIVE DATE]`, etc.),
      but flagging that no signable instance exists yet.
- [x] **ITCH 5.0 adapter had zero CI coverage** -- fixed by
      `.github/workflows/itch50_ci.yml` (added 2026-09-07): builds and
      smoke-tests `bench_itch_ingest` on Linux/GCC, Linux/Clang, and
      Windows/MSVC on every push/PR touching `adapters/itch50/`,
      `animus-eval-kit/include/`, `include/animus/`, or `CMakeLists.txt`.
      Each job asserts the harness's own zero-decode-failure /
      zero-sequence-corruption / zero-hot-path-heap-allocation verdict line
      plus a clean process exit. First run:
      https://github.com/alakshendra-roy/AnimusCore_v1/actions/runs/34112907821
      (all three jobs green).

## Should-fix

- [ ] **Liability-cap inconsistency unresolved** -- the paid
      `PILOT_CONTRACT.md` uses a mutual cap; the free-tier
      `PILOT_AGREEMENT.md`/`LEGAL_EULA.md` cap only Animus's liability.
      Flagged as an open decision in `LEGAL_VERIFICATION_AUDIT.md` §4,
      never made.
- [ ] **`PILOT_AGREEMENT.md` still fully genericized** -- never updated to
      name the incorporated entity, unlike the other legal docs; unclear
      whether that's intentional.
- [ ] **No W-8BEN-E guide** -- `W8BEN_GUIDE.md` carries a superseded notice
      (post-incorporation US payments need a W-8BEN-E, not an individual
      W-8BEN) but no replacement guide was written. Payment/tax readiness
      for a US-based client is incomplete.
- [ ] **No macOS verification anywhere** -- CLAUDE.md/READMEs imply
      cross-platform support but only Windows and Linux have actually been
      built/run; Clang has now been verified (via the new CI workflow, on
      Linux) but never on macOS specifically.

## Cosmetic

- [ ] **`docs/OUTBOUND_TRACKER.md` / `docs/ICP_TARGET_LIST.md` unaudited**
      -- worth a quick pass to confirm nothing placeholder- or
      fabricated-looking could leak to a prospect.
