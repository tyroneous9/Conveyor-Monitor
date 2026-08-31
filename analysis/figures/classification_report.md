**Against synthetic labels only** (backend/seed_sample_data.py's hand-picked signal model) -- not validated against a real observed worn belt.

Threshold: `belt_band_amplitude` > 2.351 (baseline 1.925 + 3×0.142 std)

| | Predicted healthy | Predicted worn |
|---|---|---|
| **True healthy** | 30 | 0 |
| **True worn** | 0 | 100 |

Accuracy: 100.0% · Precision: 100.0% · Recall: 100.0% (n=130)
