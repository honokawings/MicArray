"""Generate explicit ONNX interface test artifacts; never performance fixtures."""

import argparse
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper
from PIL import Image

from .data import FusionDataset


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--export-dir", required=True)
    parser.add_argument("--dataset", required=True)
    args = parser.parse_args()
    output = Path(args.export_dir)
    with np.load(output / "golden_input_output.npz", allow_pickle=False) as example:
        for key in ("input", "output"):
            values = np.asarray(example[key], dtype="<f4")
            destination = output / f"golden_{key}.f32"
            with destination.open("xb") as handle:
                handle.write(values.tobytes(order="C"))
    dataset = FusionDataset(args.dataset, "val")
    with np.load(dataset.files[0], allow_pickle=False) as sample:
        Image.fromarray(sample["rgb"]).save(output / "golden_rgb.png")
        heat = np.asarray(sample["heat"], dtype="<f4")
        with (output / "golden_heat.f32").open("xb") as handle:
            handle.write(heat.tobytes(order="C"))
        (output / "golden_heat.json").write_text(json.dumps({"rows": heat.shape[0], "columns": heat.shape[1],
                                                           "dtype": "float32_le", "layout": "row_major"}), encoding="utf-8")
    wrong = helper.make_tensor("bad", TensorProto.FLOAT, [1, 1260, 8], [0.0] * (1260 * 8))
    graph = helper.make_graph([helper.make_node("Constant", [], ["detections"], value=wrong)],
                              "explicit_shape_rejection_fixture",
                              [helper.make_tensor_value_info("rgb_srp", TensorProto.FLOAT, [1, 4, 192, 320])],
                              [helper.make_tensor_value_info("detections", TensorProto.FLOAT, [1, 1260, 8])])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=9)
    onnx.checker.check_model(model)
    onnx.save(model, output / "shape-invalid.onnx")
    print(str(output), flush=True)


if __name__ == "__main__":
    main()
