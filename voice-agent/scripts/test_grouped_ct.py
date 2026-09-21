import numpy as np
import onnx
from onnx import helper, TensorProto
import onnxruntime as ort

# 复现 Kokoro 模型中失败的节点: /N.1/pool/ConvTranspose
# group=512, kernel=[3], strides=[2], output_padding=[1], pads=[1,1]
C = 512
L = 10
w = np.random.randn(C, 1, 3).astype(np.float32)
b = np.random.randn(C).astype(np.float32)

x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, C, L])
y = helper.make_tensor_value_info("y", TensorProto.FLOAT, None)

node = helper.make_node(
    "ConvTranspose", ["x", "W", "B"], ["y"],
    group=C, kernel_shape=[3], strides=[2], output_padding=[1], pads=[1, 1],
)

graph = helper.make_graph(
    [node], "grouped_ct", [x],
    [y],
    initializer=[
        helper.make_tensor("W", TensorProto.FLOAT, [C, 1, 3], w.flatten()),
        helper.make_tensor("B", TensorProto.FLOAT, [C], b),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 14)])
model.ir_version = 7

for ep in ["DmlExecutionProvider", "CPUExecutionProvider"]:
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    try:
        sess = ort.InferenceSession(model.SerializeToString(), so, providers=[ep])
        out = sess.run(None, {"x": np.random.randn(1, C, L).astype(np.float32)})
        print(f"{ep}: OK, out shape {out[0].shape}")
    except Exception as e:
        print(f"{ep}: FAIL -> {str(e)[:300]}")
