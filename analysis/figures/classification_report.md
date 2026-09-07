Ground truth: 429 healthy window(s), 646 worn window(s) (labeled via analysis/labels.py from operator-recorded recording sessions).

Threshold: `belt_band_amplitude` > 23.979 (baseline 3.682 + 3×6.766 std)

| | Predicted healthy | Predicted worn |
|---|---|---|
| **True healthy** | 125 | 4 |
| **True worn** | 219 | 427 |

Accuracy: 71.2% · Precision: 99.1% · Recall: 66.1% (n=775)
