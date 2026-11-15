#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Regenerate tests/fixtures/model_import/ (lake#681), offline.

The engine reads models trained elsewhere (ONNX tree ensembles, XGBoost JSON and
UBJSON, LightGBM text) with its own readers. These fixtures are small models made
with the libraries a customer uses, each beside the rows it was checked on and
the reference library's own predictions for them, so the engine's conversion is
tested against the source of truth without any of those libraries being a build
or test dependency (ADR-17). Run it in a throwaway virtual environment:

    uv venv /tmp/fixtures && VIRTUAL_ENV=/tmp/fixtures uv pip install \
        numpy scikit-learn xgboost lightgbm onnx onnxruntime skl2onnx onnxmltools
    /tmp/fixtures/bin/python scripts/gen-model-import-fixtures.py

Each fixture is <name>.<ext> plus <name>.expected.json:
    {"format", "kind", "feature_names", "rows" (null = missing), "expected"}
where `expected` is the probability of the positive class for a classifier and
the prediction for a regressor.
"""
import json
import pathlib
import sys

import lightgbm
import numpy
import onnx.helper
import onnxruntime
import xgboost
from onnxmltools import convert_lightgbm, convert_xgboost
from onnxmltools.convert.common.data_types import FloatTensorType as MlFloat
from skl2onnx import convert_sklearn
from skl2onnx.common.data_types import FloatTensorType
from sklearn.neural_network import MLPRegressor
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.ensemble import (GradientBoostingClassifier, GradientBoostingRegressor,
                              HistGradientBoostingClassifier, RandomForestClassifier,
                              RandomForestRegressor)

OUT = pathlib.Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "model_import"
OUT.mkdir(parents=True, exist_ok=True)
NAMES = ["tenure", "spend", "tickets", "age"]

# With --check-exports DIR, check the engine's ONNX exports instead: the
# ModelImport.ExportedOnnxScoresAsTheEngineDoes test writes <name>.onnx and
# <name>.engine.json (its rows and the engine's own scores) into DIR when
# ZETTA_MODEL_EXPORT_DIR is set; onnxruntime must score each the same.
if len(sys.argv) == 3 and sys.argv[1] == "--check-exports":
    worst = 0.0
    for exported in sorted(pathlib.Path(sys.argv[2]).glob("*.onnx")):
        reference = json.loads(exported.with_suffix(".engine.json").read_text())
        rows = numpy.array([[numpy.nan if v is None else v for v in row]
                            for row in reference["rows"]], dtype=numpy.float64)
        session = onnxruntime.InferenceSession(exported.read_bytes(),
                                               providers=["CPUExecutionProvider"])
        outputs = session.run(None, {"input": rows})
        scores = (outputs[1][:, 1] if reference["kind"] == "classifier"
                  else outputs[0].reshape(-1))
        difference = float(numpy.max(numpy.abs(scores - numpy.array(reference["expected"]))))
        worst = max(worst, difference)
        print(f"{exported.name}: largest difference {difference:.3g}")
    print(f"worst: {worst:.3g}")
    sys.exit(0 if worst < 1e-5 else 1)

rng = numpy.random.default_rng(681)
ROWS = 400
X = numpy.column_stack([
    rng.integers(0, 60, ROWS).astype(float),
    numpy.round(rng.normal(80, 30, ROWS), 2),
    rng.integers(0, 8, ROWS).astype(float),
    rng.integers(18, 80, ROWS).astype(float),
])
raw = 0.08 * X[:, 2] * 4 - 0.05 * X[:, 0] + 0.01 * (X[:, 1] - 80) + rng.normal(0, 0.6, ROWS)
y_class = (raw > numpy.median(raw)).astype(int)
y_value = 3.0 * X[:, 2] - 0.4 * X[:, 0] + 0.05 * X[:, 1] + rng.normal(0, 1.0, ROWS)

# The same X with missing spend, for the models that learn a missing direction.
X_missing = X.copy()
X_missing[rng.random(ROWS) < 0.15, 1] = numpy.nan

# The rows each fixture is checked on: the first 40, plus rows on the split thresholds
# would be ideal but are model specific, so integers and two-decimal values cover it.
CHECK = slice(0, 40)


def rows_json(matrix):
    return [[None if numpy.isnan(v) else float(v) for v in row] for row in matrix]


def write(name, ext, data, kind, fmt, matrix, expected, names=NAMES):
    path = OUT / f"{name}.{ext}"
    if isinstance(data, str):
        path.write_text(data)
    else:
        path.write_bytes(data)
    (OUT / f"{name}.expected.json").write_text(json.dumps({
        "format": fmt, "kind": kind, "feature_names": names,
        "rows": rows_json(matrix), "expected": [float(v) for v in expected]}, indent=1) + "\n")
    print(f"{path.name}: {path.stat().st_size} bytes")


def onnx_predict(model_bytes, matrix, kind):
    session = onnxruntime.InferenceSession(model_bytes, providers=["CPUExecutionProvider"])
    feeds = {session.get_inputs()[0].name: matrix.astype(numpy.float32)}
    outputs = session.run(None, feeds)
    if kind == "regressor":
        return outputs[0].reshape(-1)
    probabilities = outputs[1]
    if isinstance(probabilities, list):  # ZipMap: a list of {label: probability}
        return numpy.array([row[1] for row in probabilities])
    return probabilities[:, 1]


def onnx_fixture(name, onnx_model, matrix, kind):
    data = onnx_model.SerializeToString()
    write(name, "onnx", data, kind, "onnx", matrix, onnx_predict(data, matrix, kind))


# skl2onnx 1.20 hands HistGradientBoosting's missing-value directions to ONNX as booleans,
# which onnx refuses; ONNX's attribute is a list of 0/1 ints, so pass them as ints.
_make_attribute = onnx.helper.make_attribute


def _make_attribute_with_int_flags(key, value, *args, **kwargs):
    if key == "nodes_missing_value_tracks_true":
        value = [int(bool(flag)) for flag in value]
    return _make_attribute(key, value, *args, **kwargs)


onnx.helper.make_attribute = _make_attribute_with_int_flags

sk_type = [("input", FloatTensorType([None, len(NAMES)]))]
ml_type = [("input", MlFloat([None, len(NAMES)]))]

# scikit-learn through skl2onnx.
gbc = GradientBoostingClassifier(n_estimators=12, max_depth=3, random_state=0).fit(X, y_class)
onnx_fixture("sklearn_gbc", convert_sklearn(gbc, initial_types=sk_type,
                                            options={id(gbc): {"zipmap": False}}),
             X[CHECK], "classifier")
gbr = GradientBoostingRegressor(n_estimators=12, max_depth=3, random_state=0).fit(X, y_value)
onnx_fixture("sklearn_gbr", convert_sklearn(gbr, initial_types=sk_type), X[CHECK], "regressor")
rfr = RandomForestRegressor(n_estimators=6, max_depth=4, random_state=0).fit(X, y_value)
onnx_fixture("sklearn_rfr", convert_sklearn(rfr, initial_types=sk_type), X[CHECK], "regressor")
hgb = HistGradientBoostingClassifier(max_iter=10, max_depth=3, random_state=0).fit(X_missing, y_class)
onnx_fixture("sklearn_hgb", convert_sklearn(hgb, initial_types=sk_type,
                                            options={id(hgb): {"zipmap": False}}),
             X_missing[CHECK], "classifier")
# Refused: averaged probabilities, not a logistic link.
rfc = RandomForestClassifier(n_estimators=4, max_depth=3, random_state=0).fit(X, y_class)
onnx_fixture("sklearn_rfc", convert_sklearn(rfc, initial_types=sk_type,
                                            options={id(rfc): {"zipmap": False}}),
             X[CHECK], "classifier")

# Refused: a preprocessing step beside the trees, and a neural network.
pipeline = make_pipeline(StandardScaler(), GradientBoostingRegressor(n_estimators=3, max_depth=2,
                                                                     random_state=0)).fit(X, y_value)
onnx_fixture("sklearn_scaler_pipeline", convert_sklearn(pipeline, initial_types=sk_type),
             X[CHECK], "regressor")
mlp = MLPRegressor(hidden_layer_sizes=(3,), max_iter=50, random_state=0).fit(X, y_value)
onnx_fixture("sklearn_mlp", convert_sklearn(mlp, initial_types=sk_type), X[CHECK], "regressor")

# XGBoost: its own JSON and UBJSON, and ONNX through onnxmltools.
xgb_params = {"max_depth": 3, "eta": 0.3, "seed": 0}
train = xgboost.DMatrix(X_missing, label=y_class, feature_names=NAMES)
xgb_class = xgboost.train({**xgb_params, "objective": "binary:logistic"}, train, 12)
check = xgboost.DMatrix(X_missing[CHECK], feature_names=NAMES)
xgb_class.save_model(str(OUT / "xgboost_binary.json"))
xgb_class.save_model(str(OUT / "xgboost_binary_ubj.ubj"))
expected = xgb_class.predict(check)
for ext in ("json", "ubj"):
    data = (OUT / ("xgboost_binary.json" if ext == "json" else "xgboost_binary_ubj.ubj")).read_bytes()
    write("xgboost_binary" if ext == "json" else "xgboost_binary_ubj", ext,
          data if ext == "ubj" else data.decode(), "classifier", f"xgboost_{ext}",
          X_missing[CHECK], expected)
xgb_reg = xgboost.train({**xgb_params, "objective": "reg:squarederror"},
                        xgboost.DMatrix(X_missing, label=y_value, feature_names=NAMES), 12)
xgb_reg.save_model(str(OUT / "xgboost_regression.json"))
write("xgboost_regression", "json", (OUT / "xgboost_regression.json").read_text(), "regressor",
      "xgboost_json", X_missing[CHECK],
      xgb_reg.predict(xgboost.DMatrix(X_missing[CHECK], feature_names=NAMES)))
xgb_sk = xgboost.XGBClassifier(n_estimators=12, max_depth=3, learning_rate=0.3,
                               random_state=0).fit(X_missing, y_class)
onnx_fixture("xgboost_binary_onnx", convert_xgboost(xgb_sk, initial_types=ml_type),
             X_missing[CHECK], "classifier")

# LightGBM: its text model, and ONNX through onnxmltools.
lgb_params = {"num_leaves": 8, "learning_rate": 0.3, "min_data_in_leaf": 5, "verbose": -1,
              "seed": 0}
lgb_class = lightgbm.train({**lgb_params, "objective": "binary"},
                           lightgbm.Dataset(X_missing, label=y_class, feature_name=NAMES), 12)
write("lightgbm_binary", "txt", lgb_class.model_to_string(), "classifier", "lightgbm",
      X_missing[CHECK], lgb_class.predict(X_missing[CHECK]))
lgb_reg = lightgbm.train({**lgb_params, "objective": "regression"},
                         lightgbm.Dataset(X, label=y_value, feature_name=NAMES), 12)
write("lightgbm_regression", "txt", lgb_reg.model_to_string(), "regressor", "lightgbm",
      X[CHECK], lgb_reg.predict(X[CHECK]))
lgb_sk = lightgbm.LGBMClassifier(n_estimators=12, num_leaves=8, learning_rate=0.3,
                                 min_child_samples=5, verbose=-1,
                                 random_state=0).fit(X_missing, y_class)
onnx_fixture("lightgbm_binary_onnx",
             convert_lightgbm(lgb_sk, initial_types=ml_type, zipmap=False),
             X_missing[CHECK], "classifier")
