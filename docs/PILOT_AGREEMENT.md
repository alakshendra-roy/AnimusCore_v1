# Animus Core -- Pilot Evaluation & Commercial License Agreement (TEMPLATE)

> **THIS IS A PLACEHOLDER TEMPLATE, NOT A FINISHED AGREEMENT.** Written by
> an AI coding assistant at the copyright holder's request as a structural
> starting point only, in the same spirit as this repository's own
> `LICENSE` file. It is **not legal advice** and **must not** be sent to a
> prospective customer, relied upon, or treated as binding until a
> qualified attorney (licensed in the relevant jurisdiction) has reviewed
> and adapted it -- including the governing-law, liability-cap, and
> export-control provisions, which are placeholders below.
>
> **Review status:** counsel review not yet started. Open items are
> tracked in [issue #7](https://github.com/alakshendra-roy/AnimusCore_v1/issues/7) --
> do not remove that tracking link until every checklist item there is
> resolved.

---

**PILOT EVALUATION & COMMERCIAL LICENSE AGREEMENT**

This Agreement is entered into as of the Effective Date by and between
**[Licensor Legal Name]** ("**Licensor**") and **[Evaluator Legal Name]**
("**Evaluator**"), collectively the "Parties," governing Evaluator's
pilot use of the Animus Core engine, including its compiled binaries,
headers, and Python SDK (the "**Software**").

**1. Evaluation License; 30-Day Term.** Subject to this Agreement,
Licensor grants Evaluator a limited, non-exclusive, non-transferable,
revocable license to install and use the Software solely for internal,
non-production evaluation ("**Pilot**") for a period of **thirty (30)
calendar days** from the date the Software is first activated via a
license file issued under `AnimusCore_v1/license_tools/sign_license.ps1`
(the "**Pilot Term**"). No right to use the Software in production, to
process live customer or third-party data with it, or to redistribute it
is granted. Licensor may extend the Pilot Term in writing at its sole
discretion.

**2. Hardware Lock (HWID Binding).** The Software's license file is
cryptographically bound, via `animus_verify_license`, to a fingerprint
derived from the Evaluator's designated machine (its OS-assigned
MachineGuid and a hardware network adapter MAC address) and, where
applicable, to a maximum licensed core count. Evaluator shall use the
Software only on the single machine for which a license file was issued.
Evaluator shall not attempt to defeat, reverse-engineer, or circumvent
this binding, the RSA-2048 signature verification, or the expiration
check; any such attempt immediately terminates this Agreement. Re-hosting
to a different machine requires a newly issued license file from
Licensor.

**3. Intellectual Property.**

**3.1 Software IP.** The Software, including all source code,
object code, documentation, and any modifications, improvements, or
derivative works Licensor makes to it (whether or not incorporating
Evaluator feedback), is and remains the sole and exclusive property of
Licensor. No license, title, or ownership interest is transferred to
Evaluator except the limited evaluation rights expressly granted in
Section 1. Any feedback, bug reports, or suggestions Evaluator provides
regarding the Software may be used by Licensor for any purpose, including
commercial purposes, without obligation or compensation to Evaluator.

**3.2 Evaluator Data and Output.** As between the Parties, **Evaluator
retains 100% ownership of all trading strategies, alpha signals, models,
algorithms, market-data schemas, and any other data or content Evaluator
processes through the Software during the Pilot Term, and of any output,
signal, or result produced by running that data through the Software**
("**Evaluator Data**"). Licensor acquires no license, ownership interest,
or right to use Evaluator Data for any purpose other than as necessary to
provide the evaluation contemplated by this Agreement, and shall not
retain, reuse, reverse-engineer from, or disclose Evaluator Data after
the Pilot Term, except as required to comply with Section 6
(Confidentiality) or applicable law.

**4. Disclaimer of Warranties.** THE SOFTWARE IS PROVIDED "AS IS" AND "AS
AVAILABLE," WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
WITHOUT LIMITATION THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
A PARTICULAR PURPOSE, NON-INFRINGEMENT, OR THAT THE SOFTWARE WILL BE
ERROR-FREE, UNINTERRUPTED, OR SECURE. THE SOFTWARE IS A PRE-COMMERCIAL
PILOT BUILD AND IS NOT WARRANTED FOR USE IN PRODUCTION, LIVE TRADING, OR
ANY OTHER SETTING WHERE FAILURE COULD CAUSE FINANCIAL LOSS OR OTHER HARM.

**5. Limitation of Liability.** TO THE MAXIMUM EXTENT PERMITTED BY LAW,
IN NO EVENT SHALL LICENSOR BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
SPECIAL, CONSEQUENTIAL, OR PUNITIVE DAMAGES, OR ANY LOSS OF PROFITS,
REVENUE, DATA, OR TRADING LOSSES, ARISING OUT OF OR RELATING TO THIS
AGREEMENT OR THE SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
DAMAGES. LICENSOR'S TOTAL AGGREGATE LIABILITY UNDER THIS AGREEMENT SHALL
NOT EXCEED **[USD $______ / the fees paid by Evaluator, if any]**.

**6. Confidentiality.** Each Party shall keep confidential the other's
non-public technical, business, and pricing information disclosed under
this Agreement, and use it solely to evaluate a potential commercial
relationship, for **[two (2) years]** from disclosure.

**7. Commercial Conversion.** Before expiration of the Pilot Term,
Evaluator may elect to convert this Pilot into a paid commercial license
by executing a separate commercial license and order form with Licensor
specifying fees, entitled core count, support terms, and production-use
rights. Absent such a signed conversion, this Agreement and the
associated license file **automatically terminate** at the end of the
Pilot Term, all evaluation rights cease, and Evaluator shall cease all
use of the Software and, if requested, certify its deletion.

**8. Term & Termination.** This Agreement terminates automatically upon
expiration of the Pilot Term (Section 7), upon a breach of Section 2 or
Section 3, or upon either Party's written notice for any reason. Sections
3-6 and 8-9 survive termination.

**9. General.**

**9.1 Governing Law; Arbitration.** This Agreement is governed by the
laws of **[Jurisdiction]**, without regard to conflict-of-laws
principles. Any dispute, controversy, or claim arising out of or
relating to this Agreement, including any question regarding its
existence, validity, or termination, shall be referred to and finally
resolved by arbitration administered by the **Singapore International
Arbitration Centre ("SIAC")** in accordance with the Arbitration Rules
of the Singapore International Arbitration Centre ("**SIAC Rules**")
for the time being in force, which rules are deemed to be incorporated
by reference into this Section 9.1. The seat of the arbitration shall
be **Singapore**. The tribunal shall consist of **one (1) arbitrator**,
unless the Parties otherwise agree or the SIAC Rules require otherwise
given the amount in dispute. The language of the arbitration shall be
**English**. Nothing in this Section 9.1 prevents either Party from
seeking interim or injunctive relief from a court of competent
jurisdiction pending constitution of the arbitral tribunal — matching
the arbitration clause in `PILOT_CONTRACT.md` §8.1, so an evaluator
converting from this free Pilot into the paid Institutional Pilot
Program is not surprised by a different dispute-resolution mechanism
partway through the relationship.

**9.2 Entire Agreement.** This Agreement constitutes the entire
agreement between the Parties regarding the Pilot, superseding all
prior discussions. Export and sanctions-compliance obligations
applicable to the Software, if any, are addressed in a separate rider
and are not covered by this template.

| | |
|---|---|
| **Licensor:** _______________________ | **Evaluator:** _______________________ |
| By: _____________  Date: ________ | By: _____________  Date: ________ |
