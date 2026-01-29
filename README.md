# mlsblk

macOS port of Linux `lsblk` — list block devices as a tree.

## Data sources

- **diskutil list -plist** — disk/partition structure (one call)
- **getmntinfo()** — mount points
- **diskutil info -plist** — FSTYPE, UUID, LABEL (per device when using `-f`)

## Build

```bash
make
make install   # install to /usr/local/bin (override with PREFIX=...)
```

Requires macOS (CoreFoundation). No external dependencies beyond the system.

## Usage

```bash
mlsblk                    # tree: NAME SIZE TYPE MOUNTPOINT
mlsblk -f                 # add FSTYPE, LABEL, UUID
mlsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT
mlsblk -J                 # JSON output
mlsblk -l                 # list format (no tree)
```

## Columns

| Column     | Source              |
|-----------|----------------------|
| NAME      | disk0, disk0s1, …    |
| SIZE      | diskutil (bytes)     |
| TYPE      | disk / part          |
| MOUNTPOINT| getmntinfo / plist   |
| FSTYPE    | diskutil info        |
| LABEL     | diskutil info        |
| UUID      | diskutil info        |

## Not supported (vs Linux lsblk)

- LVM, dm-crypt
- Loop devices
- MAJ:MIN device numbers

## License

MIT License. See [LICENSE](LICENSE) for details. Copyright (c) 2026 Murat Kaan Tekeli.
