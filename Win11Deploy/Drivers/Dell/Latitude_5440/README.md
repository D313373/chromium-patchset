# Drivers: Dell Latitude 5440 (example)

Extract the Dell Windows 11 driver CAB / pack for **Latitude 5440** into this folder.

Required: one or more `.inf` files (subdirectories are fine).  
`Build-USB.ps1` fails validation if this folder has zero INFs (unless `-SkipValidation`).

Suggested components: chipset, storage (NVMe), NIC/Wi-Fi, GPU, audio, Thunderbolt/dock.
