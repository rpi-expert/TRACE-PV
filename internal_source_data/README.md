# Internal validation inputs

This directory holds local field measurements used by the capacitor validation
case study. The CSV data is intentionally excluded from Git because it is
site-specific source data and is not downloadable by the public project
scripts.

Expected filenames follow this pattern:

```text
export_data_YYYYMMDD-YYYYMMDD_<unit>.csv
```

The validation scripts expect these columns:

```text
time,AE_GHI,SP_ambient_temp,SP_rh,SP_ac_power,SP_cap_temp,SP_relay_temp,SP_internal_rh
```

Keep the original CSV files in this directory on the machine running the case
study. Do not commit them unless their publication and licensing have been
reviewed separately.
