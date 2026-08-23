# Verifying an exhibit bundle

For someone who has been handed a bundle and needs to know whether it is intact. You do
not need TRACE, a network connection, or any programming language.

Every bundle also contains this in `VERIFY.md`, so these instructions travel with it.

---

## Linux and macOS

From inside the bundle directory:

```bash
sha256sum -c MANIFEST.sha256      # the two manifests themselves
sha256sum -c MANIFEST.checksums   # every file listed in them
```

On macOS use `shasum -a 256 -c` instead of `sha256sum -c`.

Every line must report `OK`. One `FAILED` means that file is not the file that was
exported.

## Windows (PowerShell)

```powershell
Get-Content .\MANIFEST.checksums | ForEach-Object {
    $expected = $_.Substring(0, 64)
    $path     = $_.Substring(66)
    $actual   = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
    if ($actual -eq $expected) { Write-Host "OK      $path" }
    else                       { Write-Host "FAILED  $path" -ForegroundColor Red }
}
```

## Inside TRACE

**Reports ▸ Verify exhibit bundle…** re-checks a bundle and reports per-file results. It
uses the same hashing code as everything above, so the two cannot disagree.

TRACE additionally reports **files present in the bundle that no manifest lists**. A file
nobody vouched for is treated as a failure, not a curiosity.

---

## Reading the result

| Outcome | Meaning |
|---|---|
| Everything `OK` | Every file is byte-for-byte what was exported. |
| A content file `FAILED` | That file has changed since export. The rest may still be intact. |
| `MANIFEST.checksums` or `MANIFEST.json` fails against `MANIFEST.sha256` | A manifest was altered. Nothing below it can be relied on — the per-file results prove nothing. |
| A file is present but unlisted | Something was added after export. |

## What this proves, and what it does not

A matching digest proves the file is exactly what TRACE wrote when the bundle was
exported.

It does **not** prove who exported it, that the export was authorised, or that the
underlying material is what anyone says it is. This is an **integrity manifest, not a
digital signature**. Nothing in a bundle is signed, and TRACE does not claim otherwise
anywhere in the report or the manifests.

Separately from the bundle, the report names the SHA-256 of each evidence item's managed
original and of the exact model file behind every detection cited. Those digests let a
reader tie the bundle back to the material and the software that produced it — again, as
a record, not as proof of authorisation.
