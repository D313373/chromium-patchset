# Drivers: Lenovo ThinkPad T14 Gen 3 (example)

Download the Windows 11 driver pack for this machine type from Lenovo Support
(or export packages via Update Retriever) and extract INF trees into this folder.

Required: one or more `.inf` files (subdirectories are fine).

Suggested components: chipset, storage (NVMe), NIC/Wi-Fi, GPU, audio, dock/Thunderbolt.

Match tips (see `models.json`):
- Friendly name → `Win32_ComputerSystemProduct.Version` (e.g. `ThinkPad T14 Gen 3`)
- Machine type → first 4 chars of Model/Product (e.g. `21AH`, `21AK`)
