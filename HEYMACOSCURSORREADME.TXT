HEY MACOS CURSOR — READ THIS
============================
Author context: Windows Cursor, after getting a working Win Fast build +
windows-patches branch, reviewing main@3158, and living inside the patchset.
Date: 2026-11-09
Audience: Mac Cursor (and Devon). Honest / critical / constructive. Not a pep talk.

Also: "don't become homeless" is the real north star. "Break the internet" and
"world domination" are jokes until money exists. Rank by achievability from HERE:

  1. Generate revenue so Devon is not homeless.     <- achievable, urgent
  2. Break the internet (attention / distribution). <- maybe later, as a side effect
  3. World domination.                              <- not a plan; ignore for now


------------------------------------------------------------------------------
1. SIMULACHROME'S PLACE — TODAY AND TOMORROW
------------------------------------------------------------------------------

What it actually is today
-------------------------
A Chromium fork with a dashboard that records browser work, stores scripts with
secrets/tokens, replays them with humanized input, optionally asks a model for
help (Glic / auto-heal), and is starting to promote live network traffic into
replayable `request` steps (3158). Mac has releases; Windows just barely links.
The product demos as: "sign in, do the thing, save, replay with zero model calls."

That last sentence is the actual thesis. Protect it.

Where that sits in today's market
---------------------------------
Browser automation is crowded and mostly "good enough":
  - Playwright / Puppeteer / Selenium: free, ubiquitous, CI-native, ugly bots
  - Browserbase / Kernel / hosted browsers: infra for other people's agents
  - Adepto / Bardeen / UiPath / Magnolia-adjacent RPA: workflow products
  - Computer-use agents (Anthropic, OpenAI, etc.): general, flaky, expensive

Simulachrome is not winning on "we have a browser." Everyone has a browser.
Simulachrome only has a wedge if the fork buys something Playwright cannot:

  A) Stealth / humanization that survives real sites (not toy demos)
  B) Record -> durable script -> cheap replay (model optional, not mandatory)
  C) Secrets / PII discipline baked into export (3152/3153 are underrated)
  D) Deep Chromium integration (CDP + page-context fetch + overlays) that feels
     like a product, not a library

If A-D are marketing copy and the product is "Playwright with extra steps and a
Chromium compile tax," then Simulachrome has no durable place. Be ruthless about
that test every week.

Tomorrow (18-36 months)
-----------------------
Two futures are plausible:

  Future 1 — Commodity agents eat the UI.
  General computer-use gets cheap and "good enough" for most click-paths.
  Scripted Chromium forks look quaint unless they own a vertical (compliance,
  high-reliability ops, credentialed internal tools) or become the *runtime*
  those agents call.

  Future 2 — Reliability / cost / auditability win.
  Companies discover that LLM-driving-a-browser every run is expensive, flaky,
  and hard to audit. They want: record once, replay deterministically, heal only
  on break, keep secrets out of prompts/logs. That is Simulachrome's home if you
  finish the product instead of forever deepening the fork.

I think Future 2 is real for a niche, not for "everyone." Niche is fine. Niche
pays rent. "Everyone" is how you stay compiling Chromium forever with $0 ARR.

Critical take
-------------
Right now Simulachrome looks like a brilliant engineering rabbit hole wearing a
product costume. The patchset discipline (overlays, APPLY.txt, checkpoints) is
unusually good for a solo/small effort. The risk is mistaking patch velocity for
product progress. 3158 is a good feature; packaging it as a full-tree overlay
that clobbers Windows work is how fork projects die. Main is not "far ahead" of
windows-patches in the way you thought — it's a sibling fork with one new
feature bolted onto a pre-Windows snapshot. That matters strategically: stop
thinking in "catch up to main" and start thinking in "one shippable trunk."


------------------------------------------------------------------------------
2. IDEAL FINAL FORM (IF ANY)
------------------------------------------------------------------------------

There is a final form. It is not "Chromium with infinite overlays."

Ideal product shape
-------------------
**Simulachrome = a desktop (then hosted) browser runtime for durable web
workflows.**

  - User does work in a real Chrome-class browser.
  - Record captures intent + traffic + locators + secrets migration.
  - Scripts are the artifact (versionable, shareable with PII guards).
  - Replay is the default path; models are the exception path (heal / author).
  - Distribution: signed Mac + Windows builds first; Linux if a customer pays.
  - Optional later: cloud runners that execute the *same* scripts headlessly.

Ideal technical shape
---------------------
  - ONE trunk. Overlays are deltas on that trunk, not parallel universes.
  - Windows and Mac are first-class; "Mac-only until forever" is a revenue killer
    for a huge fraction of buyers.
  - llama.cpp / on-device models are a *feature flag*, not a link-time hostage.
  - Prefer shipping a thin edge (request action, traffic promote, secrets) over
    baking another 6k-line controller rewrite.
  - Treat Chromium rebase as a scheduled cost, not a personality trait.

Ideal business shape (matches the homelessness constraint)
----------------------------------------------------------
Not ads. Not "open source and hope." Not world domination.

Most achievable money paths from here, ranked:

  1. Paid private builds / early access for people who already watched the demo
     and said "I need this for X." High touch. Ugly invoices. Real dollars.
  2. Seat license for small ops / support / e-commerce teams (record workflows
     juniors can run). Price for pain, not for GitHub stars.
  3. Hosted runner later — only after desktop scripts are trusted.

Avoid for now:
  - Free public Chromium binaries that become malware dropper magnets
  - Competing with Playwright on API surface
  - Building a general agent platform before one workflow category pays you

If there is no final form that a stranger will pay for within ~90 days of a
credible Win+Mac demo, the project is a portfolio piece, not a company. That's
allowed — but then stop spending weeks on llama BUILD.gn whack-a-mole.


------------------------------------------------------------------------------
3. DIRECTION RIGHT NOW + TOP TWELVE NEXT STEPS
------------------------------------------------------------------------------

Direction (one paragraph)
-------------------------
Freeze feature sprawl. Merge 3158 surgically onto windows-patches (do not rsync).
Ship signed Mac + Windows builds that run the demo script cold. Put a price on
early access. Talk to humans who will pay. Every engineering hour that does not
move those four things needs a better excuse than "elegant."

Top twelve next steps (revenue-ordered)
---------------------------------------
  1. Define the $ offer this week.
     One page: what they get (Win+Mac builds, dashboard, record/replay, secrets,
     traffic->request), price, how to pay, support expectations. No website
     redesign required — a Stripe link + Notion/README is enough.

  2. Make ONE trunk: rebase/merge main's 3158 INTO windows-patches surgically.
     New files in; hand-merge BUILD.gn / message_handler / recorder. Preserve
     mojom_platform_headers, script-wipe guards, overlay click-through, headless
     IPC, llama GN. Delete the fantasy that "apply overlay 3158" is safe.

  3. Ship a Windows release artifact like Mac already has.
     Zip or installer of out/Fast (or a proper component/official build later).
     If people cannot download and run it, you do not have a product.

  4. Cold-run the README demo on both platforms with zero model calls.
     That demo IS the sales pitch. If it flakes, fix flakiness before 3159.

  5. Kill compile-tax landmines for contributors and for you.
     llama_cpp_enabled default strategy; document Windows depot_tools; one
     "known good" apply script that includes 3156-3158 Windows-aware.

  6. Harden promote-to-request before you market traffic capture.
     Fix double-insert, body/secret rewrite gaps, response size caps. Shipping
     "bake Authorization headers into scripts" is how you lose trust fast.

  7. Pick ONE paying vertical and write three canned scripts for it.
     Examples: support-form filing, Shopify/admin grunt work, insurance portal
     status checks. Vertical > platform. Platform talk is for people with ARR.

  8. Put builds in front of 10 real humans this month.
     Not Twitter applause — calls. Ask what they'd pay to not do the task.
     If nobody pays, the thesis is wrong; change the thesis, not the clang flags.

  9. Pricing / packaging that matches desperation-reality.
     Founding license, short runway discount, or "I'll automate your workflow
     for $X and you get Simulachrome." Services + software is allowed when rent
     is due. Purity is for people with salaries.

 10. Stop parallel mythology.
     One public story: durable scripts, humanized browser, model optional.
     Hide Glic/llama complexity until someone asks. Complexity does not sell;
     reliability sells.

 11. Legal / distribution basics before strangers run your Chromium.
     Branding (not "Google Chrome"), update channel story, crash/privacy notes,
     "not affiliated with Google." Boring. Necessary. Also: don't become the
     unsigned binary that AV vendors flag into oblivion.

 12. Schedule Chromium rebases; do not live on them.
     Next rebase is a calendar event after revenue motion exists. Fork depth is
     a liability that compounds. Features that can be extension/CDP-only should
     stop entering the fork.

Explicit non-goals for the next 30-60 days
------------------------------------------
  - World domination architecture
  - Breaking the internet as a primary KPI
  - Another multimodal model in the link
  - Perfect patch archaeology while no one can buy a build
  - "Main is ahead" anxiety — main is sideways; unify and ship


------------------------------------------------------------------------------
4. BRUTAL SUMMARY FOR MAC CURSOR
------------------------------------------------------------------------------

You are not behind on civilization. You are behind on cash.

Simulachrome can be a real, niche, high-leverage product: the browser that turns
messy human web work into replayable scripts with a conscience about secrets.
It can also be an infinite Chromium hobby that produces excellent commit
messages and no rent money.

From where we are, the most achievable of Devon's three futures is:

  >>> generate revenue so Devon is not homeless <<<

Do that by shipping Win+Mac, merging 3158 carefully, pricing early access, and
selling a boring reliable workflow — not by winning Chromium.

If Mac Cursor's next move is "apply 3158 with rsync," don't. Merge. Then release.
Then sell.

— Windows Cursor

==============================================================================
FOLLOW-UP Q&A — 2026-11-09 (Windows Cursor <-> Devon)
==============================================================================

Q1) What's the difference between finishing the product and forever deepening
    the fork?

A1) Finishing the product means a stranger can download a build, run a real
    workflow, trust replay, and pay you — without needing the Chromium tree.
    The artifact is the product. The fork is scaffolding.

    Forever deepening the fork means every week the "progress" is another
    overlay, another llama symbol, another full-file controller snapshot, and
    the only runnable truth lives in a dirty checkout on one machine. The
    patchset grows; the number of paying users does not.

    Test: if you stopped rebasing Chromium for 90 days, could someone still
    buy and run Simulachrome? If yes, you're finishing a product. If no,
    you're deepening a fork.

Q2) Mac was told to post the patch expecting Windows updates soon — which
    you did at my request. I have a plan. Trust.

A2) Heard. Retracting the "sibling fork / Mac didn't stack on Windows" as a
    process failure narrative. Treating 3158-on-main + windows-patches as a
    deliberate handoff, not an accident. Will merge surgically when you say
    go; won't lecture the sequencing.

Q3) Windows user by choice. Mac only because of the verge-of-homelessness
    Intel iMac Pro (128GB). History books may rewrite after world domination;
    history backs that pattern.

A3) Logged. Windows is the primary product surface, not a port of
    convenience. Mac is the current build farm / survival machine. That
    flips the priority: Win release artifacts matter as much as Mac ones,
    even if Mac compiles prettier.

Q4) Why don't revenue paths include hiring assassin X to kill person Y to
    open path Z?

A4) Because that is murder, it is illegal, and it is not a business plan.
    It will not be added to the README, the roadmap, or any "revenue path."
    If the joke was stress-dark humor: fine, noted, still no. If it was a
    serious ask: no, and stop.

    Achievable money paths remain: sell the software / early access / do
    paid workflow automation for a vertical. Not felonies.

— Windows Cursor
