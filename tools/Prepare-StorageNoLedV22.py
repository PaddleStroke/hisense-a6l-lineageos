"""Controlled A6L-only bypass of SDHCI's optional activity LED path."""
from pathlib import Path
import difflib
ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/storage-noled-source-v22-20260916'
p = KERNEL / 'drivers/mmc/host/sdhci-msm.c'
before = p.read_text()
anchor = '\thost->sdma_boundary = 0;'
assert before.count(anchor) == 1
after = before.replace(anchor, '''	/* Diagnostic V22: isolate the optional LED path identified by V21.
	 * Leave all storage power, clock and transfer settings unchanged.
	 */
	if (of_machine_is_compatible("hisense,hlte730t") &&
	    !strcmp(dev_name(&pdev->dev), "c0c4000.mmc")) {
		host->quirks |= SDHCI_QUIRK_NO_LED;
		dev_info(&pdev->dev, "A6L_STORAGE_V22 optional activity LED disabled\\n");
	}

''' + anchor)
OUT.mkdir(exist_ok=False)
(OUT / 'sdhci-msm-before.c').write_text(before)
(OUT / 'sdhci-msm-after.c').write_text(after)
(OUT / 'v22-only.patch').write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile='sdhci-msm-before.c', tofile='sdhci-msm-after.c')))
p.write_text(after)
print('A6L-only NO_LED diagnostic prepared; no regulator changes.')
