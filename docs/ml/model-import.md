# Bringing a model in, and taking its weights out

[lake#681](https://github.com/oramasearch/lake/issues/681). A customer's tree models come from
scikit-learn, XGBoost and LightGBM. They are saved in those libraries' own files or exported to
ONNX. `register_model` reads each format into the engine's own gradient-boosted tree model
(`GbdtModel`, [gbdt.md](gbdt.md)). The model is then used exactly like one trained here by `score`,
`explain`, `expose_model`, `predict`, `list_models` and `delete_model`. `export_model` takes any
model's weights back out. The code is `include/zetta/ml/model_import.hpp` and
`src/core/ml/model_import.cpp`.

## The formats

| `format` | The file | How it is read |
|---|---|---|
| `onnx` | ONNX with one `ai.onnx.ml` `TreeEnsembleRegressor` or `TreeEnsembleClassifier`, opset 1 to 3. scikit-learn exports it through skl2onnx (gradient boosting, histogram gradient boosting, decision tree and random forest regressors); XGBoost and LightGBM through onnxmltools | the engine's own protobuf wire-format reader (`src/core/ml/protobuf_wire.hpp`), which reads only the ONNX messages a tree ensemble uses |
| `xgboost_json`, `xgboost_ubjson` | XGBoost's `Booster.save_model("model.json")` or `("model.ubj")`, booster `gbtree` | `nlohmann::json` (it parses UBJSON as well) |
| `lightgbm` | LightGBM's `Booster.save_model("model.txt")` | a parser here |
| `zetta` | the engine's own JSON weights | `GbdtModel::from_json` |

**Detection.** The format is read from the file unless `format` names it:

- a LightGBM model starts with `tree`;
- JSON with `learner` is XGBoost's, and JSON with `trees` is the engine's;
- a file that is not JSON but parses as UBJSON is XGBoost's;
- anything else is tried as ONNX.

**Sending the file.** `register_model` takes a text file as `weights` and any file as
`weights_base64`. A binary file (ONNX or UBJSON) must use `weights_base64`.

**Column names.** ONNX names no columns, so an ONNX model reads `f0`, `f1`, … unless
`feature_names` names them in order. A model the engine wrote carries its names in the ONNX
metadata (`feature_names`). For the other formats, `feature_names` renames the columns the file
names.

## What "exact" means

A converted model routes every row through the same leaves as its source library. Only the sum of
the leaves can differ, by float rounding, because the libraries add in 32-bit floats. The tests
allow 1e-5.

- **Splits.** The engine's tree goes left when `x < threshold`. A source's `<`, `<=`, `>` and `>=`
  each become a strict `<` with the true branch on the matching side. A `<=` threshold `t` becomes
  the next double above `t`.
- **32-bit comparisons.** XGBoost and onnxruntime with a float input compare 32-bit floats:
  `float(x) < t`. `float_less_bound(t)` gives the double `d` with `float(x) < t ⇔ x < d` for every
  double `x`. `d` is the midpoint between `t` and the float below it, nudged by
  round-to-nearest-even at the tie. LightGBM, and ONNX with a double input, compare doubles.
- **Missing values.** `TreeNode::missing_left` records where NaN goes:
  - XGBoost uses `default_left`.
  - ONNX uses `nodes_missing_value_tracks_true`; untracked, NaN fails every comparison and takes the
    false branch.
  - LightGBM depends on the split's missing type. `NaN` takes the default direction. `None` turns
    NaN into 0, so it goes where 0 goes. `Zero` is accepted only where zero and the default
    direction agree.

  An absent `missing_left` means right, which is what the strict `<` does with NaN and how every
  model trained here behaves, so a trained model's body is unchanged.
- **Link and base score.**
  - A logistic classifier stays `binary_logistic`: ONNX `LOGISTIC`, XGBoost `binary:logistic`,
    LightGBM `binary` (its `sigmoid:` factor is folded into the leaves). LightGBM writes the
    objective's parameters on the same line (`objective=binary sigmoid:1`); the first word is the
    objective, `sigmoid:` is read, and the other parameters are ignored. A refused objective is
    quoted by its first word (lake#780).
  - Regressors whose prediction is the sum of their trees become `squared_error`.
  - XGBoost's `base_score` is a probability for a logistic model, so the engine keeps its logit.
  - ONNX `AVERAGE` divides each leaf by the number of trees; so does LightGBM's `average_output`.
- **Covers.** XGBoost's `sum_hessian` and LightGBM's leaf and internal weights become the nodes'
  `cover`, which TreeSHAP needs ([tree-shap.md](tree-shap.md)). ONNX keeps no training weight, so
  each branch is weighted by its number of leaves. An ONNX model's explanations are therefore
  exact SHAP values over that background, not over the training data.

## What is refused

Each of these answers `not_supported`, says why, and keeps nothing. The refusal also lists what can
be brought.

| The file | Why |
|---|---|
| a pickle or joblib file | never unpickled: loading one runs code |
| an ONNX graph with anything but the ensemble (a `Scaler`, a neural network, `TreeEnsemble` from opset 5) | the engine runs the trees only. `Identity`, `Cast`, `ZipMap` and a multiplication by 1 are allowed around them, because they change no value |
| a multiclass or multi-target model | the engine scores one value |
| a classifier without a logistic link (random forest, decision tree, `post_transform` `NONE`) | its probabilities are averaged, and the engine's classifier output is a sigmoid. Bringing these needs an output rule of its own, which is deferred |
| categorical splits, LightGBM linear trees or `reg_sqrt`, XGBoost `dart` or `gblinear`, exponential objectives (`count:poisson`, `reg:gamma`, `tweedie`), `EQ`/`NEQ` splits | no exact form in the engine's trees |

## `export_model`

`export_model` is a read, `api:query`, and changes nothing. It takes `model_id` and `format`, and
answers `{model_id, name, format, file_name, media_type, encoding, content, bytes, features?}`.

- **`zetta`** (the default) works for every model. It answers the weights body exactly as kept, as
  text (`encoding: utf-8`).
- **`onnx`** works for a `binary_logistic` or `squared_error` tree model. It answers an ONNX file in
  base64, written with the same wire-format code:
  - the model is a `TreeEnsembleClassifier` (labels 0 and 1, `LOGISTIC`) or a
    `TreeEnsembleRegressor`;
  - it uses `ai.onnx.ml` opset 3, with double thresholds, leaf weights and base value, `BRANCH_LT`,
    and `missing_left` as `nodes_missing_value_tracks_true`;
  - its input is one double tensor `input` [N, features];
  - the column names are in the model's metadata.

  onnxruntime then scores it as the engine does. A Platt calibration is folded into the leaves and
  base value. An isotonic calibration, a ranker and every other kind of model are refused, and
  their weights download as `zetta`.

## Tests and fixtures

- **Fixtures.** `tests/fixtures/model_import/` holds small models, about 400 KB in all, made with
  scikit-learn, XGBoost, LightGBM and onnxruntime. Each sits beside the rows it was checked on and
  the library's own predictions (`<name>.expected.json`). `scripts/gen-model-import-fixtures.py`
  regenerates them offline in a throwaway virtual environment; none of those libraries is a build
  or test dependency (ADR-17).
- **`tests/ml/test_model_import.cpp`** checks that:
  - every format scores as its library does, rows with missing values included;
  - `float_less_bound` is exact at and around every midpoint;
  - each refusal names its reason;
  - a LightGBM objective line with parameters long enough to be held on the heap is read, its
    `sigmoid:` value applied, and a refused one quoted as written (lake#780, a use-after-free
    before);
  - an ONNX export reads back to the same scores;
  - a Platt calibration folds into the export;
  - `missing_left` round-trips and leaves a trained body unchanged.
- **Checking exports against onnxruntime.** Run the suite with `ZETTA_MODEL_EXPORT_DIR=<dir>` to
  write each export and the engine's scores. Then
  `scripts/gen-model-import-fixtures.py --check-exports <dir>` scores them with onnxruntime.
- **`tests/api/test_standalone_models.cpp`** (`BringCommonFormatsAndDownloadWeights`) brings ONNX,
  LightGBM and XGBoost UBJSON files through `register_model`, scores one, refuses a pickle and a
  random forest, and downloads weights both ways.
