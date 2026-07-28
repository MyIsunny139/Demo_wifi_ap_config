# Customer Delivery Manual Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a verified Chinese Word manual for customers operating the current `dist\video-ai` green-directory release.

**Architecture:** Generate the DOCX deterministically with `python-docx`, using the `compact_reference_guide` preset and `customer_pack` opening pattern. Validate required content by extracting DOCX text, then render every page with the document skill renderer and inspect the PNGs before delivery.

**Tech Stack:** Python, python-docx, OOXML, LibreOffice, `render_docx.py`.

---

### Task 1: Build the customer manual

**Files:**
- Create: `.codex-docx/create_delivery_manual.py`
- Create: `docs/video-ai-客户交付使用手册.docx`

- [ ] **Step 1: Encode the document design**

Set Letter portrait pages, 1-inch margins, 0.492-inch header/footer distances, Microsoft YaHei Chinese font, Calibri Latin font, 11 pt body, 1.25 line spacing, and the heading/table tokens from `compact_reference_guide`. Add a quiet running header and PAGE field footer.

- [ ] **Step 2: Write the approved content**

Create the seven approved sections: delivery scope, system description, hardware wiring, installation/startup, operation workflow, shutdown, and troubleshooting. Use fixed-width tables for delivery contents, camera pins, peripheral wiring, alarm states, and troubleshooting; use real numbered lists for procedures and warning callouts for 5V GPIO safety and process shutdown.

- [ ] **Step 3: Generate the DOCX**

Run:

```powershell
python .codex-docx\create_delivery_manual.py
```

Expected: `docs/video-ai-客户交付使用手册.docx` exists and is non-empty.

### Task 2: Verify content contracts

**Files:**
- Verify: `docs/video-ai-客户交付使用手册.docx`

- [ ] **Step 1: Extract document text**

Use `python-docx` to collect paragraph and table-cell text, then assert the presence of these contracts:

```text
192.168.1.3
http://127.0.0.1:8000
video-ai.exe
_internal
GPIO48
GPIO14
taskkill /IM video-ai.exe /T /F
logs\video-ai.log
captures\
```

- [ ] **Step 2: Verify document structure**

Assert that the DOCX contains at least seven level-one headings, all required tables have fixed DXA grids, no table row has a fixed exact height, and the document contains no `TBD`, `TODO`, or placeholder text.

### Task 3: Render and visually inspect

**Files:**
- Render: `docs/video-ai-客户交付使用手册.docx`
- Create for QA only: `.codex-docx/rendered/page-*.png`

- [ ] **Step 1: Render every page**

Run:

```powershell
python C:\Users\Administrator\.codex\plugins\cache\openai-primary-runtime\documents\26.715.12143\skills\documents\render_docx.py docs\video-ai-客户交付使用手册.docx --output_dir .codex-docx\rendered --emit_pdf
```

Expected: one PNG per page and a PDF in the QA directory.

- [ ] **Step 2: Inspect all page images**

Open every `page-*.png` and check title hierarchy, Chinese glyphs, page breaks, table wrapping, callout spacing, page numbers, clipping, overlap, isolated headings, and excessive blank space.

- [ ] **Step 3: Iterate if needed**

Adjust `.codex-docx/create_delivery_manual.py`, regenerate the DOCX, rerun content checks, and render again until every page is clean.

### Task 4: Final delivery check

**Files:**
- Deliver: `docs/video-ai-客户交付使用手册.docx`

- [ ] **Step 1: Re-run content and OOXML checks**

Expected: every required fact is present, tables use fixed geometry, and no placeholder text remains.

- [ ] **Step 2: Confirm only the requested artifact is delivered**

Keep `.codex-docx` rendering files as internal QA artifacts and link only the final DOCX in the user response.
