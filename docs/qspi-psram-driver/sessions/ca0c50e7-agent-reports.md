### 2026-09-01T17:19 Agent "Survey QSPI PSRAM market" finished
Research complete. Here is the structured vendor list.

# QSPI/QPI (SQPI) PSRAM market survey — 2026

All parts below: SPI (1‑1‑1) + QPI (4‑4‑4), SOP‑8/USON‑8 (some WLCSP), self-refresh pseudo-SRAM. Read ID = opcode **0x9F** (SPI mode only) returning MF ID + KGD (0x5D = pass) + EID, unless noted.

## 1. AP Memory (Taiwan) — the originator of this device class
- APS1604M-SQR (16Mb, 1.8V, 144MHz), APS1604M-3SQR (16Mb, 3.0V, 133MHz), APS1604M-DQRA (16Mb, 1.8V, x4 DDR, WLCSP only)
- APS3204L-3SQN (32Mb, 3.0V, 133MHz)
- APS6404L-SQN/-SQR/-SQH/-SQRH (64Mb, 1.8V, 144MHz), APS6404L-3SQN/-3SQR (64Mb, 3.0V, 133MHz)
- APS12804O-SQRH (128Mb, 1.8V, 144MHz quad SDR), APS12804O-DQ (128Mb, 1.8V, quad DDR)
- Read ID: yes, 0x9F → MF ID 0x0D, KGD 0x5D. Product list: [apmemory.com SPI/QSPI](https://www.apmemory.com/en/product/iotram/SPIQSPI), [datasheet example](https://www.mouser.com/datasheet/2/1127/APM_PSRAM_QSPI_APS1604M_SQ_v2_7_PKG-1954808.pdf)

## 2. Espressif (rebrand of AP Memory dies)
- ESP-PSRAM32 (32Mb, 1.8V), ESP-PSRAM64 (64Mb, 1.8V, 144MHz), ESP-PSRAM64H (64Mb, 3.3V, 133MHz)
- Read ID: yes ('h9F, KGD table present — verified in datasheet PDF). Command set/ID identical to APS6404L; universally treated as AP Memory silicon. [Datasheet](https://www.espressif.com/sites/default/files/documentation/esp-psram64_esp-psram64h_datasheet_en.pdf)
- Marketplace aliases of the same die: "APM6404", "SP-PSRAM64H".

## 3. ISSI (Integrated Silicon Solution)
- IS66WVS1M8ALL/BLL (8Mb), IS66WVS2M8ALL/BLL (16Mb), IS66WVS4M8ALL/BLL (32Mb), IS66WVS8M8DALL/FALL/FBLL (64Mb) — all 104MHz, 8-SOIC; **ALL = 1.65–1.95V, BLL = 2.7–3.6V**
- Read ID: yes, 0x9F per datasheet (returns ISSI MF ID; community reports 0x9D + KGD 0x5D — I could not open the datasheet PDF directly, so treat the exact ID byte as unverified). Independent design, not an AP rebrand (ISSI is a major PSRAM maker in its own right). [DigiKey family](https://www.digikey.com/en/product-highlight/i/issi/serial-ram-and-quad-ram-solutions), [16Mb part](https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS66WVS2M8ALL-104NLI/16260614), [64Mb part](https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS66WVS8M8FBLL-104NLI/24617443)

## 4. IPUS Limited (HK/China)
- IPS1604LSQ (16Mb, 3.0V), IPS1604LSQL (16Mb, 1.8V), IPS3204JSQ (32Mb, 1.8V), IPS6404LSQ (64Mb, 3.0V, 104MHz), IPS6404LSQL (64Mb, 1.8V, 133MHz); legacy IPS1704L-SQ (64Mb, discontinued → IPS6404)
- Read ID: **confirmed from datasheet text I extracted**: 0x9F → MF ID 0x0D, KGD 0x5D, EID — bit-for-bit AP Memory protocol (clone/compatible design; even uses AP's MF ID). [Datasheet](https://dl.sipeed.com/TANG/Nano/Spec/IPUS_64Mbit_SQPI_Datasheet%C2%A0(1704).pdf)

## 5. Lyontek (Taiwan)
- Current: LY68L6400 (64Mb, 3.3V, 100MHz), LY68S6400 (64Mb, 1.8V, 143MHz), SOP-8/DFN-8; legacy: LY68S3200 (32Mb, still at LCSC/JLCPCB), LY68L3200
- Read ID: yes — "SPI Read ID 'h9F (SPI mode only)", KGD 0x5D per datasheet. AP-compatible command set; independent Taiwanese SRAM house. [Family page](https://www.lyontek.com.tw/en/serialsram.html), [LY68L6400 datasheet](https://www.lyontek.com.tw/pdf/ddr/LY68L6400-1.2.pdf)

## 6. Vilsion Technology (China)
- VTI7064LSM (64Mb, 1.8V), VTI7064MSM (64Mb, 3.0V) — 104MHz, SOP-8; part-number scheme defines 16Mb/32Mb codes (VTI7016/7032) but only 64Mb is on the market
- Read ID: **confirmed from datasheet I extracted**: 0x9F, outputs EID (SPI mode only). Command set (0x35/0xF5/0x38/0xEB) identical to AP Memory — a clone-class design. Datasheet is branded with Ramsun (sramsun.com) contact info. [Datasheet](https://w.electrodragon.com/w/images/8/81/VTI7064LSMxx.pdf), [LCSC](https://www.lcsc.com/product-detail/SRAM_Vilsion-Tech-VTI7064MSME_C139966.html)

## 7. Ramsun Micro-electronics (Shenzhen) — "RS" brand
- RS3204JSQ/JSQI/JSQC/JSQT/JSQCT (32Mb, 1.8V), RS6404LSQ (64Mb, 3.0V), RS6404LSQL (64Mb, 1.8V)
- Sold side-by-side with IPUS parts using identical suffix grammar; Vilsion datasheets carry Ramsun's header — almost certainly rebranded IPUS/Vilsion dies. **Read ID not directly confirmed** (no public datasheet found) — presumed 0x9F/0x0D via lineage. [Catalog](https://www.sramsun.com/list-354-1.html)

## 8. CascadeTeq (Taiwan, PSMC foundry partner; China rep: Shenzhen Mandu) — newest entrant
- CSS1604 (16Mb, incl. CSS1604LS-MJ), CSS3204 (32Mb, e.g. CSS3204SU-LI-A2), CSS6404SS-L (64Mb, 1.8V, SOP-8, 133MHz), CSS6404SU-L (64Mb, 2.7–3.6V, USON), CSS12804SS-O (128Mb, 1.8V, SOP-8, 133MHz) — 1.8V and 3.3V options, AEC-Q100 claimed
- They also sell octal CSS6408SO parts (excluded here). **Read ID support not confirmed** — no public full datasheet; flag for verification. [CSS6404SS-L](https://en.manduic.com/cpzx/3740/CSS6404SS-L), [CSS12804SS-O](https://en.manduic.com/cpzx/3747/CSS12804SS-O)

## Vendors checked — no quad-SPI PSRAM offering
- **Winbond**: parallel PSRAM + HyperRAM/Xccela only ([PSRAM page](https://www.winbond.com/hq/product/customized-memory-solution/psram/?__locale=en)). **XTX**: NOR/NAND flash only. **GigaDevice, Puya, Zetta, AMIC, Fudan Micro, Giantec, Zbit, Dosilicon, UniIC, Longsys/FORESEE**: no serial PSRAM found. **Netsol**: quad-SPI *MRAM*, not PSRAM. **Everspin/Avalanche**: QSPI MRAM/persistent-SRAM (out of scope). **Microchip/Lyontek LY68W**: small serial SRAM, not PSRAM.

## Cross-cutting findings
- Density range actually shipping in quad: **8–128 Mbit**. No 256Mbit quad-SPI PSRAM exists from any vendor — 256Mb is octal/OPI only (AP APS25616, etc.).
- Every part whose datasheet I could open (AP, Espressif, ISSI, IPUS, Lyontek, Vilsion) supports **Read ID 0x9F, SPI mode only**, returning MF ID + KGD (0x5D = pass) + 45-bit EID; the AP-lineage parts (AP, Espressif, IPUS, and by strong implication Ramsun) return MF ID **0x0D**. Only ISSI likely returns a different MF ID byte.
- Unverified Read ID: **CascadeTeq** (no public datasheet) and **Ramsun** (rebrand, presumed inherited).

Sources: [AP Memory SPI/QSPI list](https://www.apmemory.com/en/product/iotram/SPIQSPI), [Mouser AP QSPI PSRAM](https://www.mouser.com/en/new/ap-memory/ap-memory-quad-spi-psram-sdrs), [ESP-PSRAM64/64H datasheet](https://www.espressif.com/sites/default/files/documentation/esp-psram64_esp-psram64h_datasheet_en.pdf), [ESP-IDF external RAM guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/external-ram.html), [ISSI serial RAM highlight (DigiKey)](https://www.digikey.com/en/product-highlight/i/issi/serial-ram-and-quad-ram-solutions), [IS66WVS2M8ALL](https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS66WVS2M8ALL-104NLI/16260614), [IS66WVS8M8FBLL](https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS66WVS8M8FBLL-104NLI/24617443), [IPUS datasheet](https://dl.sipeed.com/TANG/Nano/Spec/IPUS_64Mbit_SQPI_Datasheet%C2%A0(1704).pdf), [IPS6404L-SQ-SPN](https://www.snapeda.com/parts/IPS6404L-SQ-SPN/IPUS/datasheet/), [Lyontek serial pSRAM](https://www.lyontek.com.tw/en/serialsram.html), [LY68L6400 datasheet](https://www.lyontek.com.tw/pdf/ddr/LY68L6400-1.2.pdf), [Vilsion VTI7064MSM](http://www.vilsion.com/VTI7064MSM.shtml), [VTI7064LSM datasheet PDF](https://w.electrodragon.com/w/images/8/81/VTI7064LSMxx.pdf), [Ramsun SPI pSRAM catalog](https://www.sramsun.com/list-354-1.html), [CascadeTeq CSS6404SS-L](https://en.manduic.com/cpzx/3740/CSS6404SS-L), [CascadeTeq CSS12804SS-O](https://en.manduic.com/cpzx/3747/CSS12804SS-O), [Winbond PSRAM](https://www.winbond.com/hq/product/customized-memory-solution/psram/?__locale=en), [Adafruit 4677 (generic APS6404)](https://www.adafruit.com/product/4677)

### 2026-09-01T17:24 Agent "AP Memory ID research" finished
I have everything needed. Here is the consolidated result.

## How Read ID works in this family (identical across all vendors)

Command **0x9F**, SPI (1-bit) mode only, followed by a **24-bit don't-care address phase**, no wait cycles. The device then shifts out, MSB-first: **byte0 = MF ID, byte1 = KGD, bytes2..7 = EID[47:0]** (48 bits), and the EID repeats as long as CE# stays low. AP datasheets warn Read ID is only guaranteed as a **power-up initialization step** (Power-up &gt;150 µs → Global Reset → tRST ≥ 50 ns → EID read). esp-idf reads exactly 8 bytes this way.

## Table: part → ID data

| Part | MF ID | KGD (pass/fail) | EID[47:45] density | EID[44:0] | tCEM | Wrap/page |
|---|---|---|---|---|---|---|
| AP APS6404L-SQH/SQRH/SQR/SQN/3SQR/3SQN (64Mb, all suffixes incl. 144 MHz SQH) | **0x0D** | 0x5D / 0x55 | **010** | "manufacturing ID" (per-die, undocumented) | 8 µs std-temp, **3 µs** extended (105 °C) grade (was 4 µs before Oct 2021 rev) | 1 KB page |
| AP APS1604M-SQR (16Mb) | **0x0D** | 0x5D / 0x55 | **000** | same | 8 µs std / 3 µs ext | 1 KB |
| AP APS3204L-3SQN(A) (32Mb) | **0x0D** | 0x5D / 0x55 | figure shows **010** (!) — family encoding says 001; likely a datasheet copy-paste, unverified on silicon | same | 8 µs std / 3 µs ext | 1 KB |
| AP APS12804O-SQRH (128Mb, newest quad) | **0x0D** | 0x5D / 0x55 | not stated (figure shows bit indices only; presumably 011) | same | 8 µs std / 3 µs ext | 1 KB |
| Espressif ESP-PSRAM64 / 64H | **0x0D** (printed in Fig. 6-7 of Espressif's own datasheet) | 0x5D / 0x55 | 010 | same | 8 µs | 1 KB |
| Espressif ESP-PSRAM32 (32Mb, 1.8 V) | no public datasheet; esp-idf handles it as KGD 0x5D, EID[47:40] == **0x20** ("32MBIT_VER0", needs special clock mode) | | 001 | | | |
| IPUS IPS6404L-SQ (64Mb, 104 MHz) | **0x0D** | 0x5D / 0x55 | 010 | same | **8 µs** (no 3 µs grade; 85 °C only) | 1 KB |
| IPUS IPS1704L-SQ (64Mb 1.8 V, 2nd gen) | **0x0D** | 0x5D / 0x55 | 010 ("device type") | same | 8 µs | 1 KB |
| IPUS IPS1604/IPS3204 | no public datasheets found; IPUS sells 16/32/64Mb "fully compliant with the IoT RAM specification" — same ID scheme expected | | | | | |
| Lyontek LY68L6400 (64Mb, 133 MHz) | **0x0D** | 0x5D / 0x55 | not broken out — figure just says EID[47:0] | same | 8 µs | 1 KB (toggleable 32-byte wrap) |
| Lyontek LY68S3200 (32Mb) | PDF unobtainable (LCSC/Lyontek block direct fetch); same family, same scheme expected | | | | | |

## What esp-idf actually trusts (practice check)

- ESP32 legacy driver (`components/esp_psram/esp32/esp_psram_impl_quad.c`): sends 0x9F + 24-bit zero address, reads 64 bits. Validity = **`PSRAM_KGD(id) == 0x5D` only — the MF byte is never checked**. Density = EID[47:45] (0=16Mb, 1=32Mb, 2=64Mb), with two hardcoded exceptions: EID[47:40]==0x20 → "32Mbit VER0" older die (different clock mode), EID[47:40]==0x26 → "64Mbit trial" samples that report the 32Mb size code but are 64Mb.
- Modern driver (`components/esp_psram/device/esp_quad_psram_defs_ap.h` + `esp_psram_impl_ap_quad.c` — note Espressif literally names the files "**ap**"): defines `PSRAM_QUAD_MFID_AP 0xD`, but still validates by KGD only; MFID is merely logged, and `MFID == 0x0D` is used solely as a gate for an AP-die-specific "2T mode" workaround.

## Verdict: what is and is not distinguishable by ID

1. **Vendor is NOT identifiable.** IPUS and Lyontek deliberately clone the entire ID, including MF ID = 0x0D (their own datasheet figures print "MF ID (0x0D)"), and Espressif modules are AP dies. Every part in this ecosystem answers `0D 5D xx …`. There is no documented bit that separates genuine AP from IPUS or Lyontek silicon.
2. **Density IS identifiable** from EID[47:45] (000/001/010, presumably 011 for 128Mb) — the only reliably decodable model attribute. Caveat: the APS3204L-3SQNA datasheet figure shows 010, so treat 32Mb detection with suspicion until measured.
3. **Speed/temperature grade is NOT identifiable**: APS6404L-SQ vs -SQR vs -SQH (84/104/133/144 MHz, 85 °C vs 105 °C, tCEM 8 µs vs 3 µs) all share the same documented ID fields. A driver cannot learn tCEM or max clock from the chip; it must come from devicetree/config.
4. **EID[44:0] is per-die.** AP calls it "manufacturing ID" (lot/trace data) and documents no structure; two chips of the same model differ there, so full-EID matching must never be used for model detection. The upper EID byte (EID[47:40]) does carry some generation info in practice (0x20/0x26/0x46-style values), which Espressif exploits only for two hardcoded quirk cases.
5. Practical detection recipe (what everything in the field converges on): check KGD == 0x5D for presence/health, decode EID[47:45] for size, ignore the rest. That is exactly what esp-idf does, and it is the most that the ID bytes can honestly support.

Downloaded datasheets are in `/private/tmp/claude-501/-Users-kiro-CLionProjects-zephyr/ca0c50e7-2140-4345-b60b-7afd84168b67/scratchpad/` (`aps6404l_sqh.pdf`, `aps1604m.pdf`, `aps3204l.pdf`, `aps6404l_sqr.pdf`, `aps12804o.pdf`, `ips6404.pdf`, `ips1704.pdf`, `ly68l6400.pdf`, `esp_psram64.pdf`, plus the esp-idf sources `esp_psram_impl_quad.c`, `defs_ap.h`, `impl_ap_quad.c`).

Sources:
- [AP Memory APS6404L-SQH datasheet Rev 4.1 (apmemory.com)](https://www.apmemory.com/en/downloadFiles/0324112120r9601773)
- [AP Memory APS1604M-SQR datasheet Rev 3.0](https://www.apmemory.com/tw/downloadFiles/0324112120r6586620)
- [AP Memory SPI/QSPI product list](https://www.apmemory.com/en/product/iotram/SPIQSPI) (APS3204L-3SQN and APS12804O-SQRH datasheets fetched from its download links)
- [IPUS IPS6404L-SQ datasheet (esp32-cam-ai-thinker repo)](https://github.com/raphaelbs/esp32-cam-ai-thinker/blob/master/assets/IPUS_IPS6404_Datasheet.pdf)
- [IPUS IPS1704L-SQ datasheet (sipeed)](https://dl.sipeed.com/TANG/Nano/Spec/IPUS_64Mbit_SQPI_Datasheet%C2%A0(1704).pdf)
- [Lyontek LY68L6400 Rev 1.2](https://www.lyontek.com.tw/pdf/ddr/LY68L6400-1.2.pdf)
- [Espressif ESP-PSRAM64/64H datasheet](https://cdn-shop.adafruit.com/product-files/4677/4677_esp-psram64_esp-psram64h_datasheet_en.pdf)
- [esp-idf esp32 quad PSRAM driver](https://github.com/espressif/esp-idf/blob/master/components/esp_psram/esp32/esp_psram_impl_quad.c), [esp_quad_psram_defs_ap.h](https://github.com/espressif/esp-idf/blob/master/components/esp_psram/device/esp_quad_psram_defs_ap.h), [esp_psram_impl_ap_quad.c](https://github.com/espressif/esp-idf/blob/master/components/esp_psram/device/esp_psram_impl_ap_quad.c)
- [ESP-IDF external RAM docs (ESP-PSRAM32 is 1.8 V)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/external-ram.html)

### 2026-09-01T17:27 Agent "ISSI and others ID research" finished
All research complete. Here is the full report.

# Read ID behavior of ISSI serial/quad PSRAM and non-AP QSPI PSRAM vendors

## Sources (datasheets actually read, full text extracted)

- ISSI IS66/67WVS2M8ALL/BLL (16Mb), Rev A1, 11/2023 — https://www.issi.com/WW/pdf/66-67WVS2M8ALL-BLL.pdf
- ISSI IS66/67WVS4M8ALL/BLL (32Mb), Rev A2 — https://www.issi.com/WW/pdf/66-67WVS4M8ALL-BLL.pdf
- ISSI IS66/67WVS8M8FALL/BLL (64Mb, "F" die), Rev A, 01/16/2025 — https://www.issi.com/WW/pdf/66-67WVS8M8FALL-BLL.pdf (issi.com bot-blocks after a few hits; retrieved via Wayback snapshot http://web.archive.org/web/20250621201126/https://www.issi.com/WW/pdf/66-67WVS8M8FALL-BLL.pdf)
- Lyontek LY68L6400 Rev 0.4 — https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/kinetis/41021/1/LY68L6400-0.4.pdf (Rev 1.2: https://www.lyontek.com.tw/pdf/ddr/LY68L6400-1.2.pdf)
- IPUS IPS1704L-SQ 64Mb (DS-SQ64-011) — https://dl.sipeed.com/TANG/Nano/Spec/IPUS_64Mbit_SQPI_Datasheet%C2%A0(1704).pdf
- Espressif ESP-PSRAM64/64H — https://cdn-shop.adafruit.com/product-files/4677/4677_esp-psram64_esp-psram64h_datasheet_en.pdf
- AP Memory APS1604M-SQR Rev 3.0 — https://www.apmemory.com/tw/downloadFiles/0324112120r6586620 ; APS6404L-SQH Rev 4.1 (already in scratchpad); APS6404L-SQN v3.6 — https://resources.ampheo.com/static/datasheets/ap-memory/aps6404l-sqn-sn.pdf
- ESP-IDF PSRAM ID decode (AP-family EID density encoding): https://github.com/espressif/esp-idf/blob/master/components/esp_psram/esp32/esp_psram_impl_quad.c
- Vilsion VTI7064: LCSC datasheet C139966 (JS-gated; Read-ID wording confirmed identical to AP via LCSC search index) + third-party driver https://github.com/qiaohechn-joe/pyrometer/blob/587e635f/src/bsp/dev/vti7064x.c which checks `(read_id() &gt;&gt; 48) == 0x0D5D`

Local PDFs/texts are in `/private/tmp/claude-501/-Users-kiro-CLionProjects-zephyr/ca0c50e7-2140-4345-b60b-7afd84168b67/scratchpad/` (`66-67WVS2M8ALL-BLL.pdf/.txt`, `66-67WVS4M8ALL-BLL.pdf/.txt`, `8M8F.txt`, `ly68_04.txt`, `ipus1704.txt`, `aps6404l_sqh.txt`, `aps1604m.txt`).

## ISSI Read ID protocol (identical across 2M8/4M8/8M8F datasheets)

- Opcode **9Fh**. SPI mode: "similar to Fast Read. But 24-bit address cycles are don't care and there are no wait cycles between address and data" — so **3 dummy address bytes are required**, 0 wait cycles, up to 104 MHz. **QPI mode works too** (unlike AP first-gen): command table row `Read ID 9Fh | SPI: S/S/0-wait/S/104 | QPI: Q/no addr/6 wait/Q/104`.
- Returns the **64-bit ID register MSB-first**: MF[63:56], KGD[55:48], EID[47:0]. **"The data is wrapped to bit 7 of MF ID data again after bit 0 of EID data until CE# goes to HIGH"** — ID repeats indefinitely while clocked.
- ISSI has no "Read ID only after reset" restriction (AP does: their 9F is only valid as power-up initialization right after Global Reset).

## Table: part → ID contents

| Part | MF[63:56] | KGD[55:48] | Density EID[47:45] | EID[44:0] | tCEM |
|---|---|---|---|---|---|
| ISSI IS66/67WVS1M8 (8Mb) | 0x9D | 0x5D pass / 0x55 fail | **000** (first-gen table: 000=8Mb, 001=16Mb, 010=32Mb) | "Reserved" — no documented per-die data | 4 µs ≤85 °C / 1 µs ≤105 °C |
| ISSI IS66/67WVS2M8ALL/BLL (16Mb) | 0x9D | 0x5D/0x55 | **001** | Reserved | 4 µs ≤85 °C / 1 µs ≤105 °C |
| ISSI IS66/67WVS4M8ALL/BLL (32Mb) | 0x9D | 0x5D/0x55 | **010** | Reserved | 4 µs ≤85 °C / 1 µs ≤105 °C |
| ISSI IS66/67WVS8M8**F**ALL/BLL (64Mb) | 0x9D | 0x5D/0x55 | **011** (F-die table: 000,001 reserved; 010=32Mb; 011=64Mb; 100=128Mb) | Reserved | 4 µs ≤85 °C / 1 µs ≤105 °C |
| ISSI IS66WVS16M8F (128Mb, exists) | 0x9D | 0x5D/0x55 | **100** | Reserved | (same family) |
| AP APS1604M/3204/6404L | 0x0D | 0x5D/0x55 | 000=16Mb, 001=32Mb, **010=64Mb** (esp-idf decode) | manufacturing ID (per-die) | 8 µs std (85 °C) / 3 µs extended (105 °C, was 4 µs pre-2021) |
| Lyontek LY68L6400 (64Mb) | **0x0D** (datasheet figure) | 0x5D/0x55 | EID[47:45] labeled "density" | EID[44:0] undocumented | 8 µs |
| IPUS IPS1704L/IPS6404 (64Mb) | **0x0D** (datasheet figure) | 0x5D/0x55 | EID[47:45] "device type" | EID[44:0] undocumented | 8 µs |
| Vilsion VTI7064 (64Mb) | **0x0D** (driver evidence + AP-identical datasheet wording) | 0x5D | EID as AP | — | (AP-clone) |
| Espressif ESP-PSRAM64/64H (64Mb, AP rebrand) | 0x0D | 0x5D/0x55 | AP encoding | — | 8 µs |
| CascadeTeq CSS6404/CSS3204 | datasheet not public; AP-class clone sold as APS6404 replacement | | | | |

Clone Read ID protocol note: Lyontek/IPUS/ESP/AP-first-gen all mark 9F "available **only in SPI mode**" (with the 24-bit dummy address); newer AP revisions added QPI read-ID limited to ≤84 MHz. ISSI supports it in both modes at 104 MHz.

## Vendor landscape

The entire quad-SPI PSRAM market splits into exactly two ID families: **ISSI (MF 0x9D)** and **AP Memory plus its clones/rebrands (MF 0x0D: AP, Lyontek, IPUS, Vilsion, Espressif rebrand, CascadeTeq)**. Searched and found **no** quad-SPI PSRAM from XTX, Zetta, AMIC, Netsol (SPI MRAM only), Fudan Micro, Winbond (HyperRAM/custom PSRAM only), GigaDevice, or Puya (flash only).

## Verdict — what ID bytes can and cannot tell you

Distinguishable:
- **ISSI vs AP-family**: yes, MF 0x9D vs 0x0D.
- **IS66WVS2M8 vs 4M8 vs 8M8**: yes — EID[47:45] = 001 / 010 / 011. Encoding is consistent across generations (code n → 8Mb &lt;&lt; n).

NOT distinguishable:
- **ALL (1.8 V) vs BLL (3.0 V)**: one shared datasheet, one ID register — identical ID. Not detectable.
- **IS66 (industrial) vs IS67 A1 (auto 85 °C) vs IS67 A2 (auto 105 °C)**: same datasheet and ID. This matters for tCEM: the datasheet gives 4 µs (≤85 °C) and 1 µs (≤105 °C) as *temperature-dependent limits in every grade's datasheet*, so a driver cannot learn the safe tCEM from ID — for unknown boards the safe assumption is **1 µs**.
- **Vendors within the AP-clone family**: Lyontek, IPUS, Vilsion, CascadeTeq and AP itself all return MF 0x0D, KGD 0x5D — mutually indistinguishable.
- **Per-die serial**: ISSI documents EID[44:0] as plain "Reserved" (no trace data claim); AP documents EID[44:0] as per-die "manufacturing ID". So on ISSI parts don't rely on EID[44:0] to differ between chips; on AP-family parts it *will* differ chip-to-chip and must be masked out when matching.

Cross-vendor trap: **density codes collide across families** — EID[47:45]=010 means 32 Mb on ISSI but 64 Mb on AP/clones. Density must be decoded conditionally on MF ID (ISSI: size = 8Mb&lt;&lt;n; AP-family: size = 16Mb&lt;&lt;n).

Other extracted facts relevant to the driver: ISSI page size is 1024 B and read/write bursts **always wrap within the 1 KB page** (no linear cross-page burst at all, unlike AP's linear mode); wrapped burst length is toggleable 1024/32 via C0h; the parts power up in SPI mode; QPI entry 35h / exit F5h; KGD 0x55 means the die never passed factory test (reject), and a fresh Read ID can be issued anytime, with the 64-bit pattern repeating while CE# stays low.

### 2026-09-01T20:28 Agent "/code-review medium сделай ревью незакомиченных и…" finished
I have full context on the driver, the binding, the overlay, and the user's own research notes (which corroborate the ID table: AP density 16Mbit&lt;&lt;n, ISSI 8Mbit&lt;&lt;n — the table entries all check out, including the density-collision handling by keying on MF). I performed the finder angles and verification in-context across the whole file. Summary of verification:

- ID table vs. research notes: AP codes 0/1/3 → 16/32/128 Mbit, ISSI 0/1/4 → 8/16/128 Mbit, named entries cover AP-2/ISSI-2/ISSI-3 — all consistent, masks correct. REFUTED as a bug.
- Sparse-table holes: with AUTO used every named entry is compiled in (`|| QSPI_PSRAM_AUTO_USED`), so the `mask[0] == 0` hole guard is safely defensive. REFUTED.
- `DT_INST_ENUM_HAS_VALUE(n, chip_variant, auto)` lowercase token matches the existing working `esp64h` pattern. REFUTED.
- BUILD_ASSERT bound change: with `!AUTO_USED` max designated index is 2 → size ≤ 3 = `QSPI_PSRAM_VARIANT_AUTO`. Correct. REFUTED.
- Moved `mem_size` check now runs after bus transactions — ordering change only, error still raised. REFUTED.
- ce-break-config coupled override: mechanism confirmed in code (lines 828-832); trigger needs a DT with one cell zero — PLAUSIBLE.
- Dead `tcem_us` argument in `QSPI_PSRAM_AUTO_ENTRY`: family rows are reachable only through `qspi_psram_auto_detect`, which only runs when `cfg-&gt;auto_detect`, and line 826 then unconditionally overwrites `time_to_break` — CONFIRMED (cosmetic/maintainability).
- `auto_detect` duplicates `verify_id`'s save/restore-addr-length + command-read block — CONFIRMED duplication.
- tests.yaml references an overlay that exists only as an untracked file — CONFIRMED from git status.

```json
[
  {
    "file": "drivers/memc/memc_mspi_qspi_psram.c",
    "line": 828,
    "summary": "In AUTO mode a partially-specified ce-break-config replaces BOTH mem_boundary and time_to_break, so a zero cell silently disables one of the two CE protections the table had set",
    "failure_scenario": "DT gives ce-break-config = &lt;2048 0&gt; (bound bursts, no time limit): the nonzero mem_boundary makes the driver take both DT cells, so time_to_break becomes 0 instead of the worst-grade tCEM (1/3 us) — on controllers that honor time_to_break the CE can stay asserted past tCEM and the PSRAM loses refresh, corrupting data; symmetrically &lt;0 3&gt; zeroes mem_boundary and lets bursts cross the 1 KB page/CE limit. Overriding only the nonzero cell (or rejecting a zero cell with a warning) would preserve the protection."
  },
  {
    "file": "drivers/memc/memc_mspi_qspi_psram.c",
    "line": 235,
    "summary": "The tcem_us parameter of QSPI_PSRAM_AUTO_ENTRY (8/4) is dead: family rows are only ever matched in AUTO mode, where time_to_break is unconditionally replaced by QSPI_PSRAM_WORST_TCEM_US",
    "failure_scenario": "Maintainability cost: a reader (or future editor) of the six family rows assumes the 8 us / 4 us values are applied, but qspi_psram_auto_detect is the only path to these entries and init (line 826) always overwrites time_to_break when cfg-&gt;auto_detect; the comment at line 231 even says so. Dropping the parameter (or setting .ce_refresh_us from QSPI_PSRAM_WORST_TCEM_US(mf) in the macro) removes the misleading dead data."
  },
  {
    "file": "samples/drivers/memc/tests.yaml",
    "line": 70,
    "summary": "The new twister case references boards/b_u585i_iot02a_mspi_qspi_psram_auto.overlay, which is currently untracked — committing the staged files without it breaks the test",
    "failure_scenario": "git status shows the overlay as '??' while tests.yaml is modified; a commit that misses the overlay makes sample.drivers.memc.stm32_mspi.qspi_psram.auto fail in CI with a missing DTC_OVERLAY_FILE."
  },
  {
    "file": "drivers/memc/memc_mspi_qspi_psram.c",
    "line": 460,
    "summary": "qspi_psram_auto_detect re-implements verify_id's Read-ID preamble (save addr_length, force 3-byte address, command_read, restore, error log) instead of sharing a helper",
    "failure_scenario": "Duplicated protocol detail: a future fix to the Read-ID transaction (e.g. a chip needing a different address phase or a wrap workaround) must be applied in two places; a small qspi_psram_read_id(psram, cmd, buf, len) helper used by both qspi_psram_verify_id (line 411) and qspi_psram_auto_detect would keep them in lockstep."
  }
]
```
