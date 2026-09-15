Сессия Apple Home 2026-09-15 01:54, плата A, прошивка `sram-s3` (ZMS 4 сектора; данные Matter/crypto, код crypto и арена malloc в SRAM — `../../tuning/sram/README.md`), хранилище стёрто перед сессией. **Сопряжение прошло: обе fabric Apple записаны.**

Время на плате (uptime консоли):

| Шаг | fabric 1 (00:00:29,3 → 00:00:47,9) |
|---|---|
| PASE (session establishment) | 1,5 с |
| до ArmFailSafe(60 s) — чтения Apple | 2,7 с |
| ArmFailSafe → AttestationRequest successful | 3,2 с |
| Attestation → CSRRequest successful (второй ArmFailSafe от Apple) | 5,6 с |
| → AddTrustedRoot successful | 0,9 с |
| → NOC chain validation successful | 0,8 с |
| Sigma1 → CASE session established | 1,5 с |
| CommissioningComplete → «Commissioning completed successfully» | 0,5 с |
| **Итого** | **18,5 с** |

Fabric 2 (Apple добавляет вторую fabric с fail-safe 30 с): ArmFailSafe(30 s) 00:01:13,7 → CSR 00:01:15,1 → AddNOC 00:01:17,8 → CASE 00:01:21,5 → CommissioningComplete 00:01:21,7, fabric 0x2 записана 00:01:21,9 — **~8 с, с большим запасом до 30 с**. В сессиях 2026-09-14 (`../20260914-2305`, `../20260914-2341`, прошивка с crypto/кучей в PSRAM) именно этот шаг срывался.

Самые долгие шаги fabric 1 (Attestation, CSR) — паузы на стороне Apple, вычисления на плате занимают доли секунды. Прочее в логе: `Long dispatch time` 224–605 мс вокруг CASE, `GetClock_RealTimeMS` не поддерживается (fallback на Last Known Good UTC), ошибки FRAM mb85rsxx и MAC при старте — известные, к сопряжению не относятся.
